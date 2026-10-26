#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace voxel::svraster
{
constexpr uint32_t kVoxelTraceBranching = 4u;
constexpr uint32_t kVoxelTraceChildCount = 64u;
constexpr uint32_t kVoxelTraceNode64TerminalFlag = 1u;

constexpr uint32_t kVoxelTraceFormatInvalid = 0u;
constexpr uint32_t kVoxelTraceFormatCubic64 = 1u;
constexpr uint32_t kVoxelTraceFormatBounds8 = 2u;
constexpr uint32_t kVoxelTraceFormatBounds16 = 3u;

constexpr uint32_t kVoxelTracePageValid = 1u << 0u;
constexpr uint32_t kVoxelTracePageConservativeFallback = 1u << 1u;

constexpr uint32_t kVoxelTraceInvalidOffset = std::numeric_limits<uint32_t>::max();

// Direct virtual-geometry trace page descriptor.
//
// This is the future shared page-table trace view: one immutable descriptor
// names the page identity/generation, the page-local trace-node range, the
// brick/material ranges, and the optional error sidecar. It intentionally does
// not replace SparseVoxelTree64NodeGPU yet; Phase 1 only freezes the new ABI.
struct alignas(16) VoxelTracePageGPU
{
  uint32_t pageId = 0u;
  uint32_t generation = 0u;
  uint32_t geometryRevisionLo = 0u;
  uint32_t geometryRevisionHi = 0u;

  uint32_t format = kVoxelTraceFormatInvalid;
  uint32_t rootNode = 0u;
  uint32_t traceNodeOffset = 0u;
  uint32_t traceNodeCount = 0u;

  uint32_t brickOffset = 0u;
  uint32_t brickCount = 0u;
  uint32_t materialOffset = 0u;
  uint32_t materialCount = 0u;

  float localOriginX = 0.0f;
  float localOriginY = 0.0f;
  float localOriginZ = 0.0f;
  float rootSide = 0.0f;

  float errorMin = 0.0f;
  float errorMax = 0.0f;
  uint32_t errorSidecarOffset = kVoxelTraceInvalidOffset;
  uint32_t flags = 0u;
};

// Cubic64 hot trace node.
//
// Children are indexed x + 4*y + 16*z. Internal nodes encode childBase << 1 in
// childBaseAndFlags. Terminal nodes set bit zero and use the occupancy mask as
// their final child-cell payload. payloadOrSidecar is deliberately cold/format
// specific: current traversal can ignore it, while later phases can use it for
// material, sidecar, or fallback metadata without widening the node again.
struct VoxelTraceNode64GPU
{
  uint32_t childBaseAndFlags = 0u;
  uint32_t occupancyMaskLo = 0u;
  uint32_t occupancyMaskHi = 0u;
  uint32_t payloadOrSidecar = 0u;
};

// Page-local q8 error range descriptor. The packed child-error words live in a
// separate sidecar stream so visibility tracing can opt in without making every
// ray fetch 64 child errors on the hot path.
struct alignas(16) VoxelTraceErrorSidecarGPU
{
  float errorMin = 0.0f;
  float errorMax = 0.0f;
  uint32_t childErrorWordOffset = kVoxelTraceInvalidOffset;
  uint32_t childErrorWordCount = 0u;
};

// CWBVH-style arbitrary child-bounds trace nodes for installed non-cubic
// virtual-geometry clusters. Bounds8 intentionally stays 80 bytes, matching
// the existing packed CWBVH node budget for eight children. Bounds16 widens the
// same layout to sixteen children for experiments where fewer hierarchy levels
// beat the extra child tests.
struct alignas(16) VoxelTraceBounds8NodeGPU
{
  float originX = 0.0f;
  float originY = 0.0f;
  float originZ = 0.0f;
  uint32_t scaleExponentsAndChildCount = 0u;

  uint32_t childBase = 0u;
  uint32_t payloadBase = 0u;
  uint32_t childMask = 0u;
  uint32_t internalMask = 0u;

  uint32_t qLoX[2]{};
  uint32_t qLoY[2]{};
  uint32_t qLoZ[2]{};
  uint32_t qHiX[2]{};
  uint32_t qHiY[2]{};
  uint32_t qHiZ[2]{};
};

struct alignas(16) VoxelTraceBounds16NodeGPU
{
  float originX = 0.0f;
  float originY = 0.0f;
  float originZ = 0.0f;
  uint32_t scaleExponentsAndChildCount = 0u;

  uint32_t childBase = 0u;
  uint32_t payloadBase = 0u;
  uint32_t childMask = 0u;
  uint32_t internalMask = 0u;

  uint32_t qLoX[4]{};
  uint32_t qLoY[4]{};
  uint32_t qLoZ[4]{};
  uint32_t qHiX[4]{};
  uint32_t qHiY[4]{};
  uint32_t qHiZ[4]{};
};

struct VoxelTraceBoundsChildCPU
{
  std::array<float, 3> minimum{};
  std::array<float, 3> maximum{};
  bool internal = false;
  bool occupied = true;
};

struct VoxelTraceBoundsDecodedChildCPU
{
  std::array<float, 3> minimum{};
  std::array<float, 3> maximum{};
  uint32_t childOrPayload = kVoxelTraceInvalidOffset;
  bool internal = false;
  bool occupied = false;
};

enum class VoxelTraceCullExpansionDecision : uint32_t
{
  ExactFallback = 0u,
  DefinitelySkip = 1u,
  MustExpand = 2u,
};

struct VoxelTraceCullErrorSidecarCPU
{
  float errorMin = 0.0f;
  float errorMax = 0.0f;
  std::vector<uint32_t> childErrorWords{};

  bool empty() const
  {
    return childErrorWords.empty();
  }
};

enum class VoxelTraceTraversalStatus : uint32_t
{
  Miss = 0u,
  Hit = 1u,
  Invalid = 2u,
  QueryLimitExceeded = 3u,
};

enum class VoxelTraceTraversalMode : uint32_t
{
  Exact = 0u,
  ConservativeVisibility = 1u,
};

enum class VoxelTraceTraversalVariant : uint32_t
{
  Child64 = 0u,
  GroupedOctant = 1u,
};

enum class VoxelTraceRuntimeBackend : uint32_t
{
  Tree64 = 0u,
  DirectVirtual = 1u,
};

enum class VoxelTraceErrorSidecarMode : uint32_t
{
  Off = 0u,
  CullOnly = 1u,
  TraceLOD = 2u,
};

struct VoxelTraceRuntimeSettingsCPU
{
  VoxelTraceRuntimeBackend backend = VoxelTraceRuntimeBackend::Tree64;
  uint32_t nodeFormat = kVoxelTraceFormatCubic64;
  VoxelTraceTraversalVariant traversalVariant = VoxelTraceTraversalVariant::Child64;
  VoxelTraceErrorSidecarMode errorSidecarMode = VoxelTraceErrorSidecarMode::Off;
  bool screenTracesEnabled = true;
  bool exactMaterialResolution = true;
};

struct VoxelTraceRuntimeRouteCPU
{
  bool valid = false;
  bool useLegacyTree64 = true;
  bool useDirectVirtualTrace = false;
  bool requireDirectTraceView = false;
  uint32_t traceFormat = kVoxelTraceFormatCubic64;
  VoxelTraceTraversalVariant traversalVariant = VoxelTraceTraversalVariant::Child64;
  bool useCullErrorSidecar = false;
  bool useTraceLODErrorSidecar = false;
  bool screenTracesEnabled = true;
  bool exactMaterialResolution = true;
};

inline bool voxelTraceFormatKnown(const uint32_t format)
{
  return format == kVoxelTraceFormatCubic64 ||
      format == kVoxelTraceFormatBounds8 ||
      format == kVoxelTraceFormatBounds16;
}

inline const char *voxelTraceFormatName(const uint32_t format)
{
  switch (format)
  {
  case kVoxelTraceFormatCubic64:
    return "cubic64";
  case kVoxelTraceFormatBounds8:
    return "bounds8";
  case kVoxelTraceFormatBounds16:
    return "bounds16";
  default:
    return "invalid";
  }
}

inline const char *voxelTraceRuntimeBackendName(const VoxelTraceRuntimeBackend backend)
{
  switch (backend)
  {
  case VoxelTraceRuntimeBackend::Tree64:
    return "tree64";
  case VoxelTraceRuntimeBackend::DirectVirtual:
    return "direct-virtual";
  }
  return "unknown";
}

inline const char *voxelTraceTraversalVariantName(const VoxelTraceTraversalVariant variant)
{
  switch (variant)
  {
  case VoxelTraceTraversalVariant::Child64:
    return "child64";
  case VoxelTraceTraversalVariant::GroupedOctant:
    return "grouped-octant";
  }
  return "unknown";
}

inline const char *voxelTraceErrorSidecarModeName(const VoxelTraceErrorSidecarMode mode)
{
  switch (mode)
  {
  case VoxelTraceErrorSidecarMode::Off:
    return "off";
  case VoxelTraceErrorSidecarMode::CullOnly:
    return "cull-only";
  case VoxelTraceErrorSidecarMode::TraceLOD:
    return "trace-lod";
  }
  return "unknown";
}

inline bool voxelTraceParseFormat(std::string_view value, uint32_t &format)
{
  if (value == "cubic64" || value == "tree64")
  {
    format = kVoxelTraceFormatCubic64;
    return true;
  }
  if (value == "bounds8")
  {
    format = kVoxelTraceFormatBounds8;
    return true;
  }
  if (value == "bounds16")
  {
    format = kVoxelTraceFormatBounds16;
    return true;
  }
  return false;
}

inline bool voxelTraceParseRuntimeBackend(std::string_view value, VoxelTraceRuntimeBackend &backend)
{
  if (value == "tree64")
  {
    backend = VoxelTraceRuntimeBackend::Tree64;
    return true;
  }
  if (value == "direct-virtual" || value == "virtual")
  {
    backend = VoxelTraceRuntimeBackend::DirectVirtual;
    return true;
  }
  return false;
}

inline bool voxelTraceParseTraversalVariant(std::string_view value, VoxelTraceTraversalVariant &variant)
{
  if (value == "child64" || value == "region")
  {
    variant = VoxelTraceTraversalVariant::Child64;
    return true;
  }
  if (value == "grouped-octant")
  {
    variant = VoxelTraceTraversalVariant::GroupedOctant;
    return true;
  }
  return false;
}

inline bool voxelTraceParseErrorSidecarMode(std::string_view value, VoxelTraceErrorSidecarMode &mode)
{
  if (value == "off" || value == "none")
  {
    mode = VoxelTraceErrorSidecarMode::Off;
    return true;
  }
  if (value == "cull-only")
  {
    mode = VoxelTraceErrorSidecarMode::CullOnly;
    return true;
  }
  if (value == "trace-lod")
  {
    mode = VoxelTraceErrorSidecarMode::TraceLOD;
    return true;
  }
  return false;
}

inline bool voxelTraceRuntimeSettingsValid(
    const VoxelTraceRuntimeSettingsCPU &settings,
    std::string *error = nullptr)
{
  const auto fail = [error](const char *message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (!voxelTraceFormatKnown(settings.nodeFormat))
    return fail("virtual trace runtime node format is invalid");
  if (settings.backend == VoxelTraceRuntimeBackend::Tree64 && settings.nodeFormat != kVoxelTraceFormatCubic64)
    return fail("legacy Tree64 backend only supports cubic64 trace format");
  if (settings.traversalVariant == VoxelTraceTraversalVariant::GroupedOctant &&
      settings.nodeFormat != kVoxelTraceFormatCubic64)
    return fail("grouped-octant traversal requires cubic64 trace format");
  if (settings.errorSidecarMode == VoxelTraceErrorSidecarMode::TraceLOD &&
      settings.backend != VoxelTraceRuntimeBackend::DirectVirtual)
    return fail("trace-lod error sidecar requires direct virtual tracing");
  if (error != nullptr)
    error->clear();
  return true;
}

inline VoxelTraceRuntimeRouteCPU voxelTraceRuntimeRoute(
    const VoxelTraceRuntimeSettingsCPU &settings,
    std::string *error = nullptr)
{
  VoxelTraceRuntimeRouteCPU route{};
  route.valid = voxelTraceRuntimeSettingsValid(settings, error);
  if (!route.valid)
    return route;

  route.useDirectVirtualTrace = settings.backend == VoxelTraceRuntimeBackend::DirectVirtual;
  route.useLegacyTree64 = !route.useDirectVirtualTrace;
  route.requireDirectTraceView = route.useDirectVirtualTrace;
  route.traceFormat = settings.nodeFormat;
  route.traversalVariant = settings.traversalVariant;
  route.useCullErrorSidecar = settings.errorSidecarMode != VoxelTraceErrorSidecarMode::Off;
  route.useTraceLODErrorSidecar = settings.errorSidecarMode == VoxelTraceErrorSidecarMode::TraceLOD;
  route.screenTracesEnabled = settings.screenTracesEnabled;
  route.exactMaterialResolution = settings.exactMaterialResolution;
  if (error != nullptr)
    error->clear();
  return route;
}

struct VoxelTraceRayCPU
{
  std::array<float, 3> origin{};
  std::array<float, 3> direction{};
  float tMin = 0.0f;
  float tMax = std::numeric_limits<float>::max();
};

struct VoxelTraceTraversalOptionsCPU
{
  uint32_t expectedGeneration = std::numeric_limits<uint32_t>::max();
  uint32_t maxNodeVisits = 1024u;
  uint32_t skipPayload = kVoxelTraceInvalidOffset;
  VoxelTraceTraversalMode mode = VoxelTraceTraversalMode::Exact;
  VoxelTraceTraversalVariant variant = VoxelTraceTraversalVariant::Child64;
  bool allowExactCoarseFallback = false;
  float projectedErrorThreshold = 0.0f;
  float minimumCameraDistance = 0.001f;
  std::array<float, 3> cameraPosition{};
  const std::vector<VoxelTraceCullErrorSidecarCPU> *nodeErrorSidecars = nullptr;
  bool sortCandidatesByError = true;
};

struct VoxelTraceHitCPU
{
  VoxelTraceTraversalStatus status = VoxelTraceTraversalStatus::Miss;
  float t = std::numeric_limits<float>::max();
  uint32_t nodeIndex = kVoxelTraceInvalidOffset;
  uint32_t childIndex = kVoxelTraceInvalidOffset;
  uint32_t payload = kVoxelTraceInvalidOffset;
  uint32_t depth = 0u;
  uint32_t nodeVisits = 0u;
  uint32_t childTests = 0u;
  uint32_t groupTests = 0u;
  uint32_t skippedChildGroups = 0u;
  bool coarseFallback = false;
  float error = std::numeric_limits<float>::max();
};

inline uint32_t voxelTraceNode64ChildIndex(
    const uint32_t x,
    const uint32_t y,
    const uint32_t z)
{
  return x + kVoxelTraceBranching * y + kVoxelTraceBranching * kVoxelTraceBranching * z;
}

inline uint64_t voxelTraceNode64PopulationMask(const VoxelTraceNode64GPU &node)
{
  return uint64_t{node.occupancyMaskLo} | (uint64_t{node.occupancyMaskHi} << 32u);
}

inline void voxelTraceNode64SetPopulationMask(
    VoxelTraceNode64GPU &node,
    const uint64_t mask)
{
  node.occupancyMaskLo = static_cast<uint32_t>(mask & 0xffff'ffffull);
  node.occupancyMaskHi = static_cast<uint32_t>(mask >> 32u);
}

inline uint32_t voxelTracePopulationCount64(uint64_t value)
{
  uint32_t count = 0u;
  while (value != 0u)
  {
    value &= value - 1u;
    ++count;
  }
  return count;
}

inline bool voxelTraceNode64PopulationContains(
    const VoxelTraceNode64GPU &node,
    const uint32_t childIndex)
{
  if (childIndex >= kVoxelTraceChildCount)
    return false;
  return (voxelTraceNode64PopulationMask(node) & (uint64_t{1u} << childIndex)) != 0u;
}

inline uint32_t voxelTraceNode64PopulationCountBefore(
    const VoxelTraceNode64GPU &node,
    const uint32_t childIndex)
{
  if (childIndex == 0u)
    return 0u;
  const uint64_t prefix = childIndex >= kVoxelTraceChildCount
      ? std::numeric_limits<uint64_t>::max()
      : (uint64_t{1u} << childIndex) - 1u;
  return voxelTracePopulationCount64(voxelTraceNode64PopulationMask(node) & prefix);
}

inline bool voxelTraceNode64Terminal(const VoxelTraceNode64GPU &node)
{
  return (node.childBaseAndFlags & kVoxelTraceNode64TerminalFlag) != 0u;
}

inline uint32_t voxelTraceNode64ChildBase(const VoxelTraceNode64GPU &node)
{
  return node.childBaseAndFlags >> 1u;
}

inline VoxelTraceNode64GPU voxelTraceNode64MakeInternal(
    const uint32_t childBase,
    const uint64_t occupancyMask,
    const uint32_t payloadOrSidecar = 0u)
{
  VoxelTraceNode64GPU node{};
  node.childBaseAndFlags = childBase << 1u;
  voxelTraceNode64SetPopulationMask(node, occupancyMask);
  node.payloadOrSidecar = payloadOrSidecar;
  return node;
}

inline VoxelTraceNode64GPU voxelTraceNode64MakeTerminal(
    const uint64_t occupancyMask,
    const uint32_t payloadOrSidecar = 0u)
{
  VoxelTraceNode64GPU node{};
  node.childBaseAndFlags = kVoxelTraceNode64TerminalFlag;
  voxelTraceNode64SetPopulationMask(node, occupancyMask);
  node.payloadOrSidecar = payloadOrSidecar;
  return node;
}

inline uint32_t voxelTracePackQ8Error4(
    const uint8_t x,
    const uint8_t y,
    const uint8_t z,
    const uint8_t w)
{
  return uint32_t{x} |
      (uint32_t{y} << 8u) |
      (uint32_t{z} << 16u) |
      (uint32_t{w} << 24u);
}

inline uint8_t voxelTraceUnpackQ8Error(
    const uint32_t packed,
    const uint32_t lane)
{
  return static_cast<uint8_t>((packed >> ((lane & 3u) * 8u)) & 0xffu);
}

inline uint8_t voxelTraceEncodeQ8Error(
    const float error,
    const float errorMin,
    const float errorMax)
{
  if (!std::isfinite(error) || !std::isfinite(errorMin) || !std::isfinite(errorMax))
    return 0u;
  const float range = errorMax - errorMin;
  if (!(range > 0.0f))
    return 0u;
  const float normalized = std::clamp((error - errorMin) / range, 0.0f, 1.0f);
  return static_cast<uint8_t>(std::lround(normalized * 255.0f));
}

inline float voxelTraceDecodeQ8Error(
    const uint8_t encoded,
    const float errorMin,
    const float errorMax)
{
  if (!std::isfinite(errorMin) || !std::isfinite(errorMax))
    return 0.0f;
  const float range = errorMax - errorMin;
  if (!(range > 0.0f))
    return errorMin;
  return errorMin + (static_cast<float>(encoded) / 255.0f) * range;
}

inline uint32_t voxelTraceQ8ErrorWordCount(const uint32_t childCount)
{
  return (childCount + 3u) / 4u;
}

inline uint32_t voxelTracePack4U8(
    const uint32_t x,
    const uint32_t y,
    const uint32_t z,
    const uint32_t w)
{
  return (x & 0xffu) |
      ((y & 0xffu) << 8u) |
      ((z & 0xffu) << 16u) |
      ((w & 0xffu) << 24u);
}

inline int32_t voxelTraceDecodeSignedU8(const uint32_t value)
{
  const uint32_t byteValue = value & 0xffu;
  return byteValue >= 128u
      ? static_cast<int32_t>(byteValue) - 256
      : static_cast<int32_t>(byteValue);
}

inline uint8_t voxelTraceEncodeSignedExponent8(const float extent)
{
  if (!(extent > 0.0f) || !std::isfinite(extent))
    return 0u;
  int32_t exponent = std::clamp(
      static_cast<int32_t>(std::ceil(std::log2(extent / 255.0f))),
      -128,
      127);
  const float scale = std::exp2(static_cast<float>(exponent));
  if (exponent < 127 && std::ceil(extent / scale) > 255.0f)
    ++exponent;
  return static_cast<uint8_t>(exponent < 0 ? exponent + 256 : exponent);
}

inline float voxelTraceDecodeExponentScale(const uint8_t encodedExponent)
{
  return std::exp2(static_cast<float>(voxelTraceDecodeSignedU8(encodedExponent)));
}

inline uint8_t voxelTraceBoundsQuantizeLower(
    const float value,
    const float parentMin,
    const float scale)
{
  if (!(scale > 0.0f) || !std::isfinite(value) || !std::isfinite(parentMin))
    return 0u;
  uint32_t q = static_cast<uint32_t>(std::clamp(
      std::floor((value - parentMin) / scale),
      0.0f,
      255.0f));
  if (q > 0u)
    --q;
  return static_cast<uint8_t>(q);
}

inline uint8_t voxelTraceBoundsQuantizeUpper(
    const float value,
    const float parentMin,
    const float scale)
{
  if (!(scale > 0.0f) || !std::isfinite(value) || !std::isfinite(parentMin))
    return 0u;
  uint32_t q = static_cast<uint32_t>(std::clamp(
      std::ceil((value - parentMin) / scale),
      0.0f,
      255.0f));
  if (q < 255u)
    ++q;
  return static_cast<uint8_t>(q);
}

inline uint32_t voxelTraceBoundsPackScaleExponents(
    const uint8_t x,
    const uint8_t y,
    const uint8_t z,
    const uint32_t childCount)
{
  return voxelTracePack4U8(x, y, z, childCount);
}

inline uint8_t voxelTraceBoundsScaleExponent(
    const uint32_t scaleExponentsAndChildCount,
    const uint32_t axis)
{
  return voxelTraceUnpackQ8Error(scaleExponentsAndChildCount, axis);
}

inline uint32_t voxelTraceBoundsChildCount(const uint32_t scaleExponentsAndChildCount)
{
  return voxelTraceUnpackQ8Error(scaleExponentsAndChildCount, 3u);
}

inline uint32_t voxelTraceBoundsWordCount(const uint32_t childCount)
{
  return (childCount + 3u) / 4u;
}

inline uint8_t voxelTraceBoundsUnpackQ8(
    const uint32_t *words,
    const uint32_t slot)
{
  return voxelTraceUnpackQ8Error(words[slot >> 2u], slot & 3u);
}

template <typename BoundsNode>
inline std::array<float, 3> voxelTraceBoundsNodeOrigin(const BoundsNode &node)
{
  return {node.originX, node.originY, node.originZ};
}

template <typename BoundsNode>
inline std::array<float, 3> voxelTraceBoundsNodeScale(const BoundsNode &node)
{
  return {
    voxelTraceDecodeExponentScale(voxelTraceBoundsScaleExponent(node.scaleExponentsAndChildCount, 0u)),
    voxelTraceDecodeExponentScale(voxelTraceBoundsScaleExponent(node.scaleExponentsAndChildCount, 1u)),
    voxelTraceDecodeExponentScale(voxelTraceBoundsScaleExponent(node.scaleExponentsAndChildCount, 2u)),
  };
}

template <typename BoundsNode>
inline bool voxelTraceBoundsDecodeChild(
    const BoundsNode &node,
    const uint32_t maxChildren,
    const uint32_t slot,
    VoxelTraceBoundsDecodedChildCPU &child)
{
  child = {};
  const uint32_t childCount = std::min(voxelTraceBoundsChildCount(node.scaleExponentsAndChildCount), maxChildren);
  if (slot >= childCount || (node.childMask & (1u << slot)) == 0u)
    return false;

  const auto origin = voxelTraceBoundsNodeOrigin(node);
  const auto scale = voxelTraceBoundsNodeScale(node);
  const uint8_t qLoX = voxelTraceBoundsUnpackQ8(node.qLoX, slot);
  const uint8_t qLoY = voxelTraceBoundsUnpackQ8(node.qLoY, slot);
  const uint8_t qLoZ = voxelTraceBoundsUnpackQ8(node.qLoZ, slot);
  const uint8_t qHiX = voxelTraceBoundsUnpackQ8(node.qHiX, slot);
  const uint8_t qHiY = voxelTraceBoundsUnpackQ8(node.qHiY, slot);
  const uint8_t qHiZ = voxelTraceBoundsUnpackQ8(node.qHiZ, slot);

  child.minimum = {
    origin[0] + static_cast<float>(qLoX) * scale[0],
    origin[1] + static_cast<float>(qLoY) * scale[1],
    origin[2] + static_cast<float>(qLoZ) * scale[2],
  };
  child.maximum = {
    origin[0] + static_cast<float>(qHiX) * scale[0],
    origin[1] + static_cast<float>(qHiY) * scale[1],
    origin[2] + static_cast<float>(qHiZ) * scale[2],
  };
  child.internal = (node.internalMask & (1u << slot)) != 0u;
  child.occupied = true;
  const uint32_t precedingMask = slot == 0u ? 0u : ((1u << slot) - 1u);
  if (child.internal)
    child.childOrPayload = node.childBase + voxelTracePopulationCount64(node.internalMask & precedingMask);
  else
    child.childOrPayload = node.payloadBase + voxelTracePopulationCount64((node.childMask & ~node.internalMask) & precedingMask);
  return true;
}

template <typename BoundsNode>
inline bool voxelTraceBuildBoundsNode(
    const std::array<float, 3> &parentMinimum,
    const std::array<float, 3> &parentMaximum,
    const std::vector<VoxelTraceBoundsChildCPU> &children,
    const uint32_t maxChildren,
    const uint32_t childBase,
    const uint32_t payloadBase,
    BoundsNode &node,
    std::string *error = nullptr)
{
  node = {};
  if (children.empty() || children.size() > maxChildren)
  {
    if (error != nullptr)
      *error = "voxel trace bounds node needs 1..maxChildren children";
    return false;
  }

  std::array<uint8_t, 3> exponents{};
  std::array<float, 3> scale{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(parentMinimum[axis]) || !std::isfinite(parentMaximum[axis]) || parentMaximum[axis] < parentMinimum[axis])
    {
      if (error != nullptr)
        *error = "voxel trace bounds parent bounds are invalid";
      return false;
    }
    exponents[axis] = voxelTraceEncodeSignedExponent8(parentMaximum[axis] - parentMinimum[axis]);
    scale[axis] = voxelTraceDecodeExponentScale(exponents[axis]);
  }

  node.originX = parentMinimum[0];
  node.originY = parentMinimum[1];
  node.originZ = parentMinimum[2];
  node.scaleExponentsAndChildCount = voxelTraceBoundsPackScaleExponents(
      exponents[0],
      exponents[1],
      exponents[2],
      static_cast<uint32_t>(children.size()));
  node.childBase = childBase;
  node.payloadBase = payloadBase;

  for (uint32_t slot = 0u; slot < static_cast<uint32_t>(children.size()); ++slot)
  {
    const auto &child = children[slot];
    if (!child.occupied)
      continue;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      if (!std::isfinite(child.minimum[axis]) || !std::isfinite(child.maximum[axis]) || child.maximum[axis] < child.minimum[axis])
      {
        if (error != nullptr)
          *error = "voxel trace bounds child bounds are invalid";
        return false;
      }
      const float quantizedParentMaximum = parentMinimum[axis] + 255.0f * scale[axis];
      if (child.minimum[axis] < parentMinimum[axis] || child.maximum[axis] > quantizedParentMaximum)
      {
        if (error != nullptr)
          *error = "voxel trace bounds child exceeds quantized parent range";
        return false;
      }
    }

    const uint32_t word = slot >> 2u;
    const uint32_t shift = (slot & 3u) * 8u;
    node.childMask |= 1u << slot;
    if (child.internal)
      node.internalMask |= 1u << slot;
    node.qLoX[word] |= uint32_t{voxelTraceBoundsQuantizeLower(child.minimum[0], parentMinimum[0], scale[0])} << shift;
    node.qLoY[word] |= uint32_t{voxelTraceBoundsQuantizeLower(child.minimum[1], parentMinimum[1], scale[1])} << shift;
    node.qLoZ[word] |= uint32_t{voxelTraceBoundsQuantizeLower(child.minimum[2], parentMinimum[2], scale[2])} << shift;
    node.qHiX[word] |= uint32_t{voxelTraceBoundsQuantizeUpper(child.maximum[0], parentMinimum[0], scale[0])} << shift;
    node.qHiY[word] |= uint32_t{voxelTraceBoundsQuantizeUpper(child.maximum[1], parentMinimum[1], scale[1])} << shift;
    node.qHiZ[word] |= uint32_t{voxelTraceBoundsQuantizeUpper(child.maximum[2], parentMinimum[2], scale[2])} << shift;
  }

  if (error != nullptr)
    error->clear();
  return true;
}

inline bool voxelTraceBuildCullErrorSidecar(
    const std::vector<float> &childErrors,
    VoxelTraceCullErrorSidecarCPU &sidecar,
    std::string *error = nullptr)
{
  sidecar = {};
  if (childErrors.empty() || childErrors.size() > kVoxelTraceChildCount)
  {
    if (error != nullptr)
      *error = "voxel trace cull sidecar needs 1..64 child errors";
    return false;
  }

  float errorMin = std::numeric_limits<float>::max();
  float errorMax = -std::numeric_limits<float>::max();
  for (const float value : childErrors)
  {
    if (!std::isfinite(value))
    {
      if (error != nullptr)
        *error = "voxel trace cull sidecar child error is not finite";
      return false;
    }
    errorMin = std::min(errorMin, value);
    errorMax = std::max(errorMax, value);
  }

  sidecar.errorMin = errorMin;
  sidecar.errorMax = errorMax;
  sidecar.childErrorWords.assign(
      voxelTraceQ8ErrorWordCount(static_cast<uint32_t>(childErrors.size())),
      0u);
  for (uint32_t child = 0u; child < static_cast<uint32_t>(childErrors.size()); ++child)
  {
    const uint8_t encoded =
        voxelTraceEncodeQ8Error(childErrors[child], errorMin, errorMax);
    const uint32_t lane = child & 3u;
    const uint32_t word = child >> 2u;
    sidecar.childErrorWords[word] &= ~(0xffu << (lane * 8u));
    sidecar.childErrorWords[word] |= uint32_t{encoded} << (lane * 8u);
  }

  if (error != nullptr)
    error->clear();
  return true;
}

inline uint8_t voxelTraceCullSidecarEncodedError(
    const VoxelTraceCullErrorSidecarCPU &sidecar,
    const uint32_t childIndex)
{
  const uint32_t word = childIndex >> 2u;
  if (word >= sidecar.childErrorWords.size())
    return 0u;
  return voxelTraceUnpackQ8Error(sidecar.childErrorWords[word], childIndex & 3u);
}

inline float voxelTraceCullSidecarDecodedError(
    const VoxelTraceCullErrorSidecarCPU &sidecar,
    const uint32_t childIndex)
{
  return voxelTraceDecodeQ8Error(
      voxelTraceCullSidecarEncodedError(sidecar, childIndex),
      sidecar.errorMin,
      sidecar.errorMax);
}

inline VoxelTraceCullExpansionDecision voxelTraceCullExpansionDecisionQ8(
    const uint8_t encodedError,
    const float errorMin,
    const float errorMax,
    const float threshold)
{
  if (!std::isfinite(errorMin) || !std::isfinite(errorMax) || !std::isfinite(threshold))
    return VoxelTraceCullExpansionDecision::ExactFallback;
  const float range = errorMax - errorMin;
  if (!(range > 0.0f))
    return errorMin > threshold
        ? VoxelTraceCullExpansionDecision::MustExpand
        : VoxelTraceCullExpansionDecision::DefinitelySkip;

  const float decoded = voxelTraceDecodeQ8Error(encodedError, errorMin, errorMax);
  const float halfStep = range / 510.0f;
  const float possibleMin =
      encodedError == 0u ? errorMin : std::max(errorMin, decoded - halfStep);
  const float possibleMax =
      encodedError == 255u ? errorMax : std::min(errorMax, decoded + halfStep);
  if (possibleMax <= threshold)
    return VoxelTraceCullExpansionDecision::DefinitelySkip;
  if (possibleMin > threshold)
    return VoxelTraceCullExpansionDecision::MustExpand;
  return VoxelTraceCullExpansionDecision::ExactFallback;
}

inline VoxelTraceCullExpansionDecision voxelTraceCullExpansionDecision(
    const VoxelTraceCullErrorSidecarCPU *sidecar,
    const uint32_t childIndex,
    const float threshold)
{
  if (sidecar == nullptr || sidecar->empty() || !std::isfinite(threshold))
    return VoxelTraceCullExpansionDecision::ExactFallback;
  const uint32_t word = childIndex >> 2u;
  if (word >= sidecar->childErrorWords.size())
    return VoxelTraceCullExpansionDecision::ExactFallback;
  return voxelTraceCullExpansionDecisionQ8(
      voxelTraceCullSidecarEncodedError(*sidecar, childIndex),
      sidecar->errorMin,
      sidecar->errorMax,
      threshold);
}

inline bool voxelTraceRayAabb(
    const VoxelTraceRayCPU &ray,
    const std::array<float, 3> &minimum,
    const std::array<float, 3> &maximum,
    float &nearT,
    float &farT)
{
  nearT = ray.tMin;
  farT = ray.tMax;
  if (!std::isfinite(ray.tMin) || !std::isfinite(ray.tMax) || ray.tMax < ray.tMin)
    return false;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float origin = ray.origin[axis];
    const float direction = ray.direction[axis];
    const float minValue = minimum[axis];
    const float maxValue = maximum[axis];
    if (!std::isfinite(origin) || !std::isfinite(direction) || !std::isfinite(minValue) || !std::isfinite(maxValue) || maxValue < minValue)
      return false;
    if (std::fabs(direction) <= 1.0e-8f)
    {
      if (origin < minValue || origin > maxValue)
        return false;
      continue;
    }
    const float invDirection = 1.0f / direction;
    float axisNear = (minValue - origin) * invDirection;
    float axisFar = (maxValue - origin) * invDirection;
    if (axisNear > axisFar)
      std::swap(axisNear, axisFar);
    nearT = std::max(nearT, axisNear);
    farT = std::min(farT, axisFar);
    if (nearT > farT)
      return false;
  }
  return true;
}

inline std::array<uint32_t, 3> voxelTraceNode64ChildCoord(const uint32_t childIndex)
{
  return {
    childIndex & 3u,
    (childIndex >> 2u) & 3u,
    (childIndex >> 4u) & 3u,
  };
}

inline uint32_t voxelTraceNode64ChildOctant(const uint32_t childIndex)
{
  const auto coord = voxelTraceNode64ChildCoord(childIndex);
  return (coord[0] >> 1u) | ((coord[1] >> 1u) << 1u) | ((coord[2] >> 1u) << 2u);
}

inline uint64_t voxelTraceNode64OctantMask(const uint32_t octant)
{
  if (octant >= 8u)
    return 0u;
  uint64_t mask = 0u;
  const uint32_t baseX = (octant & 1u) != 0u ? 2u : 0u;
  const uint32_t baseY = (octant & 2u) != 0u ? 2u : 0u;
  const uint32_t baseZ = (octant & 4u) != 0u ? 2u : 0u;
  for (uint32_t z = 0u; z < 2u; ++z)
    for (uint32_t y = 0u; y < 2u; ++y)
      for (uint32_t x = 0u; x < 2u; ++x)
        mask |= uint64_t{1u} << voxelTraceNode64ChildIndex(baseX + x, baseY + y, baseZ + z);
  return mask;
}

inline std::array<float, 3> voxelTraceNode64ChildOrigin(
    const std::array<float, 3> &nodeOrigin,
    const float nodeSide,
    const uint32_t childIndex)
{
  const auto coord = voxelTraceNode64ChildCoord(childIndex);
  const float childSide = nodeSide / static_cast<float>(kVoxelTraceBranching);
  return {
    nodeOrigin[0] + static_cast<float>(coord[0]) * childSide,
    nodeOrigin[1] + static_cast<float>(coord[1]) * childSide,
    nodeOrigin[2] + static_cast<float>(coord[2]) * childSide,
  };
}

inline std::array<float, 3> voxelTraceNode64OctantOrigin(
    const std::array<float, 3> &nodeOrigin,
    const float nodeSide,
    const uint32_t octant)
{
  const float octantSide = nodeSide * 0.5f;
  return {
    nodeOrigin[0] + ((octant & 1u) != 0u ? octantSide : 0.0f),
    nodeOrigin[1] + ((octant & 2u) != 0u ? octantSide : 0.0f),
    nodeOrigin[2] + ((octant & 4u) != 0u ? octantSide : 0.0f),
  };
}

inline float voxelTraceDistanceToAabb(
    const std::array<float, 3> &point,
    const std::array<float, 3> &minimum,
    const std::array<float, 3> &maximum)
{
  float squared = 0.0f;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(point[axis]) || !std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]))
      return std::numeric_limits<float>::max();
    float delta = 0.0f;
    if (point[axis] < minimum[axis])
      delta = minimum[axis] - point[axis];
    else if (point[axis] > maximum[axis])
      delta = point[axis] - maximum[axis];
    squared += delta * delta;
  }
  return std::sqrt(squared);
}

inline float voxelTraceProjectedErrorDistanceThreshold(
    const VoxelTraceTraversalOptionsCPU &options,
    const std::array<float, 3> &minimum,
    const std::array<float, 3> &maximum)
{
  if (!std::isfinite(options.projectedErrorThreshold) || !(options.projectedErrorThreshold > 0.0f))
    return 0.0f;
  const float distance = std::max(
      voxelTraceDistanceToAabb(options.cameraPosition, minimum, maximum),
      std::max(options.minimumCameraDistance, 0.000001f));
  return options.projectedErrorThreshold * distance;
}

inline const VoxelTraceCullErrorSidecarCPU *voxelTraceSidecarForNode(
    const VoxelTracePageGPU &page,
    const uint32_t nodeIndex,
    const VoxelTraceTraversalOptionsCPU &options)
{
  if (options.nodeErrorSidecars == nullptr || nodeIndex < page.traceNodeOffset)
    return nullptr;
  const uint32_t localIndex = nodeIndex - page.traceNodeOffset;
  if (localIndex >= options.nodeErrorSidecars->size())
    return nullptr;
  const auto &sidecar = (*options.nodeErrorSidecars)[localIndex];
  return sidecar.empty() ? nullptr : &sidecar;
}

inline bool voxelTraceChildDecodedError(
    const VoxelTracePageGPU &page,
    const uint32_t nodeIndex,
    const uint32_t childIndex,
    const VoxelTraceTraversalOptionsCPU &options,
    float &error)
{
  error = std::numeric_limits<float>::max();
  const auto *sidecar = voxelTraceSidecarForNode(page, nodeIndex, options);
  if (sidecar == nullptr)
    return false;
  const uint32_t word = childIndex >> 2u;
  if (word >= sidecar->childErrorWords.size())
    return false;
  error = voxelTraceCullSidecarDecodedError(*sidecar, childIndex);
  return std::isfinite(error);
}

inline bool voxelTraceAllowsCoarseFallback(const VoxelTraceTraversalOptionsCPU &options)
{
  return options.mode == VoxelTraceTraversalMode::ConservativeVisibility ||
      options.allowExactCoarseFallback;
}

inline bool voxelTraceFormatCanBeImplicitFallbackFor(
    const uint32_t candidateFormat,
    const uint32_t requestedFormat,
    const uint32_t pageFlags)
{
  if (candidateFormat == requestedFormat)
    return true;
  const bool conservative = (pageFlags & kVoxelTracePageConservativeFallback) != 0u;
  if (!conservative)
    return false;
  if (requestedFormat == kVoxelTraceFormatCubic64)
    return candidateFormat == kVoxelTraceFormatBounds8 || candidateFormat == kVoxelTraceFormatBounds16;
  return false;
}

inline bool voxelTraceUsesErrorOrdering(const VoxelTraceTraversalOptionsCPU &options)
{
  return options.sortCandidatesByError && voxelTraceAllowsCoarseFallback(options);
}

inline VoxelTraceCullExpansionDecision voxelTraceChildExpansionDecision(
    const VoxelTracePageGPU &page,
    const uint32_t nodeIndex,
    const uint32_t childIndex,
    const std::array<float, 3> &childMinimum,
    const std::array<float, 3> &childMaximum,
    const VoxelTraceTraversalOptionsCPU &options)
{
  if (!voxelTraceAllowsCoarseFallback(options))
    return VoxelTraceCullExpansionDecision::MustExpand;
  const auto *sidecar = voxelTraceSidecarForNode(page, nodeIndex, options);
  if (sidecar == nullptr)
    return VoxelTraceCullExpansionDecision::ExactFallback;
  const float distanceThreshold =
      voxelTraceProjectedErrorDistanceThreshold(options, childMinimum, childMaximum);
  if (!(distanceThreshold > 0.0f))
    return VoxelTraceCullExpansionDecision::ExactFallback;
  return voxelTraceCullExpansionDecision(sidecar, childIndex, distanceThreshold);
}

inline bool voxelTracePageValidForCubic64(
    const VoxelTracePageGPU &page,
    const std::vector<VoxelTraceNode64GPU> &nodes,
    const VoxelTraceTraversalOptionsCPU &options)
{
  if ((page.flags & kVoxelTracePageValid) == 0u || page.format != kVoxelTraceFormatCubic64)
    return false;
  if (options.expectedGeneration != std::numeric_limits<uint32_t>::max() && page.generation != options.expectedGeneration)
    return false;
  if (!std::isfinite(page.localOriginX) || !std::isfinite(page.localOriginY) || !std::isfinite(page.localOriginZ) || !std::isfinite(page.rootSide) || !(page.rootSide > 0.0f))
    return false;
  if (page.rootNode >= page.traceNodeCount)
    return false;
  const uint64_t first = page.traceNodeOffset;
  const uint64_t count = page.traceNodeCount;
  if (count == 0u || first + count > nodes.size())
    return false;
  return true;
}

inline bool voxelTraceCubic64(
    const VoxelTracePageGPU &page,
    const std::vector<VoxelTraceNode64GPU> &nodes,
    const VoxelTraceRayCPU &ray,
    const VoxelTraceTraversalOptionsCPU &options,
    VoxelTraceHitCPU &hit)
{
  hit = {};
  if (!voxelTracePageValidForCubic64(page, nodes, options))
  {
    hit.status = VoxelTraceTraversalStatus::Invalid;
    return false;
  }

  const std::array<float, 3> rootOrigin{page.localOriginX, page.localOriginY, page.localOriginZ};
  const std::array<float, 3> rootMax{
    page.localOriginX + page.rootSide,
    page.localOriginY + page.rootSide,
    page.localOriginZ + page.rootSide,
  };
  float rootNear = 0.0f;
  float rootFar = 0.0f;
  if (!voxelTraceRayAabb(ray, rootOrigin, rootMax, rootNear, rootFar))
  {
    hit.status = VoxelTraceTraversalStatus::Miss;
    return true;
  }

  struct Candidate
  {
    uint32_t nodeIndex = 0u;
    std::array<float, 3> origin{};
    float side = 0.0f;
    float nearT = 0.0f;
    float error = std::numeric_limits<float>::max();
    uint32_t depth = 0u;
    bool errorKnown = false;
  };

  std::vector<Candidate> stack;
  stack.push_back({page.traceNodeOffset + page.rootNode, rootOrigin, page.rootSide, rootNear, 0.0f, 0u, true});
  bool limited = false;
  const bool errorOrdering = voxelTraceUsesErrorOrdering(options);
  while (!stack.empty())
  {
    const Candidate current = stack.back();
    stack.pop_back();
    if (!errorOrdering && current.nearT > hit.t)
      continue;
    if (errorOrdering && current.errorKnown && hit.status == VoxelTraceTraversalStatus::Hit && current.error > hit.error)
      continue;
    if (hit.nodeVisits >= options.maxNodeVisits)
    {
      limited = true;
      break;
    }
    ++hit.nodeVisits;
    if (current.nodeIndex < page.traceNodeOffset || current.nodeIndex >= page.traceNodeOffset + page.traceNodeCount || current.nodeIndex >= nodes.size())
    {
      hit.status = VoxelTraceTraversalStatus::Invalid;
      return false;
    }

    const VoxelTraceNode64GPU &node = nodes[current.nodeIndex];
    const float childSide = current.side / static_cast<float>(kVoxelTraceBranching);
    std::array<bool, 8u> octantHits{};
    octantHits.fill(true);
    if (options.variant == VoxelTraceTraversalVariant::GroupedOctant)
    {
      octantHits.fill(false);
      const uint64_t nodeMask = voxelTraceNode64PopulationMask(node);
      const float octantSide = current.side * 0.5f;
      for (uint32_t octant = 0u; octant < 8u; ++octant)
      {
        if ((nodeMask & voxelTraceNode64OctantMask(octant)) == 0u)
          continue;
        ++hit.groupTests;
        const auto octantOrigin = voxelTraceNode64OctantOrigin(current.origin, current.side, octant);
        const std::array<float, 3> octantMax{
          octantOrigin[0] + octantSide,
          octantOrigin[1] + octantSide,
          octantOrigin[2] + octantSide,
        };
        float octantNear = 0.0f;
        float octantFar = 0.0f;
        octantHits[octant] =
            voxelTraceRayAabb(ray, octantOrigin, octantMax, octantNear, octantFar) &&
            (errorOrdering || octantNear <= hit.t);
      }
    }
    struct ChildHit
    {
      uint32_t child = 0u;
      uint32_t nodeIndex = 0u;
      uint32_t payload = 0u;
      std::array<float, 3> origin{};
      float nearT = 0.0f;
      float error = std::numeric_limits<float>::max();
      bool terminal = false;
      bool coarseFallback = false;
      bool errorKnown = false;
    };
    std::vector<ChildHit> children;
    for (uint32_t child = 0u; child < kVoxelTraceChildCount; ++child)
    {
      if (!voxelTraceNode64PopulationContains(node, child))
        continue;
      if (!octantHits[voxelTraceNode64ChildOctant(child)])
      {
        ++hit.skippedChildGroups;
        continue;
      }
      ++hit.childTests;
      const auto childOrigin = voxelTraceNode64ChildOrigin(current.origin, current.side, child);
      const std::array<float, 3> childMax{
        childOrigin[0] + childSide,
        childOrigin[1] + childSide,
        childOrigin[2] + childSide,
      };
      float childNear = 0.0f;
      float childFar = 0.0f;
      if (!voxelTraceRayAabb(ray, childOrigin, childMax, childNear, childFar) ||
          (!errorOrdering && childNear > hit.t))
        continue;
      float childError = std::numeric_limits<float>::max();
      const bool childErrorKnown = voxelTraceChildDecodedError(
          page,
          current.nodeIndex,
          child,
          options,
          childError);
      if (errorOrdering && childErrorKnown && hit.status == VoxelTraceTraversalStatus::Hit && childError > hit.error)
        continue;

      if (voxelTraceNode64Terminal(node))
      {
        const uint32_t payload = node.payloadOrSidecar + child;
        if (payload == options.skipPayload)
          continue;
        children.push_back({child, current.nodeIndex, payload, childOrigin, childNear, 0.0f, true, false, true});
      }
      else
      {
        const auto decision = voxelTraceChildExpansionDecision(
            page,
            current.nodeIndex,
            child,
            childOrigin,
            childMax,
            options);
        if (decision == VoxelTraceCullExpansionDecision::DefinitelySkip)
        {
          children.push_back({child, current.nodeIndex, kVoxelTraceInvalidOffset, childOrigin, childNear, childError, true, true, childErrorKnown});
          continue;
        }
        const uint32_t compactOffset = voxelTraceNode64PopulationCountBefore(node, child);
        const uint32_t childNode = page.traceNodeOffset + voxelTraceNode64ChildBase(node) + compactOffset;
        children.push_back({child, childNode, kVoxelTraceInvalidOffset, childOrigin, childNear, childError, false, false, childErrorKnown});
      }
    }

    std::sort(
        children.begin(),
        children.end(),
        [errorOrdering](const ChildHit &left, const ChildHit &right)
        {
          if (errorOrdering)
          {
            const float leftError = left.errorKnown ? left.error : std::numeric_limits<float>::max();
            const float rightError = right.errorKnown ? right.error : std::numeric_limits<float>::max();
            if (leftError != rightError)
              return leftError < rightError;
          }
          if (left.nearT != right.nearT)
            return left.nearT < right.nearT;
          return left.child < right.child;
        });

    for (auto childIt = children.rbegin(); childIt != children.rend(); ++childIt)
    {
      const auto &child = *childIt;
      if (child.terminal)
      {
        const bool improvesError =
            errorOrdering &&
            child.errorKnown &&
            (hit.status != VoxelTraceTraversalStatus::Hit || child.error < hit.error);
        const bool tiesOrImprovesDistance =
            hit.status != VoxelTraceTraversalStatus::Hit ||
            child.nearT <= hit.t;
        if (improvesError || (!errorOrdering && tiesOrImprovesDistance) ||
            (errorOrdering && child.error <= hit.error && tiesOrImprovesDistance))
        {
          hit.status = VoxelTraceTraversalStatus::Hit;
          hit.t = child.nearT;
          hit.nodeIndex = child.nodeIndex;
          hit.childIndex = child.child;
          hit.payload = child.payload;
          hit.depth = current.depth + 1u;
          hit.coarseFallback = child.coarseFallback;
          hit.error = child.error;
        }
      }
      else
      {
        stack.push_back({child.nodeIndex, child.origin, childSide, child.nearT, child.error, current.depth + 1u, child.errorKnown});
      }
    }
  }

  if (limited && hit.status != VoxelTraceTraversalStatus::Hit)
    hit.status = VoxelTraceTraversalStatus::QueryLimitExceeded;
  else if (hit.status != VoxelTraceTraversalStatus::Hit)
    hit.status = VoxelTraceTraversalStatus::Miss;
  return hit.status != VoxelTraceTraversalStatus::Invalid;
}

template <typename BoundsNode>
inline bool voxelTracePageValidForBounds(
    const VoxelTracePageGPU &page,
    const std::vector<BoundsNode> &nodes,
    const VoxelTraceTraversalOptionsCPU &options,
    const uint32_t expectedFormat)
{
  if ((page.flags & kVoxelTracePageValid) == 0u || page.format != expectedFormat)
    return false;
  if (options.expectedGeneration != std::numeric_limits<uint32_t>::max() && page.generation != options.expectedGeneration)
    return false;
  if (page.rootNode >= page.traceNodeCount)
    return false;
  const uint64_t first = page.traceNodeOffset;
  const uint64_t count = page.traceNodeCount;
  if (count == 0u || first + count > nodes.size())
    return false;
  return true;
}

template <typename BoundsNode>
inline bool voxelTraceBounds(
    const VoxelTracePageGPU &page,
    const std::vector<BoundsNode> &nodes,
    const VoxelTraceRayCPU &ray,
    const VoxelTraceTraversalOptionsCPU &options,
    const uint32_t expectedFormat,
    const uint32_t maxChildren,
    VoxelTraceHitCPU &hit)
{
  hit = {};
  if (!voxelTracePageValidForBounds(page, nodes, options, expectedFormat))
  {
    hit.status = VoxelTraceTraversalStatus::Invalid;
    return false;
  }

  struct Candidate
  {
    uint32_t nodeIndex = 0u;
    float nearT = 0.0f;
    uint32_t depth = 0u;
  };

  std::vector<Candidate> stack;
  stack.push_back({page.traceNodeOffset + page.rootNode, ray.tMin, 0u});
  bool limited = false;
  while (!stack.empty())
  {
    const Candidate current = stack.back();
    stack.pop_back();
    if (current.nearT > hit.t)
      continue;
    if (hit.nodeVisits >= options.maxNodeVisits)
    {
      limited = true;
      break;
    }
    ++hit.nodeVisits;
    if (current.nodeIndex < page.traceNodeOffset || current.nodeIndex >= page.traceNodeOffset + page.traceNodeCount || current.nodeIndex >= nodes.size())
    {
      hit.status = VoxelTraceTraversalStatus::Invalid;
      return false;
    }

    const BoundsNode &node = nodes[current.nodeIndex];
    struct ChildHit
    {
      uint32_t slot = 0u;
      uint32_t childOrPayload = 0u;
      float nearT = 0.0f;
      bool internal = false;
    };
    std::vector<ChildHit> children;
    const uint32_t childCount = std::min(voxelTraceBoundsChildCount(node.scaleExponentsAndChildCount), maxChildren);
    for (uint32_t slot = 0u; slot < childCount; ++slot)
    {
      VoxelTraceBoundsDecodedChildCPU child{};
      if (!voxelTraceBoundsDecodeChild(node, maxChildren, slot, child))
        continue;
      ++hit.childTests;
      float nearT = 0.0f;
      float farT = 0.0f;
      if (!voxelTraceRayAabb(ray, child.minimum, child.maximum, nearT, farT) || nearT > hit.t)
        continue;
      children.push_back({slot, child.childOrPayload, nearT, child.internal});
    }

    std::sort(
        children.begin(),
        children.end(),
        [](const ChildHit &left, const ChildHit &right)
        {
          if (left.nearT != right.nearT)
            return left.nearT < right.nearT;
          return left.slot < right.slot;
        });

    for (auto childIt = children.rbegin(); childIt != children.rend(); ++childIt)
    {
      const auto &child = *childIt;
      if (child.internal)
      {
        stack.push_back({page.traceNodeOffset + child.childOrPayload, child.nearT, current.depth + 1u});
      }
      else if (child.childOrPayload != options.skipPayload)
      {
        hit.status = VoxelTraceTraversalStatus::Hit;
        hit.t = child.nearT;
        hit.nodeIndex = current.nodeIndex;
        hit.childIndex = child.slot;
        hit.payload = child.childOrPayload;
        hit.depth = current.depth + 1u;
        hit.coarseFallback = false;
        hit.error = 0.0f;
      }
    }
  }

  if (limited && hit.status != VoxelTraceTraversalStatus::Hit)
    hit.status = VoxelTraceTraversalStatus::QueryLimitExceeded;
  else if (hit.status != VoxelTraceTraversalStatus::Hit)
    hit.status = VoxelTraceTraversalStatus::Miss;
  return hit.status != VoxelTraceTraversalStatus::Invalid;
}

inline bool voxelTraceBounds8(
    const VoxelTracePageGPU &page,
    const std::vector<VoxelTraceBounds8NodeGPU> &nodes,
    const VoxelTraceRayCPU &ray,
    const VoxelTraceTraversalOptionsCPU &options,
    VoxelTraceHitCPU &hit)
{
  return voxelTraceBounds(
      page,
      nodes,
      ray,
      options,
      kVoxelTraceFormatBounds8,
      8u,
      hit);
}

inline bool voxelTraceBounds16(
    const VoxelTracePageGPU &page,
    const std::vector<VoxelTraceBounds16NodeGPU> &nodes,
    const VoxelTraceRayCPU &ray,
    const VoxelTraceTraversalOptionsCPU &options,
    VoxelTraceHitCPU &hit)
{
  return voxelTraceBounds(
      page,
      nodes,
      ray,
      options,
      kVoxelTraceFormatBounds16,
      16u,
      hit);
}

static_assert(sizeof(VoxelTracePageGPU) == 80u);
static_assert(alignof(VoxelTracePageGPU) == 16u);
static_assert(sizeof(VoxelTraceNode64GPU) == 16u);
static_assert(sizeof(VoxelTraceErrorSidecarGPU) == 16u);
static_assert(alignof(VoxelTraceErrorSidecarGPU) == 16u);
static_assert(sizeof(VoxelTraceBounds8NodeGPU) == 80u);
static_assert(alignof(VoxelTraceBounds8NodeGPU) == 16u);
static_assert(sizeof(VoxelTraceBounds16NodeGPU) == 128u);
static_assert(alignof(VoxelTraceBounds16NodeGPU) == 16u);
} // namespace voxel::svraster
