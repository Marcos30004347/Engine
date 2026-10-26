#pragma once

#include "virtualgeometry/CityVoxelStaticScene.hpp"
#include "voxel/svraster/CityVoxelExactAppearanceLookup.hpp"
#include "voxel/svraster/SparseVoxelTree64Dynamic.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace virtualgeometry {

// CPU reference and Tree64 publication adapter for the opaque LOD0 city cut.
// Encoded geometry and appearance arrays are owned by the caller and must
// outlive this object. Each distinct geometry builds exactly one Tree64 BLAS;
// placement records remain the 32-byte CityVoxelStaticScene records.
class CityVoxelTraceScene {
public:
  enum class Status : uint8_t { Miss, Hit, Unavailable, QueryLimit, Invalid };
  struct Ray {
    std::array<double, 3> origin{};
    std::array<double, 3> direction{};
    double maximumDistance = 1.0e30;
  };
  struct Hit {
    Status status = Status::Invalid;
    double distance = 0.0;
    uint64_t stableId = 0;
    uint32_t placementIndex = UINT32_MAX;
    std::array<int32_t, 3> modelCell{};
    std::array<float, 3> worldNormal{};
    uint32_t rgba = 0;
    uint32_t materialProfile = 0;
    float opacity = 0.0f;
  };
  struct Stats {
    uint32_t uniqueGeometry = 0;
    uint32_t placements = 0;
    uint32_t tlasNodes = 0;
    uint64_t treeNodeBytes = 0;
    uint64_t tlasBytes = 0;
    uint64_t appearanceScratchBytes = 0;
  };

  bool build(const CityVoxelStaticScene &city,
      const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry,
      const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances,
      std::string *error = nullptr);
  // One transaction publishes geometry and appearance readiness together.
  // A missing candidate returns Unavailable rather than a false visible ray.
  bool publishResidency(const std::vector<uint8_t> &geometryReady,
      const std::vector<uint8_t> &appearanceReady, uint64_t revision,
      std::string *error = nullptr);
  Hit trace(const Ray &ray, uint32_t maximumTreeQueries = UINT32_MAX) const;
  // Conservative shadow query: a missing candidate blocks the ray.
  bool shadowOccluded(const Ray &ray) const;

  // Existing Tree64 dynamic scene/build-chain input. Model payloads are
  // exported once; instances use placement-index tokens to recover u64 IDs.
  void exportTree64Sources(
      std::vector<voxel::svraster::SparseVoxelTree64DynamicModelSource> &models,
      std::vector<voxel::svraster::SparseVoxelTree64DynamicInstanceSource> &instances) const;
  uint64_t stableIdForTree64Instance(uint32_t oneBasedToken) const;
  Stats stats() const;

private:
  struct Bounds { std::array<float, 3> minimum{}, maximum{}; };
  struct Node { Bounds bounds; uint32_t first = 0, count = 0, right = 0; };
  struct Model {
    voxel::svraster::SparseVoxelTree64Chunk tree;
    std::array<int32_t, 3> extent{};
    uint64_t geometryId = 0;
  };
  uint32_t buildNode(uint32_t first, uint32_t count);
  Bounds placementBounds(uint32_t placementIndex) const;
  voxel::svraster::SparseVoxelTree64Ray localRay(uint32_t placementIndex,
      const Ray &ray) const;
  std::array<float, 3> worldNormal(uint32_t placementIndex,
      const voxel::svraster::SparseVoxelTree64Ray &local,
      const voxel::svraster::SparseVoxelTree64TraceResult &hit) const;

  const CityVoxelStaticScene *city_ = nullptr;
  const std::vector<voxel::svraster::CityVoxelCompiledGeometry> *geometry_ = nullptr;
  const std::vector<voxel::svraster::CityVoxelCompiledAppearance> *appearances_ = nullptr;
  std::vector<Model> models_;
  std::vector<uint32_t> modelForGeometry_;
  std::vector<voxel::svraster::CityVoxelExactAppearanceLookup> lookup_;
  std::vector<Bounds> placementBounds_;
  std::vector<uint32_t> placementOrder_;
  std::vector<Node> tlas_;
  std::vector<uint8_t> geometryReady_, appearanceReady_;
  uint64_t residencyRevision_ = 0;
};

} // namespace virtualgeometry
