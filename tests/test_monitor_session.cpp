#include <gtest/gtest.h>

#include <array>

#include "monitor/monitor_session.hpp"

using namespace cymon;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::array<ChannelConfig, 2> MakeTwoChannels() {
  return {ChannelConfig{{1, 10}, "ch0"}, ChannelConfig{{2, 20}, "ch1"}};
}

// ---------------------------------------------------------------------------
// ConfigureChannels
// ---------------------------------------------------------------------------
TEST(MonitorSession, ConfigureChannels) {
  MonitorSession session(1);
  auto cfgs = MakeTwoChannels();
  session.ConfigureChannels(cfgs);
  EXPECT_EQ(session.NumChannels(), 2u);
}

// ---------------------------------------------------------------------------
// FeedSample routes to correct ring
// ---------------------------------------------------------------------------
TEST(MonitorSession, FeedSampleRouting) {
  MonitorSession session(1);
  auto cfgs = MakeTwoChannels();
  session.ConfigureChannels(cfgs);

  session.FeedSample(1, 10, 1000, 1.0F);
  session.FeedSample(1, 10, 2000, 2.0F);
  session.FeedSample(2, 20, 3000, 3.0F);

  // Channel that doesn't exist — should be no-op
  session.FeedSample(99, 99, 4000, 99.0F);

  GraphDataResponse response{};
  session.GetGraphData(response);
  EXPECT_EQ(response.num_channels, 2u);
  EXPECT_EQ(response.channels[0].count, 2u);
  EXPECT_EQ(response.channels[1].count, 1u);
  EXPECT_FLOAT_EQ(response.channels[0].samples[0].value, 1.0F);
  EXPECT_FLOAT_EQ(response.channels[1].samples[0].value, 3.0F);
}

// ---------------------------------------------------------------------------
// Rolling mode never stops
// ---------------------------------------------------------------------------
TEST(MonitorSession, RollingMode) {
  MonitorSession session(1);
  auto cfgs = MakeTwoChannels();
  session.ConfigureChannels(cfgs);
  session.SetMode(SessionMode::kRolling);

  for (int i = 0; i < 2000; ++i) {
    session.FeedSample(1, 10, static_cast<uint64_t>(i * 100), static_cast<float>(i));
  }

  GraphDataResponse response{};
  session.GetGraphData(response);
  EXPECT_EQ(response.channels[0].count, kDefaultRingDepth);
}

// ---------------------------------------------------------------------------
// Triggered mode — armed but no trigger → keeps rolling, no freeze
// ---------------------------------------------------------------------------
TEST(MonitorSession, TriggeredModeArmedNoTrigger) {
  MonitorSession session(2);
  std::array<ChannelConfig, 1> cfgs{ChannelConfig{{1, 10}, "ch0"}};
  session.ConfigureChannels(cfgs);
  session.SetMode(SessionMode::kTriggered);

  TriggerConfig trig{};
  trig.channel_index = 0;
  trig.level = 10.0F;
  trig.rising_edge = true;
  session.ConfigureTrigger(trig);
  session.Arm();

  // Push values all below the trigger level
  for (int i = 0; i < 50; ++i) {
    session.FeedSample(1, 10, static_cast<uint64_t>(i * 100), static_cast<float>(i));
  }

  GraphDataResponse response{};
  session.GetGraphData(response);
  // Pre-trigger data should be captured
  EXPECT_GT(response.channels[0].count, 0u);
}

// ---------------------------------------------------------------------------
// Triggered mode — rising edge fires correctly
// ---------------------------------------------------------------------------
TEST(MonitorSession, TriggeredModeRisingEdge) {
  MonitorSession session(3);
  std::array<ChannelConfig, 1> cfgs{ChannelConfig{{1, 10}, "ch0"}};
  session.ConfigureChannels(cfgs);
  session.SetMode(SessionMode::kTriggered);

  TriggerConfig trig{};
  trig.channel_index = 0;
  trig.level = 5.0F;
  trig.rising_edge = true;
  session.ConfigureTrigger(trig);
  session.Arm();

  // Push value below trigger
  session.FeedSample(1, 10, 1000, 3.0F);
  // Push value crossing threshold
  session.FeedSample(1, 10, 2000, 6.0F);

  GraphDataResponse response{};
  session.GetGraphData(response);
  EXPECT_EQ(response.channels[0].count, 2u);
}

// ---------------------------------------------------------------------------
// Disarm prevents further sample capture in triggered mode
// ---------------------------------------------------------------------------
TEST(MonitorSession, TriggeredModeDisarmed) {
  MonitorSession session(4);
  std::array<ChannelConfig, 1> cfgs{ChannelConfig{{1, 10}, "ch0"}};
  session.ConfigureChannels(cfgs);
  session.SetMode(SessionMode::kTriggered);
  // Do NOT arm

  session.FeedSample(1, 10, 1000, 1.0F);

  GraphDataResponse response{};
  session.GetGraphData(response);
  EXPECT_EQ(response.channels[0].count, 0u);
}
