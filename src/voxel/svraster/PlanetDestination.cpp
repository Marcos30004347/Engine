#include "voxel/svraster/PlanetDestination.hpp"

#include "procedural/PolygonTerrain.hpp"

#include <algorithm>
#include <cmath>

namespace voxel::svraster
{
namespace
{

uint16_t readU16(const uint8_t *bytes)
{
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8u;
}

double rawHeight(const std::vector<uint8_t> &bytes, const size_t texel, const procedural::PlanetPackageFormat format)
{
  const uint16_t bits = readU16(bytes.data() + texel * 2u);
  if (format == procedural::PlanetPackageFormat::R16SInt)
    return static_cast<double>(static_cast<int16_t>(bits));
  return static_cast<double>(bits);
}

bool rasterCoordinates(const procedural::SpherePoint &direction, const procedural::PlanetPackageBlockRecord &record, double &u, double &v)
{
  procedural::PlanetTileAddress sampled{};
  return procedural::planetCubeTile(direction, record.key.tile.level, sampled, &u, &v);
}

} // namespace

const char *planetSurfaceMaterialName(const uint8_t material)
{
  using Material = procedural::PolygonTerrainMaterial;
  switch (static_cast<Material>(material))
  {
  case Material::Ocean:
    return "Ocean";
  case Material::Lake:
    return "Lake";
  case Material::Beach:
    return "Beach";
  case Material::Grass:
    return "Grass";
  case Material::Rock:
    return "Rock";
  case Material::Snow:
    return "Snow";
  case Material::River:
    return "River";
  case Material::Marsh:
    return "Marsh";
  case Material::Ice:
    return "Ice";
  case Material::Tundra:
    return "Tundra";
  case Material::Bare:
    return "Bare";
  case Material::Scorched:
    return "Scorched";
  case Material::Taiga:
    return "Taiga";
  case Material::Shrubland:
    return "Shrubland";
  case Material::TemperateDesert:
    return "Temperate desert";
  case Material::TemperateRainForest:
    return "Temperate rain forest";
  case Material::TemperateDeciduousForest:
    return "Temperate deciduous forest";
  case Material::Grassland:
    return "Grassland";
  case Material::TropicalRainForest:
    return "Tropical rain forest";
  case Material::TropicalSeasonalForest:
    return "Tropical seasonal forest";
  case Material::SubtropicalDesert:
    return "Subtropical desert";
  }
  return "Unknown";
}

bool PlanetDestinationSampler::block(const procedural::PlanetPackageBlockRecord &record, const std::vector<uint8_t> *&bytes, std::string *error)
{
  auto [entry, inserted] = blockCache_.try_emplace(record.key);
  if (inserted && !package_->readBlock(record, entry->second, error))
  {
    blockCache_.erase(entry);
    bytes = nullptr;
    return false;
  }
  bytes = &entry->second;
  return true;
}

bool PlanetDestinationSampler::sample(const procedural::SpherePoint &direction, const uint8_t targetLevel, PlanetDestinationSelection &selection, std::string *error)
{
  selection = {};
  if (error != nullptr)
    error->clear();
  if (package_ == nullptr || !package_->isOpen() || targetLevel > 31u)
  {
    if (error != nullptr)
      *error = "Destination sampling needs an open planet package";
    return false;
  }
  const double squaredLength = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
  if (!(squaredLength > 1.0e-24) || !std::isfinite(squaredLength))
  {
    if (error != nullptr)
      *error = "Destination direction is invalid";
    return false;
  }
  const double inverseLength = 1.0 / std::sqrt(squaredLength);
  selection.direction = {direction.x * inverseLength, direction.y * inverseLength, direction.z * inverseLength};
  if (!procedural::planetCubeTile(selection.direction, targetLevel, selection.tile))
  {
    if (error != nullptr)
      *error = "Destination direction could not be mapped to a cube tile";
    return false;
  }

  const uint32_t heightChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Height);
  const uint32_t materialChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Material);
  const auto *heightDescriptor = package_->findChannel(procedural::PlanetPackageDomain::Raster, heightChannel);
  const auto *materialDescriptor = package_->findChannel(procedural::PlanetPackageDomain::Raster, materialChannel);
  const auto *heightRecord = package_->findBestRasterBlock(heightChannel, selection.tile);
  const auto *materialRecord = package_->findBestRasterBlock(materialChannel, selection.tile);
  if (heightDescriptor == nullptr || materialDescriptor == nullptr || heightRecord == nullptr || materialRecord == nullptr ||
      (heightDescriptor->format != procedural::PlanetPackageFormat::R16UInt && heightDescriptor->format != procedural::PlanetPackageFormat::R16SInt) ||
      materialDescriptor->format != procedural::PlanetPackageFormat::R8UInt)
  {
    if (error != nullptr)
      *error = "Planet destination needs height and material raster channels";
    return false;
  }

  const auto &metadata = package_->metadata();
  const uint32_t storedSide = metadata.tileSizePixels + 2u * metadata.gutterPixels;
  if (metadata.tileSizePixels == 0u || storedSide == 0u)
  {
    if (error != nullptr)
      *error = "Planet raster dimensions are invalid";
    return false;
  }
  const size_t texelCount = static_cast<size_t>(storedSide) * storedSide;
  const std::vector<uint8_t> *heightBytes = nullptr;
  const std::vector<uint8_t> *materialBytes = nullptr;
  if (!block(*heightRecord, heightBytes, error) || !block(*materialRecord, materialBytes, error))
    return false;
  if (heightBytes->size() != texelCount * 2u || materialBytes->size() != texelCount)
  {
    if (error != nullptr)
      *error = "Planet destination raster block size is invalid";
    return false;
  }

  double heightU = 0.0;
  double heightV = 0.0;
  double materialU = 0.0;
  double materialV = 0.0;
  if (!rasterCoordinates(selection.direction, *heightRecord, heightU, heightV) || !rasterCoordinates(selection.direction, *materialRecord, materialU, materialV))
  {
    if (error != nullptr)
      *error = "Planet destination raster coordinates are invalid";
    return false;
  }

  const auto bilinearCoordinate = [&](const double value)
  {
    return std::clamp(static_cast<double>(metadata.gutterPixels) + std::clamp(value, 0.0, 1.0) * metadata.tileSizePixels - 0.5, 0.0, static_cast<double>(storedSide - 1u));
  };
  const double x = bilinearCoordinate(heightU);
  const double y = bilinearCoordinate(heightV);
  const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
  const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
  const uint32_t x1 = std::min(x0 + 1u, storedSide - 1u);
  const uint32_t y1 = std::min(y0 + 1u, storedSide - 1u);
  const double tx = x - x0;
  const double ty = y - y0;
  const auto raw = [&](const uint32_t px, const uint32_t py)
  {
    return rawHeight(*heightBytes, static_cast<size_t>(py) * storedSide + px, heightDescriptor->format);
  };
  const double top = raw(x0, y0) * (1.0 - tx) + raw(x1, y0) * tx;
  const double bottom = raw(x0, y1) * (1.0 - tx) + raw(x1, y1) * tx;
  selection.elevationMetres = (top * (1.0 - ty) + bottom * ty) * heightDescriptor->scale + heightDescriptor->bias;

  const uint32_t materialX = std::min(
      static_cast<uint32_t>(metadata.gutterPixels) + static_cast<uint32_t>(std::floor(std::clamp(materialU, 0.0, 1.0) * metadata.tileSizePixels)),
      static_cast<uint32_t>(metadata.gutterPixels) + metadata.tileSizePixels - 1u);
  const uint32_t materialY = std::min(
      static_cast<uint32_t>(metadata.gutterPixels) + static_cast<uint32_t>(std::floor(std::clamp(materialV, 0.0, 1.0) * metadata.tileSizePixels)),
      static_cast<uint32_t>(metadata.gutterPixels) + metadata.tileSizePixels - 1u);
  selection.material = (*materialBytes)[static_cast<size_t>(materialY) * storedSide + materialX];
  selection.heightSourceTile = heightRecord->key.tile;
  selection.materialSourceTile = materialRecord->key.tile;
  return true;
}

} // namespace voxel::svraster
