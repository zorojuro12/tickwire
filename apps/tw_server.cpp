#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include "net/udp.h"
#include "server/clock.h"
#include "server/mutex_ring.h"
#include "server/poll_set.h"
#include "server/server.h"
#include "server/spsc_ring.h"
#include "server/threaded_runner.h"
#include "server/tick_timer.h"
#include "sim/sim.h"

namespace {

// std::atomic rather than volatile sig_atomic_t: the latter's guarantee is
// limited to a signal interrupting execution on the *same* thread that later
// reads it, with no cross-thread visibility or ordering. The threaded path
// (--threads 2) has a signal handler write this while a background thread
// polls it -- exactly the case volatile sig_atomic_t doesn't cover. A
// lock-free atomic is explicitly permitted from a signal handler.
static_assert(std::atomic<int>::is_always_lock_free);
std::atomic<int> g_stop{0};
void handleSigint(int) { g_stop.store(1, std::memory_order_relaxed); }

void printUsage() {
  std::fprintf(
      stderr,
      "usage: tw_server [--port <n>] [--ticks <n>] [--threads <1|2>] [--ring <spsc|mutex>]\n"
      "                 [--sim-load-us <n>] [--batch-ingest]\n"
      "  --port <n>          bind port; 0 for an ephemeral port (default 41234)\n"
      "  --ticks <n>         stop after n ticks; 0 to run forever (default 0)\n"
      "  --threads <1|2>     1: single-threaded epoll loop (default). 2: split\n"
      "                      ingest and tick across two threads\n"
      "  --ring <spsc|mutex> ring arm for --threads 2 (default spsc)\n"
      "  --sim-load-us <n>   synthetic per-tick busy-wait, microseconds (default 0)\n"
      "  --batch-ingest      use recvmmsg batching on the I/O thread (--threads 2 only)\n");
}

// Runs the two-thread path on an explicitly named ring type, polling the
// SIGINT-set g_stop flag from this thread (the calling thread is otherwise
// free while the runner's own sim loop runs on a background thread) and
// forwarding it to the runner via the signal-safe requestStop().
template <typename Ring>
uint32_t runThreaded(net::UdpTransport& transport, uint32_t ticks_limit, uint32_t sim_load_us,
                      bool batch_ingest) {
  auto srv = std::make_unique<server::Server<net::UdpTransport, Ring>>(transport);
  const uint32_t effective_limit =
      ticks_limit == 0 ? std::numeric_limits<uint32_t>::max() : ticks_limit;
  server::ThreadedRunner<net::UdpTransport, Ring> runner(
      *srv, transport, sim::kTickHz, &server::monotonicNs, &server::monotonicMs, sim_load_us,
      batch_ingest);

  std::atomic<bool> done{false};
  std::thread t([&] {
    runner.run(effective_limit);
    done.store(true, std::memory_order_relaxed);
  });
  while (!done.load(std::memory_order_relaxed)) {
    if (g_stop.load(std::memory_order_relaxed) != 0) runner.requestStop();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  t.join();

  std::printf("ticks=%u\n", runner.ticksRun());
  std::printf("snapshot_bytes=%llu full_equiv_bytes=%llu deltas=%llu keyframes=%llu\n",
              static_cast<unsigned long long>(srv->snapshotBytesSent()),
              static_cast<unsigned long long>(srv->snapshotBytesFullEquivalent()),
              static_cast<unsigned long long>(srv->deltasSent()),
              static_cast<unsigned long long>(srv->keyframesSent()));
  std::printf("jitter_ns p50=%llu p99=%llu max=%llu samples=%zu dropped=%zu\n",
              static_cast<unsigned long long>(runner.jitter().percentileNs(0.50)),
              static_cast<unsigned long long>(runner.jitter().percentileNs(0.99)),
              static_cast<unsigned long long>(runner.jitter().maxNs()), runner.jitter().count(),
              runner.jitter().dropped());
  std::printf("packets_ingested=%llu ingest_overflows=%llu\n",
              static_cast<unsigned long long>(runner.packetsIngested()),
              static_cast<unsigned long long>(srv->ingestOverflows()));
  std::printf("lagcomp rewound_shots=%llu rewinds_rejected=%llu\n",
              static_cast<unsigned long long>(srv->rewoundShots()),
              static_cast<unsigned long long>(srv->rewindsRejected()));
  return runner.ticksRun();
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 41234;
  uint32_t ticks_limit = 0;
  int threads = 1;
  std::string ring = "spsc";
  uint32_t sim_load_us = 0;
  bool batch_ingest = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--ticks" && i + 1 < argc) {
      ticks_limit = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (arg == "--threads" && i + 1 < argc) {
      threads = std::atoi(argv[++i]);
      if (threads != 1 && threads != 2) {
        printUsage();
        return 1;
      }
    } else if (arg == "--ring" && i + 1 < argc) {
      ring = argv[++i];
      if (ring != "spsc" && ring != "mutex") {
        printUsage();
        return 1;
      }
    } else if (arg == "--sim-load-us" && i + 1 < argc) {
      sim_load_us = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (arg == "--batch-ingest") {
      batch_ingest = true;
    } else {
      printUsage();
      return 1;
    }
  }

  std::signal(SIGINT, handleSigint);

  auto transport = std::make_unique<net::UdpTransport>();
  if (!transport->bind(htonl(INADDR_LOOPBACK), port)) {
    std::fprintf(stderr, "tw_server: failed to bind port %u\n", port);
    return 1;
  }
  std::printf("port=%u\n", ntohs(transport->localEndpoint().port_be));
  std::printf("threads=%d ring=%s sim_load_us=%u batch_ingest=%d\n", threads,
               threads == 2 ? ring.c_str() : "none", sim_load_us, batch_ingest ? 1 : 0);
  std::fflush(stdout);

  if (threads == 2) {
    if (ring == "mutex") {
      runThreaded<server::MutexRing<net::PacketSlot, server::kIngestCapacity>>(
          *transport, ticks_limit, sim_load_us, batch_ingest);
    } else {
      runThreaded<server::SpscRing<net::PacketSlot, server::kIngestCapacity>>(
          *transport, ticks_limit, sim_load_us, batch_ingest);
    }
    std::fflush(stdout);
    return 0;
  }

  auto srv = std::make_unique<server::Server<net::UdpTransport>>(*transport);

  server::PollSet poll;
  server::TickTimer timer(sim::kTickHz);
  poll.add(transport->nativeHandle(), 2);
  poll.add(timer.fd(), 1);

  uint32_t ticks_run = 0;
  std::array<uint32_t, 8> ready{};
  while (g_stop.load(std::memory_order_relaxed) == 0 &&
         (ticks_limit == 0 || ticks_run < ticks_limit)) {
    const size_t n = poll.wait(50, ready);
    for (size_t i = 0; i < n; ++i) {
      if (ready[i] == 2) {
        srv->ingest();
      } else if (ready[i] == 1) {
        uint64_t expirations = timer.consumeExpirations();
        while (expirations > 0 && (ticks_limit == 0 || ticks_run < ticks_limit)) {
          srv->tick(server::monotonicMs());
          ++ticks_run;
          --expirations;
        }
      }
    }
  }

  std::printf("ticks=%u\n", ticks_run);
  std::printf("snapshot_bytes=%llu full_equiv_bytes=%llu deltas=%llu keyframes=%llu\n",
              static_cast<unsigned long long>(srv->snapshotBytesSent()),
              static_cast<unsigned long long>(srv->snapshotBytesFullEquivalent()),
              static_cast<unsigned long long>(srv->deltasSent()),
              static_cast<unsigned long long>(srv->keyframesSent()));
  std::printf("lagcomp rewound_shots=%llu rewinds_rejected=%llu\n",
              static_cast<unsigned long long>(srv->rewoundShots()),
              static_cast<unsigned long long>(srv->rewindsRejected()));
  std::fflush(stdout);
  return 0;
}
