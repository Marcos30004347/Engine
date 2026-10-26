#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster {

// A city's ground as compact vector data, evaluated per terrain column on
// demand. Terrain pages ask `sample` for every column they build; nothing is
// rasterised ahead of time, so the whole city costs a few megabytes however
// fine the terrain voxel is.
//
// All coordinates are integer millimetres in the city site's local x/z; the
// terrain caller converts world positions into it. Only the two grids carry an
// origin; prism rings are absolute site-local millimetres.
// The ground is 2.5D: graded relief from a coarse grid, then the highest
// paving prism (road, sidewalk, court, platform, stair band) over it.
struct CityGroundPrism {
  // rings[0] is the outer boundary, any further rings are holes.
  std::vector<std::vector<std::array<int32_t, 2>>> rings;
  int32_t bottomMillimetres = 0;
  int32_t topMillimetres = 0;
  bool terrainFollowing = false; // Heights are offsets above the graded ground.
  uint8_t topMaterial = 0;  // PolygonTerrainMaterial value.
  uint8_t sideMaterial = 0;
};

struct CityGroundOverlayData {
  // World x/z of site-local (0,0); the site frame is a pure translation.
  std::array<int64_t, 2> siteOriginMillimetres{};
  std::array<int64_t, 2> originMillimetres{}; // Grid sample (0,0) in site-local x/z.
  // Graded ground, bilinear between samples. `groundWeight` is the share of
  // the city ground against the world terrain under it (255 = all city), so
  // the city edge blends into the surrounding map instead of stepping.
  uint32_t groundSpacingMillimetres = 500;
  uint32_t groundWidth = 0, groundHeight = 0;
  std::vector<int32_t> groundMillimetres;
  std::vector<uint8_t> groundWeight;
  // Unpaved ground material (grass, earth, gravel); 255 means "keep the
  // world map's own material".
  uint32_t paintSpacingMillimetres = 250;
  uint32_t paintWidth = 0, paintHeight = 0;
  std::vector<uint8_t> paint;
  std::vector<CityGroundPrism> prisms;
};

struct CityGroundColumn {
  double heightMetres = 0.0;
  double weight = 0.0;          // 0 outside the city, 1 fully inside.
  uint8_t material = 255;       // 255: no city material here.
  bool paved = false;           // A prism, not the graded ground, is on top.
};

class CityGroundOverlay {
public:
  bool load(CityGroundOverlayData data, std::string *error = nullptr);
  // Site-local metres. A column outside the overlay returns weight 0.
  CityGroundColumn sample(double x, double z) const;
  CityGroundColumn sampleWorld(double worldX, double worldZ) const {
    return sample(worldX - double(data_.siteOriginMillimetres[0]) * 0.001,
                  worldZ - double(data_.siteOriginMillimetres[1]) * 0.001);
  }
  const CityGroundOverlayData &data() const { return data_; }
  bool empty() const { return data_.groundWidth == 0; }
  // The overlay as one word buffer for city_ground_overlay.wgsl, which
  // evaluates `sample` per terrain column on the GPU. Layout in the shader.
  // `heightOffsetMetres` is the renderer's map-minus-city datum.
  void packGPU(double heightOffsetMetres, std::vector<uint32_t> &words) const;

private:
  double ground(double x, double z, double &weight) const;
  CityGroundOverlayData data_;
  // Uniform 8 m bins over the prism bounds, each listing prisms touching it.
  int64_t binMinimumX_ = 0, binMinimumZ_ = 0;
  uint32_t binsX_ = 0, binsZ_ = 0;
  std::vector<uint32_t> binFirst_, binPrisms_;
  std::vector<std::array<int32_t, 4>> prismBounds_; // minX, minZ, maxX, maxZ.
};

// The overlay's top material on a regular grid, for consumers that colour
// terrain by world position (the VG terrain route carries no per-voxel
// material). 255 = not city ground. Rows are z, columns x, in site-local
// metres from `originMetres`.
struct CityGroundMaterialGrid {
  std::array<double, 2> originMetres{};
  double texelMetres = 0.5;
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> materials;
};
bool rasterizeCityGroundMaterials(const CityGroundOverlay &overlay, double texelMetres,
                                  uint32_t workers, CityGroundMaterialGrid &grid,
                                  std::string *error = nullptr);

// One LZ4-compressed little-endian blob, checksummed. Writing goes through a
// temporary file and an atomic rename.
bool writeCityGroundOverlay(const std::string &path, const CityGroundOverlayData &,
                            std::string *error = nullptr);
// A disabled overlay for the GPU binding when there is no city.
inline std::vector<uint32_t> disabledCityGroundGPU() { return std::vector<uint32_t>(32u, 0u); }

bool readCityGroundOverlay(const std::string &path, CityGroundOverlayData &,
                           std::string *error = nullptr);

} // namespace voxel::svraster
