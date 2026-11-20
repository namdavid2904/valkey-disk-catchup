/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "server.h"
}

/* Two distinct replication IDs (40 hex chars, CONFIG_RUN_ID_SIZE). */
static const char *ID_A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *ID_B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char *ID_C = "cccccccccccccccccccccccccccccccccccccccc";

class CatchupTest : public ::testing::Test {
  protected:
    list *saved_holds;

    void SetUp() override {
        saved_holds = server.catchup_holds;
        server.catchup_holds = listCreate();
        listSetFreeMethod(server.catchup_holds, zfree);
        server.catchup_hold_ttl = 0;
        server.unixtime = 1000;
        memcpy(server.replid, ID_A, CONFIG_RUN_ID_SIZE + 1);
        memset(server.replid2, '0', CONFIG_RUN_ID_SIZE);
        server.replid2[CONFIG_RUN_ID_SIZE] = '\0';
        server.second_replid_offset = -1;
    }

    void TearDown() override {
        listRelease(server.catchup_holds);
        server.catchup_holds = saved_holds;
    }
};

/* catchupDecide() must follow: history, ring, too-old, suffix, sibling, full.
 * Check all 32 combinations against that rule. */
TEST_F(CatchupTest, DecideMatrix) {
    for (int bits = 0; bits < 32; bits++) {
        int history_ok = (bits >> 0) & 1;
        int in_ring = (bits >> 1) & 1;
        int too_old = (bits >> 2) & 1;
        int in_suffix = (bits >> 3) & 1;
        int sibling_has = (bits >> 4) & 1;

        catchupDecision expected;
        if (!history_ok)
            expected = CATCHUP_FULL;
        else if (in_ring)
            expected = CATCHUP_MEMORY;
        else if (!too_old)
            expected = CATCHUP_FULL;
        else if (in_suffix)
            expected = CATCHUP_DISK;
        else if (sibling_has)
            expected = CATCHUP_REDIRECT;
        else
            expected = CATCHUP_FULL;

        EXPECT_EQ(expected, catchupDecide(history_ok, in_ring, too_old, in_suffix, sibling_has))
            << "history_ok=" << history_ok << " in_ring=" << in_ring << " too_old=" << too_old
            << " in_suffix=" << in_suffix << " sibling_has=" << sibling_has;
    }
}

/* Key invariants, spelled out on their own. */
TEST_F(CatchupTest, DecideInvariants) {
    /* A bad replication ID never reaches disk or redirect. */
    EXPECT_EQ(CATCHUP_FULL, catchupDecide(0, 0, 1, 1, 1));
    /* An offset that is not too old (ahead of us / no backlog) is FULL. */
    EXPECT_EQ(CATCHUP_FULL, catchupDecide(1, 0, 0, 1, 1));
    /* Memory wins over disk. */
    EXPECT_EQ(CATCHUP_MEMORY, catchupDecide(1, 1, 0, 1, 1));
    /* Disk wins over redirect. */
    EXPECT_EQ(CATCHUP_DISK, catchupDecide(1, 0, 1, 1, 1));
}

TEST_F(CatchupTest, StubsBehaveLikeStock) {
    EXPECT_EQ(0, replSuffixHasRange(100));
    EXPECT_EQ(0, catchupSiblingHasRange(100));
}

TEST_F(CatchupTest, EmptyHoldsMeansNoSpill) {
    EXPECT_EQ(0UL, listLength(server.catchup_holds));
    EXPECT_EQ(-1, replHoldsMinOffset());
}

TEST_F(CatchupTest, AddReplaceRemove) {
    replHoldAdd("127.0.0.1:7002", ID_A, 100);
    replHoldAdd("127.0.0.1:7003", ID_A, 50);
    EXPECT_EQ(2UL, listLength(server.catchup_holds));
    /* The replica will PSYNC from ack + 1. */
    EXPECT_EQ(51, replHoldsMinOffset());

    /* Same replica disconnecting again refreshes its hold, no duplicate. */
    replHoldAdd("127.0.0.1:7003", ID_A, 200);
    EXPECT_EQ(2UL, listLength(server.catchup_holds));
    EXPECT_EQ(101, replHoldsMinOffset());

    replHoldRemove("127.0.0.1:7002");
    EXPECT_EQ(1UL, listLength(server.catchup_holds));
    EXPECT_EQ(201, replHoldsMinOffset());

    replHoldRemove("127.0.0.1:9999"); /* Unknown name: no-op. */
    EXPECT_EQ(1UL, listLength(server.catchup_holds));

    replHoldsClear();
    EXPECT_EQ(-1, replHoldsMinOffset());
}

TEST_F(CatchupTest, SuffixBaseMovedDropsUncoveredHolds) {
    replHoldAdd("r1:1", ID_A, 99);  /* needs 100 */
    replHoldAdd("r2:1", ID_A, 199); /* needs 200 */
    replHoldAdd("r3:1", ID_A, 299); /* needs 300 */

    replHoldsSuffixBaseMoved(200); /* File still starts at 200: r2 is fine. */
    EXPECT_EQ(2UL, listLength(server.catchup_holds));
    EXPECT_EQ(200, replHoldsMinOffset());

    replHoldsSuffixBaseMoved(301);
    EXPECT_EQ(0UL, listLength(server.catchup_holds));
}

TEST_F(CatchupTest, PruneByTtl) {
    server.catchup_hold_ttl = 10;
    replHoldAdd("old:1", ID_A, 10); /* created at 1000 */
    server.unixtime = 1005;
    replHoldAdd("new:1", ID_A, 20); /* created at 1005 */

    server.unixtime = 1010; /* old is exactly 10s old: kept. */
    replHoldsPrune();
    EXPECT_EQ(2UL, listLength(server.catchup_holds));

    server.unixtime = 1011; /* old is 11s old: expired. */
    replHoldsPrune();
    EXPECT_EQ(1UL, listLength(server.catchup_holds));
    EXPECT_EQ(21, replHoldsMinOffset());

    server.catchup_hold_ttl = 0; /* 0 means never expire. */
    server.unixtime = 1000000;
    replHoldsPrune();
    EXPECT_EQ(1UL, listLength(server.catchup_holds));
}

TEST_F(CatchupTest, PruneWrongReplicationId) {
    replHoldAdd("a:1", ID_A, 10);
    replHoldAdd("c:1", ID_C, 10);
    replHoldsPrune();
    /* Only the hold on our current history survives. */
    EXPECT_EQ(1UL, listLength(server.catchup_holds));
    replHold *h = (replHold *)listNodeValue(listFirst(server.catchup_holds));
    EXPECT_STREQ("a:1", h->name);
}

/* After a promotion (shiftReplicationId), the old ID becomes replid2 and is
 * only honored up to second_replid_offset, like the PSYNC history check. */
TEST_F(CatchupTest, Replid2HonoredOnlyUpToSecondOffset) {
    memcpy(server.replid, ID_B, CONFIG_RUN_ID_SIZE + 1);
    memcpy(server.replid2, ID_A, CONFIG_RUN_ID_SIZE + 1);
    server.second_replid_offset = 501;

    replHoldAdd("ok:1", ID_A, 500);      /* will ask for 501: allowed */
    replHoldAdd("too-far:1", ID_A, 501); /* will ask for 502: not allowed */
    replHoldAdd("current:1", ID_B, 900); /* current history: always valid */

    replHold ok;
    memset(&ok, 0, sizeof(ok));
    memcpy(ok.replid, ID_A, CONFIG_RUN_ID_SIZE + 1);
    ok.ack_off = 500;
    replHold too_far = ok;
    too_far.ack_off = 501;
    EXPECT_TRUE(replHoldIsValid(&ok));
    EXPECT_FALSE(replHoldIsValid(&too_far));

    replHoldsPrune();
    EXPECT_EQ(2UL, listLength(server.catchup_holds));
    EXPECT_EQ(501, replHoldsMinOffset());
}
