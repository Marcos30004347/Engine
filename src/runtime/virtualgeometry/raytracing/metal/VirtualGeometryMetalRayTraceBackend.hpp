#pragma once

#include "virtualgeometry/raytracing/VirtualGeometryRayTraceTypes.hpp"
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace virtualgeometry::raytracing
{
// Native objects cross the Objective-C++ boundary as borrowed opaque pointers.
// All input resources are retained by the submission until GPU completion.
class VirtualGeometryMetalRayTraceBackend
{
public:
  struct BudgetExceeded : std::runtime_error { using std::runtime_error::runtime_error; };
  struct Configuration
  {
    std::string metallibPath;
    uint32_t maxInstances = 65536;
    uint32_t maxRays = 1920 * 1080;
    uint32_t frameSlots = 3;
    uint64_t blasBudgetBytes = 512ull * 1024 * 1024;
    uint32_t maxLightVolumes = 256;
    uint32_t maxBlasBuildsPerFrame = UINT32_MAX;
    uint32_t maxBlasPreparationsPerFrame = UINT32_MAX;
    uint64_t maxBlasBuildBytesPerFrame = UINT64_MAX; // Sum of AS output sizes.
    bool useResidencySets = true;

  };
  struct Capabilities
  {
    bool available = false;
    bool appleFamily9OrNewer = false;
    bool residencySets = false;
    std::string device;
    std::string reason;
  };
  struct Geometry
  {
    GeometryKey key;
    void *vertexBuffer = nullptr;
    void *indexBuffer = nullptr;
    uint64_t vertexOffset = 0;
    uint64_t indexOffset = 0;
    uint32_t vertexStride = 48;
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    bool index32 = true;
    // Optional producer dependency retained until the immutable BLAS is built.
    void *readyEvent = nullptr;
    uint64_t readyValue = 0;
  };
  struct Frame
  {
    enum class Work { Full, BuildOnly, BuildAndTlas, TraceOnly, TlasAndTrace };
    enum class Query { AnyHit, ClosestHit };
    Query query = Query::AnyHit;
    // GPU-written counts are uint32 values at byte offset zero. Buffers must
    // cover the configured capacities; overflow produces InvalidScene results.
    void *instances = nullptr;
    void *instanceCount = nullptr;
    void *rays = nullptr;
    void *rayCount = nullptr;
    void *results = nullptr;
    // Optional conservative light cull of an already valid all-direction cut.
    // All three buffers must be supplied together. Bounds are indexed by input
    // instance; volumes enclose all receiver-to-emitter segments. Counts are GPU
    // uint32 values. This is not a replacement for hierarchy/LOD selection.
    void *casterBounds = nullptr;
    void *lightVolumes = nullptr;
    void *lightVolumeCount = nullptr;
    // Optional shared-event handoff, matching RHI::cmdMetalCompute.
    void *sharedEvent = nullptr;
    uint64_t readyValue = 0;
    uint64_t doneValue = 0;
    uint32_t slot = 0;
    // Isolated workloads for benchmarking. TraceOnly requires a previous TLAS
    // in this slot and retains precisely the BLAS references used to build it.
    Work work = Work::Full;
  };
  struct CacheStatistics
  {
    uint64_t hits = 0, misses = 0, builds = 0, evictions = 0;
    uint64_t residentBytes = 0, retiredBytes = 0, scratchBytes = 0;
    uint64_t tlasBytes = 0;
    uint64_t pendingBuilds = 0;
  };
  struct Completion
  {
    bool completed = false;
    bool succeeded = false;
    std::string error;
    double gpuMilliseconds = 0;
    RayTraceFrameStatistics statistics{};
  };

  // nullptr device selects the system device, for standalone tests/benchmarks.
  explicit VirtualGeometryMetalRayTraceBackend(Configuration, void *device = nullptr);
  ~VirtualGeometryMetalRayTraceBackend();
  VirtualGeometryMetalRayTraceBackend(const VirtualGeometryMetalRayTraceBackend &) = delete;
  VirtualGeometryMetalRayTraceBackend &operator=(const VirtualGeometryMetalRayTraceBackend &) = delete;
  const Capabilities &capabilities() const;
  const Configuration &configuration() const;
  // Returns an existing handle on cache hit; encoding its build is deferred to
  // submit. The caller must order GPU geometry writes before the frame's ready event.
  uint32_t cacheGeometry(const Geometry &);
  void evictGeometry(const GeometryKey &);
  bool tryEvictGeometryAfterInputReads(const GeometryKey &);
  enum class GeometryState { Missing, Pending, Ready, Failed };
  // Durable per-geometry completion, independent of recycled frame serials.
  GeometryState geometryState(uint32_t handle);
  GeometryState geometryStates(const std::vector<uint32_t> &handles);
  bool geometryInputsInFlight(uint32_t handle);
  void drainGeometryInputs(uint32_t handle); // Teardown only.
  // Install-time GPU position/index expansion into temporary BLAS inputs.
  // Encoded page layout is the engine's 34-word cluster descriptor ABI.
  std::vector<uint32_t> cacheEncodedPage(uint64_t object, uint64_t revision,
      const uint32_t *words, uint64_t wordCount, uint32_t headerWords, float unitScale);

  // Nonblocking; throws if slot is still in use or resources are invalid. CPU
  // cache/submit calls are serialized internally. No per-frame count readback.
  uint64_t submit(const Frame &);
  Completion poll(uint64_t serial);
  // Diagnostic/benchmark convenience ONLY. Never call from the renderer frame path.
  Completion waitForTesting(uint64_t serial);
  CacheStatistics cacheStatistics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
