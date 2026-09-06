#include "net/simulated.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "net/loopback.h"

namespace net {
namespace {

constexpr Endpoint kEndpointA{0x7F000001u, 0x1F90u};
constexpr Endpoint kEndpointB{0x7F000001u, 0x1F91u};

TEST(SimulatedTransportTest, TransparentWhenEverythingIsZeroed) {
  auto a = std::make_unique<LoopbackTransport>(kEndpointA);
  auto b = std::make_unique<LoopbackTransport>(kEndpointB);
  a->connect(*b);

  SimulatedTransport<LoopbackTransport> sim_b(*b, SimConfig{});
  EXPECT_EQ(sim_b.tick(), 0u);

  const std::array<std::byte, 5> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                             std::byte{0x04}, std::byte{0x05}};
  ASSERT_TRUE(a->send(b->self(), payload));

  PacketSlot slot;
  ASSERT_TRUE(sim_b.tryReceive(slot));
  EXPECT_EQ(slot.len, 5u);
  EXPECT_EQ(std::memcmp(slot.data.data(), payload.data(), payload.size()), 0);
  EXPECT_EQ(slot.peer, a->self());

  EXPECT_FALSE(sim_b.tryReceive(slot));
  EXPECT_EQ(sim_b.droppedByLoss(), 0u);
  EXPECT_EQ(sim_b.droppedByCapacity(), 0u);

  ASSERT_TRUE(sim_b.send(a->self(), payload));
  PacketSlot slot_a;
  ASSERT_TRUE(a->tryReceive(slot_a));
  EXPECT_EQ(slot_a.peer, b->self());

  EXPECT_EQ(msToTicks(0), 0u);
  EXPECT_EQ(msToTicks(16), 1u);
  EXPECT_EQ(msToTicks(100), 6u);
  EXPECT_EQ(msToTicks(200), 12u);
}

TEST(SimulatedTransportTest, LatencyDelaysDeliveryByExactTicks) {
  auto a = std::make_unique<LoopbackTransport>(kEndpointA);
  auto b = std::make_unique<LoopbackTransport>(kEndpointB);
  a->connect(*b);

  SimulatedTransport<LoopbackTransport> sim_b(
      *b, SimConfig{.latency_ms = 200, .jitter_ms = 0, .loss_permille = 0, .seed = 1});
  ASSERT_EQ(msToTicks(200), 12u);

  const std::array<std::byte, 1> payload{std::byte{0xAB}};
  ASSERT_TRUE(a->send(b->self(), payload));

  for (uint64_t t = 0; t < 20; ++t) {
    PacketSlot slot;
    const bool received = sim_b.tryReceive(slot);
    if (t == 12) {
      EXPECT_TRUE(received) << "tick " << t;
      EXPECT_EQ(slot.data[0], std::byte{0xAB});
    } else {
      EXPECT_FALSE(received) << "tick " << t;
    }
    sim_b.advanceTick();
  }

}

TEST(SimulatedTransportTest, LatencyIsMeasuredFromArrivalNotConstruction) {
  auto a = std::make_unique<LoopbackTransport>(kEndpointA);
  auto b = std::make_unique<LoopbackTransport>(kEndpointB);
  a->connect(*b);

  SimulatedTransport<LoopbackTransport> sim_b(
      *b, SimConfig{.latency_ms = 200, .jitter_ms = 0, .loss_permille = 0, .seed = 1});

  for (uint64_t t = 0; t < 5; ++t) sim_b.advanceTick();
  ASSERT_EQ(sim_b.tick(), 5u);

  const std::array<std::byte, 1> payload{std::byte{0xCD}};
  ASSERT_TRUE(a->send(b->self(), payload));

  for (uint64_t t = sim_b.tick(); t < 17; ++t) {
    PacketSlot slot;
    EXPECT_FALSE(sim_b.tryReceive(slot)) << "tick " << t;
    sim_b.advanceTick();
  }
  ASSERT_EQ(sim_b.tick(), 17u);
  PacketSlot slot;
  ASSERT_TRUE(sim_b.tryReceive(slot));
  EXPECT_EQ(slot.data[0], std::byte{0xCD});
}

std::vector<uint32_t> runLossTrial(const SimConfig& cfg) {
  LoopbackTransport a(kEndpointA);
  LoopbackTransport b(kEndpointB);
  a.connect(b);
  SimulatedTransport<LoopbackTransport> sim_b(b, cfg);

  std::vector<uint32_t> delivered;
  for (uint32_t i = 0; i < 1000; ++i) {
    std::array<std::byte, 4> payload{};
    std::memcpy(payload.data(), &i, sizeof(i));
    EXPECT_TRUE(a.send(b.self(), payload));

    PacketSlot slot;
    while (sim_b.tryReceive(slot)) {
      uint32_t v = 0;
      std::memcpy(&v, slot.data.data(), sizeof(v));
      delivered.push_back(v);
    }
    sim_b.advanceTick();
  }
  return delivered;
}

TEST(SimulatedTransportTest, LossIsProbabilisticAndReproducibleForIdenticalSeeds) {
  {
    const auto delivered =
        runLossTrial(SimConfig{.latency_ms = 0, .jitter_ms = 0, .loss_permille = 0, .seed = 7});
    EXPECT_EQ(delivered.size(), 1000u);
    for (uint32_t i = 0; i < delivered.size(); ++i) EXPECT_EQ(delivered[i], i);
  }

  {
    LoopbackTransport a(kEndpointA);
    LoopbackTransport b(kEndpointB);
    a.connect(b);
    SimulatedTransport<LoopbackTransport> sim_b(
        b, SimConfig{.latency_ms = 0, .jitter_ms = 0, .loss_permille = 1000, .seed = 7});
    const auto delivered = [&] {
      std::vector<uint32_t> out;
      for (uint32_t i = 0; i < 1000; ++i) {
        std::array<std::byte, 4> payload{};
        std::memcpy(payload.data(), &i, sizeof(i));
        EXPECT_TRUE(a.send(b.self(), payload));
        PacketSlot slot;
        while (sim_b.tryReceive(slot)) out.push_back(i);
        sim_b.advanceTick();
      }
      return out;
    }();
    EXPECT_EQ(delivered.size(), 0u);
    EXPECT_EQ(sim_b.droppedByLoss(), 1000u);
  }

  const SimConfig partial_cfg{.latency_ms = 0, .jitter_ms = 0, .loss_permille = 250, .seed = 7};
  const auto partial = runLossTrial(partial_cfg);
  EXPECT_GT(partial.size(), 0u);
  EXPECT_LT(partial.size(), 1000u);
  for (size_t i = 1; i < partial.size(); ++i) EXPECT_LT(partial[i - 1], partial[i]);

  {
    LoopbackTransport a(kEndpointA);
    LoopbackTransport b(kEndpointB);
    a.connect(b);
    SimulatedTransport<LoopbackTransport> sim_b(b, partial_cfg);
    uint32_t delivered_count = 0;
    for (uint32_t i = 0; i < 1000; ++i) {
      std::array<std::byte, 4> payload{};
      std::memcpy(payload.data(), &i, sizeof(i));
      ASSERT_TRUE(a.send(b.self(), payload));
      PacketSlot slot;
      while (sim_b.tryReceive(slot)) ++delivered_count;
      sim_b.advanceTick();
    }
    EXPECT_EQ(sim_b.droppedByLoss() + delivered_count, 1000u);
  }

  const auto partial_again = runLossTrial(partial_cfg);
  EXPECT_EQ(partial, partial_again);

  const auto other_seed = runLossTrial(
      SimConfig{.latency_ms = 0, .jitter_ms = 0, .loss_permille = 250, .seed = 8});
  EXPECT_NE(partial, other_seed);
}

std::vector<uint32_t> runJitterTrial(uint64_t seed) {
  LoopbackTransport a(kEndpointA);
  LoopbackTransport b(kEndpointB);
  a.connect(b);
  SimulatedTransport<LoopbackTransport> sim_b(
      b, SimConfig{.latency_ms = 100, .jitter_ms = 100, .loss_permille = 0, .seed = seed});

  std::vector<uint32_t> delivered;
  for (uint32_t t = 0; t < 80; ++t) {
    if (t < 50) {
      std::array<std::byte, 4> payload{};
      std::memcpy(payload.data(), &t, sizeof(t));
      EXPECT_TRUE(a.send(b.self(), payload));
    }
    PacketSlot slot;
    while (sim_b.tryReceive(slot)) {
      uint32_t v = 0;
      std::memcpy(&v, slot.data.data(), sizeof(v));
      delivered.push_back(v);
    }
    sim_b.advanceTick();
  }
  return delivered;
}

TEST(SimulatedTransportTest, JitterReordersDeliveryReproducibly) {
  ASSERT_EQ(msToTicks(100), 6u);

  const auto delivered = runJitterTrial(42);
  EXPECT_EQ(delivered.size(), 50u);

  bool has_inversion = false;
  for (size_t i = 1; i < delivered.size(); ++i) {
    if (delivered[i - 1] > delivered[i]) {
      has_inversion = true;
      break;
    }
  }
  EXPECT_TRUE(has_inversion);

  const auto delivered_again = runJitterTrial(42);
  EXPECT_EQ(delivered, delivered_again);

  const auto delivered_other_seed = runJitterTrial(43);
  EXPECT_NE(delivered, delivered_other_seed);
}

}  // namespace
}  // namespace net
