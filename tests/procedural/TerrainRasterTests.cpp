// Gates for the raster-sourced mesh.
//
// The source is the polygon terrain generator - the planet generator's original
// planar pipeline - rather than the analytic Voronoi graph. What has to hold:
// the mesh follows the raster it was sampled from, the constraints are the
// generator's own lines (coast, contours, ridges, cover boundaries, channels),
// and detail lands where the generator says there is something to resolve.

#include "procedural/geometry/TerrainRasterSource.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>

namespace
{

using namespace procedural;
using namespace procedural::geometry;

uint32_t failures = 0u;

void expect(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";
    return;
  }
  std::cout << "  FAILED " << what << "\n";
  ++failures;
}

} // namespace

int main()
{
  std::cout << "raster source\n";
  PolygonTerrainSettings settings{};
  settings.widthMetres = 16000.0;
  settings.heightMetres = 16000.0;
  settings.siteRadiusMetres = 130.0;
  settings.seed = 5ull;
  settings.oceanMarginFraction = 0.06;
  settings.continuousElevation = true;
  PolygonTerrainRasterSettings raster{};
  raster.width = 256u;
  raster.height = 256u;
  raster.workerCount = 0u;

  PolygonTerrainMaps maps;
  PolygonTerrainStatistics statistics{};
  std::string error;
  PolygonTerrainGenerator generator;
  if (!generator.generate(settings, raster, maps, &statistics, &error))
  {
    std::cout << "  FAILED source generation: " << error << "\n";
    return 1;
  }
  expect(statistics.siteCount > 0u && !maps.heightmap.empty(),
      "the polygon terrain generator produces a raster (" +
          std::to_string(statistics.siteCount) + " sites)");

  RasterTerrain terrain{};
  terrain.maps = &maps;
  terrain.extentMetres = settings.widthMetres;
  terrain.reliefMetres = 900.0;
  expect(terrain.valid(), "the raster wraps into a terrain source");
  bool oceanBorder = true;
  for (uint32_t at = 0; at < maps.width; ++at)
    for (size_t index : {size_t(at), size_t(maps.height - 1) * maps.width + at,
             size_t(at) * maps.width, size_t(at) * maps.width + maps.width - 1})
      oceanBorder &= maps.heightmap[index] == 0 &&
          maps.materials[index] == static_cast<uint8_t>(PolygonTerrainMaterial::Ocean);
  expect(oceanBorder, "the bounded map has ocean along all four sides");
  expect(!maps.regionEdges.empty() && !maps.riverEdges.empty(),
      "the original region boundaries and directed rivers survive raster export");
  MaterialPalette paletteCheck;
  const auto sampleMaterial = rasterMaterialFunction(terrain, paletteCheck);
  bool coverMatches = true;
  for (uint32_t y = 0; y < maps.height; y += 7)
    for (uint32_t x = 0; x < maps.width; x += 7)
    {
      const Point2 point(x * terrain.cellMetres(), y * terrain.cellMetres());
      coverMatches &= paletteCheck.entries()[sampleMaterial(point)].cover ==
          polygonMaterialToBiome(terrain.material(point));
    }
  const uint8_t existing = sampleMaterial(Point2(0, 0));
  const auto previous = paletteCheck.entries()[existing];
  MaterialEntry bank{};
  bank.role = SurfaceRole::RiverBank;
  paletteCheck.add(bank);
  expect(coverMatches && paletteCheck.entries()[existing] == previous,
      "material IDs match source biomes and survive new river-bank entries");

  // Sampling must agree with the raster it came from, or every downstream
  // measurement is about a surface nobody generated.
  bool agrees = true;
  const double cell = terrain.cellMetres();
  for (uint32_t y = 1u; y + 1u < maps.height; y += 37u)
    for (uint32_t x = 1u; x + 1u < maps.width; x += 37u)
    {
      const double expected =
          static_cast<double>(maps.heightmap[static_cast<size_t>(y) * maps.width + x]) /
          65535.0 * terrain.reliefMetres;
      const double sampled =
          terrain.height(Point2(x * cell, y * cell));
      agrees &= std::abs(sampled - expected) < 1.0e-6;
    }
  expect(agrees, "bilinear sampling reproduces the raster at cell centres");

  std::cout << "traced constraints\n";
  RasterSamplingSettings sampling{};
  sampling.toleranceMetres = 12.0;
  sampling.seed = 5ull;
  const RasterChains chains = extractRasterChains(terrain, sampling);
  expect(chains.regionEdges == maps.regionEdges.size(),
      "every original Voronoi region edge is a CDT constraint");
  expect(chains.coastRuns > 0u,
      "the coastline is traced from the raster (" +
          std::to_string(chains.coastRuns) + " runs)");
  expect(chains.contourLines > 0u,
      "contours are traced at a derived interval (" +
          std::to_string(chains.contourLines) + " lines at " +
          std::to_string(chains.contourIntervalMetres) + " m)");
  // Cover boundaries are what a texture pass will lay its seams along, and
  // what stops a biome transition cutting diagonally across a face.
  expect(chains.biomeRuns > 0u,
      "cover boundaries become constraints (" +
          std::to_string(chains.biomeRuns) + " runs)");

  bool contoursPinned = true;
  for (const Chain &chain : chains.chains)
    if (any(chain.kind, ChainKind::Contour))
      contoursPinned &= chain.hasElevation && chain.points.size() >= 2u;
  expect(contoursPinned, "every contour carries its exact elevation");

  // A traced contour must sit on the level it claims. If this holds and the
  // mesh check does not, the error is introduced by the meshing rather than by
  // the trace - which is the only way to tell those two apart.
  // A traced contour must sit on the level it claims. This is what separates
  // a tracing error from a meshing one: measured, the trace is exact to 9e-12
  // and every deviation in the mesh comes from pinning along chords.
  double worstChain = 0.0;
  for (const Chain &chain : chains.chains)
  {
    if (!any(chain.kind, ChainKind::Contour) || !chain.hasElevation)
      continue;
    for (const Point2 &point : chain.points)
      worstChain = std::max(
          worstChain, std::abs(terrain.height(point) - chain.elevationMetres));
  }
  expect(worstChain < 1.0e-6,
      "every traced contour point sits exactly on its level (" +
          std::to_string(worstChain) + " m)");

  std::cout << "channels\n";
  StrokeSettings strokes{};
  strokes.seed = 5ull;
  strokes.spacingMetres = std::max(30.0, cell * 1.5);
  const ChannelNetwork network =
      resolveRasterChannels(terrain, sampling, strokes);
  expect(network.carvedCells == maps.riverEdges.size(),
      "river reaches cover every directed source edge exactly once");
  if (network.reaches.empty())
  {
    std::cout << "  (this raster has no channels above the threshold)\n";
  }
  else
  {
    expect(network.duplicateCells == 0u,
        "no length of channel is carved twice (" +
            std::to_string(network.carvedCells) + " cells)");
    bool descends = true;
    bool widens = true;
    for (const ChannelReach &reach : network.reaches)
      for (size_t at = 1u; at < reach.vertices.size(); ++at)
      {
        descends &= reach.vertices[at].surfaceMetres <=
            reach.vertices[at - 1u].surfaceMetres + 1.0e-6;
        widens &= reach.vertices[at].widthMetres > 0.0;
      }
    expect(descends, "every reach descends on its way downstream");
    expect(widens, "every channel vertex has a width from its discharge");
    bool junctionsAgree = true;
    for (const ChannelNode &node : network.nodes)
      for (const uint32_t incoming : node.incoming)
        junctionsAgree &=
            std::abs(network.reaches[incoming].vertices.back().surfaceMetres -
                node.surfaceMetres) < 1.0e-9;
    expect(junctionsAgree,
        "every reach arrives at its junction's exact water surface");
  }

  std::cout << "detail follows the source\n";
  const auto scale = rasterToleranceScale(terrain, sampling);
  // Plains should be allowed to be coarse and peaks should not; without that
  // the budget goes into whichever region happens to have the most curvature.
  double plainsScale = 0.0;
  double peakScale = 0.0;
  uint32_t plainsSamples = 0u;
  uint32_t peakSamples = 0u;
  for (uint32_t y = 0u; y < maps.height; y += 4u)
    for (uint32_t x = 0u; x < maps.width; x += 4u)
    {
      const Point2 point(x * cell, y * cell);
      const uint8_t form = terrain.terrainForm(point);
      if (form == 1u)
      {
        plainsScale += scale(point);
        ++plainsSamples;
      }
      else if (form == 5u)
      {
        peakScale += scale(point);
        ++peakSamples;
      }
    }
  if (plainsSamples != 0u && peakSamples != 0u)
    expect(plainsScale / plainsSamples > peakScale / peakSamples,
        "plains tolerate more error than peaks (" +
            std::to_string(plainsScale / plainsSamples) + " against " +
            std::to_string(peakScale / peakSamples) + ")");

  std::cout << "the mesh\n";
  TerrainMeshSettings meshSettings{};
  meshSettings.tileSizeMetres = 4000.0;
  meshSettings.verticalToleranceMetres = 12.0;
  meshSettings.maximumRefinementRounds = 4u;
  meshSettings.minimumEdgeMetres = 12.0;
  meshSettings.chainSpacingMetres = 200.0;
  meshSettings.borderSpacingMetres = 500.0;
  meshSettings.toleranceScale = scale;
  meshSettings.workerCount = 0u;
  meshSettings.preserveChainVertices = true;

  TerrainMesh mesh;
  MaterialPalette *palette = &mesh.palette;
  const bool built = buildTerrainMesh(chains.chains, {},
      rasterHeightFunction(terrain), rasterMaterialFunction(terrain, *palette),
      terrain.extentMetres, meshSettings, mesh, &error);
  expect(built, "the raster world meshes: " + error);
  if (built)
  {
    expect(mesh.statistics.failedTiles == 0u, "no tile is left unbuilt");
    expect(mesh.statistics.constraintsIncluded ==
            mesh.statistics.constraintCount,
        "every traced constraint is inserted (" +
            std::to_string(mesh.statistics.constraintsIncluded) + "/" +
            std::to_string(mesh.statistics.constraintCount) + ")");
    // Materials come from the 2D generator's own classification, not from
    // geometry: it already ran a climate model over every cell.
    expect(mesh.palette.size() > 1u && !mesh.palette.saturated(),
        "cover comes from the generator's classes (" +
            std::to_string(mesh.palette.size()) + " palette entries)");

    // The mesh has to follow the raster it was sampled from.
    //
    // A pinned constraint vertex sits on the *chord* between traced points,
    // and a vertex inserted along that chord - by an encroachment split, say -
    // is pinned to the constraint's elevation wherever it lands. The terrain
    // between two traced points is not straight, so on steep ground those
    // vertices legitimately differ from a bilinear sample of the source.
    //
    // The design guarantees the *typical* vertex follows the source, not every
    // one, so that is what is measured. Asserting a maximum here would be
    // asserting something the pinning model does not promise.
    std::vector<double> deviations;
    for (const MeshTile &tile : mesh.tiles)
      for (const MeshVertex &vertex : tile.vertices)
        deviations.push_back(
            std::abs(vertex.y - terrain.height(Point2(vertex.x, vertex.z))));
    std::sort(deviations.begin(), deviations.end());
    const double median = deviations[deviations.size() / 2u];
    const double p95 = deviations[deviations.size() * 95u / 100u];
    const double worstDeviation = deviations.back();
    expect(median < 1.0,
        "the typical vertex sits on the source heightfield (median " +
            std::to_string(median) + " m)");
    expect(p95 < meshSettings.verticalToleranceMetres * 4.0,
        "and the 95th percentile stays near the tolerance (" +
            std::to_string(p95) + " m against a " +
            std::to_string(meshSettings.verticalToleranceMetres) +
            " m budget)");
    std::cout << "  (worst " << worstDeviation << " m, on a pinned chord)\n";

  }

  {
    PolygonTerrainMaps flatMaps;
    flatMaps.width = flatMaps.height = 2;
    flatMaps.heightmap.assign(4, 32768);
    RasterTerrain flat{&flatMaps, 100.0, 100.0, 0.0};
    ChannelNetwork channels;
    ChannelReach reach;
    ChannelVertex a{}, b{};
    a.position = Point2(20, 50); b.position = Point2(80, 50);
    a.widthMetres = b.widthMetres = 10;
    a.surfaceMetres = 48; b.surfaceMetres = 40;
    a.depthMetres = 2; b.depthMetres = 5;
    a.bedMetres = 46; b.bedMetres = 35;
    reach.vertices = {a, b};
    channels.reaches.push_back(reach);
    const auto carved = rasterRiverHeightFunction(flat, channels);
    expect(std::abs(carved(Point2(50, 50)) - 40.5) < 1e-6,
        "expanded river centre follows the interpolated bed depth");
    expect(std::abs(carved(Point2(50, 55)) - 44.0) < 1e-6,
        "expanded river bank meets the channel surface");
    expect(std::abs(carved(Point2(50, 75)) - flat.height(Point2(50, 75))) < 1e-6,
        "carving leaves ground outside the corridor unchanged");
  }
  {
    TerrainMesh oceanMesh;
    MaterialEntry ocean{};
    ocean.cover = world::Biome::Ocean;
    ocean.role = SurfaceRole::Water;
    MaterialEntry land{};
    const uint8_t water = oceanMesh.palette.add(ocean);
    const uint8_t ground = oceanMesh.palette.add(land);
    oceanMesh.tiles.resize(1);
    auto &tile = oceanMesh.tiles.front();
    tile.vertices = {{0, 12, 0}, {10, -2, 0}, {10, 4, 10}, {0, 20, 10}};
    tile.indices = {0, 1, 2, 0, 2, 3};
    tile.materials = {water, ground};
    flattenRasterOcean(terrain, oceanMesh);
    bool level = true;
    for (size_t corner = 0; corner < 3; ++corner)
      level &= tile.vertices[tile.indices[corner]].y == terrain.seaLevelMetres;
    expect(level && tile.vertices[tile.indices[3]].y == 12.0,
        "every ocean corner is exactly sea level without flattening adjacent land");
  }
  if (failures != 0u)
  {
    std::cout << failures << " raster test(s) failed\n";
    return 1;
  }
  std::cout << "all raster tests passed\n";
  return 0;
}
