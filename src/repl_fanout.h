#ifndef REPL_FANOUT_H
#define REPL_FANOUT_H

/* Token bucket that caps disk catch-up traffic. Live replication never goes through it. */
typedef struct catchupBucket {
    long long tokens;
    long long last_us;
} catchupBucket;

long long catchupBucketTake(catchupBucket *b, long long rate, long long want, long long now_us);
long long catchupAllowance(long long want);

#endif
