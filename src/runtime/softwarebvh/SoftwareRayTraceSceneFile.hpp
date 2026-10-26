#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "softwarebvh/CWBVHBuilder.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace softwarebvh
{

constexpr uint32_t SOFTWARE_RAYTRACE_SCENE_FILE_MAGIC = 0x54525753u; // SWRT
constexpr uint32_t SOFTWARE_RAYTRACE_SCENE_FILE_VERSION_1 = 1u;
constexpr uint32_t SOFTWARE_RAYTRACE_SCENE_FILE_VERSION_2 = 2u;
constexpr uint32_t SOFTWARE_RAYTRACE_SCENE_FILE_VERSION = 3u;
constexpr uint32_t SOFTWARE_RAYTRACE_SCENE_FILE_ENDIAN_LITTLE = 0x12345678u;

struct SoftwareRayTraceSceneInstanceData
{
  virtualgeometry::InstanceId objectInstanceId = virtualgeometry::INVALID_INSTANCE_ID;
  virtualgeometry::SceneInstanceId sceneInstanceId = virtualgeometry::INVALID_SCENE_INSTANCE_ID;
  uint32_t shapeIndex = UINT32_MAX;
  uint32_t lodLevel = 0u;
  uint32_t flags = 0u;
  std::string sourcePath;
  virtualgeometry::FallbackMeshData fallbackMesh;
  CWBVHBuilder::BuildResult build;
};

struct SoftwareRayTraceSceneFileEntryHeader
{
  virtualgeometry::InstanceId objectInstanceId = virtualgeometry::INVALID_INSTANCE_ID;
  virtualgeometry::SceneInstanceId sceneInstanceId = virtualgeometry::INVALID_SCENE_INSTANCE_ID;
  uint32_t shapeIndex = UINT32_MAX;
  uint32_t lodLevel = 0u;
  uint32_t flags = 0u;
  std::string sourcePath;
  uint64_t payloadOffset = 0u;
  uint32_t compressedSize = 0u;
  uint32_t uncompressedSize = 0u;
  uint64_t cwbvhPayloadOffset = 0u;
  uint32_t cwbvhCompressedSize = 0u;
  uint32_t cwbvhUncompressedSize = 0u;
  uint64_t fallbackPayloadOffset = 0u;
  uint32_t fallbackCompressedSize = 0u;
  uint32_t fallbackUncompressedSize = 0u;
};

struct SoftwareRayTraceSceneFileHeader
{
  uint32_t version = SOFTWARE_RAYTRACE_SCENE_FILE_VERSION;
  uint32_t headerSize = 0u;
  std::vector<SoftwareRayTraceSceneFileEntryHeader> entries;
};

class SoftwareRayTraceSceneFile
{
public:
  static SoftwareRayTraceSceneInstanceData buildInstance(
      const virtualgeometry::FallbackMeshData &fallbackMesh,
      virtualgeometry::InstanceId objectInstanceId,
      virtualgeometry::SceneInstanceId sceneInstanceId,
      uint32_t shapeIndex,
      std::string sourcePath = {});

  static bool save(const std::string &path, const SoftwareRayTraceSceneInstanceData &data);
  static bool save(const std::string &path, const std::vector<SoftwareRayTraceSceneInstanceData> &data);
  static bool loadHeader(const std::string &path, SoftwareRayTraceSceneFileHeader &outHeader);
  static bool load(const std::string &path, SoftwareRayTraceSceneInstanceData &outData);
  static bool load(const std::string &path, std::vector<SoftwareRayTraceSceneInstanceData> &outData);
  static bool loadEntry(const std::string &path, uint32_t entryIndex, SoftwareRayTraceSceneInstanceData &outData);
  static bool loadEntryCWBVH(const std::string &path, uint32_t entryIndex, SoftwareRayTraceSceneInstanceData &outData);
  static bool loadEntryFallbackMesh(const std::string &path, uint32_t entryIndex, SoftwareRayTraceSceneInstanceData &outData);
  static bool findEntryIndex(const SoftwareRayTraceSceneFileHeader &header, uint32_t lodLevel, uint32_t shapeIndex, uint32_t &outEntryIndex);
  static bool findEntryIndexForShape(const SoftwareRayTraceSceneFileHeader &header, uint32_t shapeIndex, uint32_t &outEntryIndex);
  static bool loadEntry(const std::string &path, uint32_t lodLevel, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData);
  static bool loadEntryCWBVH(const std::string &path, uint32_t lodLevel, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData);
  static bool loadEntryFallbackMesh(const std::string &path, uint32_t lodLevel, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData);
  static bool loadEntryForShape(const std::string &path, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData);
  static bool loadEntryCWBVHForShape(const std::string &path, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData);
  static bool loadEntryFallbackMeshForShape(const std::string &path, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData);
};

} // namespace softwarebvh
