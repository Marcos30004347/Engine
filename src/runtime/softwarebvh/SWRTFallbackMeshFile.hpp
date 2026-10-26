#pragma once

#include <string>
#include <vector>

#include "softwarebvh/CWBVHBuilder.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"

namespace softwarebvh
{

constexpr uint32_t SWRT_FALLBACK_MESH_MAGIC = 0x54525753u; // SWRT
constexpr uint32_t SWRT_FALLBACK_MESH_VERSION = 2u;
constexpr uint32_t SWRT_FALLBACK_MESH_VERSION_COLLECTION = 3u;
constexpr uint32_t SWRT_FALLBACK_MESH_ENDIAN_LITTLE = 0x12345678u;

struct SWRTFallbackMeshData
{
  virtualgeometry::FallbackMeshData fallbackMesh;
  CWBVHBuilder::BuildResult build;
};

class SWRTFallbackMeshFile
{
public:
  static SWRTFallbackMeshData build(const virtualgeometry::FallbackMeshData &fallbackMesh);
  static bool save(const std::string &path, const SWRTFallbackMeshData &data);
  static bool save(const std::string &path, const std::vector<SWRTFallbackMeshData> &data);
  static bool load(const std::string &path, SWRTFallbackMeshData &outData);
  static bool load(const std::string &path, std::vector<SWRTFallbackMeshData> &outData);
  static bool loadEntry(const std::string &path, uint32_t meshIndex, SWRTFallbackMeshData &outData);
};

} // namespace softwarebvh
