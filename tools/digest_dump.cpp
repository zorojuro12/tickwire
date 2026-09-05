#include <cstdio>

#include "sim/sim.h"

int main() {
  float pos = 1.0f;
  const float vel = 2.0f;
  for (int i = 0; i < 10000; ++i) {
    pos = sim::advance(pos, vel);
  }
  std::printf("%a\n", pos);
  return 0;
}
