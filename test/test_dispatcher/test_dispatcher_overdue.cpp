#include <gtest/gtest.h>
#include <vector>
#include "Dispatcher.h"
#include "helpers/StaticPoolPacketManager.h"

using namespace mesh;

// Radio that "transmits" instantly and records the first payload byte of each sent packet.
class FakeRadio : public Radio {
public:
  std::vector<uint8_t> sent_ids;
  int recvRaw(uint8_t* bytes, int sz) override { return 0; }
  uint32_t getEstAirtimeFor(int len_bytes) override { return len_bytes; }
  float packetScore(float snr, int packet_len) override { return 1.0f; }
  bool startSendRaw(const uint8_t* bytes, int len) override {
    sent_ids.push_back(bytes[len - 1]);   // payload is last; test packets have a 1-byte payload
    return true;
  }
  bool isSendComplete() override { return true; }
  void onSendFinished() override { }
  bool isInRecvMode() const override { return true; }
};

class FakeClock : public MillisecondClock {
public:
  unsigned long now = 1000;
  unsigned long getMillis() override { return now; }
};

class TestDispatcher : public Dispatcher {
public:
  uint32_t max_overdue = MAX_FLOOD_TX_OVERDUE_MILLIS;

  TestDispatcher(Radio& radio, MillisecondClock& ms, PacketManager& mgr) : Dispatcher(radio, ms, mgr) { }

  DispatcherAction onRecvPacket(Packet* pkt) override { return ACTION_RELEASE; }
  float getAirtimeBudgetFactor() const override { return 0.0f; }   // 100% duty cycle: never budget-limited
  uint32_t getMaxFloodTxOverdue() const override { return max_overdue; }

  void queue(uint8_t route_type, uint8_t id, uint8_t priority, uint32_t delay_millis = 0) {
    Packet* p = obtainNewPacket();
    p->header = route_type | (PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT);
    p->path_len = 0;
    p->payload[0] = id;
    p->payload_len = 1;
    sendPacket(p, priority, delay_millis);
  }
};

class DispatcherOverdueTest : public ::testing::Test {
protected:
  static constexpr int POOL_SIZE = 32;
  FakeRadio radio;
  FakeClock clock;
  StaticPoolPacketManager mgr{POOL_SIZE};
  TestDispatcher disp{radio, clock, mgr};

  void SetUp() override { disp.begin(); }

  // advance the clock, then run the dispatcher loop (one send, or one send-completion, per call)
  void step(unsigned long millis) {
    clock.now += millis;
    disp.loop();
  }
  bool wasSent(uint8_t id) const {
    for (auto s : radio.sent_ids) if (s == id) return true;
    return false;
  }
};

TEST_F(DispatcherOverdueTest, FloodSentWhenOnTime) {
  disp.queue(ROUTE_TYPE_FLOOD, 0xF1, 1);
  step(10);
  EXPECT_TRUE(wasSent(0xF1));
}

TEST_F(DispatcherOverdueTest, FloodSentWhenOverdueButWithinLimit) {
  disp.queue(ROUTE_TYPE_FLOOD, 0xF1, 1);
  step(MAX_FLOOD_TX_OVERDUE_MILLIS - 1);
  EXPECT_TRUE(wasSent(0xF1));
}

TEST_F(DispatcherOverdueTest, OverdueFloodDroppedNotSent) {
  disp.queue(ROUTE_TYPE_FLOOD, 0xF1, 1);
  step(MAX_FLOOD_TX_OVERDUE_MILLIS + 1);
  step(10);

  EXPECT_FALSE(wasSent(0xF1));
  EXPECT_EQ(0, mgr.getOutboundTotal());
  EXPECT_EQ(POOL_SIZE, mgr.getFreeCount());   // dropped packet returned to pool (no leak)
}

TEST_F(DispatcherOverdueTest, OverdueTransportFloodDropped) {
  disp.queue(ROUTE_TYPE_TRANSPORT_FLOOD, 0xF1, 1);
  step(MAX_FLOOD_TX_OVERDUE_MILLIS + 1);
  EXPECT_FALSE(wasSent(0xF1));
  EXPECT_EQ(POOL_SIZE, mgr.getFreeCount());
}

TEST_F(DispatcherOverdueTest, OverdueDirectStillSent) {
  disp.queue(ROUTE_TYPE_DIRECT, 0xD1, 1);
  step(10 * 60 * 1000);
  EXPECT_TRUE(wasSent(0xD1));
}

TEST_F(DispatcherOverdueTest, DelayedFloodNotDroppedBeforeItsScheduledTime) {
  disp.queue(ROUTE_TYPE_FLOOD, 0xF1, 1, 90000);   // scheduled 90s ahead (longer than the limit)
  step(80000);
  EXPECT_FALSE(wasSent(0xF1));
  EXPECT_EQ(1, mgr.getOutboundTotal());   // still queued
  step(20000);
  EXPECT_TRUE(wasSent(0xF1));
}

TEST_F(DispatcherOverdueTest, LimitOfZeroDisablesDropping) {
  disp.max_overdue = 0;
  disp.queue(ROUTE_TYPE_FLOOD, 0xF1, 1);
  step(10 * 60 * 1000);
  EXPECT_TRUE(wasSent(0xF1));
}

// Scenario: a long-path flood (low priority) is starved by a steady stream of higher priority
// traffic. Once the stream stops, it must not be sent late.
static void runStarvation(TestDispatcher& disp, unsigned long& now, uint32_t max_overdue) {
  disp.max_overdue = max_overdue;
  disp.queue(ROUTE_TYPE_FLOOD, 0xF1, 8);   // 8 hops away: low priority
  for (int i = 0; i < 90; i++) {           // 90s of back-to-back priority 0 traffic
    disp.queue(ROUTE_TYPE_DIRECT, 0xD0, 0);
    now += 500; disp.loop();   // sends the direct packet (flood loses on priority)
    now += 500; disp.loop();   // send completes
  }
  for (int i = 0; i < 10; i++) {           // traffic stops
    now += 500; disp.loop();
  }
}

TEST_F(DispatcherOverdueTest, StarvedFloodIsDroppedInsteadOfSentLate) {
  runStarvation(disp, clock.now, MAX_FLOOD_TX_OVERDUE_MILLIS);
  EXPECT_FALSE(wasSent(0xF1));
  EXPECT_EQ(0, mgr.getOutboundTotal());
  EXPECT_EQ(POOL_SIZE, mgr.getFreeCount());
}

TEST_F(DispatcherOverdueTest, StarvedFloodIsSentLateWhenLimitDisabled) {
  // demonstrates the old behaviour this change prevents
  runStarvation(disp, clock.now, 0);
  ASSERT_TRUE(wasSent(0xF1));
  EXPECT_EQ(0xF1, radio.sent_ids.back());   // only after all the higher-priority traffic
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
