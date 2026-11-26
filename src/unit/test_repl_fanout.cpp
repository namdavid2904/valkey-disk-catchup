/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "repl_fanout.h"
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
