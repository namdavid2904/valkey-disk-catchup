#ifndef REPL_FANOUT_H
#define REPL_FANOUT_H

#include <stdint.h>

/* Token bucket that caps disk catch-up traffic. Live replication never goes through it. */
typedef struct catchupBucket {
    long long tokens;
    long long last_us;
} catchupBucket;

long long catchupBucketTake(catchupBucket *b, long long rate, long long want, long long now_us);
long long catchupAllowance(long long want);

/* Shared catch-up read. Replicas that ask for the same (replid, start) within
 * repl-catchup-delay join one group: the suffix is read once and every chunk is
 * written to all of them. Waiters are client ids, so a freed client simply
 * stops answering write/pending. */
typedef struct catchupFanoutOps {
    /* Suffix bytes at off: count read, 0 once the ring covers off, -1 on error. Set by the suffix code. */
    long long (*read)(const char *replid, long long off, char *buf, long long len);
    /* Gap sent; continue this replica from the in-memory ring at off. Set by the suffix code. */
    void (*done)(uint64_t id, long long off);
    int (*write)(uint64_t id, const char *buf, long long len); /* -1 if the client is gone */
    long long (*pending)(uint64_t id);                         /* queued output bytes, -1 if gone */
    void (*drop)(uint64_t id);                                 /* this replica alone falls back to FULL */
    long long (*allow)(long long want);                        /* bandwidth cap */
} catchupFanoutOps;

typedef struct catchupFanoutStats {
    long long disk_bytes_read;   /* suffix bytes read, once per group */
    long long bytes_sent;        /* catch-up bytes written to replicas */
    long long groups;            /* shared reads started */
    long long waiters_coalesced; /* replicas that joined an existing group */
    long long fallbacks;         /* replicas dropped to FULL (slow, or read error) */
} catchupFanoutStats;

#define CATCHUP_CHUNK (16 * 1024)
#define CATCHUP_HIGH_WATER (1024 * 1024)      /* stop a step once the fastest waiter has this much queued */
#define CATCHUP_SLOW_BYTES (64 * 1024 * 1024) /* a waiter this far behind is dropped */

extern catchupFanoutOps catchupOps;
extern catchupFanoutStats catchupStats;

int catchupFanoutJoin(uint64_t id, const char *replid, long long start, long long now_us);
void catchupFanoutStep(long long now_us);
void catchupFanoutReset(void);
unsigned long catchupFanoutWaiting(void);

#endif
