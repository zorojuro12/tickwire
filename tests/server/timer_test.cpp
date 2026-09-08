#include "server/tick_timer.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace server
