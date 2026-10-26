#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rendering/animation/Skeleton.hpp"

namespace virtualgeometry
{

constexpr uint32_t FALLBACK_MESH_MAGIC = 0x4D455348u;   // MESH
constexpr uint32_t FALLBACK_MESH_VERSION = 4u;
constexpr uint32_t FALLBACK_MESH_VERSION_1 = 1u; // legacy, no capsule data
constexpr uint32_t FALLBACK_MESH_VERSION_2 = 2u; // legacy, no baked surface shading data
constexpr uint32_t FALLBACK_MESH_VERSION_COLLECTION = 5u;
constexpr uint32_t FALLBACK_MESH_VERSION_COLLECTION_3 = 3u; // legacy collection with version 2 mesh payloads
constexpr uint32_t FALLBACK_MESH_ENDIAN_LITTLE = 0x12345678u;

enum FallbackMeshSurfaceFlags : uint32_t
{
  FallbackMeshSurfaceFlag_None = 0u,
  FallbackMeshSurfaceFlag_BakedVertexShading = 1u << 0u,
};

struct FallbackMeshVertex
{
  float position[3] = {0.0f, 0.0f, 0.0f};
  float normal[3] = {0.0f, 0.0f, 1.0f};
  float uv[2] = {0.0f, 0.0f};
  uint32_t boneIndices[4] = {0u, 0u, 0u, 0u};
  float boneWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float surfaceBaseColorOpacity[4] = {0.85f, 0.85f, 0.85f, 1.0f};
  float surfaceNormalRoughness[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float surfaceEmissiveMetallic[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t surfaceFlags[4] = {FallbackMeshSurfaceFlag_None, 0u, 0u, 0u};
};

struct FallbackMeshSection
{
  uint32_t indexOffset = 0u;
  uint32_t indexCount = 0u;
  uint32_t materialIndex = 0u;
};

struct FallbackMeshTriangleSurfaceHint
{
  float uvCircleCenter[2] = {0.0f, 0.0f};
  float uvCircleRadius = 0.0f;
  float textureMipLevel = 0.0f;
  uint32_t materialIndex = 0u;
  uint32_t _padding[3] = {0u, 0u, 0u};
};

struct FallbackMeshData
{
  uint32_t lodLevel = 0u;
  float targetError = 0.0f;
  float relativeError = 0.0f;
  float boundsMin[3] = {0.0f, 0.0f, 0.0f};
  float boundsMax[3] = {0.0f, 0.0f, 0.0f};
  std::vector<FallbackMeshVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<FallbackMeshSection> sections;
  std::vector<FallbackMeshTriangleSurfaceHint> triangleSurfaceHints;
  std::vector<std::string> materialFiles;
  rendering::animation::Skeleton skeleton;
};

class FallbackMeshFile
{
public:
  static bool save(const std::string &path, const FallbackMeshData &data);
  static bool save(const std::string &path, const std::vector<FallbackMeshData> &meshes);
  static bool load(const std::string &path, FallbackMeshData &outData);
  static bool load(const std::string &path, std::vector<FallbackMeshData> &outMeshes);
  static bool loadEntry(const std::string &path, uint32_t meshIndex, FallbackMeshData &outData);
};

} // namespace virtualgeometry
