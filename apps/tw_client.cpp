#include <raylib.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

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

  // The full render/input/latency-slider loop is Checkpoint 3.
  (void)host;
  (void)port;
  (void)latency_ms;
  printUsage();
  return 1;
}
