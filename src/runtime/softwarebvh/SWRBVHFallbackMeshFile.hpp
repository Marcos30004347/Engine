#pragma once

#include <string>
#include <vector>

#include "softwarebvh/CWBVHBuilder.hpp"
#include "softwarebvh/RBVHBuilder.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"

namespace softwarebvh
{

constexpr uint32_t SWRBVH_FALLBACK_MESH_MAGIC = 0x48564252u; // RBVH
constexpr uint32_t SWRBVH_FALLBACK_MESH_VERSION = 15u;
constexpr uint32_t SWRBVH_FALLBACK_MESH_VERSION_COLLECTION = 16u;
constexpr uint32_t SWRBVH_FALLBACK_MESH_ENDIAN_LITTLE = 0x12345678u;
constexpr uint32_t SWRBVH_FALLBACK_MESH_LOD_COUNT = 4u;

struct SWRBVHFallbackMeshData
{
  virtualgeometry::FallbackMeshData fallbackMesh;
  CWBVHBuilder::BuildResult triangleBuild;
  RBVHBuildResult rbvhBuild;
  std::vector<RBVHBuildResult> rbvhBuilds;
};

class SWRBVHFallbackMeshFile
{
public:
  static SWRBVHFallbackMeshData build(const virtualgeometry::FallbackMeshData &fallbackMesh);
  static SWRBVHFallbackMeshData build(const virtualgeometry::FallbackMeshData &fallbackMesh, const RBVHBuildOptions &options);
  static SWRBVHFallbackMeshData build(const virtualgeometry::FallbackMeshData &fallbackMesh, const virtualgeometry::FallbackMeshData &rbvhSourceMesh, const RBVHBuildOptions &options);
  static bool save(const std::string &path, const SWRBVHFallbackMeshData &data);
  static bool save(const std::string &path, const std::vector<SWRBVHFallbackMeshData> &data);
  static bool load(const std::string &path, SWRBVHFallbackMeshData &outData);
  static bool load(const std::string &path, std::vector<SWRBVHFallbackMeshData> &outData);
  static bool loadEntry(const std::string &path, uint32_t meshIndex, SWRBVHFallbackMeshData &outData);
};

} // namespace softwarebvh
