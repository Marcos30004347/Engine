#pragma once

#include "procedural/world/WorldSurfaceArrangement.hpp"

#include <algorithm>
#include <utility>

namespace procedural::world::finalpatches {

using rewrite::Position;

// How near two stations along a boundary must be to count as the same point.
// A single named constant, because the audit and the freeze have to agree
// about it or the audit rejects what the freeze just built.
inline constexpr double kTolerance = 3e-6;

// Exact equality. The frozen vertices are the arrangement's own, copied and
// not recomputed, so anything but bitwise equality means something moved them.
inline bool samePosition(const Position &a, const Position &b)
{
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

// An unordered pair as an ordered one, by value.
//
// Not std::minmax: that returns a pair of *references to its arguments*, which
// dangles the moment it is stored from temporaries. That exact trap cost a day
// elsewhere in this generator, so the shared helper does not offer it.
template <class T> std::pair<T, T> orderedPair(const T &a, const T &b)
{
  return a < b ? std::pair<T, T>{a, b} : std::pair<T, T>{b, a};
}

// Where the segment pq projects onto the segment ab, as a station interval.
//
// Returns false when pq is not collinear with ab within the tolerance, when it
// falls outside it, or when the overlap has no length - which is how a source
// chain proves an arrangement edge really does lie along it rather than merely
// near it.
bool projectInterval(Position a, Position b, Position p, Position q, double &lo, double &hi);

} // namespace procedural::world::finalpatches
