#pragma once

#include "Generator.hpp"
#include "PatchTiling.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::world::generator {

// A district's polygon (a face of the road graph, simple, possibly concave)
// is cut into lots with chords: from a point on an edge, the perpendicular is
// extended across the polygon until it reaches the boundary again; the chord
// splits the polygon and the halves are cut again until every lot is at the
// target area. The cut is chosen, not drawn: every candidate (edges long
// enough, a few points along each) is scored on the rectangularity of both
// halves, their frontage (an original road edge, or the chord itself when
// chords are alleys) and the balance of their areas; a cut that would leave
// a half narrower than a house or with a corner sharper than the minimum is
// refused. Every lot keeps, per edge, whether it lies on a road, on a chord
// or on nothing, so the building stage can inset it accordingly.
enum class LotEdgeKind : uint8_t
{
  None,
  Road,
  Chord
};

struct LotSplitInput
{
  std::vector<Position> polygon;      // Simple, CCW.
  std::vector<LotEdgeKind> edgeKinds; // Per polygon edge (empty: every edge a road).
  double targetArea = 600;            // Lots stop splitting below targetArea * (1 + spread * (r - .5)).
  double spread = .6;
  double minimumWidth = 6;    // No half narrower than this across any edge normal.
  double minimumCorner = .52; // No half with a corner under this angle (radians).
  double frontageWeight = .5; // Score terms.
  double balanceWeight = .6;
  bool chordsAreRoads = true; // A chord counts as frontage for both halves (an alley or minor road runs on it).
  bool convexLots = true;     // A concave piece keeps splitting until its lots are convex (the building stage insets convex lots).
  uint32_t maximumDepth = 16;
  uint64_t key = 1;
  std::function<double(uint64_t, uint64_t)> random;
};

struct Lot
{
  std::vector<Position> ring;
  std::vector<LotEdgeKind> edgeKinds;
};

struct LotSplitPlan
{
  std::vector<Lot> lots;
  std::vector<std::pair<Position, Position>> chords; // The cuts made, in order (alleys or minor roads).
  uint32_t cuts = 0, refused = 0;
};

class LotSplitter final : public Generator<LotSplitInput, LotSplitPlan>
{
public:
  explicit LotSplitter(uint64_t domain);

  static bool plan(const LotSplitInput &input, LotSplitPlan &output, Ledger &ledger, std::string *error);

  // Split a simple polygon by the chord between two points on its boundary
  // (each given by the edge it lies on and the position). Returns false when
  // the chord leaves the polygon.
  static bool cut(const std::vector<Position> &polygon, size_t edgeA, Position a, size_t edgeB, Position b, std::vector<Position> &first, std::vector<Position> &second);

protected:
  bool run(const LotSplitInput &input, LotSplitPlan &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
