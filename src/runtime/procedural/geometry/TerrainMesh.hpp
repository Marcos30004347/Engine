#pragma once

#include "procedural/Predicates.hpp"
#include "procedural/geometry/TerrainMaterial.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::geometry
{

// What a constraint chain means. A segment can carry several of these at once -
// a contour that runs along a region border is both - so this is a bitfield and
// the PSLG pass unions them when it collapses collinear overlaps.
enum class ChainKind : uint16_t
{
  None = 0u,
  WorldBorder = 1u << 0u,
  Coast = 1u << 1u,
  Contour = 1u << 2u,
  RegionBorder = 1u << 3u,
  // Reserved for the stages that follow: rivers and roads (strokes), ridges,
  // plateau brinks and toes, ramp edges.
  Stroke = 1u << 4u,
  Ridge = 1u << 5u,
  Brink = 1u << 6u,
  Toe = 1u << 7u,
  Slope = 1u << 8u,
  // A parcel boundary: the edge of a field, a wood or a settlement plot. These
  // are what make a cell's material uniform, so they are constraints even where
  // nothing stands on them.
  Parcel = 1u << 9u,
};

constexpr ChainKind operator|(const ChainKind left, const ChainKind right)
{
  return static_cast<ChainKind>(
      static_cast<uint16_t>(left) | static_cast<uint16_t>(right));
}

constexpr bool any(const ChainKind value, const ChainKind mask)
{
  return (static_cast<uint16_t>(value) & static_cast<uint16_t>(mask)) != 0u;
}

// A semantic line of the map, in world metres.
//
// `elevationMetres` is meaningful only when the chain is an isoline - a contour,
// or later a plateau brink snapped onto one. Everything else samples its height
// from the surface, and `hasElevation` is what distinguishes the two.
struct Chain
{
  std::vector<Point2> points;
  ChainKind kind = ChainKind::None;
  double elevationMetres = 0.0;
  // Per-point elevations, when the chain's height varies along it. A contour
  // is one elevation; a carved river bed is a different one at every point,
  // and without this the triangulation runs the ground straight across the
  // channel and roofs over the trench the faces just cut.
  std::vector<double> elevations;
  bool hasElevation = false;
  bool closed = false;

  double elevationAt(const size_t index) const
  {
    return index < elevations.size() ? elevations[index] : elevationMetres;
  }
};

// One quad of a step face: the ruled strip between a brink span and the toe
// span below it.
//
// Both chains are resampled to the same arc-length parameterisation, so a step
// is a 1:1 ladder of these and is watertight by construction. The face is
// appended to the tile that owns its centroid rather than produced by the
// triangulator, because a vertical discontinuity is two heights at one plan
// position and a 2D triangulation has no way to express that.
struct StepQuad
{
  Point2 brinkA;
  Point2 brinkB;
  Point2 toeA;
  Point2 toeB;
  double brinkAHeight = 0.0;
  double brinkBHeight = 0.0;
  double toeAHeight = 0.0;
  double toeBHeight = 0.0;
  uint8_t material = 0u;
  // What this face is, before anything about where it is. Resolved to a
  // palette index once the world's rock and cover are known.
  SurfaceRole role = SurfaceRole::Face;
  // Set where the face is gentle enough to walk: a ramp's *surface* carries
  // ground cover even though its shoulders stay rock.
  bool walkable = false;
  PlanetLithology lithology = PlanetLithology::Sedimentary;
  world::Biome cover = world::Biome::Grassland;
};

// Vertex of the built mesh. Positions are world metres; `y` is up.
struct MeshVertex
{
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

// One built tile. Triangles index into `vertices`, which are tile-local: a tile
// is independently renderable and independently storable, and the border
// contract is what makes neighbouring tiles agree without sharing an index
// space.
struct MeshTile
{
  uint32_t tileX = 0u;
  uint32_t tileZ = 0u;
  double minX = 0.0;
  double minZ = 0.0;
  double sizeMetres = 0.0;

  std::vector<MeshVertex> vertices;
  // Three vertex indices per triangle, counter-clockwise seen from above.
  std::vector<uint32_t> indices;
  // One per triangle. Values are procedural::world::CellType.
  std::vector<uint8_t> materials;

  // Ordered border vertex positions per side, for the agreement gate: south,
  // north, west, east. Hashing these is how a build proves it did not crack.
  std::array<std::vector<Point2>, 4u> borders;

  // Constraint accounting. A tile that failed to insert a constraint has a
  // hole in a line the map said was there, so this is a gate and not a
  // diagnostic.
  uint64_t constraintCount = 0u;
  uint64_t constraintsIncluded = 0u;
  // Free interior vertices removed from planar patches.
  uint64_t simplifiedVertices = 0;
  // The Delaunay restoration had to be relaxed for this tile to converge.
  bool relaxedDelaunay = false;
  // Rounds of encroached-segment splitting this tile needed.
  uint32_t encroachmentSplits = 0u;
  // Constraints the triangulator could not insert even after splitting, and
  // which were dropped so the tile could be built at all. Counted, never
  // silent: a rising number means the chain producers are emitting geometry
  // that cannot be triangulated.
  uint32_t droppedConstraints = 0u;
  uint32_t qualityRounds = 0u;
  // Triangles appended as step faces rather than produced by the triangulator.
  uint32_t stepFaceCount = 0u;

  size_t triangleCount() const
  {
    return indices.size() / 3u;
  }
};

struct TerrainMeshStatistics
{
  uint32_t tileCount = 0u;
  uint64_t vertexCount = 0u;
  uint64_t triangleCount = 0u;
  uint64_t constraintCount = 0u;
  uint64_t constraintsIncluded = 0u;
  uint64_t simplifiedVertices = 0;
  uint32_t refinementRounds = 0u;
  uint32_t failedTiles = 0u;
  uint32_t relaxedTiles = 0u;
  uint64_t encroachmentSplitRounds = 0u;
  uint64_t droppedConstraints = 0u;
  uint32_t degradedTiles = 0u;
  uint64_t stepFaceTriangles = 0u;
  uint32_t stepCount = 0u;
  uint32_t strokeCourses = 0u;
  uint32_t strokeJunctions = 0u;
  uint32_t strokeDuplicates = 0u;
  double buildSeconds = 0.0;
  double peakTileSeconds = 0.0;
  double minimumTriangleAngleDegrees = 180.0;
  // Triangles under five degrees. The single minimum is nearly meaningless on
  // a million-triangle mesh - one sliver sets it - so the count is what says
  // whether quality is a problem.
  uint64_t sliverTriangleCount = 0u;
  // What those slivers are actually worth. A count says how many faces are
  // thin; the area fraction says whether anyone can see them.
  double sliverAreaSquareMetres = 0.0;
  double totalAreaSquareMetres = 0.0;
};

// A node of the terrace graph: a plateau block, or the base terrace.
constexpr uint32_t kBaseTerrace = 0xffffffffu;

struct TerraceEdge
{
  uint32_t from = kBaseTerrace;
  uint32_t to = kBaseTerrace;
  // Steepest gradient anywhere along the ramp. The gate is that this stays
  // under the walkable bound; a ramp that does not is a cliff with a dent.
  double gradient = 0.0;
  double widthMetres = 0.0;
  Point2 position;
};

// Nodes are blocks plus the base terrace; edges are ramps. This is the
// structure navigation, settlement placement and road routing all want, and
// none of them can recover it from triangles - which is why it is exported.
struct TerraceGraph
{
  std::vector<TerraceEdge> edges;
  // Blocks with no ramp at all: unreachable ground, and a bug rather than a
  // style. Empty is the gate.
  std::vector<uint32_t> unreachableBlocks;
  uint32_t blockCount = 0u;
  double steepestRampGradient = 0.0;

  bool connected() const
  {
    return unreachableBlocks.empty();
  }
};

// Cliff geometry derived from a world's plateau blocks: the brink chains, the
// toe chains offset outward from them, and the quads between.
//
// The setback that separates brink from toe is what makes a cliff a slope
// rather than a wall, and it is not a free parameter - it comes from the talus
// angle of the rock, which `PlanetGeology.hpp` already tabulates. Weak rock
// stands at 28 degrees and granite at 62, so the same riser produces a debris
// slope or a sheer face depending on what it is cut into.

struct TerrainMesh
{
  std::vector<MeshTile> tiles;
  TerraceGraph terraces;
  // Per-face material vocabulary. Faces carry a byte indexing this, which is
  // the form the virtual-geometry payload wants too.
  MaterialPalette palette;
  double extentMetres = 0.0;
  double tileSizeMetres = 0.0;
  uint32_t tilesPerAxis = 0u;
  TerrainMeshStatistics statistics{};

  uint64_t triangleCount() const;
  // Stable signature over every tile's geometry. This is the determinism gate:
  // two builds, in any tile order, on any thread count, must agree.
  uint64_t signature() const;
};

} // namespace procedural::geometry
