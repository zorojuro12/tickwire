#include "sim/sim.h"

namespace sim {

float advance(float pos, float vel) { return pos + vel * kTickDt; }

}  // namespace sim
