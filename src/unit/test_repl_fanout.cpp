/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <map>

extern "C" {
#include "repl_fanout.h"
#include "server.h"
}

static const long long MB = 1024 * 1024;

TEST(CatchupBucketTest, ZeroRateIsUnlimited) {
    catchupBucket b = {};
    EXPECT_EQ(catchupBucketTake(&b, 0, 500 * MB, 0), 500 * MB);
    EXPECT_EQ(catchupBucketTake(&b, 0, 500 * MB, 0), 500 * MB);
}

TEST(CatchupBucketTest, FreshBucketAllowsOneSecondBurst) {
    catchupBucket b = {};
    EXPECT_EQ(catchupBucketTake(&b, 10 * MB, 64 * MB, 5000000), 10 * MB);
    EXPECT_EQ(catchupBucketTake(&b, 10 * MB, 64 * MB, 5000000), 0);
}

TEST(CatchupBucketTest, RefillsByElapsedTime) {
    catchupBucket b = {};
    catchupBucketTake(&b, 10 * MB, 10 * MB, 1000000);
    /* 250 ms at 10 MB/s is 2.5 MB. */
    EXPECT_EQ(catchupBucketTake(&b, 10 * MB, 64 * MB, 1250000), 10 * MB / 4);
}

TEST(CatchupBucketTest, SmallAsksLeaveTheRest) {
    catchupBucket b = {};
    EXPECT_EQ(catchupBucketTake(&b, 10 * MB, 4 * MB, 1000000), 4 * MB);
    EXPECT_EQ(catchupBucketTake(&b, 10 * MB, 64 * MB, 1000000), 6 * MB);
}

TEST(CatchupBucketTest, LongIdleDoesNotOverfill) {
    catchupBucket b = {};
    catchupBucketTake(&b, 10 * MB, 10 * MB, 1000000);
    EXPECT_EQ(catchupBucketTake(&b, 10 * MB, 1000 * MB, 3600LL * 1000000), 10 * MB);
}

TEST(CatchupBucketTest, ClockGoingBackGivesNothingNew) {
    catchupBucket b = {};
    catchupBucketTake(&b, 10 * MB, 10 * MB, 2000000);
    EXPECT_EQ(catchupBucketTake(&b, 10 * MB, 64 * MB, 1000000), 0);
}

TEST(CatchupBucketTest, LoweringRateClampsSavedTokens) {
    catchupBucket b = {};
    catchupBucketTake(&b, 100 * MB, 0, 1000000);
    EXPECT_EQ(catchupBucketTake(&b, 1 * MB, 64 * MB, 1000000), 1 * MB);
}

static const char *ID_A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *ID_B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

struct FakeReplica {
    long long pending = 0;
    long long received = 0;
    long long done_at = -1;
    bool gone = false;
    bool dropped = false;
    bool drains = true; /* a fast replica empties its buffer between steps */
};

static std::map<uint64_t, FakeReplica> replicas;
static long long suffix_end; /* read returns 0 at this offset */
static long long read_calls;
static bool read_fails;
static long long allow_left; /* bytes the cap still grants, -1 = unlimited */

static long long fakeRead(const char *, long long off, char *buf, long long len) {
    read_calls++;
    if (read_fails) return -1;
    long long n = suffix_end - off < len ? suffix_end - off : len;
    memset(buf, 'x', n);
    return n;
}
static void fakeDone(uint64_t id, long long off) {
    replicas[id].done_at = off;
}
static int fakeWrite(uint64_t id, const char *, long long len) {
    FakeReplica &r = replicas[id];
    if (r.gone) return -1;
    r.pending += len;
    r.received += len;
    return 0;
}
static long long fakePending(uint64_t id) {
    return replicas[id].gone ? -1 : replicas[id].pending;
}
static void fakeDrop(uint64_t id) {
    replicas[id].dropped = true;
}
static long long fakeAllow(long long want) {
    if (allow_left < 0) return want;
    long long got = want < allow_left ? want : allow_left;
    allow_left -= got;
    return got;
}

class CatchupFanoutTest : public ::testing::Test {
  protected:
    catchupFanoutOps saved;
    int saved_delay;

    void SetUp() override {
        saved = catchupOps;
        saved_delay = server.repl_catchup_delay_ms;
        catchupOps = {fakeRead, fakeDone, fakeWrite, fakePending, fakeDrop, fakeAllow};
        catchupFanoutReset();
        replicas.clear();
        suffix_end = 0;
        read_calls = 0;
        read_fails = false;
        allow_left = -1;
        server.repl_catchup_delay_ms = 1000;
    }

    void TearDown() override {
        catchupFanoutReset();
        catchupOps = saved;
        server.repl_catchup_delay_ms = saved_delay;
    }

    /* Steps until no group is left, draining fast replicas between steps like the event loop would. */
    void runToEnd(long long now_us) {
        for (int i = 0; i < 100000 && catchupFanoutWaiting(); i++) {
            catchupFanoutStep(now_us);
            for (auto &kv : replicas)
                if (kv.second.drains) kv.second.pending = 0;
        }
    }
};

TEST_F(CatchupFanoutTest, NoReaderMeansNoDiskPath) {
    catchupOps.read = nullptr;
    EXPECT_EQ(catchupFanoutJoin(1, ID_A, 100, 0), C_ERR);
    EXPECT_EQ(catchupFanoutWaiting(), 0u);
}

TEST_F(CatchupFanoutTest, ThreeReplicasSameGapReadTheDiskOnce) {
    suffix_end = 100 + 5 * 1024 * 1024;
    for (uint64_t id = 1; id <= 3; id++) ASSERT_EQ(catchupFanoutJoin(id, ID_A, 100, id * 1000), C_OK);
    runToEnd(2000000);
    EXPECT_EQ(catchupStats.disk_bytes_read, 5 * 1024 * 1024);
    EXPECT_EQ(catchupStats.bytes_sent, 15 * 1024 * 1024);
    EXPECT_EQ(catchupStats.groups, 1);
    EXPECT_EQ(catchupStats.waiters_coalesced, 2);
    for (uint64_t id = 1; id <= 3; id++) {
        EXPECT_EQ(replicas[id].received, 5 * 1024 * 1024);
        EXPECT_EQ(replicas[id].done_at, suffix_end);
    }
}

TEST_F(CatchupFanoutTest, NothingIsSentBeforeTheDelay) {
    suffix_end = 200;
    catchupFanoutJoin(1, ID_A, 100, 0);
    catchupFanoutStep(999999);
    EXPECT_EQ(read_calls, 0);
    catchupFanoutStep(1000000);
    EXPECT_GT(read_calls, 0);
}

TEST_F(CatchupFanoutTest, ZeroDelayStartsOnTheNextStep) {
    server.repl_catchup_delay_ms = 0;
    suffix_end = 200;
    catchupFanoutJoin(1, ID_A, 100, 0);
    catchupFanoutStep(0);
    EXPECT_EQ(replicas[1].done_at, 200);
}

TEST_F(CatchupFanoutTest, DifferentStartOrReplidGetsItsOwnRead) {
    suffix_end = 10000;
    catchupFanoutJoin(1, ID_A, 100, 0);
    catchupFanoutJoin(2, ID_A, 200, 0);
    catchupFanoutJoin(3, ID_B, 100, 0);
    runToEnd(1000000);
    EXPECT_EQ(catchupStats.groups, 3);
    EXPECT_EQ(catchupStats.waiters_coalesced, 0);
    EXPECT_EQ(catchupStats.disk_bytes_read, 9900 + 9800 + 9900);
}

TEST_F(CatchupFanoutTest, LateArrivalAfterSendStartsGetsANewGroup) {
    suffix_end = 100 + 4 * 1024 * 1024;
    catchupFanoutJoin(1, ID_A, 100, 0);
    catchupFanoutStep(1000000);
    catchupFanoutJoin(2, ID_A, 100, 1000000);
    runToEnd(3000000);
    EXPECT_EQ(catchupStats.groups, 2);
    EXPECT_EQ(replicas[2].received, 4 * 1024 * 1024);
}

TEST_F(CatchupFanoutTest, SlowReplicaIsDroppedAloneAndTheOthersFinish) {
    suffix_end = 100 + 80 * 1024 * 1024;
    catchupFanoutJoin(1, ID_A, 100, 0);
    catchupFanoutJoin(2, ID_A, 100, 0);
    replicas[2].drains = false;
    runToEnd(1000000);
    EXPECT_TRUE(replicas[2].dropped);
    EXPECT_FALSE(replicas[1].dropped);
    EXPECT_EQ(replicas[1].done_at, suffix_end);
    EXPECT_EQ(replicas[2].done_at, -1);
    EXPECT_EQ(catchupStats.fallbacks, 1);
    EXPECT_EQ(catchupStats.disk_bytes_read, 80 * 1024 * 1024);
}

TEST_F(CatchupFanoutTest, ALoneSlowReplicaIsNotDropped) {
    /* With nobody faster, slow is just the link speed; the group waits for it. */
    suffix_end = 100 + 2 * 1024 * 1024;
    catchupFanoutJoin(1, ID_A, 100, 0);
    replicas[1].drains = false;
    for (int i = 0; i < 50; i++) catchupFanoutStep(1000000);
    EXPECT_FALSE(replicas[1].dropped);
    EXPECT_LE(replicas[1].pending, CATCHUP_HIGH_WATER + CATCHUP_CHUNK);
    replicas[1].drains = true;
    runToEnd(1000000);
    EXPECT_EQ(replicas[1].done_at, suffix_end);
}

TEST_F(CatchupFanoutTest, GoneClientIsForgottenWithoutAFallback) {
    suffix_end = 100 + 1024 * 1024;
    catchupFanoutJoin(1, ID_A, 100, 0);
    catchupFanoutJoin(2, ID_A, 100, 0);
    replicas[2].gone = true;
    runToEnd(1000000);
    EXPECT_EQ(replicas[1].done_at, suffix_end);
    EXPECT_FALSE(replicas[2].dropped);
    EXPECT_EQ(catchupStats.fallbacks, 0);
}

TEST_F(CatchupFanoutTest, ReadErrorDropsTheWholeGroup) {
    suffix_end = 10000;
    read_fails = true;
    catchupFanoutJoin(1, ID_A, 100, 0);
    catchupFanoutJoin(2, ID_A, 100, 0);
    catchupFanoutStep(1000000);
    EXPECT_TRUE(replicas[1].dropped);
    EXPECT_TRUE(replicas[2].dropped);
    EXPECT_EQ(catchupStats.fallbacks, 2);
    EXPECT_EQ(catchupFanoutWaiting(), 0u);
}

TEST_F(CatchupFanoutTest, BandwidthCapLimitsBytesPerStep) {
    suffix_end = 100 + 1024 * 1024;
    allow_left = 40000;
    catchupFanoutJoin(1, ID_A, 100, 0);
    catchupFanoutJoin(2, ID_A, 100, 0);
    catchupFanoutStep(1000000);
    /* The cap counts every byte sent, so two replicas share it. */
    EXPECT_EQ(catchupStats.bytes_sent, 40000);
    EXPECT_EQ(catchupStats.disk_bytes_read, 20000);
    EXPECT_EQ(replicas[1].received, 20000);
    EXPECT_EQ(replicas[2].received, 20000);
}

TEST_F(CatchupFanoutTest, BandwidthCapNeverOvershootsWithUnevenSplit) {
    suffix_end = 100 + 1024 * 1024;
    allow_left = 40000;
    for (uint64_t id = 1; id <= 3; id++) catchupFanoutJoin(id, ID_A, 100, 0);
    catchupFanoutStep(1000000);
    EXPECT_LE(catchupStats.bytes_sent, 40000);
    EXPECT_EQ(catchupStats.bytes_sent, 3 * catchupStats.disk_bytes_read);
    EXPECT_GT(catchupStats.bytes_sent, 40000 - 3);
}
