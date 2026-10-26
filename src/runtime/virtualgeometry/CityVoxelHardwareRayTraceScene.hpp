#pragma once

#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
#include "virtualgeometry/CityVoxelHardwareMesh.hpp"
#include "virtualgeometry/CityVoxelStaticScene.hpp"
#include "virtualgeometry/CityVoxelSceneRuntime.hpp"
#include "virtualgeometry/raytracing/metal/VirtualGeometryMetalRayTraceBackend.hpp"
#include "voxel/svraster/CityVoxelExactAppearanceLookup.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include <memory>

namespace virtualgeometry {
struct CityVoxelHardwareRayBudgets {
  uint32_t maximumActiveInstances = 250000;
  uint32_t maximumInstallsPerUpdate = 4;
  uint64_t maximumInputBytesPerUpdate = 64ull << 20u;
  uint64_t maximumResidentInputBytes = 256ull << 20u;
  uint64_t maximumBlasBytes = 256ull << 20u;
};

// One Metal BLAS per resident trace group of shared geometry. A placement
// expands to one TLAS record per group; the ray hit maps back to its original
// placement and exact voxel appearance. Incomplete cuts are explicit.
class CityVoxelHardwareRayTraceScene {
public:
  using Backend = raytracing::VirtualGeometryMetalRayTraceBackend;
  struct ResolvedHit {
    uint64_t stableId = 0;
    uint32_t placementIndex = UINT32_MAX;
    std::array<int32_t,3> modelCell{};
    std::array<float,3> worldNormal{};
    uint32_t rgba = 0;
    uint32_t materialProfile = 0;
    float opacity = 0;
  };
  CityVoxelHardwareRayTraceScene(rendering::RenderGraph &graph,
      const CityVoxelStaticScene &city,
      const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry,
      const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances,
      uint32_t maximumRays, uint32_t frameSlots,
      CityVoxelHardwareRayBudgets budgets = {});
  CityVoxelHardwareRayTraceScene(rendering::RenderGraph &graph,
      const CityVoxelStaticScene &city, const CityVoxelSceneRuntime &runtime,
      uint32_t maximumRays, uint32_t frameSlots,
      CityVoxelHardwareRayBudgets budgets = {});
  ~CityVoxelHardwareRayTraceScene();
  CityVoxelHardwareRayTraceScene(const CityVoxelHardwareRayTraceScene &) = delete;
  CityVoxelHardwareRayTraceScene &operator=(const CityVoxelHardwareRayTraceScene &) = delete;
  void publishResidency(const std::vector<uint8_t> &geometryReady,
      const std::vector<uint8_t> &appearanceReady);
  // Publish complete spatial cells selected by a conservative ray-reach
  // streamer. Resident trace group pages supply BLAS inputs.
  void publishFromRuntime(const CityVoxelSceneRuntime &runtime);
  void update();
  // Attach the published TLAS cut to a native trace frame only when its
  // page, BLAS and instance coverage is complete.
  bool bindTraceFrame(Backend::Frame &,std::string *error = nullptr);
  bool resolveClosestHit(const raytracing::ClosestHitResult &, ResolvedHit &) const;
  const std::shared_ptr<Backend> &backend() const { return backend_; }
  rendering::Buffer instances() const { return instances_; }
  rendering::Buffer instanceCount() const { return count_; }
  uint64_t triangleCount() const { return triangles_; }
  uint64_t residentInputBytes() const { return residentInputBytes_; }
  uint32_t residentGeometryCount() const;
  uint32_t residentGroupCount() const;
  uint32_t activeRayInstances() const { return uint32_t(rayInstances_.size()); }
  bool coverageComplete() const { return coverageComplete_; }
private:
  rendering::Buffer create(const char *, uint64_t, bool);
  void initialise(uint32_t maximumRays, uint32_t frameSlots);
  void reconcileGeometry();
  struct ModelInput {
    rendering::Buffer vertices, indices;
    uint64_t bytes = 0;
    uint32_t triangles = 0;
  };
  struct ModelGroup {
    uint32_t handle=0, keyIndex=0;
    ModelInput input;
    CityVoxelHardwareMesh mesh;
  };
  struct RayInstanceSource { uint32_t placement=0, group=0; };
  rendering::RenderGraph &graph_;
  const CityVoxelStaticScene &city_;
  std::vector<voxel::svraster::CityVoxelCompiledGeometry> ownedGeometry_;
  std::vector<voxel::svraster::CityVoxelCompiledAppearance> ownedAppearances_;
  const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry_;
  const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances_;
  std::shared_ptr<Backend> backend_;
  CityVoxelHardwareRayBudgets budgets_;
  rendering::Buffer instances_, count_;
  std::vector<uint32_t> modelForGeometry_, sourceGeometry_;
  std::vector<std::vector<ModelGroup>> groups_;
  std::vector<voxel::svraster::CityVoxelExactAppearanceLookup> lookups_;
  std::vector<uint8_t> geometryReady_, appearanceReady_, lookupBound_;
  std::vector<RayInstanceSource> rayInstances_;
  const CityVoxelSceneRuntime *streamed_ = nullptr;
  const CityVoxelSceneRuntime *validatedRuntime_ = nullptr;
  uint64_t validatedRuntimeGeneration_ = 0;
  bool ownsAssets_ = false;
  bool coverageComplete_ = false;
  uint64_t triangles_ = 0, residentInputBytes_ = 0, owner_ = 0;
  uint32_t bufferSerial_ = 0;
};
} // namespace virtualgeometry
#endif
