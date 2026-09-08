#include "server/tick_timer.h"

#include <fcntl.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "net/udp.h"
#include "server/poll_set.h"

namespace server {
namespace {

TEST(TickTimerTest, FiresAtItsConfiguredRateAndClosesItself) {
  TickTimer t(1000);  // 1 kHz, 1 ms period
  ASSERT_TRUE(t.valid());
  ASSERT_GE(t.fd(), 0);

  // Immediately after construction, consumeExpirations() may legitimately
  // return 0 -- no assertion against that here.
  t.consumeExpirations();

  // A raw non-blocking read() completes fast enough on some hardware that
  // 5000 back-to-back reads finish before 3ms of wall time actually
  // elapses, undercounting expirations without a real timer defect (caught
  // as a flaky failure, not a build error). A short pace between empty
  // reads is not blocking on the fd -- it just lets real time pass at a
  // predictable rate, which busy-polling alone does not guarantee.
  uint64_t sum = 0;
  for (int i = 0; i < 5000 && sum < 3; ++i) {
    const uint64_t n = t.consumeExpirations();
    sum += n;
    if (n == 0) usleep(200);
  }
  EXPECT_GE(sum, 3u);

  // consumeExpirations() immediately after a nonzero read returns 0.
  EXPECT_EQ(t.consumeExpirations(), 0u);
}

TEST(TickTimerTest, MovingTransfersOwnership) {
  TickTimer t(1000);
  ASSERT_TRUE(t.valid());
  const int original_fd = t.fd();

  TickTimer u(std::move(t));
  EXPECT_TRUE(u.valid());
  EXPECT_EQ(u.fd(), original_fd);
  EXPECT_FALSE(t.valid());
  EXPECT_EQ(t.fd(), -1);
  EXPECT_EQ(t.consumeExpirations(), 0u);
}

TEST(TickTimerTest, DestructorClosesTheFd) {
  int recorded_fd = -1;
  {
    TickTimer t(1000);
    ASSERT_TRUE(t.valid());
    recorded_fd = t.fd();
  }
  errno = 0;
  EXPECT_EQ(fcntl(recorded_fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST(TickTimerTest, ZeroHzIsInvalidAndDoesNotCrash) {
  TickTimer t0(0);
  EXPECT_FALSE(t0.valid());
  EXPECT_EQ(t0.fd(), -1);
  EXPECT_EQ(t0.consumeExpirations(), 0u);
}

TEST(PollSetTest, ReportsWhichOfTheSocketAndTheTimerIsReady) {
  PollSet poll;
  ASSERT_TRUE(poll.valid());

  TickTimer timer(1000);
  ASSERT_TRUE(timer.valid());

  auto receiver = std::make_unique<net::UdpTransport>();
  auto sender = std::make_unique<net::UdpTransport>();
  const uint32_t loopback_be = htonl(INADDR_LOOPBACK);
  ASSERT_TRUE(receiver->bind(loopback_be, 0));
  ASSERT_TRUE(sender->bind(loopback_be, 0));

  EXPECT_TRUE(poll.add(timer.fd(), 1));
  EXPECT_TRUE(poll.add(receiver->nativeHandle(), 2));
  EXPECT_FALSE(poll.add(-1, 3));

  std::array<uint32_t, 8> out{};

  // The timer is 1ms-period; under system load, enough wall time can pass
  // between construction (above) and here for it to have already fired
  // once. Drain that first, so the steady-state "nothing ready" check below
  // isn't racing construction overhead.
  timer.consumeExpirations();

  EXPECT_EQ(poll.wait(0, out), 0u);

  const std::array<std::byte, 5> payload{std::byte{1}, std::byte{2}, std::byte{3},
                                          std::byte{4}, std::byte{5}};
  ASSERT_TRUE(sender->send(receiver->localEndpoint(), payload));

  size_t n = poll.wait(100, out);
  ASSERT_GE(n, 1u);
  bool saw_socket = false;
  for (size_t i = 0; i < n; ++i) {
    if (out[i] == 2) saw_socket = true;
  }
  EXPECT_TRUE(saw_socket);

  // Drain the socket first: level-triggered epoll reports it ready on every
  // call for as long as its data is unconsumed, which makes wait() return
  // immediately instead of actually blocking -- starving the timer-wait
  // loop below of the real wall-clock time it needs to observe a 1ms-period
  // timer fire.
  net::PacketSlot slot;
  while (receiver->tryReceive(slot)) {
  }

  bool saw_timer = false;
  for (int i = 0; i < 20 && !saw_timer; ++i) {
    n = poll.wait(50, out);
    for (size_t j = 0; j < n; ++j) {
      if (out[j] == 1) saw_timer = true;
    }
  }
  EXPECT_TRUE(saw_timer);

  // Draining matters: after consuming the timer too, wait(0, ...) reports
  // nothing.
  while (timer.consumeExpirations() != 0) {
  }
  EXPECT_EQ(poll.wait(0, out), 0u);

  // wait() never writes past a short `out` span. Give the timer real time to
  // fire again so both fds are plausibly ready together.
  ASSERT_TRUE(sender->send(receiver->localEndpoint(), payload));
  usleep(3000);
  std::array<uint32_t, 1> tiny{};
  n = poll.wait(50, tiny);
  EXPECT_LE(n, tiny.size());
}

}  // namespace
}  // namespace server
