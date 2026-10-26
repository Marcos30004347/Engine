#pragma once

#include "procedural/SphericalVoronoiMesh.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace procedural
{

// Planet-scale procedural generator inspired by Red Blob's sphere experiment:
// jittered/coarse regions, randomized plate fill, and plate movement provide
// the base crust, broad convergence uplift, and divergent subsidence. Seamless
// 3D noise only varies crustal resistance; hydraulic erosion owns the final
// valleys and drainage instead of a separate mountain-curve generator.
//
// This is intentionally a source-world model. The streamed voxel terrain should
// later bake rectangular windows from this planet into the existing terrain
// package instead of regenerating the planet at runtime.

// Fluvial relief solver supplied by the caller, so the generator can stay free
// of any rendering dependency while the solver runs on the GPU.
//
// The division of labour is deliberate and is the whole point of the split: the
// graph owns tectonics, plates, climate and lakes, and the solver owns all
// fluvial and hillslope relief. There is no second erosion model behind this
// interface — when no solver is supplied, the raster carries no fluvial relief
// at all beyond what the graph's own erosion already put into the macro field.
class SphericalTerrainFluvialSolver
{
public:
  struct Request
  {
    uint32_t width = 0u;
    uint32_t height = 0u;
    double planetRadiusMetres = 0.0;
    // Physical span of the normalized height encoding, so the solver can work
    // in metres. Erosion parameters are meaningless without it.
    double reliefMetres = 0.0;
    // Normalized land elevation, same encoding as SphericalTerrainMaps::heightmap.
    // Solved in place.
    std::vector<uint16_t> *heightmap = nullptr;
    // Ocean is the outlet boundary condition; a closed sphere has no other.
    const std::vector<uint8_t> *oceanMask = nullptr;
    // Uplift field. This is exactly the reference solver's uplift map.
    const std::vector<uint8_t> *tectonics = nullptr;
    // Geology, as PlanetRockSuite plus a quantized bedding offset. Optional:
    // without it every cell erodes at the same rate, which is what makes a
    // landscape read as noise-driven rather than geology-driven, since cliffs
    // and benches are differential erosion.
    //
    // The solver resolves suite plus current height to a rock type every
    // iteration rather than taking a fixed rock index, so incision exposes the
    // bed beneath as it cuts.
    const std::vector<uint8_t> *rockSuite = nullptr;
    const std::vector<uint8_t> *bedPhase = nullptr;
    // Filled by the solver: the rock finally exposed at the eroded surface.
    std::vector<uint8_t> *exposedLithology = nullptr;
  };

  virtual ~SphericalTerrainFluvialSolver() = default;
  virtual bool solve(const Request &request, std::string *error) = 0;
};

struct SphericalTerrainSettings
{
  uint64_t seed = 0x7f4a7c159e3779b9ull;
  uint32_t width = 512u;
  uint32_t height = 256u;
  bool rasterizeMaps = true; // False keeps only the authoritative graph/mesh; atlases can be baked for export later.
  uint32_t plateCount = 80u; // 0 also selects Orogen's resolution-independent default of 80.
  double oceanicPlateFraction = 0.58;
  double jitter = 0.75;             // Orogen default: organic cells without maximum scattering.
  double collisionThreshold = 0.24; // Higher values make fewer plate contacts produce crustal uplift.
  double terrainWarp = 0.75;        // Tangent-space warp of the complete tectonic elevation lookup.
  double roughness = 0.40;          // High-frequency crust detail, independent from Voronoi jitter.
  uint32_t graphWorkerCount = 0u;   // 0 uses up to twice the detected hardware threads.
  uint32_t graphRegionCount = 204000u;
  // Applied first to the authoritative graph and again to the derived raster.
  // The second pass cuts sub-region drainage into high-resolution exports
  // without changing plate topology or the stable graph mesh.
  uint32_t hydraulicIterations = 10u;
  uint32_t thermalIterations = 1u;
  double hydraulicErosion = 0.00030;
  double thermalErosion = 0.015;
  double talusSlope = 1.16;
  uint32_t ridgeSharpeningIterations = 3u;
  double ridgeSharpening = 0.04;
  uint32_t soilCreepIterations = 3u;
  double soilCreep = 0.1125;
  double mountainUplift = 1.0;
  double mountainBeltWidth = 1.0;
  double mountainRidgeSpacing = 1.0;
  double mountainRidgeHeight = 1.0;
  double coastalPlainWidth = 1.0;
  double shorelineDetail = 1.0;
  double interiorRelief = 1.0;
  double lakeAbundance = 1.0;
  double desertStrength = 1.0;
  double biomeVariation = 1.0;
  double climateWetness = 1.0;
  // Physical scale. The generator itself is unit-free, but erosion is not:
  // stream power is a function of ground distance and metres of relief.
  double planetRadiusMetres = 100000.0;
  double reliefMetres = 3000.0;
  // Optional; not owned. Supplies all fluvial and hillslope relief.
  SphericalTerrainFluvialSolver *fluvialSolver = nullptr;
};

// Wall-clock cost of every generation stage, in milliseconds. This exists so
// erosion work can be judged against a measured baseline instead of a guess:
// the graph and the raster run different models over different domains, and
// only a per-stage split shows which one a change actually moved.
struct SphericalTerrainProfile
{
  // Authoritative Voronoi graph. graphTotal covers the whole macro pass
  // including the sub-stages below, which do not sum to it.
  double graphMeshMs = 0.0;    // Poisson sites, Delaunay/Voronoi dual mesh
  double graphFieldsMs = 0.0;  // plate fill, tectonic uplift, margins
  double graphErosionMs = 0.0; // erodeSphericalGraph: implicit SPL + thermal
  double graphClimateMs = 0.0; // wind, temperature, precipitation, drylands
  double graphRiversMs = 0.0;  // Delaunay routing, lakes, classification
  double graphSurfaceMs = 0.0;   // displaced dual mesh for the preview
  double graphRasterizeMs = 0.0; // graph regions binned into the macro atlas
  double graphTotalMs = 0.0;
  // Derived equirectangular raster.
  double macroBlurMs = 0.0;         // macro field reconstruction blurs
  double rasterStructureMs = 0.0;   // coasts, relief, bathymetry, base climate
  double rasterCoastMs = 0.0;       // distance-from-ocean transform
  double rasterMountainMs = 0.0;    // slope/curvature mountain classification
  double rasterClimateMs = 0.0;     // rain shadows, drying, field quantization
  double rasterRoutingMs = 0.0;     // routeRainfallRunoff, both invocations
  double rasterErosionMs = 0.0;     // erodeSphericalRasterMaps
  double rasterClassifyMs = 0.0;    // per-texel material classification
  double totalMs = 0.0;
};

struct SphericalTerrainLine
{
  SpherePoint a;
  SpherePoint b;
  std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
  float width = 0.003f;
};

// Authoritative per-region payload. This is intentionally independent from an
// export atlas so future planet LOD/chunk streaming can consume the graph
// directly and bake only the visible or requested windows.
struct SphericalTerrainNode
{
  static constexpr uint32_t noRegion = 0xffffffffu;
  static constexpr uint8_t oceanFlag = 1u << 0u;
  static constexpr uint8_t lakeFlag = 1u << 1u;

  SpherePoint position;
  // Visible radial surface. Lakes use their flat water surface here for
  // compatibility with the graph-native preview.
  uint16_t elevation = 0u;
  uint8_t plate = 0u;
  uint8_t continent = 0u;
  uint8_t tectonics = 0u;
  uint8_t mountain = 0u;
  uint8_t river = 0u;
  uint8_t temperature = 0u;
  uint8_t precipitation = 0u;
  uint8_t windExposure = 0u;
  uint8_t windEast = 127u;
  uint8_t windNorth = 127u;
  uint8_t plateMotionEast = 127u;
  uint8_t plateMotionNorth = 127u;
  uint8_t material = 0u;
  // Drainage/gameplay fields retained specifically for export. Terrain and
  // water elevations use the same encoding as elevation; waterElevation is
  // zero for dry land. River discharge remains an unquantized graph value.
  uint16_t terrainElevation = 0u;
  uint16_t waterElevation = 0u;
  uint32_t downstreamRegion = noRegion;
  float riverDischarge = 0.0f;
  uint8_t flags = 0u;
};

struct SphericalTerrainMaps
{
  uint32_t width = 0u;
  uint32_t height = 0u;
  // Solid terrain only. Ocean texels are zero/no-terrain and must be
  // interpreted through oceanMask rather than as a water-surface elevation.
  std::vector<uint16_t> heightmap;
  // Bathymetric height below the ocean datum. Raw values encode positive
  // depth, while the package channel applies a negative scale so sampling it
  // returns a negative terrain elevation. Dry land and lakes are zero.
  std::vector<uint16_t> oceanFloorHeightmap;
  // Water is independent from terrain: ocean uses the zero-elevation datum,
  // lakes retain their generated spill surface, and dry texels are zero.
  std::vector<uint16_t> waterHeightmap;
  std::vector<uint8_t> oceanMask;
  std::vector<uint8_t> lakeMask;
  // Stable ID of the source Voronoi region at every exported texel. Unlike
  // the 8-bit plate visualization this survives future chunk/LOD streaming.
  std::vector<uint32_t> sourceRegions;
  std::vector<uint8_t> plates;
  std::vector<uint8_t> continents;
  std::vector<uint8_t> tectonics;
  std::vector<uint8_t> mountains;
  std::vector<uint8_t> rivers;
  std::vector<uint8_t> temperature;
  std::vector<uint8_t> precipitation;
  std::vector<uint8_t> wind;
  std::vector<uint8_t> windEast;
  std::vector<uint8_t> windNorth;
  std::vector<uint8_t> plateMotionEast;
  std::vector<uint8_t> plateMotionNorth;
  std::vector<uint8_t> materials;
  // Geology, as a rock suite plus a bedding offset rather than a fixed rock
  // type. The rock actually exposed is a function of the *current* surface
  // height, so a valley that cuts down exposes the bed below it; baking one
  // rock index per texel would freeze the pre-erosion surface's geology in
  // place and no amount of incision would ever reveal a layer.
  std::vector<uint8_t> rockSuite;
  // Bedding offset, quantized over one stratum thickness.
  std::vector<uint8_t> bedPhase;
  // PlanetLithology exposed at the final surface, derived from the two fields
  // above once erosion has settled. This is the field consumers read.
  std::vector<uint8_t> lithology;
  std::vector<SpherePoint> regionPositions;
  std::vector<uint8_t> regionElevations;
  std::vector<uint8_t> regionRivers;
  std::vector<SphericalTerrainNode> regionNodes;
  // CSR graph adjacency: neighbours for node i are in
  // [regionNeighbourOffsets[i], regionNeighbourOffsets[i + 1]).
  std::vector<uint32_t> regionNeighbourOffsets;
  std::vector<uint32_t> regionNeighbourIndices;
  // Red Blob-style dual mesh used by the preview. Positions contain all
  // Voronoi region sites followed by all Delaunay triangle centres. The
  // indices choose the ridge or valley diagonal for every half-edge.
  std::vector<SpherePoint> surfacePositions;
  std::vector<uint16_t> surfaceElevations;
  // Per-vertex graph attributes. These are authoritative preview data, not
  // samples reconstructed from the equirectangular export.
  std::vector<uint8_t> surfaceContinents;
  std::vector<uint8_t> surfaceTectonics;
  std::vector<uint8_t> surfaceMountains;
  std::vector<uint8_t> surfaceTemperature;
  std::vector<uint8_t> surfacePrecipitation;
  std::vector<uint8_t> surfacePlates;
  std::vector<uint8_t> surfaceRivers;
  std::vector<uint8_t> surfaceMaterials;
  std::vector<uint32_t> surfaceIndices;
  std::vector<SphericalTerrainLine> sphereLines;
};

struct SphericalTerrainStatistics
{
  uint32_t plateCount = 0u;
  uint32_t continentalPlateCount = 0u;
  uint32_t oceanicPlateCount = 0u;
  uint32_t landPixelCount = 0u;
  uint32_t oceanPixelCount = 0u;
  uint32_t mountainPixelCount = 0u;
  uint32_t lakePixelCount = 0u;
  uint32_t riverSourceCount = 0u;
  uint32_t riverPixelCount = 0u;
};

class SphericalTerrainGenerator
{
public:
  bool generate(const SphericalTerrainSettings &settings, SphericalTerrainMaps &maps, SphericalTerrainStatistics *statistics = nullptr, SphericalTerrainProfile *profile = nullptr);
};

} // namespace procedural
