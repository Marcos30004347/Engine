#include "procedural/PlanetCubeMapping.hpp"
#include "procedural/PlanetPackage.hpp"
#include "procedural/PolygonTerrain.hpp"
#include "voxel/svraster/PlanetDestination.hpp"
#include "voxel/svraster/PlanetNavigation.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

bool near(const double a, const double b, const double epsilon = 1.0e-9)
{
  return std::abs(a - b) <= epsilon;
}

double dot(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

uint32_t raster(const procedural::PlanetRasterChannel channel)
{
  return static_cast<uint32_t>(channel);
}

std::vector<uint8_t> uniformR16(const uint16_t value)
{
  std::vector<uint8_t> bytes(8u);
  for (size_t texel = 0u; texel < 4u; ++texel)
  {
    bytes[texel * 2u] = static_cast<uint8_t>(value & 0xffu);
    bytes[texel * 2u + 1u] = static_cast<uint8_t>(value >> 8u);
  }
  return bytes;
}

void testCursorRayAndSphereHit()
{
  namespace svr = voxel::svraster;
  std::cout << "overview cursor ray\n";
  const svr::PlanetTangentFrame frame = svr::planetTangentFrame({0.0, 0.0, 1.0});
  svr::PlanetOverviewRay ray{};
  double distance = 0.0;
  procedural::SpherePoint position{};
  check(
      svr::planetOverviewCursorRay(400.0, 300.0, 800u, 600u, std::acos(-1.0) / 3.0, frame, 2.2, ray) && svr::planetRaySphereIntersection(ray, 1.0, distance, &position) && near(distance, 1.2) && near(position.x, 0.0) &&
          near(position.y, 0.0) && near(position.z, 1.0),
      "viewport centre unprojects to the sub-camera point on the sphere");

  procedural::SpherePoint direction{};
  check(
      svr::planetOverviewPickDirection(650.0, 300.0, 800u, 600u, std::acos(-1.0) / 3.0, frame, 2.2, direction) && direction.x > 0.0 && direction.z > 0.0, "an off-centre visible point intersects the expected hemisphere");
  check(!svr::planetOverviewPickDirection(700.0, 300.0, 800u, 600u, std::acos(-1.0) / 3.0, frame, 2.2, direction), "a cursor beyond the apparent limb does not select the planet");

  const procedural::SpherePoint north{0.0, 0.8, 0.6};
  std::array<double, 2> northPixel{};
  procedural::SpherePoint northPick{};
  check(
      svr::planetOverviewProjectDirection(
          north, 800u, 600u, std::acos(-1.0) / 3.0, frame, 3.0,
          northPixel) &&
          northPixel[1] < 300.0 &&
          svr::planetOverviewPickDirection(
              northPixel[0], northPixel[1], 800u, 600u,
              std::acos(-1.0) / 3.0, frame, 3.0, northPick) &&
          dot(north, northPick) > 0.999999,
      "a displayed northern point projects above centre and picks the same hemisphere");
}

void testCubeSeamsAndPoles()
{
  std::cout << "destination cube mapping\n";
  constexpr double epsilon = 1.0e-9;
  procedural::PlanetTileAddress xSide{};
  procedural::PlanetTileAddress zSide{};
  procedural::PlanetTileAddress northPole{};
  procedural::PlanetTileAddress southPole{};
  check(
      procedural::planetCubeTile({1.0 + epsilon, 0.0, 1.0}, 12u, xSide) && procedural::planetCubeTile({1.0, 0.0, 1.0 + epsilon}, 12u, zSide) && xSide.face == 0u && zSide.face == 4u &&
          procedural::validPlanetTileAddress(xSide) && procedural::validPlanetTileAddress(zSide),
      "directions on either side of a cube seam choose adjacent valid faces");
  check(
      procedural::planetCubeTile({epsilon, 1.0, -epsilon}, 12u, northPole) && procedural::planetCubeTile({-epsilon, -1.0, epsilon}, 12u, southPole) && northPole.face == 2u && southPole.face == 3u,
      "near-pole directions map deterministically to the polar faces");

  bool axesMatchFaces = true;
  const std::array<procedural::SpherePoint, 6> axes{{{1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}}};
  for (uint8_t face = 0u; face < axes.size(); ++face)
  {
    procedural::PlanetTileAddress tile{};
    axesMatchFaces &= procedural::planetCubeTile(axes[face], 8u, tile) && tile.face == face;
  }
  check(axesMatchFaces, "all six face centres preserve their package face IDs");
}

void testClickDragDisambiguation()
{
  namespace svr = voxel::svraster;
  std::cout << "click and orbit drag\n";
  svr::PlanetNavigationPose pose{};
  pose.direction = {0.0, 0.0, 1.0};
  svr::PlanetSurfaceCameraPose camera{};
  svr::PlanetNavigationController controller;
  controller.settings().ascentDurationSeconds = 0.0;
  controller.initialize(pose, camera, 6371000.0);
  controller.toggleOverview(pose, camera, 1.0);

  controller.beginOrbitPointer(100.0, 100.0);
  controller.updateOrbitPointer(103.0, 102.0);
  const auto click = controller.endOrbitPointer(103.0, 102.0);
  check(click && near((*click)[0], 103.0) && near((*click)[1], 102.0), "movement inside the threshold resolves as a click");

  const procedural::SpherePoint before = controller.pose().direction;
  controller.beginOrbitPointer(100.0, 100.0);
  controller.updateOrbitPointer(110.0, 100.0);
  const auto dragged = controller.endOrbitPointer(110.0, 100.0);
  check(!dragged && dot(before, controller.pose().direction) < 0.99999, "movement outside the threshold rotates the globe and suppresses selection");
}

void testPackageDestinationSampling()
{
  namespace svr = voxel::svraster;
  std::cout << "package destination sampling\n";
  const std::filesystem::path directory = std::filesystem::temp_directory_path() / "engine-sparse-voxel-planet-picking-tests";
  const std::filesystem::path path = directory / "selection.planetpkg";
  std::error_code filesystemError;
  std::filesystem::remove_all(directory, filesystemError);
  std::filesystem::create_directories(directory, filesystemError);

  procedural::PlanetPackageMetadata metadata{};
  metadata.planetRadiusMetres = 6371000.0;
  metadata.tileSizePixels = 2u;
  metadata.gutterPixels = 0u;
  metadata.maximumTileLevel = 0u;
  const std::vector<procedural::PlanetPackageChannel> channels{
    {procedural::PlanetPackageDomain::Raster, raster(procedural::PlanetRasterChannel::Height), procedural::PlanetPackageFormat::R16UInt, procedural::PlanetPackageMipPolicy::Average, 0u, 2.0, -10.0, "height"},
    {procedural::PlanetPackageDomain::Raster, raster(procedural::PlanetRasterChannel::Material), procedural::PlanetPackageFormat::R8UInt, procedural::PlanetPackageMipPolicy::Mode, 0u, 1.0, 0.0, "material"},
  };
  std::vector<procedural::PlanetPackageBlockInput> blocks;
  const std::array<uint8_t, 6> faceMaterials{{
      static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Ocean),
      static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Lake),
      static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Grass),
      static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Ocean),
      static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Rock),
      static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Snow),
  }};
  for (uint8_t face = 0u; face < 6u; ++face)
  {
    const procedural::PlanetTileAddress root{face, 0u, 0u, 0u};
    blocks.push_back({procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Height), root), uniformR16(static_cast<uint16_t>(100u + face))});
    blocks.push_back({procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Material), root), std::vector<uint8_t>(4u, faceMaterials[face])});
  }

  std::string error;
  check(!filesystemError && procedural::writePlanetPackage(path, metadata, channels, blocks, {}, nullptr, &error), "a six-face raster fixture is written");
  procedural::PlanetPackageReader reader;
  check(reader.open(path, &error), "destination fixture package opens");
  svr::PlanetDestinationSampler sampler(reader);
  const std::array<procedural::SpherePoint, 6> axes{{{1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}}};
  bool samplesMatch = true;
  for (uint8_t face = 0u; face < axes.size(); ++face)
  {
    svr::PlanetDestinationSelection selection{};
    samplesMatch &= sampler.sample(axes[face], 7u, selection, &error) && selection.tile.face == face && selection.tile.level == 7u && selection.heightSourceTile == procedural::PlanetTileAddress{face, 0u, 0u, 0u} &&
                    selection.materialSourceTile == procedural::PlanetTileAddress{face, 0u, 0u, 0u} && near(selection.elevationMetres, (100.0 + face) * 2.0 - 10.0) && selection.material == faceMaterials[face];
  }
  check(samplesMatch, "selection maps to the requested fine tile and samples cached root ancestors");

  const svr::PlanetTangentFrame frame =
      svr::planetTangentFrame({0.0, 0.0, 1.0});
  const procedural::SpherePoint displayedLand{0.0, 0.8, 0.6};
  std::array<double, 2> landPixel{};
  procedural::SpherePoint pickedLand{};
  svr::PlanetDestinationSelection landSelection{};
  check(
      svr::planetOverviewProjectDirection(
          displayedLand, 800u, 600u, std::acos(-1.0) / 3.0, frame, 3.0,
          landPixel) &&
          svr::planetOverviewPickDirection(
              landPixel[0], landPixel[1], 800u, 600u,
              std::acos(-1.0) / 3.0, frame, 3.0, pickedLand) &&
          sampler.sample(pickedLand, 7u, landSelection, &error) &&
          landSelection.tile.face ==
              static_cast<uint8_t>(procedural::PlanetCubeFace::PositiveY) &&
          landSelection.material ==
              static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Grass),
      "clicking displayed northern land samples land instead of mirrored southern ocean");

  svr::PlanetDestinationSelection repeated{};
  check(
      sampler.sample(axes[5], 7u, repeated, &error) && repeated.material == 5u && std::string(svr::planetSurfaceMaterialName(repeated.material)) == "Snow",
      "repeated selection reuses decoded blocks and exposes a material name");

  reader.close();
  std::filesystem::remove_all(directory, filesystemError);
}

} // namespace

int main()
{
  std::cout << "sparse voxel planet picking\n";
  testCursorRayAndSphereHit();
  testCubeSeamsAndPoles();
  testClickDragDisambiguation();
  testPackageDestinationSampling();
  return failures == 0 ? 0 : 1;
}
