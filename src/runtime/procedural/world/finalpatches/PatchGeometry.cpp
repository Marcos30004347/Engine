#include "procedural/world/finalpatches/PatchGeometry.hpp"

#include <cmath>

namespace procedural::world::finalpatches {

bool projectInterval(const Position a, const Position b, const Position p, const Position q, double &lo, double &hi)
{
  const double dx = b.x - a.x, dz = b.z - a.z;
  const double length = std::hypot(dx, dz);

  if (length <= kTolerance)
  {
    return false;
  }

  const auto station = [&](const Position v) {
    return ((v.x - a.x) * dx + (v.z - a.z) * dz) / length;
  };
  const auto distance = [&](const Position v) {
    return std::abs(dx * (v.z - a.z) - dz * (v.x - a.x)) / length;
  };

  lo = std::min(station(p), station(q));
  hi = std::max(station(p), station(q));

  return distance(p) <= kTolerance && distance(q) <= kTolerance && lo >= -kTolerance && hi <= length + kTolerance && hi - lo > 1e-9;
}

} // namespace procedural::world::finalpatches
