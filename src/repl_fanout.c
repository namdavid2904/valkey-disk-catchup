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
