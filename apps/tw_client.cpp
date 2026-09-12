#include <arpa/inet.h>
#include <netinet/in.h>
#include <raylib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "client/client.h"
#include "client/view.h"
#include "net/simulated.h"
#include "net/udp.h"
#include "server/clock.h"
#include "sim/sim.h"

namespace {

constexpr float kSide = 800.0f;
constexpr uint32_t kLatencyStepMs = 25;
constexpr uint32_t kMaxLatencyMs = 500;

void printUsage() {
  std::fprintf(stderr,
                "usage: tw_client --selftest\n"
                "       tw_client --host <ip> --port <n> [--latency-ms <n>]\n");
}

int runSelftest() {
  if (std::getenv("DISPLAY") == nullptr || std::getenv("DISPLAY")[0] == '\0') {
    return 77;
  }
  InitWindow(800, 800, "tickwire");
  BeginDrawing();
  ClearBackground(BLACK);
  DrawRectangleLines(0, 0, 800, 800, RAYWHITE);
  EndDrawing();
  CloseWindow();
  return 0;
}

int runClient(const std::string& host, uint16_t port, uint32_t initial_latency_ms) {
  in_addr addr{};
  if (inet_pton(AF_INET, host.c_str(), &addr) != 1) {
    std::fprintf(stderr, "tw_client: bad --host %s\n", host.c_str());
    return 1;
  }
  const net::Endpoint server_ep{addr.s_addr, htons(port)};

  auto udp = std::make_unique<net::UdpTransport>();
  if (!udp->bind(htonl(INADDR_LOOPBACK), 0)) {
    std::fprintf(stderr, "tw_client: failed to bind a client socket\n");
    return 1;
  }

  net::SimConfig cfg;
  cfg.latency_ms = std::min(initial_latency_ms, kMaxLatencyMs);
  cfg.jitter_ms = 0;
  cfg.loss_permille = 0;
  cfg.seed = 1;

  // Held in an optional so [ and ] can reconstruct the wrapper's config in
  // place: emplace() destroys and reconstructs at the SAME storage address,
  // so the Client<T>'s T& reference into it stays valid across the change.
  std::optional<net::SimulatedTransport<net::UdpTransport>> wrapped;
  wrapped.emplace(*udp, cfg);

  auto client = std::make_unique<client::Client<net::SimulatedTransport<net::UdpTransport>>>(
      *wrapped, server_ep);
  client->beginJoin(server::monotonicMs());

  InitWindow(800, 800, "tickwire");
  SetTargetFPS(60);

  uint32_t latency_ms = cfg.latency_ms;

  while (!WindowShouldClose()) {
    const uint32_t now_ms = server::monotonicMs();
    client->tick(now_ms);
    wrapped->advanceTick();

    if (IsKeyPressed(KEY_LEFT_BRACKET)) {
      latency_ms = latency_ms >= kLatencyStepMs ? latency_ms - kLatencyStepMs : 0;
      cfg.latency_ms = latency_ms;
      wrapped.emplace(*udp, cfg);
    }
    if (IsKeyPressed(KEY_RIGHT_BRACKET)) {
      latency_ms = std::min(latency_ms + kLatencyStepMs, kMaxLatencyMs);
      cfg.latency_ms = latency_ms;
      wrapped.emplace(*udp, cfg);
    }

    float move_x = 0.0f, move_y = 0.0f;
    if (IsKeyDown(KEY_D)) move_x += 1.0f;
    if (IsKeyDown(KEY_A)) move_x -= 1.0f;
    if (IsKeyDown(KEY_W)) move_y += 1.0f;
    if (IsKeyDown(KEY_S)) move_y -= 1.0f;

    if (IsKeyPressed(KEY_P)) {
      client->setPredictionEnabled(!client->predictionEnabled());
    }
    if (IsKeyPressed(KEY_I)) {
      client->setInterpolationEnabled(!client->interpolationEnabled());
    }

    const sim::WorldSnapshot& snap = client->latestSnapshot();
    float local_x = 0.0f, local_y = 0.0f;
    const bool have_local = client->localPosition(local_x, local_y);

    const Vector2 mouse = GetMousePosition();
    const float scale = kSide / (2.0f * sim::kArenaHalf);
    const float world_mx = mouse.x / scale - sim::kArenaHalf;
    const float world_my = sim::kArenaHalf - mouse.y / scale;

    float aim_x = 0.0f, aim_y = 0.0f;
    if (have_local) client::aimFromCursor(local_x, local_y, world_mx, world_my, aim_x, aim_y);

    const bool fire = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    client->sendInput(now_ms, move_x, move_y, aim_x, aim_y, fire);

    BeginDrawing();
    ClearBackground(BLACK);
    DrawRectangleLines(0, 0, static_cast<int>(kSide), static_cast<int>(kSide), RAYWHITE);

    // Remote players are read through remotePosition() -- interpolated
    // between snapshots when interpolation is on, the raw newest snapshot
    // when it's off -- never predicted; that stays local-player-only. The
    // local player is drawn separately, from localPosition(). This guard
    // is now redundant (remotePosition() itself refuses the local id) but
    // stays: it documents the split at the call site.
    for (uint32_t i = 0; i < snap.count; ++i) {
      if (snap.players[i].id == client->playerId()) continue;
      float rx = 0.0f, ry = 0.0f;
      if (!client->remotePosition(snap.players[i].id, rx, ry)) continue;
      const client::ScreenPos sp = client::worldToScreen(rx, ry, kSide, 0.0f, 0.0f);
      const float r = client::worldToScreenRadius(snap.players[i].radius, kSide);
      // Red while interpolating, orange while not -- the same device the
      // green/yellow local player already uses for the prediction toggle.
      const Color color = client->interpolationEnabled() ? RED : ORANGE;
      DrawCircle(static_cast<int>(sp.x), static_cast<int>(sp.y), r, color);
    }

    if (have_local) {
      const client::ScreenPos sp = client::worldToScreen(local_x, local_y, kSide, 0.0f, 0.0f);
      const float r = client::worldToScreenRadius(sim::kPlayerRadius, kSide);
      // Green while predicting, yellow while not -- the toggle's state is
      // visible without reading the HUD.
      const Color color = client->predictionEnabled() ? GREEN : YELLOW;
      DrawCircle(static_cast<int>(sp.x), static_cast<int>(sp.y), r, color);
    }

    DrawText(TextFormat("tick=%u latency=%ums players=%u pred=%s rtt=%ums lead=%d err_p99=%.2f "
                          "interp=%s render=%u",
                          client->latestSnapshotTick(), latency_ms, snap.count,
                          client->predictionEnabled() ? "on" : "off", client->rttMs(),
                          client->clockLead(), client->predictionError().p99(),
                          client->interpolationEnabled() ? "on" : "off", client->renderTick()),
              10, 10, 20, RAYWHITE);
    EndDrawing();
  }

  client->leave(server::monotonicMs());
  CloseWindow();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool selftest = false;
  std::string host = "127.0.0.1";
  uint16_t port = 0;
  uint32_t latency_ms = 0;
  bool have_port = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--selftest") {
      selftest = true;
    } else if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
      have_port = true;
    } else if (arg == "--latency-ms" && i + 1 < argc) {
      latency_ms = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else {
      printUsage();
      return 1;
    }
  }

  if (selftest) return runSelftest();

  if (!have_port) {
    printUsage();
    return 1;
  }

  return runClient(host, port, latency_ms);
}
