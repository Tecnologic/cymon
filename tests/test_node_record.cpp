#include <gtest/gtest.h>

#include "monitor/node_record.hpp"
#include "monitor/session_manager.hpp"

using namespace cymon;

TEST(NodeRecord, DefaultIsOffline) {
  NodeRecord nr{};
  EXPECT_EQ(nr.health, NodeHealth::kOffline);
  EXPECT_FALSE(nr.IsOnline());
}

TEST(NodeRecord, OnlineWhenNominal) {
  NodeRecord nr{};
  nr.health = NodeHealth::kNominal;
  EXPECT_TRUE(nr.IsOnline());
}

TEST(NodeRecord, Variables) {
  NodeRecord nr{};
  nr.variables.push_back({1, "rpm", "RPM"});
  nr.variables.push_back({2, "temp", "°C"});
  EXPECT_EQ(nr.variables.size(), 2u);
  EXPECT_EQ(nr.variables[0].name, "rpm");
  EXPECT_EQ(nr.variables[1].unit, "°C");
}

TEST(NodeRecord, SessionManagerCreateDestroy) {
  // A smoke test that session_manager compiles and works
  using namespace cymon;
  SessionManager mgr;
  EXPECT_EQ(mgr.SessionCount(), 0u);

  std::array<ChannelConfig, 1> cfgs{ChannelConfig{{1, 10}, "ch0"}};
  const uint8_t id = mgr.CreateSession(cfgs, SessionMode::kRolling);
  EXPECT_NE(id, 0xFFu);
  EXPECT_EQ(mgr.SessionCount(), 1u);

  mgr.FeedSample(1, 10, 1000, 42.0F);

  GraphDataResponse resp{};
  EXPECT_TRUE(mgr.GetGraphData(id, resp));
  EXPECT_EQ(resp.channels[0].count, 1u);
  EXPECT_FLOAT_EQ(resp.channels[0].samples[0].value, 42.0F);

  EXPECT_TRUE(mgr.DestroySession(id));
  EXPECT_EQ(mgr.SessionCount(), 0u);
}
