#pragma once

#include "procedural/world/WorldGenerationContracts.hpp"
#include "procedural/world/citywfc/CityLattice.hpp"
#include "procedural/world/citywfc/CityRelax.hpp"

#include <functional>

namespace procedural::world::citywfc {

using rewrite::Position;

// The city as a grid of irregular quadrilaterals.
//
// The first milestone of the wave-function-collapse city path: the planning
// cells a settlement was granted, covered by one relaxed quad grid whose
// boundary follows their outline. Nothing is labelled yet - no walls, no roads,
// no blocks - because the grid has to be right before anything is decided on it.
//
// Why a quad, and why irregular: a quad has four sides, subdivides into four
// quads and so carries a hierarchy in its own primitive, and a four-symbol
// socket signature for the solver later. Irregular, because a regular grid
// reads as a modern grid-plan city and because the ground it covers is not
// rectangular.

struct CityGridSettings
{
  CityLatticeSettings lattice;

  // Two relaxation passes, and the order between them is the design.
  //
  // The first runs over the whole lattice with a free boundary, before anything
  // is cut away, so every cell relaxes with a full ring of neighbours. The
  // second runs after the city outline has been imposed, and only settles the
  // interior against it.
  CityRelaxSettings relax;
  CityRelaxSettings settle = [] {
    CityRelaxSettings s;
    s.iterations = 16;

    return s;
  }();

  // How far a boundary vertex may be pulled to reach the outline, as a multiple
  // of the finished cell edge.
  //
  // Kept below half a cell on purpose. A larger radius closes the staircase the
  // centroid test leaves behind, and also lets a vertex jump the neck of an
  // hourglass city and land on the far bank. The outline of a planning cell is
  // an abstraction rather than a survey line, so half a cell of slack costs
  // nothing and buying it back costs the whole mesh.
  double snapRadiusFactor = .9;

  // What a cell on the outline has to be to stay. A cell too small to hold a
  // wall or a road is not a cell, and one with a corner this sharp is a wedge
  // rather than a plot. Peeling it is not a loss of ground: the row behind it
  // becomes the rim and is stretched out to the outline in its place.
  double minimumAreaFraction = .25;
  double minimumAngleDegrees = 30;

  bool repairRim = true;
  uint32_t rimRepairPasses = 3;

  // Debris below this fraction of a cell is simplified out of the outline
  // before anything is laid on it.
  //
  // A union of real planning cells carries features the grid cannot represent
  // and should not try to: pairs of vertices a millimetre apart, chains of
  // centimetre edges wandering across a straight boundary, spikes narrower than
  // a cell. Snapping a boundary vertex onto one of those squashes its cell
  // flat, and a flat cell is what turns into a self-intersecting one the moment
  // its coordinates are quantised. Zero keeps the outline exactly as supplied.
  double outlineSimplifyFraction = .08;
  double outlineShortestEdgeFraction = .3;
};

// Everything a consumer needs to judge the grid without looking at it. Carried
// on the product rather than printed, so a test and a render read the same
// numbers.
// Everything a consumer needs to judge the grid without looking at it.
//
// The shape metrics - edges, areas, angles, aspect - are measured over the
// CORE only: cells that do not touch the city outline. A rim cell is fitted to
// the outline and cannot also be square, so holding it to the same standard
// measures the outline rather than the grid, and a city with a ragged boundary
// would score worse than one with a straight boundary built by the same code.
// The rim is measured separately and reported beside them, so it is visible
// rather than hidden.
//
// Coverage and boundary deviation are over everything, because those ARE
// questions about the whole grid.
struct CityGridQuality
{
  uint32_t faces = 0, vertices = 0, boundaryVertices = 0;
  uint32_t unpairedTriangles = 0, triangles = 0;
  uint32_t droppedOutside = 0, mergedRim = 0;

  double targetEdgeMetres = 0, targetAreaMetres = 0;
  double minEdge = 0, p05Edge = 0, medianEdge = 0, p95Edge = 0, maxEdge = 0;
  double p05Area = 0, medianArea = 0, p95Area = 0;
  double minInteriorAngle = 0, p05InteriorAngle = 0;
  double p95Aspect = 0, maxAspect = 0;

  double regionAreaMetres = 0, gridAreaMetres = 0, coverageRatio = 0;
  double boundaryDeviationMetres = 0;
  double fitResidual = 0, residualRatio = 0;

  // Interior faces against faces straddling two hexagonal patches. Equal
  // numbers are what says the relaxation ran over the whole city rather than
  // patch by patch; a seam shows up here long before it shows up in a render.
  double interiorMedianEdge = 0, seamMedianEdge = 0;
  double interiorMedianAngle = 0, seamMedianAngle = 0;
  uint32_t seamFaces = 0;

  // Faces touching the city outline against faces that do not. The outline is
  // a constraint the relaxation cannot satisfy and a square at the same time,
  // so the rim is always the worse of the two and saying by how much is the
  // difference between a diagnosis and a guess.
  uint32_t rimFaces = 0;
  double rimP05Angle = 0, coreP05Angle = 0;
  double rimMinAngle = 0, coreMinAngle = 0;
  double rimP05Area = 0, coreP05Area = 0;
};

struct CityGrid
{
  geom2::QuadMesh mesh;
  geom2::Polygon region;             // The union of the planning cells.
  std::vector<uint32_t> hexOfFace;   // Which hexagonal patch each cell came from.
  CityGridQuality quality;
  CityLatticeReport lattice;
  CityRelaxReport relax;  // The free pass over the whole lattice.
  CityRelaxReport settle; // The pass against the finished outline.
  std::vector<std::string> ledger;   // Why anything is missing.
};

// Builds the grid over the union of the given planning cells, in the frame they
// are given in.
//
// Cell polygons are read in the x-z plane, which is the plan plane this
// generator works in; `Position::y` is elevation and is ignored. The grid comes
// back in geom2's x-y, so a consumer publishing it back into the world maps
// grid y onto world z.
//
// Atomic: a budget failure produces no partial grid. An ordinary shortfall - a
// cell too small to hold a triangle, a boundary vertex too far from the outline
// - is a ledger entry and the grid still stands.
bool buildCityGrid(const std::vector<std::vector<Position>> &cells,
                   const CityGridSettings &settings,
                   CityGrid &out,
                   std::string *error = nullptr);

// The same, from a region that has already been unioned.
bool buildCityGridFromRegion(const geom2::Polygon &region,
                             const CityGridSettings &settings,
                             CityGrid &out,
                             std::string *error = nullptr);

} // namespace procedural::world::citywfc
