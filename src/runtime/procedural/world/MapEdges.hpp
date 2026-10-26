#pragma once

#include "procedural/world/MapGraph.hpp"

#include <cstdint>
#include <vector>

namespace procedural::world
{

// The map's boundaries as an explicit structure: welded vertices, edges shared
// by the two cells either side, and a loop of edges per cell.
//
// Cells carry their outlines as independent polygons, which is fine to store and
// wrong to draw from: two cells' copies of a shared boundary are separate arrays
// of points, so anything that decorates one has to be made to agree with the
// other by hand. Every attempt to wiggle the boundaries ran into that. With the
// edges shared, a boundary is decorated **once** and both cells read the result.

struct MapBoundaryVertex
{
  Point2 position;
  // Cells meeting here. Three is the ordinary case for a Voronoi vertex; two
  // means a chain along the world border.
  uint8_t cells = 0u;
  bool onWorldBorder = false;
  // Land on one side and water on the other - a point on the coastline.
  bool coastal = false;
  // At least one incident edge separates two map classifications (named
  // region, biome, content, or landform). These are the boundaries users see
  // in the filled map, irrespective of which layer is selected.
  bool classificationBoundary = false;
};

struct MapBoundaryEdge
{
  uint32_t from = 0u;
  uint32_t to = 0u;
  // kInvalidNode on the far side means the world border.
  uint32_t cellA = kInvalidNode;
  uint32_t cellB = kInvalidNode;

  bool coast = false;
  bool regionBorder = false;
  bool biomeBorder = false;
  bool contentBorder = false;
  bool landformBorder = false;
  // Any categorical transition represented by a filled map layer. This is a
  // geometry property, not a request to draw a line over the map.
  bool classificationBorder = false;
  bool worldBorder = false;
  // Set by the caller: is anything different across this edge? An edge between
  // two cells drawn the same colour is invisible, and it is the *invisible*
  // ones that matter here - they are what let a boundary run through a vertex
  // instead of stopping at it.
  bool visible = false;
  double lengthMetres = 0.0;

  // The drawn path from `from` to `to`, endpoints included. Straight until the
  // post-process fills it in.
  std::vector<Point2> path;
};

struct MapEdgeSet
{
  std::vector<MapBoundaryVertex> vertices;
  std::vector<MapBoundaryEdge> edges;
  // Per node id: the edges of that cell's loop, in order. Positive is the edge
  // walked from `from` to `to`, negative from `to` to `from`; the stored value
  // is the index plus one so zero is not ambiguous.
  std::vector<std::vector<int32_t>> loops;

  bool empty() const
  {
    return edges.empty();
  }
};

// Builds the edge set from the leaves of a graph.
//
// Vertices within `weldToleranceMetres` of each other are the same vertex,
// which is what pairs two cells' copies of a shared corner.
MapEdgeSet buildEdgeSet(
    const MapGraph &graph, double weldToleranceMetres);

struct EdgeRelaxSettings
{
  // Target length of a drawn segment. Shorter is smoother and costs more
  // points; there is no reason to go below what the display can resolve.
  double segmentMetres = 2500.0;

  // How far a point may be moved, in metres. Kept well under a cell so a
  // boundary cannot fold through the one beyond it.
  double amplitudeMetres = 4200.0;
  // Wavelength of the displacement field. This is what gives the boundaries
  // momentum: neighbouring points sample nearly the same value and so move
  // nearly together, and the line bends and holds a direction instead of
  // reversing at every step the way independent per-segment draws do.
  double wavelengthMetres = 30000.0;
  uint32_t octaves = 3u;
  double persistence = 0.5;

  // Coastlines get more of it than interior boundaries; a shore is the most
  // looked-at line on the map and an inland biome edge is not.
  double coastScale = 1.35;
  double interiorScale = 0.75;
  // Semantic fill boundaries should be calmer than coastlines. They still
  // bend with the terrain, but do not inherit cell-scale shoreline noise.
  double classificationScale = 0.18;

  uint64_t seed = 0u;
};

// Displaces every boundary through a **domain warp** - a smooth vector field of
// position - rather than by subdividing each edge on its own.
//
// This is the step that removes the agreement problem instead of solving it.
// A displacement that is a pure function of position moves every copy of a
// shared point to the same place, whoever is asking, so polygons stay closed and
// neighbours stay flush with no canonicalisation, no shared random stream and no
// special case for the edges whose two sides could not find each other. Those
// were being left straight, and it showed.
void relaxEdges(MapEdgeSet &edges, const EdgeRelaxSettings &settings);

// Low-pass filters only the interior vertices of categorical fill-boundary
// chains. Triple junctions, coasts and the world rim remain fixed; all incident
// cell edges are updated to the same shared vertex, so smoothing cannot open
// fill cracks.
void smoothClassificationJunctions(
    MapEdgeSet &edges, uint32_t passes, double strength);

// Rounds the boundaries by fitting a curve through them.
//
// The corners are the problem: three Voronoi edges meet at every vertex at
// roughly 120 degrees, and displacing the edges does nothing about that - the
// result is a wiggly line made of hard angles. What is wanted is for a boundary
// to *turn* through a vertex rather than break at it.
//
// Two things make that possible. First, boundaries are followed as **chains**:
// a run of edges that all separate the same two things, through vertices where
// only two visible edges meet. A vertex inside a chain is a bend to be
// smoothed; only a vertex where three different things meet is a real corner,
// and those are rare. Second, each cubic Bezier span **interpolates** its shared
// endpoints. A curve that passes through the shared vertices keeps every cell
// meeting where it met before; one that cuts corners pulls each cell away from
// its neighbours and opens a gap at every junction.
//
// The sampled curve is a cubic Bezier with handles derived from the preceding
// and following chain vertices. `tension` controls handle length; 0.5 is the
// balanced default and handles are clamped to prevent loops at tight corners.
void smoothChains(
    MapEdgeSet &edges, uint32_t samplesPerEdge, double tension);

// A cell's drawn outline, assembled from its loop.
std::vector<Point2> cellBoundary(const MapEdgeSet &edges, uint32_t node);

} // namespace procedural::world
