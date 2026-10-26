#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/PlanetOverviewMesh.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster::rendering
{

// Resident, immutable overview textures shared by CBT command generation and
// procedural drawing. CPU-built per-face mips keep material, displacement and
// min/max error data on the same deterministic cube-atlas footprint.
class SparseVoxelPlanetSurfaceAtlas final
{
public:
  struct Resources
  {
    ::rendering::Texture albedoRoughness;
    ::rendering::Texture terrainFeatures;
    ::rendering::Texture height;
    ::rendering::Texture oceanFloor;
    ::rendering::Texture heightMinMax;
    uint32_t faceResolution = 0u;
    uint32_t mipCount = 1u;
    double heightMinimumMetres = 0.0;
    double heightRangeMetres = 0.0;
    double oceanFloorMinimumMetres = 0.0;
    double oceanFloorRangeMetres = 0.0;

    bool valid() const
    {
      return faceResolution != 0u && albedoRoughness.isValid() &&
          terrainFeatures.isValid() && height.isValid() &&
          oceanFloor.isValid() && heightMinMax.isValid();
    }
  };

  SparseVoxelPlanetSurfaceAtlas(
      ::rendering::RenderGraph &graph,
      const PlanetOverviewMesh::SurfaceMaps &maps,
      std::string name = "SparseVoxelPlanetSurfaceAtlas")
      : graph_(&graph)
  {
    const bool available = maps.valid();
    const uint32_t width = available ? maps.width : 1u;
    const uint32_t height = available ? maps.height : 1u;
    const uint32_t mipCount = available ? maps.mipCount() : 1u;
    const auto sampledTexture = [&](const std::string &suffix,
                                    const ::rendering::Format format)
    {
      return graph_->createTexture({
        .name = name + suffix,
        .format = format,
        .height = height,
        .width = width,
        .memoryProperties =
            ::rendering::BufferUsage::BufferUsage_None,
        .mipLevels = mipCount,
        .usage = ::rendering::ImageUsage::ImageUsage_Sampled |
            ::rendering::ImageUsage::ImageUsage_TransferDst,
      });
    };
    resources_.albedoRoughness = sampledTexture(
        ".AlbedoRoughness", ::rendering::Format::Format_RGBA8UnormSrgb);
    resources_.terrainFeatures = sampledTexture(
        ".TerrainFeatures", ::rendering::Format::Format_RGBA8Unorm);
    resources_.height = sampledTexture(
        ".Height", ::rendering::Format::Format_R16Float);
    resources_.oceanFloor = sampledTexture(
        ".OceanFloor", ::rendering::Format::Format_R16Float);
    resources_.heightMinMax = sampledTexture(
        ".HeightMinMax", ::rendering::Format::Format_RG16Float);

    if (available)
    {
      resources_.faceResolution = maps.faceResolution;
      resources_.mipCount = maps.mipCount();
      resources_.heightMinimumMetres = maps.detailMinimumMetres;
      resources_.heightRangeMetres = maps.detailRangeMetres;
      resources_.oceanFloorMinimumMetres =
          maps.oceanFloorMinimumMetres;
      resources_.oceanFloorRangeMetres = maps.oceanFloorRangeMetres;
      upload(0u, maps.albedoRoughnessRGBA8,
          maps.terrainFeaturesRGBA8, maps.heightR16Float,
          maps.oceanFloorR16Float, maps.heightMinMaxRG16Float);
      for (uint32_t level = 1u; level < resources_.mipCount; ++level)
      {
        const auto &mip = maps.mipLevels[level - 1u];
        upload(level, mip.albedoRoughnessRGBA8,
            mip.terrainFeaturesRGBA8, mip.heightR16Float,
            mip.oceanFloorR16Float, mip.heightMinMaxRG16Float);
      }
    }
    else
    {
      const std::array<uint8_t, 4> white{255u, 255u, 255u, 255u};
      const std::array<uint8_t, 4> zeroFeatures{};
      const std::array<uint16_t, 1> zero{};
      const std::array<uint16_t, 2> zeroRange{};
      graph_->textureWrite(resources_.albedoRoughness, white.size(),
          const_cast<uint8_t *>(white.data()));
      graph_->textureWrite(resources_.terrainFeatures, zeroFeatures.size(),
          const_cast<uint8_t *>(zeroFeatures.data()));
      graph_->textureWrite(resources_.height, sizeof(zero),
          const_cast<uint16_t *>(zero.data()));
      graph_->textureWrite(resources_.oceanFloor, sizeof(zero),
          const_cast<uint16_t *>(zero.data()));
      graph_->textureWrite(resources_.heightMinMax, sizeof(zeroRange),
          const_cast<uint16_t *>(zeroRange.data()));
    }
  }

  ~SparseVoxelPlanetSurfaceAtlas()
  {
    graph_->deleteTexture(resources_.heightMinMax);
    graph_->deleteTexture(resources_.oceanFloor);
    graph_->deleteTexture(resources_.height);
    graph_->deleteTexture(resources_.terrainFeatures);
    graph_->deleteTexture(resources_.albedoRoughness);
  }

  SparseVoxelPlanetSurfaceAtlas(const SparseVoxelPlanetSurfaceAtlas &) = delete;
  SparseVoxelPlanetSurfaceAtlas &operator=(
      const SparseVoxelPlanetSurfaceAtlas &) = delete;

  const Resources &resources() const
  {
    return resources_;
  }

private:
  template <typename T>
  void write(
      const ::rendering::Texture texture,
      const uint32_t mipLevel,
      const std::vector<T> &values)
  {
    graph_->textureWrite(texture, values.size() * sizeof(T),
        const_cast<T *>(values.data()),
        ::rendering::ResourceLayout::UNDEFINED, mipLevel);
  }

  void upload(
      const uint32_t mipLevel,
      const std::vector<uint8_t> &albedo,
      const std::vector<uint8_t> &features,
      const std::vector<uint16_t> &height,
      const std::vector<uint16_t> &oceanFloor,
      const std::vector<uint16_t> &heightMinMax)
  {
    write(resources_.albedoRoughness, mipLevel, albedo);
    write(resources_.terrainFeatures, mipLevel, features);
    write(resources_.height, mipLevel, height);
    write(resources_.oceanFloor, mipLevel, oceanFloor);
    write(resources_.heightMinMax, mipLevel, heightMinMax);
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  Resources resources_{};
};

} // namespace voxel::svraster::rendering
