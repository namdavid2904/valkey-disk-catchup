#include "server.h"
#include "repl_fanout.h"

/* Grants up to want bytes at rate bytes/s, bursting at most one second of rate. Rate 0 is unlimited. */
long long catchupBucketTake(catchupBucket *b, long long rate, long long want, long long now_us) {
    if (rate <= 0) return want;
    long long elapsed = now_us - b->last_us;
    if (elapsed < 0) elapsed = 0;
    b->last_us = now_us;
    b->tokens += elapsed >= 1000000 ? rate : elapsed * rate / 1000000;
    if (b->tokens > rate) b->tokens = rate;
    long long got = want < b->tokens ? want : b->tokens;
    b->tokens -= got;
    return got;
}

long long catchupAllowance(long long want) {
    static catchupBucket bucket;
    return catchupBucketTake(&bucket, (long long)server.repl_catchup_max_mbps * 1024 * 1024, want, ustime());
}

typedef struct catchupGroup {
    char replid[CONFIG_RUN_ID_SIZE + 1];
    long long start;
    long long cursor;
    long long ready_us;
    int started;
    uint64_t *ids;
    int n;
} catchupGroup;

static int serverWrite(uint64_t id, const char *buf, long long len) {
    client *c = lookupClientByID(id);
    if (!c || c->flag.close_asap) return -1;
    addReplyProto(c, buf, len);
    return 0;
}

static long long serverPending(uint64_t id) {
    client *c = lookupClientByID(id);
    if (!c || c->flag.close_asap) return -1;
    return getClientOutputBufferMemoryUsage(c);
}

/* The suffix code must also make this replica's next PSYNC go FULL, or it rejoins a group. */
static void serverDrop(uint64_t id) {
    client *c = lookupClientByID(id);
    if (c) freeClientAsync(c);
}

catchupFanoutOps catchupOps = {NULL, NULL, serverWrite, serverPending, serverDrop, catchupAllowance};
catchupFanoutStats catchupStats;
static list *groups;

static void groupFree(void *g) {
    zfree(((catchupGroup *)g)->ids);
    zfree(g);
}

int catchupFanoutJoin(uint64_t id, const char *replid, long long start, long long now_us) {
    if (!catchupOps.read) return C_ERR;
    if (!groups) {
        groups = listCreate();
        listSetFreeMethod(groups, groupFree);
    }
    catchupGroup *g = NULL;
    listIter li;
    listNode *ln;
    listRewind(groups, &li);
    while ((ln = listNext(&li))) {
        catchupGroup *cur = listNodeValue(ln);
        if (!cur->started && cur->start == start && !strcmp(cur->replid, replid)) {
            g = cur;
            catchupStats.waiters_coalesced++;
            break;
        }
    }
    if (!g) {
        g = zcalloc(sizeof(*g));
        valkey_strlcpy(g->replid, replid, sizeof(g->replid));
        g->start = g->cursor = start;
        g->ready_us = now_us + (long long)server.repl_catchup_delay_ms * 1000;
        listAddNodeTail(groups, g);
    }
    g->ids = zrealloc(g->ids, sizeof(uint64_t) * (g->n + 1));
    g->ids[g->n++] = id;
    return C_OK;
}

static void removeWaiter(catchupGroup *g, int i) {
    g->ids[i] = g->ids[--g->n];
}

/* Sends at most CATCHUP_HIGH_WATER bytes. Returns 0 once the group is finished. */
static int groupStep(catchupGroup *g) {
    static char buf[CATCHUP_CHUNK];
    long long sent = 0;
    while (sent < CATCHUP_HIGH_WATER) {
        long long fastest = LLONG_MAX;
        for (int i = 0; i < g->n;) {
            long long p = catchupOps.pending(g->ids[i]);
            if (p < 0) {
                removeWaiter(g, i);
                continue;
            }
            if (p > CATCHUP_SLOW_BYTES) {
                catchupOps.drop(g->ids[i]);
                catchupStats.fallbacks++;
                removeWaiter(g, i);
                continue;
            }
            if (p < fastest) fastest = p;
            i++;
        }
        if (g->n == 0) return 0;
        /* The group moves at its fastest waiter's pace; the slow ones queue up until dropped. */
        if (fastest >= CATCHUP_HIGH_WATER) return 1;
        long long want = catchupOps.allow((long long)CATCHUP_CHUNK * g->n) / g->n;
        if (want == 0) return 1;
        long long n = catchupOps.read(g->replid, g->cursor, buf, want);
        if (n < 0) {
            for (int i = 0; i < g->n; i++) catchupOps.drop(g->ids[i]);
            catchupStats.fallbacks += g->n;
            return 0;
        }
        if (n == 0) {
            for (int i = 0; i < g->n; i++) catchupOps.done(g->ids[i], g->cursor);
            return 0;
        }
        for (int i = 0; i < g->n;) {
            if (catchupOps.write(g->ids[i], buf, n) < 0) {
                removeWaiter(g, i);
                continue;
            }
            catchupStats.bytes_sent += n;
            i++;
        }
        catchupStats.disk_bytes_read += n;
        g->cursor += n;
        sent += n;
    }
    return 1;
}

void catchupFanoutStep(long long now_us) {
    if (!groups) return;
    listIter li;
    listNode *ln;
    listRewind(groups, &li);
    while ((ln = listNext(&li))) {
        catchupGroup *g = listNodeValue(ln);
        if (now_us < g->ready_us) continue;
        if (!g->started) {
            g->started = 1;
            catchupStats.groups++;
        }
        if (!groupStep(g)) listDelNode(groups, ln);
    }
}

void catchupFanoutReset(void) {
    if (groups) listEmpty(groups);
    memset(&catchupStats, 0, sizeof(catchupStats));
}

unsigned long catchupFanoutWaiting(void) {
    return groups ? listLength(groups) : 0;
}
