#include <gtest/gtest.h>
#include "helpers/StaticPoolPacketManager.h"

using namespace mesh;

static Packet* makePacket(StaticPoolPacketManager& mgr, uint8_t route_type) {
    Packet* p = mgr.allocNew();
    p->header = route_type | (PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT);
    p->payload_len = 0;
    p->path_len = 0;
    return p;
}

// ── removeOverdueFlood ────────────────────────────────────────────────────────

TEST(StaticPoolPacketManager, RemoveOverdueFlood_EmptyQueueReturnsNull) {
    StaticPoolPacketManager mgr(4);
    EXPECT_EQ(nullptr, mgr.removeOverdueFlood(100000, 60000));
}

TEST(StaticPoolPacketManager, RemoveOverdueFlood_KeepsFloodWithinLimit) {
    StaticPoolPacketManager mgr(4);
    mgr.queueOutbound(makePacket(mgr, ROUTE_TYPE_FLOOD), 3, 1000);

    EXPECT_EQ(nullptr, mgr.removeOverdueFlood(1000 + 60000, 60000));
    EXPECT_EQ(1, mgr.getOutboundTotal());
}

TEST(StaticPoolPacketManager, RemoveOverdueFlood_KeepsFutureFlood) {
    StaticPoolPacketManager mgr(4);
    mgr.queueOutbound(makePacket(mgr, ROUTE_TYPE_FLOOD), 3, 500000);

    EXPECT_EQ(nullptr, mgr.removeOverdueFlood(1000, 60000));
    EXPECT_EQ(1, mgr.getOutboundTotal());
}

TEST(StaticPoolPacketManager, RemoveOverdueFlood_RemovesOverdueFloodAndTransportFlood) {
    StaticPoolPacketManager mgr(4);
    Packet* flood = makePacket(mgr, ROUTE_TYPE_FLOOD);
    Packet* tflood = makePacket(mgr, ROUTE_TYPE_TRANSPORT_FLOOD);
    mgr.queueOutbound(flood, 3, 1000);
    mgr.queueOutbound(tflood, 3, 1000);

    uint32_t now = 1000 + 60001;
    EXPECT_EQ(flood, mgr.removeOverdueFlood(now, 60000));
    EXPECT_EQ(tflood, mgr.removeOverdueFlood(now, 60000));
    EXPECT_EQ(nullptr, mgr.removeOverdueFlood(now, 60000));
    EXPECT_EQ(0, mgr.getOutboundTotal());
}

TEST(StaticPoolPacketManager, RemoveOverdueFlood_NeverRemovesDirect) {
    StaticPoolPacketManager mgr(4);
    mgr.queueOutbound(makePacket(mgr, ROUTE_TYPE_DIRECT), 0, 1000);
    mgr.queueOutbound(makePacket(mgr, ROUTE_TYPE_TRANSPORT_DIRECT), 0, 1000);

    EXPECT_EQ(nullptr, mgr.removeOverdueFlood(10000000, 60000));
    EXPECT_EQ(2, mgr.getOutboundTotal());
}

TEST(StaticPoolPacketManager, RemoveOverdueFlood_LeavesOtherEntriesInOrder) {
    StaticPoolPacketManager mgr(4);
    Packet* fresh = makePacket(mgr, ROUTE_TYPE_FLOOD);
    Packet* stale = makePacket(mgr, ROUTE_TYPE_FLOOD);
    Packet* direct = makePacket(mgr, ROUTE_TYPE_DIRECT);
    mgr.queueOutbound(fresh, 1, 100000);
    mgr.queueOutbound(stale, 9, 1000);
    mgr.queueOutbound(direct, 0, 1000);

    EXPECT_EQ(stale, mgr.removeOverdueFlood(100000, 60000));
    ASSERT_EQ(2, mgr.getOutboundTotal());
    EXPECT_EQ(fresh, mgr.getOutboundByIdx(0));
    EXPECT_EQ(direct, mgr.getOutboundByIdx(1));
}

TEST(StaticPoolPacketManager, RemoveOverdueFlood_HandlesMillisWraparound) {
    StaticPoolPacketManager mgr(4);
    mgr.queueOutbound(makePacket(mgr, ROUTE_TYPE_FLOOD), 3, 0xFFFFF000);

    EXPECT_EQ(nullptr, mgr.removeOverdueFlood(0x00001000, 60000));   // 8192 ms overdue, across the wrap
    EXPECT_NE(nullptr, mgr.removeOverdueFlood(0x00010000, 60000));   // ~69.6 s overdue, across the wrap
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
