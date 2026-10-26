#pragma once
#include "procedural/Geometry.hpp"
#include <cstdint>
#include <vector>

namespace procedural::geometry
{
struct ParcelSettings;

// How a piece of ground decides where to cut itself next.
//
// Land is not subdivided one way. A town divides its wards into building plots
// square to the street; an open field is divided into parallel strips a furlong
// long; enclosed farmland follows the grain of the district; and a road or a
// river bank is a boundary that was handed down and is not negotiable. These
// were four sets of conditionals threaded through one cut, which is how a
// building ended up being cut by a rule written for a hedge - the +/-90 degree
// retry that turned houses into wedges was farmland's rule reaching into a
// town. One class each, and a piece can only be cut by its own rule.
enum class ParcelStrategyKind : uint8_t
{
  Enclosure = 0u,   // hedged fields following the district grain
  OpenField,        // parallel strips, the medieval two- or three-field system
  Township,         // building plots inside a settlement, after Watabou
  Infrastructure,   // the cut is a road, a bank or a shore and is authoritative
  Count,
};

// Everything a strategy is allowed to look at.
struct ParcelCutRequest
{
  const std::vector<Point2> *polygon = nullptr;
  Point2 centre{};
  double areaSquareMetres = 0.0;
  double targetSquareMetres = 0.0;
  // The piece's own long axis, as the caller measured it.
  double axisX = 1.0, axisY = 0.0;
  // The direction the strategy was handed: the district grain for enclosure,
  // the strip direction for an open field, the cutter's normal for
  // infrastructure. Strategies are free to ignore it.
  double givenX = 1.0, givenY = 0.0;
  Point2 givenOrigin{};
  uint32_t attempt = 0u;
  uint64_t seed = 0u;
  const ParcelSettings *settings = nullptr;
};

// What it decided.
struct ParcelCutPlan
{
  Point2 at{};                        // a point the cut passes through
  double axisX = 1.0, axisY = 0.0;    // the cut's normal
  double bend = 0.0;                  // curvature; zero is a straight line
  // Whether the caller may fall back to a corner-to-corner cut when the line
  // is refused, and whether a large triangle is a reason to try again. A
  // diagonal cut makes a triangle by construction: fine for an odd corner of a
  // field, never right for a house.
  bool allowDiagonalFallback = true;
  bool refuseLargeTriangles = true;
};

class ParcelStrategy
{
public:
  virtual ~ParcelStrategy() = default;
  virtual ParcelStrategyKind kind() const = 0;
  virtual const char *name() const = 0;
  virtual ParcelCutPlan planCut(const ParcelCutRequest &request) const = 0;
};

// The strategies are stateless, so one instance each is enough.
const ParcelStrategy &parcelStrategy(ParcelStrategyKind kind);
}
