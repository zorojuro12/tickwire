#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "net/udp.h"
#include "server/clock.h"
#include "server/poll_set.h"
#include "server/server.h"
#include "server/tick_timer.h"
#include "sim/sim.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void handleSigint(int) { g_stop = 1; }

void printUsage() {
  std::fprintf(
      stderr,
      "usage: tw_server [--port <n>] [--ticks <n>] [--threads <1|2>] [--ring <spsc|mutex>]\n"
      "                 [--sim-load-us <n>]\n"
      "  --port <n>          bind port; 0 for an ephemeral port (default 41234)\n"
      "  --ticks <n>         stop after n ticks; 0 to run forever (default 0)\n"
      "  --threads <1|2>     1: single-threaded epoll loop (default). 2: split\n"
      "                      ingest and tick across two threads\n"
      "  --ring <spsc|mutex> ring arm for --threads 2 (default spsc)\n"
      "  --sim-load-us <n>   synthetic per-tick busy-wait, microseconds (default 0)\n");
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 41234;
  uint32_t ticks_limit = 0;
  int threads = 1;
  std::string ring = "spsc";
  uint32_t sim_load_us = 0;

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
  std::printf("threads=%d ring=%s sim_load_us=%u\n", threads,
               threads == 2 ? ring.c_str() : "none", sim_load_us);
  std::fflush(stdout);

  auto srv = std::make_unique<server::Server<net::UdpTransport>>(*transport);

  server::PollSet poll;
  server::TickTimer timer(sim::kTickHz);
  poll.add(transport->nativeHandle(), 2);
  poll.add(timer.fd(), 1);

  uint32_t ticks_run = 0;
  std::array<uint32_t, 8> ready{};
  while (g_stop == 0 && (ticks_limit == 0 || ticks_run < ticks_limit)) {
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
  std::fflush(stdout);
  return 0;
}
