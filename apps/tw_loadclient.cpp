#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "client/client.h"
#include "net/udp.h"
#include "sim/sim.h"

namespace {

void printUsage() {
  std::fprintf(stderr,
                "usage: tw_loadclient --host <ip> --port <n> [--players <n>] [--ticks <n>]\n");
}

void sleepMs(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 0;
  uint32_t players = 1;
  uint32_t ticks = 600;
  bool have_port = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
      have_port = true;
    } else if (arg == "--players" && i + 1 < argc) {
      players = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (arg == "--ticks" && i + 1 < argc) {
      ticks = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else {
      printUsage();
      return 1;
    }
  }
  if (!have_port || players == 0 || players > sim::kMaxPlayers) {
    printUsage();
    return 1;
  }

  in_addr addr{};
  if (inet_pton(AF_INET, host.c_str(), &addr) != 1) {
    std::fprintf(stderr, "tw_loadclient: bad --host %s\n", host.c_str());
    return 1;
  }
  const net::Endpoint server_ep{addr.s_addr, htons(port)};

  std::vector<std::unique_ptr<net::UdpTransport>> transports;
  std::vector<std::unique_ptr<client::Client<net::UdpTransport>>> clients;
  transports.reserve(players);
  clients.reserve(players);

  for (uint32_t i = 0; i < players; ++i) {
    auto tp = std::make_unique<net::UdpTransport>();
    if (!tp->bind(htonl(INADDR_LOOPBACK), 0)) {
      std::fprintf(stderr, "tw_loadclient: failed to bind a client socket\n");
      return 1;
    }
    auto c = std::make_unique<client::Client<net::UdpTransport>>(*tp, server_ep);
    transports.push_back(std::move(tp));
    clients.push_back(std::move(c));
  }

  uint32_t now_ms = 0;
  for (auto& c : clients) c->beginJoin(now_ms);

  for (uint32_t t = 0; t < ticks; ++t) {
    now_ms += 16;
    for (uint32_t i = 0; i < players; ++i) {
      client::Client<net::UdpTransport>& c = *clients[i];
      c.tick(now_ms);
      if (c.state() == client::State::kJoined) {
        const float move_x = ((t / 30 + i) % 2 == 0) ? 1.0f : -1.0f;
        c.sendInput(now_ms, move_x, 0.0f, 0.0f, 0.0f, false);
      }
    }
    sleepMs(16);
  }

  uint32_t joined = 0;
  uint32_t snapshots_total = 0;
  for (auto& c : clients) {
    if (c->state() == client::State::kJoined) ++joined;
    snapshots_total += c->snapshotsReceived();
  }

  std::printf("joined=%u\n", joined);
  std::printf("snapshots=%u\n", snapshots_total);
  std::fflush(stdout);

  return joined == players ? 0 : 1;
}
