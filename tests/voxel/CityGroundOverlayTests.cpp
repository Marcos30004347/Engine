#include "voxel/svraster/CityGroundOverlay.hpp"

#undef NDEBUG
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace voxel::svraster;

int main() {
  CityGroundOverlayData data;
  data.originMillimetres = {-1000, -1000};
  data.siteOriginMillimetres = {5000000, -2000000};
  data.groundSpacingMillimetres = 1000;
  data.groundWidth = data.groundHeight = 21; // -1 m .. 19 m.
  for (uint32_t z = 0; z < 21; ++z)
    for (uint32_t x = 0; x < 21; ++x) {
      data.groundMillimetres.push_back(int32_t(x) * 100); // 10 cm rise per metre.
      data.groundWeight.push_back(x == 0 ? 0 : 255);       // Blend in over the first metre.
    }
  data.paintSpacingMillimetres = 500;
  data.paintWidth = data.paintHeight = 40;
  data.paint.assign(1600, 3); // Grass.
  // A road at 1.2 m with a hole, and a terrain-following sidewalk 15 cm above
  // the ground that overlaps it: the higher top wins at each column.
  CityGroundPrism road;
  road.rings = {{{2000, 2000}, {12000, 2000}, {12000, 6000}, {2000, 6000}},
                {{4000, 3000}, {5000, 3000}, {5000, 4000}, {4000, 4000}}};
  road.bottomMillimetres = 0; road.topMillimetres = 1200;
  road.topMaterial = 21; road.sideMaterial = 22;
  CityGroundPrism sidewalk;
  sidewalk.rings = {{{10000, 2000}, {16000, 2000}, {16000, 4000}, {10000, 4000}}};
  sidewalk.terrainFollowing = true; sidewalk.topMillimetres = 150;
  sidewalk.topMaterial = 22; sidewalk.sideMaterial = 22;
  data.prisms = {road, sidewalk};

  CityGroundOverlay overlay;
  std::string error;
  assert(overlay.load(data, &error));
  auto c = overlay.sample(-5.0, 0.0);
  assert(c.weight == 0.0);                                   // Outside the grid.
  c = overlay.sample(-0.5, 0.0);
  assert(std::abs(c.weight - 0.5) < 1e-9);                   // Edge blend.
  c = overlay.sample(0.5, 0.5);
  assert(std::abs(c.heightMetres - 0.15) < 1e-9 && c.material == 3 && !c.paved);
  c = overlay.sample(3.0, 5.0);
  assert(c.paved && c.material == 21 && std::abs(c.heightMetres - 1.2) < 1e-9);
  c = overlay.sample(4.5, 3.5);                              // Inside the hole.
  assert(!c.paved && c.material == 3);
  c = overlay.sample(11.0, 3.0);                             // Ground 1.2 + 0.15 beats 1.2.
  assert(c.paved && c.material == 22 && std::abs(c.heightMetres - 1.35) < 1e-9);
  c = overlay.sample(11.0, 5.0);                             // Only the road.
  assert(c.paved && c.material == 21);
  c = overlay.sampleWorld(5011.0, -1995.0);                  // Same column, world frame.
  assert(c.paved && c.material == 21);

  const auto path = (std::filesystem::temp_directory_path() / "city-ground-overlay-test.cgo").string();
  assert(writeCityGroundOverlay(path, data, &error));
  CityGroundOverlayData loaded;
  assert(readCityGroundOverlay(path, loaded, &error));
  CityGroundOverlay reopened;
  assert(reopened.load(loaded, &error));
  for (double x = -1.0; x < 19.0; x += 0.37)
    for (double z = -1.0; z < 19.0; z += 0.41) {
      const auto a = overlay.sample(x, z), b = reopened.sample(x, z);
      assert(a.heightMetres == b.heightMetres && a.material == b.material &&
             a.paved == b.paved && a.weight == b.weight);
    }
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(-3, std::ios::end);
    f.put(char(0x5a));
  }
  assert(!readCityGroundOverlay(path, loaded, &error));
  std::filesystem::remove(path);
  std::cout << "City ground overlay passed (" << std::filesystem::path(path).filename() << ")\n";
}
