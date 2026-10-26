// Gates for the low-poly terrain mesh stage.
//
// The properties here are the ones the whole design rests on: flat ground costs
// nothing, curved ground refines, tiles agree at their shared borders, and the
// build is byte-identical however many threads run it.

#include "procedural/geometry/TerrainMeshBuild.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

namespace
{

using procedural::Point2;
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

constexpr double kExtent = 16000.0;

std::vector<Chain> worldBorderOnly()
{
  Chain border{};
  border.kind = ChainKind::WorldBorder;
  border.closed = true;
  border.points = {Point2(0.0, 0.0), Point2(kExtent, 0.0),
      Point2(kExtent, kExtent), Point2(0.0, kExtent)};
  return {border};
}

TerrainMeshSettings baseSettings()
{
  TerrainMeshSettings settings{};
  settings.tileSizeMetres = 4000.0;
  settings.verticalToleranceMetres = 5.0;
  settings.maximumRefinementRounds = 5u;
  settings.minimumEdgeMetres = 50.0;
  settings.chainSpacingMetres = 500.0;
  settings.borderSpacingMetres = 1000.0;
  settings.workerCount = 1u;
  return settings;
}

const MaterialFunction kGrass = [](Point2) -> uint8_t { return 3u; };

void testFlatGroundIsNearlyFree()
{
  std::cout << "flat ground\n";
  const HeightFunction flat = [](Point2) { return 120.0; };
  TerrainMesh mesh;
  std::string error;
  const bool built = buildTerrainMesh(
      worldBorderOnly(), flat, kGrass, kExtent, baseSettings(), mesh, &error);
  expect(built, "a flat world builds: " + error);
  if (!built)
    return;

  // 4x4 tiles, each a square whose border is sampled every 1000 m: four
  // corners plus three cuts a side is twelve boundary vertices, and a flat
  // interior adds none. Anything materially above that means the error
  // criterion is inserting points it cannot justify.
  const uint64_t triangles = mesh.triangleCount();
  expect(mesh.tiles.size() == 16u, "the world divides into sixteen tiles");
  expect(triangles > 0u && triangles <= 16u * 40u,
      "flat ground converges with no interior refinement (" +
          std::to_string(triangles) + " triangles)");

  double maximumHeight = -1.0e30;
  double minimumHeight = 1.0e30;
  for (const MeshTile &tile : mesh.tiles)
    for (const MeshVertex &vertex : tile.vertices)
    {
      maximumHeight = std::max(maximumHeight, vertex.y);
      minimumHeight = std::min(minimumHeight, vertex.y);
    }
  expect(std::abs(maximumHeight - 120.0) < 1.0e-9 &&
          std::abs(minimumHeight - 120.0) < 1.0e-9,
      "every vertex sits on the flat surface");
}

void testCurvedGroundRefines()
{
  std::cout << "curved ground\n";
  // A dome with 900 m of relief: far more than the five-metre tolerance, so
  // this must refine, and it must refine more than the flat world did.
  const HeightFunction dome = [](const Point2 point)
  {
    const double dx = (point.x - kExtent * 0.5) / (kExtent * 0.5);
    const double dz = (point.y - kExtent * 0.5) / (kExtent * 0.5);
    return 900.0 * std::exp(-(dx * dx + dz * dz) * 2.0);
  };
  TerrainMesh flatMesh;
  TerrainMesh domeMesh;
  std::string error;
  buildTerrainMesh(worldBorderOnly(), [](Point2) { return 120.0; }, kGrass,
      kExtent, baseSettings(), flatMesh, &error);
  const bool built = buildTerrainMesh(
      worldBorderOnly(), dome, kGrass, kExtent, baseSettings(), domeMesh, &error);
  expect(built, "a curved world builds: " + error);
  if (!built)
    return;
  expect(domeMesh.triangleCount() > flatMesh.triangleCount() * 2u,
      "curvature draws the triangles that flat ground did not (" +
          std::to_string(domeMesh.triangleCount()) + " against " +
          std::to_string(flatMesh.triangleCount()) + ")");

  // The stronger claim, and the one worth defending: an error-driven mesh has
  // an analytic triangle count. Linear interpolation of a surface with second
  // derivative f'' over an edge L has error about f'' L^2 / 8, so the tolerance
  // fixes the edge length and the edge length fixes the count. For this dome
  // f'' is about 4H/R^2 at the summit, giving ~830 triangles at a five-metre
  // tolerance. Landing far from that means the criterion is not measuring what
  // it claims to.
  const double secondDerivative = 4.0 * 900.0 / (8000.0 * 8000.0);
  const double predictedEdge =
      std::sqrt(8.0 * baseSettings().verticalToleranceMetres / secondDerivative);
  const double predictedCount =
      (kExtent * kExtent) / (0.433 * predictedEdge * predictedEdge);
  const double ratio =
      static_cast<double>(domeMesh.triangleCount()) / predictedCount;
  expect(ratio > 0.5 && ratio < 2.0,
      "the triangle count matches the analytic prediction within 2x (" +
          std::to_string(domeMesh.triangleCount()) + " against " +
          std::to_string(static_cast<uint64_t>(predictedCount)) + ")");

  // The point of the criterion: measured error under tolerance where it was
  // able to refine.
  double worst = 0.0;
  for (const MeshTile &tile : domeMesh.tiles)
    for (size_t index = 0u; index + 2u < tile.indices.size(); index += 3u)
    {
      const MeshVertex &a = tile.vertices[tile.indices[index]];
      const MeshVertex &b = tile.vertices[tile.indices[index + 1u]];
      const MeshVertex &c = tile.vertices[tile.indices[index + 2u]];
      const Point2 centroid((a.x + b.x + c.x) / 3.0, (a.z + b.z + c.z) / 3.0);
      const double longest = std::max({
          std::hypot(a.x - b.x, a.z - b.z),
          std::hypot(b.x - c.x, b.z - c.z),
          std::hypot(c.x - a.x, c.z - a.z)});
      if (longest <= 100.0)
        continue;
      worst = std::max(worst,
          std::abs(dome(centroid) - (a.y + b.y + c.y) / 3.0));
    }
  expect(worst < 60.0,
      "refinement drove the residual error down (" + std::to_string(worst) + " m)");
}

void testBordersAgree()
{
  std::cout << "border contract\n";
  const HeightFunction ramp = [](const Point2 point)
  { return point.x * 0.05 + std::sin(point.y / 900.0) * 140.0; };
  TerrainMesh mesh;
  std::string error;
  const bool built = buildTerrainMesh(
      worldBorderOnly(), ramp, kGrass, kExtent, baseSettings(), mesh, &error);
  expect(built, "the ramped world builds: " + error);
  if (!built)
    return;

  // A tile's north border must be its northern neighbour's south border, point
  // for point and bit for bit. This is the whole crack-free argument.
  bool agree = true;
  size_t compared = 0u;
  for (uint32_t z = 0u; z + 1u < mesh.tilesPerAxis; ++z)
    for (uint32_t x = 0u; x < mesh.tilesPerAxis; ++x)
    {
      const MeshTile &lower = mesh.tiles[z * mesh.tilesPerAxis + x];
      const MeshTile &upper = mesh.tiles[(z + 1u) * mesh.tilesPerAxis + x];
      agree &= lower.borders[1u].size() == upper.borders[0u].size();
      if (!agree)
        break;
      for (size_t index = 0u; index < lower.borders[1u].size(); ++index)
      {
        agree &= lower.borders[1u][index].x == upper.borders[0u][index].x &&
            lower.borders[1u][index].y == upper.borders[0u][index].y;
        ++compared;
      }
    }
  expect(agree && compared > 0u,
      "north and south borders are bit-identical across every seam (" +
          std::to_string(compared) + " vertices)");

  bool eastWest = true;
  for (uint32_t z = 0u; z < mesh.tilesPerAxis; ++z)
    for (uint32_t x = 0u; x + 1u < mesh.tilesPerAxis; ++x)
    {
      const MeshTile &left = mesh.tiles[z * mesh.tilesPerAxis + x];
      const MeshTile &right = mesh.tiles[z * mesh.tilesPerAxis + x + 1u];
      eastWest &= left.borders[3u].size() == right.borders[2u].size();
      if (!eastWest)
        break;
      for (size_t index = 0u; index < left.borders[3u].size(); ++index)
        eastWest &= left.borders[3u][index].x == right.borders[2u][index].x &&
            left.borders[3u][index].y == right.borders[2u][index].y;
    }
  expect(eastWest, "east and west borders are bit-identical across every seam");
}

void testParallelBuildIsByteIdentical()
{
  std::cout << "determinism under parallelism\n";
  const HeightFunction hills = [](const Point2 point)
  {
    return 300.0 + std::sin(point.x / 1300.0) * 210.0 +
        std::cos(point.y / 1700.0) * 180.0;
  };
  TerrainMeshSettings single = baseSettings();
  single.workerCount = 1u;
  TerrainMeshSettings many = baseSettings();
  many.workerCount = std::max(4u, std::thread::hardware_concurrency());

  TerrainMesh serial;
  TerrainMesh parallel;
  std::string error;
  const bool builtSerial = buildTerrainMesh(
      worldBorderOnly(), hills, kGrass, kExtent, single, serial, &error);
  const bool builtParallel = buildTerrainMesh(
      worldBorderOnly(), hills, kGrass, kExtent, many, parallel, &error);
  expect(builtSerial && builtParallel, "both builds succeed: " + error);
  if (!builtSerial || !builtParallel)
    return;
  expect(serial.signature() == parallel.signature(),
      "one worker and " + std::to_string(many.workerCount) +
          " produce identical bytes");
  expect(serial.triangleCount() == parallel.triangleCount(),
      "and identical triangle counts (" +
          std::to_string(serial.triangleCount()) + ")");

  TerrainMesh repeated;
  buildTerrainMesh(worldBorderOnly(), hills, kGrass, kExtent, single, repeated, &error);
  expect(serial.signature() == repeated.signature(),
      "and a repeated build reproduces the first");
}

void testContourVerticesTakeTheirExactElevation()
{
  std::cout << "contours as constraints\n";
  // A ramp with a contour line laid across it at a height the ramp does not
  // pass through. A vertex on the isoline is on the isoline by definition, so
  // it must take the contour's height and not the field's.
  const HeightFunction ramp = [](const Point2 point)
  { return point.x * 0.02; };
  std::vector<Chain> chains = worldBorderOnly();
  Chain contour{};
  contour.kind = ChainKind::Contour;
  contour.hasElevation = true;
  contour.elevationMetres = 777.0;
  contour.points = {Point2(2000.0, 0.0), Point2(2000.0, kExtent)};
  chains.push_back(contour);

  TerrainMesh mesh;
  std::string error;
  const bool built = buildTerrainMesh(
      chains, ramp, kGrass, kExtent, baseSettings(), mesh, &error);
  expect(built, "a world with a contour builds: " + error);
  if (!built)
    return;

  size_t onLine = 0u;
  bool exact = true;
  for (const MeshTile &tile : mesh.tiles)
    for (const MeshVertex &vertex : tile.vertices)
    {
      if (std::abs(vertex.x - 2000.0) > 1.0e-6)
        continue;
      ++onLine;
      exact &= std::abs(vertex.y - 777.0) < 1.0e-9;
    }
  expect(onLine > 0u, "the contour appears in the mesh (" +
      std::to_string(onLine) + " vertices)");
  expect(exact, "every vertex on the contour carries the contour's elevation");
}

void testEncroachedConstraintsStillInsert()
{
  std::cout << "encroachment\n";
  // A constraint with a point a couple of metres from it, which is well inside
  // the segment's diametral circle. Measured on real map data this is the one
  // arrangement that survives both the Delaunay relaxation and every other
  // mitigation, and it leaves a single constraint permanently uninserted - a
  // hole in a line the map said was there.
  const HeightFunction ramp = [](const Point2 point)
  { return 100.0 + point.x * 0.01; };
  std::vector<Chain> chains = worldBorderOnly();
  Chain line{};
  line.kind = ChainKind::RegionBorder;
  line.points = {Point2(1000.0, 1000.0), Point2(1120.0, 1096.0)};
  chains.push_back(line);
  // The encroaching neighbour, close to the segment's midpoint and off to one
  // side of it.
  Chain speck{};
  speck.kind = ChainKind::RegionBorder;
  speck.points = {Point2(1057.0, 1050.6), Point2(1059.0, 1052.2)};
  chains.push_back(speck);

  TerrainMeshSettings settings = baseSettings();
  settings.chainSpacingMetres = 4000.0;
  TerrainMesh mesh;
  std::string error;
  const bool built = buildTerrainMesh(
      chains, ramp, kGrass, kExtent, settings, mesh, &error);
  expect(built, "an encroached constraint still builds: " + error);
  if (!built)
    return;
  expect(mesh.statistics.constraintCount > 0u &&
          mesh.statistics.constraintsIncluded == mesh.statistics.constraintCount,
      "every constraint is inserted (" +
          std::to_string(mesh.statistics.constraintsIncluded) + "/" +
          std::to_string(mesh.statistics.constraintCount) + ")");
  expect(mesh.statistics.failedTiles == 0u, "no tile is left unbuilt");
}

void testNoDegenerateTriangles()
{
  std::cout << "triangle quality\n";
  const HeightFunction hills = [](const Point2 point)
  { return 200.0 + std::sin(point.x / 800.0) * std::cos(point.y / 950.0) * 260.0; };
  TerrainMesh mesh;
  std::string error;
  const bool built = buildTerrainMesh(
      worldBorderOnly(), hills, kGrass, kExtent, baseSettings(), mesh, &error);
  expect(built, "the hilly world builds: " + error);
  if (!built)
    return;

  uint64_t zeroArea = 0u;
  for (const MeshTile &tile : mesh.tiles)
    for (size_t index = 0u; index + 2u < tile.indices.size(); index += 3u)
    {
      const MeshVertex &a = tile.vertices[tile.indices[index]];
      const MeshVertex &b = tile.vertices[tile.indices[index + 1u]];
      const MeshVertex &c = tile.vertices[tile.indices[index + 2u]];
      const double twice =
          (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
      if (std::abs(twice) < 1.0e-6)
        ++zeroArea;
    }
  expect(zeroArea == 0u,
      "no zero-area triangle survives (" + std::to_string(zeroArea) + ")");
  expect(mesh.statistics.minimumTriangleAngleDegrees > 0.05,
      "the smallest angle stays measurable (" +
          std::to_string(mesh.statistics.minimumTriangleAngleDegrees) + " deg)");

  // Every triangle must wind counter-clockwise seen from above, or half the
  // world renders inside out under back-face culling.
  bool wound = true;
  for (const MeshTile &tile : mesh.tiles)
    for (size_t index = 0u; index + 2u < tile.indices.size(); index += 3u)
    {
      const MeshVertex &a = tile.vertices[tile.indices[index]];
      const MeshVertex &b = tile.vertices[tile.indices[index + 1u]];
      const MeshVertex &c = tile.vertices[tile.indices[index + 2u]];
      wound &= (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z) >= 0.0;
    }
  expect(wound, "every triangle winds the same way");
}

} // namespace

int main()
{
  {
    auto settings = baseSettings();
    settings.poissonSpacingMetres = 600.0;
    settings.poissonSeed = 7;
    settings.toleranceScale = [](Point2 p) { return p.x < 8000.0 ? 0.4 : 2.0; };
    settings.maximumRefinementRounds = 0;
    TerrainMesh one, many;
    std::string error;
    expect(buildTerrainMesh(worldBorderOnly(), [](Point2) { return 100.0; }, kGrass,
        kExtent, settings, one, &error), "layered Poisson mesh builds: " + error);
    settings.workerCount = 4;
    expect(buildTerrainMesh(worldBorderOnly(), [](Point2) { return 100.0; }, kGrass,
        kExtent, settings, many, &error), "parallel Poisson mesh builds: " + error);
    expect(one.signature() == many.signature(), "Poisson layers are independent of worker scheduling");
    size_t detailed = 0, plain = 0;
    for (const auto &tile : one.tiles)
      for (const auto &v : tile.vertices)
        (v.x < 8000.0 ? detailed : plain)++;
    expect(detailed > plain * 2, "additional Poisson layers concentrate on detailed terrain");
  }
  {
    auto settings = baseSettings();
    settings.preserveChainVertices = true;
    settings.maximumRefinementRounds = 0;
    Chain slope{};
    slope.kind = ChainKind::Stroke;
    slope.points = {Point2(1000, 1000), Point2(3000, 3000)};
    slope.elevations = {100.0, 300.0};
    slope.hasElevation = true;
    Chain crossing{};
    crossing.kind = ChainKind::RegionBorder;
    crossing.points = {Point2(1000, 2000), Point2(3000, 2000)};
    auto chains = worldBorderOnly();
    chains.push_back(slope);
    chains.push_back(crossing);
    TerrainMesh mesh;
    std::string error;
    expect(buildTerrainMesh(chains, [](Point2 p) { return p.x * 0.1; }, kGrass,
        kExtent, settings, mesh, &error), "crossing a sloped constraint builds: " + error);
    bool found = false;
    for (const auto &tile : mesh.tiles)
      for (const auto &v : tile.vertices)
        if (std::abs(v.x - 2000) < 1e-5 && std::abs(v.z - 2000) < 1e-5)
          found = std::abs(v.y - 200.0) < 1e-5;
    expect(found, "splitting river/region intersections preserves interpolated elevation");
  }
  {
    auto settings=baseSettings(); settings.workerCount=2;
    TerrainMesh retained,streamed;
    const auto height=[](Point2 p){return 40*std::sin(p.x/700)*std::cos(p.y/900);};
    std::string error;
    expect(buildTerrainMesh(worldBorderOnly(),height,kGrass,kExtent,settings,retained,&error),"streaming reference builds");
    std::vector<MeshTile> delivered(retained.tiles.size());
    settings.retainTiles=false;
    settings.tileSink=[&](MeshTile &tile,std::string &){delivered[size_t(tile.tileZ)*retained.tilesPerAxis+tile.tileX]=tile;return true;};
    expect(buildTerrainMesh(worldBorderOnly(),height,kGrass,kExtent,settings,streamed,&error),"bounded-memory tile stream builds");
    bool released=true;
    for(const auto &tile:streamed.tiles)released &= tile.vertices.empty()&&tile.indices.empty();
    expect(released && streamed.statistics.triangleCount==retained.statistics.triangleCount &&
        streamed.statistics.constraintsIncluded==retained.statistics.constraintsIncluded,"stream releases payloads and retains 64-bit statistics");
    TerrainMesh reconstructed=streamed;reconstructed.tiles=std::move(delivered);
    expect(reconstructed.signature()==retained.signature(),"streamed tiles are byte-identical to retained geometry");
    settings.uniformSubdivisionRounds=1;
    settings.tileSink=[&](MeshTile &tile,std::string &){delivered[size_t(tile.tileZ)*retained.tilesPerAxis+tile.tileX]=tile;return true;};
    delivered.resize(retained.tiles.size());
    expect(buildTerrainMesh(worldBorderOnly(),height,kGrass,kExtent,settings,streamed,&error)&&
        streamed.statistics.triangleCount==4*retained.statistics.triangleCount,"production subdivision emits four conforming children per CDT face");
    bool shared=true;
    for(uint32_t z=0;z<retained.tilesPerAxis;++z)for(uint32_t x=0;x+1<retained.tilesPerAxis;++x)
    {
      const auto &a=delivered[size_t(z)*retained.tilesPerAxis+x].borders[3];
      const auto &b=delivered[size_t(z)*retained.tilesPerAxis+x+1].borders[2];
      shared &= a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](Point2 p,Point2 q){return p.x==q.x&&p.y==q.y;});
    }
    expect(shared,"production subdivision preserves exact adjacent tile border samples");
    settings.maximumTriangleCount=1;
    expect(!buildTerrainMesh(worldBorderOnly(),height,kGrass,kExtent,settings,streamed,&error),"triangle budget failure never returns a truncated successful mesh");
    settings.maximumTriangleCount=0;
    settings.tileSink=[](MeshTile &,std::string &e){e="simulated output failure";return false;};
    expect(!buildTerrainMesh(worldBorderOnly(),height,kGrass,kExtent,settings,streamed,&error)&&error=="simulated output failure","tile writer failure propagates without publishing success");
  }
  testFlatGroundIsNearlyFree();
  testCurvedGroundRefines();
  testBordersAgree();
  testParallelBuildIsByteIdentical();
  testContourVerticesTakeTheirExactElevation();
  testEncroachedConstraintsStillInsert();
  testNoDegenerateTriangles();
  if (failures != 0u)
  {
    std::cout << failures << " terrain mesh test(s) failed\n";
    return 1;
  }
  std::cout << "all terrain mesh tests passed\n";
  return 0;
}
