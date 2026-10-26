#pragma once
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace virtualgeometry {
// LOD0 opaque surface input for one shared hardware BLAS. Every primitive
// retains its exact model-local source cell for appearance resolution.
struct CityVoxelHardwareMesh {
  std::vector<float> positions;
  std::vector<uint32_t> indices;
  std::vector<std::array<int32_t, 3>> cells;
  std::vector<uint32_t> triangleAddress; // (cell index << 3) | face, face = -X,+X,-Y,+Y,-Z,+Z.
  uint64_t bytes() const {
    return positions.capacity() * sizeof(float) + indices.capacity() * sizeof(uint32_t) +
           cells.capacity() * sizeof(cells[0]) + triangleAddress.capacity() * sizeof(uint32_t);
  }
};
bool buildCityVoxelHardwareMesh(const voxel::svraster::CityVoxelCompiledGeometry &,
                                CityVoxelHardwareMesh &, std::string *error = nullptr);
// Partition consecutive complete voxel faces into independently addressable
// BLAS inputs. Group triangles keep exact source-cell/face identities.
bool splitCityVoxelHardwareMesh(const CityVoxelHardwareMesh &, uint32_t maximumTriangles,
    std::vector<CityVoxelHardwareMesh> &, std::string *error = nullptr);
bool serializeCityVoxelHardwareMesh(uint64_t geometryId, uint64_t geometryHash,
    const CityVoxelHardwareMesh &, std::vector<uint8_t> &, std::string *error = nullptr);
bool deserializeCityVoxelHardwareMesh(const uint8_t *, size_t, uint64_t geometryId,
    uint64_t geometryHash, CityVoxelHardwareMesh &, std::string *error = nullptr);
} // namespace virtualgeometry
