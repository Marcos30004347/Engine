#pragma once

#include "procedural/geom2/QuadMesh.hpp"

namespace procedural::world::citywfc {

using geom2::Point;
using geom2::Polygon;
using geom2::QuadMesh;

// Relaxing an irregular quad mesh toward squares, against the city outline.
//
// The kernel is the closest-square fit: for one quad, the square sharing its
// centroid whose orientation and size minimise the sum of squared corner
// distances. Modelling the square's corners as s_k = c + R_k v, with R_k the
// rotation by k quarter turns and v free, and using I + R90 + R180 + R270 = 0:
//
//     c = mean(p_k)
//     v = (1/4) sum_k R_(-k) (p_k - c)
//     ideal_k = c + R_k v
//
// which is additions, subtractions and a division by four. The published form
// computes an angle with arctan and comes back through sin and cos; keeping the
// direction as a vector gives the same stationary point with no transcendental,
// and that is what lets the grid stay byte-identical across optimisation levels.
//
// `sqrt` is allowed and `atan2`, `sin`, `cos` and `pow` are not. IEEE 754
// requires a square root to be correctly rounded, so it gives the same bits on
// every target; the transcendentals carry no such guarantee and differ between
// libm versions. Point-to-segment projection is a ratio of dot products and
// nearest-segment selection compares squares, so the only roots here measure an
// actual length.
//
// Two things were learned by getting them wrong, and both are why this file
// looks the way it does.
//
// The fit leaves the square's SIZE free, which is right in the middle of the
// grid and wrong at its edge: a quad squashed against the outline has a small
// square as its nearest square, so a free-size fit preserves the squash instead
// of undoing it. `areaWeight` pulls every quad toward one size, and the edge of
// the city is where its absence shows.
//
// And the outline is imposed LOCALLY. An earlier version laid each boundary
// cycle onto its ring by arc length, with the parameters forced monotone so the
// cycle could not fold. That works on a rectilinear city and destroys a city
// with a narrow neck: the nearest point jumps to the far bank, the repair
// compresses the whole cycle into a short arc, and the mesh turns inside out.
// What actually prevents a fold is the move guard below - no vertex may take a
// step that leaves a face it touches non-simple - and with that in place the
// outline needs no parameterisation at all. A boundary vertex is snapped to the
// nearest point within a radius, or it is left alone.

struct CityRelaxSettings
{
  uint32_t iterations = 32;
  double lambda = .7;           // How far a vertex moves toward its ideal each pass.
  double convergenceRatio = .05; // Stop once the fit residual falls this far.

  // How hard each quad is pulled toward a square of `targetEdgeMetres` rather
  // than toward the nearest square of any size. Zero is the pure closest-square
  // fit; one ignores the quad's own scale entirely.
  double areaWeight = .35;
  double targetEdgeMetres = 6;

  // How far a boundary vertex may be pulled to reach the outline. Zero relaxes
  // with a free boundary, which is what the first pass over the uncut lattice
  // wants: every cell that survives the cut was then relaxed with neighbours on
  // all four sides.
  double snapRadiusMetres = 0;

  // The smallest area a cell may be left with, as a fraction of the target.
  //
  // Positivity is not enough. A cell can be squashed flat against a detailed
  // outline - four nearly collinear corners spanning several metres and
  // enclosing a millionth of one - and it passes every test for being simple
  // and wound the right way right up to the point where quantising the
  // coordinates flips it. A floor on the area is what stops the collapse, and
  // it is checked as "not worse than before" as well as "above the floor", so a
  // cell that is already thin can still recover instead of freezing.
  double minimumAreaFraction = .02;
};

struct CityRelaxReport
{
  uint32_t iterations = 0;
  double initialResidual = 0, finalResidual = 0;
  uint32_t refusedMoves = 0; // Steps the guard would not allow.
  bool monotonic = true;     // The residual fell at every pass.
  std::vector<double> residuals;
};

// Snaps every boundary vertex that is already within `snapRadiusMetres` of the
// outline onto it, refusing any move that would leave a face non-simple.
//
// A vertex further away than that is a gap in the coverage, not a vertex to drag
// across the city; it stays where it is and the coverage metric reports it.
bool snapBoundaryToRegion(QuadMesh &mesh, const Polygon &region, double snapRadiusMetres, double minimumAreaMetres, uint32_t *refused = nullptr);

bool relaxQuadMesh(QuadMesh &mesh, const Polygon &region, const CityRelaxSettings &settings, CityRelaxReport &report, std::string *error = nullptr);

} // namespace procedural::world::citywfc
