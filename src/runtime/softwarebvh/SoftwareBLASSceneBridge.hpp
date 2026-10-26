#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "softwarebvh/SoftwareBLASManager.hpp"
#include "softwarebvh/SoftwareRayTraceSceneFile.hpp"
#include "softwarebvh/SWRTFallbackMeshFile.hpp"
#include "softwarebvh/SWRBVHFallbackMeshFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace softwarebvh
{

class SoftwareBLASSceneBridge
{
public:
  enum class PrimitiveMode : uint32_t
  {
    Triangle = 0u,
    RBVHCard = 1u,
  };

  struct Settings
  {
    uint32_t fallbackLod = 3u;
    uint32_t rbvhLod = 0u;
    PrimitiveMode primitiveMode = PrimitiveMode::Triangle;
    bool preferRuntimeRbvhBuilds = false;
  };

  struct Diagnostics
  {
    uint32_t cachedFallbackMeshCount = 0u;
    uint32_t availableFallbackMeshCount = 0u;
    uint32_t missingFallbackMeshCount = 0u;
    uint32_t preparedMeshCount = 0u;
    uint32_t uniqueMeshCount = 0u;
    uint32_t skippedMissingRuntimeDataCount = 0u;
    uint32_t skippedMissingFallbackMeshCount = 0u;
    uint32_t primitiveMode = 0u;
    uint32_t rbvhLod = 0u;
    bool preferRuntimeRbvhBuilds = false;
    uint32_t runtimeRbvhBuildCount = 0u;
    uint32_t runtimeRbvhOriginalSourceCount = 0u;
    uint32_t runtimeRbvhFallbackSourceCount = 0u;
    uint64_t runtimeRbvhSourceTriangleCount = 0u;
    double runtimeRbvhBuildMilliseconds = 0.0;
    bool requestedRebuild = false;
    std::string lastLoadedFallbackPath;
    std::string lastMissingFallbackPath;
    std::string lastRuntimeRbvhBuildSourcePath;
  };

  explicit SoftwareBLASSceneBridge(SoftwareBLASManager &manager);
  SoftwareBLASSceneBridge(SoftwareBLASManager &manager, Settings settings);

  void sync(virtualgeometry::VirtualGeometryScene &scene);
  void registerObjectInstance(virtualgeometry::VirtualGeometryScene &scene, virtualgeometry::InstanceId objectInstanceId);
  void rebuildFromScene(virtualgeometry::VirtualGeometryScene &scene);
  void setRuntimeRBVHSourceMeshes(const std::string &virtualGeometryPath, std::vector<virtualgeometry::FallbackMeshData> meshes);

  void setSettings(Settings settings)
  {
    settings_ = settings;
  }

  const Settings &getSettings() const
  {
    return settings_;
  }

  const Diagnostics &getDiagnostics() const
  {
    return diagnostics_;
  }

private:
  struct CachedFallbackMesh
  {
    bool available = false;
    std::string cacheKey;
    std::string fallbackPath;
    bool isSWRT = false;
    std::filesystem::file_time_type lastWriteTime{};
    virtualgeometry::FallbackMeshData mesh;
    std::optional<CWBVHBuilder::BuildResult> prebuilt;
    std::optional<RBVHBuildResult> rbvhBuild;
  };

  struct CachedFallbackCollection
  {
    bool available = false;
    std::string fallbackPath;
    bool isSWRT = false;
    std::filesystem::file_time_type lastWriteTime{};
    std::vector<virtualgeometry::FallbackMeshData> meshes;
    std::vector<CWBVHBuilder::BuildResult> prebuiltBuilds;
    std::vector<RBVHBuildResult> rbvhBuilds;
    std::vector<std::vector<RBVHBuildResult>> rbvhBuildLods;
  };

  struct SourceState
  {
    std::string cacheKey;
    std::string fallbackPath;
    std::string virtualGeometryPath;
    bool isSWRT = false;
    PrimitiveMode primitiveMode = PrimitiveMode::Triangle;
    uint32_t fallbackLod = 0u;
    uint32_t rbvhLod = 0u;
    bool preferRuntimeRbvhBuilds = false;
    uint32_t shapeIndex = 0u;
    std::filesystem::file_time_type lastWriteTime{};
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t lodLevel = 0u;
    bool seenThisFrame = false;
  };

  CachedFallbackMesh *loadFallbackMesh(const std::string &virtualGeometryPath, uint32_t fallbackLod, std::optional<uint32_t> shapeIndex);
  bool addObjectInstanceSources(virtualgeometry::VirtualGeometryScene &scene, virtualgeometry::InstanceId objectInstanceId);
  bool settingsMatchApplied() const;
  void updateCacheDiagnostics();

  SoftwareBLASManager &manager_;
  Settings settings_{};
  Settings appliedSettings_{};
  bool hasAppliedSettings_ = false;
  bool sourcesDirty_ = false;
  std::unordered_map<std::string, CachedFallbackMesh> fallbackMeshCache_;
  std::unordered_map<std::string, CachedFallbackCollection> fallbackCollectionCache_;
  std::unordered_map<std::string, std::vector<virtualgeometry::FallbackMeshData>> runtimeRbvhSourceMeshes_;
  std::unordered_map<std::string, SourceState> sourceStates_;
  Diagnostics diagnostics_{};
};

} // namespace softwarebvh
