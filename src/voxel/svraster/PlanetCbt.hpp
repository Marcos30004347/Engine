#pragma once

#include "procedural/PlanetSurfaceHierarchy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace voxel::svraster
{

constexpr uint32_t kPlanetCbtNullSlot = 0xffffffffu;
constexpr uint32_t kPlanetCbtRootBisectorCount = 60u;
constexpr uint32_t kPlanetCbtDefaultDepth = 17u;
constexpr uint32_t kPlanetCbtDefaultCapacity = 1u << kPlanetCbtDefaultDepth;
constexpr uint32_t kPlanetCbtMaximumSubdivisionDepth = 24u;
constexpr uint32_t kPlanetCbtMaximumCompatibilityChain = 64u;
constexpr uint32_t kPlanetCbtMaximumForcedRequests = 64u;
constexpr uint32_t kPlanetCbtMaximumUpdateRounds = 2u;
constexpr uint32_t kPlanetCbtMaximumAllocationsPerRound = 512u;
constexpr uint32_t kPlanetCbtWarmupFrameCount = 2u;
constexpr uint32_t kPlanetCbtConvergenceFrameCount = 8u;
constexpr uint32_t kPlanetCbtMaximumActiveTriangles = 128000u;
constexpr uint64_t kPlanetCbtMaximumStorageBytes = 10u * 1024u * 1024u;
constexpr uint32_t kPlanetCbtWorkgroupSize = 256u;
constexpr uint32_t kPlanetCbtMinimumWorkgroupInvocations =
    kPlanetCbtWorkgroupSize;
// Roughly a 9 px triangle edge at the default view. The silhouette and relief
// remain smooth at deep overview zoom while hidden/off-screen decimation keeps
// the active set inside the bounded 128k pool.
constexpr float kPlanetCbtTargetProjectedArea = 36.0f;
// Projected area is allowed to coarsen hidden/off-screen regions all the way
// to their roots, but a visible globe must never expose the base icosahedron
// silhouette merely because it occupies fewer pixels. Sixty roots subdivided
// seven times provide 7,680 adaptive patches globally while remaining tiny
// beside the bounded 128k pool.
constexpr uint32_t kPlanetCbtMinimumVisibleSubdivisionDepth = 7u;
// The CBT remains a compact adaptive patch selector. Visible patches are
// recursively subdivided by the vertex shader to this absolute depth, which
// produces millions of raster triangles without a multi-million-node pool.
constexpr uint32_t kPlanetCbtRenderTargetSubdivisionDepth = 15u;
constexpr uint32_t kPlanetCbtMaximumRenderSubdivisionDepth =
    kPlanetCbtRenderTargetSubdivisionDepth -
    kPlanetCbtMinimumVisibleSubdivisionDepth;
constexpr uint32_t kPlanetCbtMaximumRenderSubtriangleCount =
    1u << kPlanetCbtMaximumRenderSubdivisionDepth;
constexpr float kPlanetCbtUpdateP95BudgetMilliseconds = 0.50f;
constexpr float kPlanetCbtUpdateMaximumBudgetMilliseconds = 0.75f;
constexpr uint32_t kPlanetCbtBitfieldUnchanged = 0u;
constexpr uint32_t kPlanetCbtBitfieldActivate = 1u;
constexpr uint32_t kPlanetCbtBitfieldRelease = 2u;

enum PlanetCbtCommandBits : uint32_t
{
  PlanetCbtCommand_None = 0u,
  PlanetCbtCommand_Split = 1u << 0u,
  PlanetCbtCommand_Merge = 1u << 1u,
  PlanetCbtCommand_Accepted = 1u << 2u,
  PlanetCbtCommand_RejectedCapacity = 1u << 3u,
};

enum PlanetCbtLifecycleFlags : uint32_t
{
  PlanetCbtLifecycleFlag_None = 0u,
  // Conformance fixtures can retain the Phase-3 all-sphere projected-area
  // policy while production decimates hidden and offscreen regions.
  PlanetCbtLifecycleFlag_DisableVisibilityDecimation = 1u << 0u,
  // Test/diagnostic escape hatch for validating unrestricted coarsening. The
  // production overview leaves the visible-depth floor enabled.
  PlanetCbtLifecycleFlag_DisableMinimumVisibleDepth = 1u << 1u,
  // Set internally by the lifecycle while the adaptive overview is hidden.
  PlanetCbtLifecycleFlag_DisableAdaptiveDraw = 1u << 2u,
};

// These structures are the host half of the storage-buffer ABI in
// sparse_voxel_planet_cbt_contract.wgsl. Keep them deliberately unpacked until
// the adaptive path is measured; stable offsets are more valuable in the
// bring-up phases than saving a few words.
struct alignas(16) PlanetCbtBisectorGPU
{
  uint32_t next = kPlanetCbtNullSlot;
  uint32_t previous = kPlanetCbtNullSlot;
  uint32_t twin = kPlanetCbtNullSlot;
  uint32_t command = PlanetCbtCommand_None;
  uint32_t rootHalfedge = 0u;
  uint32_t depth = 0u;
  uint32_t pathLow = 0u;
  uint32_t pathHigh = 0u;
  std::array<uint32_t, 4> allocation{kPlanetCbtNullSlot, kPlanetCbtNullSlot, kPlanetCbtNullSlot, kPlanetCbtNullSlot};
};

struct PlanetCbtPointerGPU
{
  uint32_t activePoolSlot = kPlanetCbtNullSlot;
  uint32_t freePoolSlot = kPlanetCbtNullSlot;
};

struct alignas(16) PlanetCbtCountersGPU
{
  uint32_t activeCount = 0u;
  uint32_t freeCount = 0u;
  uint32_t requestedAllocations = 0u;
  uint32_t overflowCount = 0u;
  uint32_t splitCount = 0u;
  uint32_t mergeCount = 0u;
  uint32_t unchangedCount = 0u;
  uint32_t maximumDepth = 0u;
};

struct alignas(16) PlanetCbtDrawIndirectGPU
{
  uint32_t vertexCount = 0u;
  uint32_t instanceCount = 1u;
  uint32_t firstVertex = 0u;
  uint32_t firstInstance = 0u;
};

struct alignas(16) PlanetCbtDispatchIndirectGPU
{
  uint32_t groupCountX = 0u;
  uint32_t groupCountY = 1u;
  uint32_t groupCountZ = 1u;
  uint32_t itemCount = 0u;
};

struct alignas(16) PlanetCbtDispatchesGPU
{
  PlanetCbtDispatchIndirectGPU activeDispatch{};
  PlanetCbtDispatchIndirectGPU freeDispatch{};
  PlanetCbtDispatchIndirectGPU pointerCacheDispatch{};
};

struct alignas(16) PlanetCbtRootTriangleGPU
{
  std::array<float, 4> vertex0{};
  std::array<float, 4> vertex1{};
  std::array<float, 4> vertex2{};
};

struct PlanetCbtRequestGPU
{
  uint32_t rootHalfedge = 0u;
  uint32_t depth = 0u;
  uint32_t pathLow = 0u;
  uint32_t pathHigh = 0u;
  uint32_t command = PlanetCbtCommand_None;
  uint32_t reserved = 0u;
};

struct alignas(16) PlanetCbtUpdateSettingsGPU
{
  // xy viewport dimensions, z vertical focal scale, w near plane.
  std::array<float, 4> viewportFocal{1.0f, 1.0f, 1.0f, 0.001f};
  std::array<float, 4> cameraDirectionDistance{0.0f, 0.0f, 1.0f, 2.0f};
  std::array<float, 4> cameraRight{1.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 4> cameraUp{0.0f, 1.0f, 0.0f, 0.0f};
  // target area, refine multiplier, merge multiplier, reserved.
  std::array<float, 4> lodThresholds{49.0f, 1.25f, 0.55f, 0.0f};
  // x base face resolution, y mip count, zw normalized height bias/range.
  std::array<float, 4> heightMapInfo{};
  uint32_t requestCount = 0u;
  uint32_t enableProjectedLod = 0u;
  uint32_t maximumDepth = kPlanetCbtMaximumSubdivisionDepth;
  uint32_t updateRoundCount = 1u;
  uint32_t frameIndex = 0u;
  uint32_t minimumFreeSlots = kPlanetCbtMaximumCompatibilityChain * 2u;
  uint32_t lifecycleFlags = 0u;
  uint32_t maximumAllocationsPerRound =
      kPlanetCbtMaximumAllocationsPerRound;
  uint32_t renderSubdivisionDepth = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  uint32_t reserved2 = 0u;
};

// CPU-only lifecycle input. The policy deliberately observes camera and LOD
// settings rather than GPU counters, keeping convergence scheduling free of
// readbacks and render-thread waits.
struct PlanetCbtLifecycleInput
{
  float overviewContribution = 1.0f;
  bool adaptiveGeometry = true;
  bool updatesEnabled = true;
  uint64_t packageGeneration = 1u;
};

struct PlanetCbtLifecycleDecision
{
  uint32_t updateRoundCount = 0u;
  uint32_t frameIndex = 0u;
  bool resetTopology = false;
  bool overviewVisible = false;
  bool settled = false;
};

struct PlanetCbtDeviceCapabilities
{
  bool compute = false;
  bool graphics = false;
  uint32_t maximumComputeWorkgroupInvocations = 0u;
  uint64_t memoryBytes = 0u;
};

class PlanetCbtLifecycle
{
public:
  PlanetCbtLifecycleDecision advance(
      const PlanetCbtUpdateSettingsGPU &settings,
      const PlanetCbtLifecycleInput &input);
  void invalidate();

private:
  bool initialized_ = false;
  bool wasVisible_ = false;
  bool haveSettings_ = false;
  uint64_t packageGeneration_ = 0u;
  uint32_t frameIndex_ = 0u;
  uint32_t warmupFramesRemaining_ = 0u;
  uint32_t convergenceFramesRemaining_ = 0u;
  PlanetCbtUpdateSettingsGPU previousSettings_{};
};

static_assert(sizeof(PlanetCbtBisectorGPU) == 48u);
static_assert(alignof(PlanetCbtBisectorGPU) == 16u);
static_assert(offsetof(PlanetCbtBisectorGPU, next) == 0u);
static_assert(offsetof(PlanetCbtBisectorGPU, previous) == 4u);
static_assert(offsetof(PlanetCbtBisectorGPU, twin) == 8u);
static_assert(offsetof(PlanetCbtBisectorGPU, command) == 12u);
static_assert(offsetof(PlanetCbtBisectorGPU, rootHalfedge) == 16u);
static_assert(offsetof(PlanetCbtBisectorGPU, depth) == 20u);
static_assert(offsetof(PlanetCbtBisectorGPU, pathLow) == 24u);
static_assert(offsetof(PlanetCbtBisectorGPU, pathHigh) == 28u);
static_assert(offsetof(PlanetCbtBisectorGPU, allocation) == 32u);
static_assert(sizeof(PlanetCbtPointerGPU) == 8u);
static_assert(alignof(PlanetCbtPointerGPU) == 4u);
static_assert(offsetof(PlanetCbtPointerGPU, activePoolSlot) == 0u);
static_assert(offsetof(PlanetCbtPointerGPU, freePoolSlot) == 4u);
static_assert(sizeof(PlanetCbtCountersGPU) == 32u);
static_assert(alignof(PlanetCbtCountersGPU) == 16u);
static_assert(offsetof(PlanetCbtCountersGPU, activeCount) == 0u);
static_assert(offsetof(PlanetCbtCountersGPU, freeCount) == 4u);
static_assert(offsetof(PlanetCbtCountersGPU, requestedAllocations) == 8u);
static_assert(offsetof(PlanetCbtCountersGPU, overflowCount) == 12u);
static_assert(offsetof(PlanetCbtCountersGPU, splitCount) == 16u);
static_assert(offsetof(PlanetCbtCountersGPU, mergeCount) == 20u);
static_assert(offsetof(PlanetCbtCountersGPU, unchangedCount) == 24u);
static_assert(offsetof(PlanetCbtCountersGPU, maximumDepth) == 28u);
static_assert(sizeof(PlanetCbtDrawIndirectGPU) == 16u);
static_assert(alignof(PlanetCbtDrawIndirectGPU) == 16u);
static_assert(offsetof(PlanetCbtDrawIndirectGPU, vertexCount) == 0u);
static_assert(offsetof(PlanetCbtDrawIndirectGPU, instanceCount) == 4u);
static_assert(offsetof(PlanetCbtDrawIndirectGPU, firstVertex) == 8u);
static_assert(offsetof(PlanetCbtDrawIndirectGPU, firstInstance) == 12u);
static_assert(sizeof(PlanetCbtDispatchIndirectGPU) == 16u);
static_assert(alignof(PlanetCbtDispatchIndirectGPU) == 16u);
static_assert(offsetof(PlanetCbtDispatchIndirectGPU, groupCountX) == 0u);
static_assert(offsetof(PlanetCbtDispatchIndirectGPU, groupCountY) == 4u);
static_assert(offsetof(PlanetCbtDispatchIndirectGPU, groupCountZ) == 8u);
static_assert(offsetof(PlanetCbtDispatchIndirectGPU, itemCount) == 12u);
static_assert(sizeof(PlanetCbtDispatchesGPU) == 48u);
static_assert(offsetof(PlanetCbtDispatchesGPU, activeDispatch) == 0u);
static_assert(offsetof(PlanetCbtDispatchesGPU, freeDispatch) == 16u);
static_assert(offsetof(PlanetCbtDispatchesGPU, pointerCacheDispatch) == 32u);
static_assert(sizeof(PlanetCbtRootTriangleGPU) == 48u);
static_assert(alignof(PlanetCbtRootTriangleGPU) == 16u);
static_assert(offsetof(PlanetCbtRootTriangleGPU, vertex0) == 0u);
static_assert(offsetof(PlanetCbtRootTriangleGPU, vertex1) == 16u);
static_assert(offsetof(PlanetCbtRootTriangleGPU, vertex2) == 32u);
static_assert(sizeof(PlanetCbtRequestGPU) == 24u);
static_assert(alignof(PlanetCbtRequestGPU) == 4u);
static_assert(offsetof(PlanetCbtRequestGPU, command) == 16u);
static_assert(sizeof(PlanetCbtUpdateSettingsGPU) == 144u);
static_assert(alignof(PlanetCbtUpdateSettingsGPU) == 16u);
static_assert(offsetof(PlanetCbtUpdateSettingsGPU, lodThresholds) == 64u);
static_assert(offsetof(PlanetCbtUpdateSettingsGPU, heightMapInfo) == 80u);
static_assert(offsetof(PlanetCbtUpdateSettingsGPU, requestCount) == 96u);
static_assert(offsetof(PlanetCbtUpdateSettingsGPU, frameIndex) == 112u);
static_assert(offsetof(PlanetCbtUpdateSettingsGPU,
                  renderSubdivisionDepth) == 128u);

procedural::PlanetSurfaceBisectorAddress planetCbtBisectorAddress(const PlanetCbtBisectorGPU &bisector);
void setPlanetCbtBisectorAddress(PlanetCbtBisectorGPU &bisector, const procedural::PlanetSurfaceBisectorAddress &address);

// Builds the regular, normalized, outward-wound icosahedron used by the CBT
// overview. Its 20 triangular faces become exactly 60 halfedge roots.
bool buildPlanetCbtIcosahedron(procedural::PlanetSurfaceHalfedgeMesh &mesh);
std::vector<PlanetCbtRootTriangleGPU> buildPlanetCbtRootTriangles(const procedural::PlanetSurfaceHalfedgeMesh &mesh);

// The CPU reference and WGSL use the same 1-based complete-tree layout: root
// at 1, leaves at 2^depth, and an intentionally unused entry at zero. Hence a
// depth-D heap contains 2^(D+1) u32 words, as budgeted in the design plan.
uint32_t planetCbtPoolCapacity(uint32_t treeDepth);
size_t planetCbtHeapEntryCount(uint32_t treeDepth);
uint64_t planetCbtStorageBytes(uint32_t treeDepth);
bool planetCbtAdaptiveSupported(
    const PlanetCbtDeviceCapabilities &capabilities,
    uint32_t treeDepth = kPlanetCbtDefaultDepth);
bool planetCbtSetLeafBit(std::vector<uint32_t> &heap, uint32_t treeDepth, uint32_t poolSlot, bool allocated);
bool planetCbtReduce(std::vector<uint32_t> &heap, uint32_t treeDepth);
uint32_t planetCbtOneToBitID(const std::vector<uint32_t> &heap, uint32_t treeDepth, uint32_t allocatedIndex);
uint32_t planetCbtZeroToBitID(const std::vector<uint32_t> &heap, uint32_t treeDepth, uint32_t freeIndex);

} // namespace voxel::svraster
