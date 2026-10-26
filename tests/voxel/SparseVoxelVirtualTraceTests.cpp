#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/SparseVoxelVirtualTrace.hpp"

// These tests use assert() as their check mechanism, and Release builds define
// NDEBUG. Keep the contract checks alive even in optimized test builds.
#undef NDEBUG
#include <cassert>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace voxel::svraster;

void testABI()
{
  assert(sizeof(SparseVoxelTree64NodeGPU) == 12u);
  assert(sizeof(SparseVoxelTree64NodePaddedGPU) == 16u);

  assert(sizeof(VoxelTracePageGPU) == 80u);
  assert(alignof(VoxelTracePageGPU) == 16u);
  assert(offsetof(VoxelTracePageGPU, pageId) == 0u);
  assert(offsetof(VoxelTracePageGPU, generation) == 4u);
  assert(offsetof(VoxelTracePageGPU, geometryRevisionLo) == 8u);
  assert(offsetof(VoxelTracePageGPU, geometryRevisionHi) == 12u);
  assert(offsetof(VoxelTracePageGPU, format) == 16u);
  assert(offsetof(VoxelTracePageGPU, rootNode) == 20u);
  assert(offsetof(VoxelTracePageGPU, traceNodeOffset) == 24u);
  assert(offsetof(VoxelTracePageGPU, traceNodeCount) == 28u);
  assert(offsetof(VoxelTracePageGPU, brickOffset) == 32u);
  assert(offsetof(VoxelTracePageGPU, brickCount) == 36u);
  assert(offsetof(VoxelTracePageGPU, materialOffset) == 40u);
  assert(offsetof(VoxelTracePageGPU, materialCount) == 44u);
  assert(offsetof(VoxelTracePageGPU, localOriginX) == 48u);
  assert(offsetof(VoxelTracePageGPU, localOriginY) == 52u);
  assert(offsetof(VoxelTracePageGPU, localOriginZ) == 56u);
  assert(offsetof(VoxelTracePageGPU, rootSide) == 60u);
  assert(offsetof(VoxelTracePageGPU, errorMin) == 64u);
  assert(offsetof(VoxelTracePageGPU, errorMax) == 68u);
  assert(offsetof(VoxelTracePageGPU, errorSidecarOffset) == 72u);
  assert(offsetof(VoxelTracePageGPU, flags) == 76u);

  assert(sizeof(VoxelTraceNode64GPU) == 16u);
  assert(alignof(VoxelTraceNode64GPU) == 4u);
  assert(offsetof(VoxelTraceNode64GPU, childBaseAndFlags) == 0u);
  assert(offsetof(VoxelTraceNode64GPU, occupancyMaskLo) == 4u);
  assert(offsetof(VoxelTraceNode64GPU, occupancyMaskHi) == 8u);
  assert(offsetof(VoxelTraceNode64GPU, payloadOrSidecar) == 12u);

  assert(sizeof(VoxelTraceErrorSidecarGPU) == 16u);
  assert(alignof(VoxelTraceErrorSidecarGPU) == 16u);
  assert(offsetof(VoxelTraceErrorSidecarGPU, errorMin) == 0u);
  assert(offsetof(VoxelTraceErrorSidecarGPU, errorMax) == 4u);
  assert(offsetof(VoxelTraceErrorSidecarGPU, childErrorWordOffset) == 8u);
  assert(offsetof(VoxelTraceErrorSidecarGPU, childErrorWordCount) == 12u);

  assert(sizeof(VoxelTraceBounds8NodeGPU) == 80u);
  assert(alignof(VoxelTraceBounds8NodeGPU) == 16u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, originX) == 0u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, originY) == 4u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, originZ) == 8u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, scaleExponentsAndChildCount) == 12u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, childBase) == 16u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, payloadBase) == 20u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, childMask) == 24u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, internalMask) == 28u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, qLoX) == 32u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, qLoY) == 40u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, qLoZ) == 48u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, qHiX) == 56u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, qHiY) == 64u);
  assert(offsetof(VoxelTraceBounds8NodeGPU, qHiZ) == 72u);

  assert(sizeof(VoxelTraceBounds16NodeGPU) == 128u);
  assert(alignof(VoxelTraceBounds16NodeGPU) == 16u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, originX) == 0u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, scaleExponentsAndChildCount) == 12u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, childBase) == 16u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, payloadBase) == 20u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, childMask) == 24u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, internalMask) == 28u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, qLoX) == 32u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, qLoY) == 48u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, qLoZ) == 64u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, qHiX) == 80u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, qHiY) == 96u);
  assert(offsetof(VoxelTraceBounds16NodeGPU, qHiZ) == 112u);
}

void testConstantsAndDefaults()
{
  assert(kVoxelTraceBranching == 4u);
  assert(kVoxelTraceChildCount == 64u);
  assert(kVoxelTraceNode64TerminalFlag == 1u);
  assert(kVoxelTraceFormatInvalid == 0u);
  assert(kVoxelTraceFormatCubic64 == 1u);
  assert(kVoxelTraceFormatBounds8 == 2u);
  assert(kVoxelTraceFormatBounds16 == 3u);
  assert(kVoxelTracePageValid == 1u);
  assert(kVoxelTracePageConservativeFallback == 2u);
  assert(kVoxelTraceInvalidOffset == std::numeric_limits<uint32_t>::max());

  const VoxelTracePageGPU page{};
  assert(page.format == kVoxelTraceFormatInvalid);
  assert(page.errorSidecarOffset == kVoxelTraceInvalidOffset);

  assert(voxelTraceFormatCanBeImplicitFallbackFor(
      kVoxelTraceFormatCubic64,
      kVoxelTraceFormatCubic64,
      0u));
  assert(!voxelTraceFormatCanBeImplicitFallbackFor(
      kVoxelTraceFormatBounds8,
      kVoxelTraceFormatCubic64,
      0u));
  assert(voxelTraceFormatCanBeImplicitFallbackFor(
      kVoxelTraceFormatBounds8,
      kVoxelTraceFormatCubic64,
      kVoxelTracePageConservativeFallback));
  assert(voxelTraceFormatCanBeImplicitFallbackFor(
      kVoxelTraceFormatBounds16,
      kVoxelTraceFormatCubic64,
      kVoxelTracePageConservativeFallback));
  assert(!voxelTraceFormatCanBeImplicitFallbackFor(
      kVoxelTraceFormatCubic64,
      kVoxelTraceFormatBounds8,
      kVoxelTracePageConservativeFallback));
}

void testChildIndexOrder()
{
  std::array<bool, kVoxelTraceChildCount> visited{};
  for (uint32_t z = 0u; z < kVoxelTraceBranching; ++z)
  {
    for (uint32_t y = 0u; y < kVoxelTraceBranching; ++y)
    {
      for (uint32_t x = 0u; x < kVoxelTraceBranching; ++x)
      {
        const uint32_t child = voxelTraceNode64ChildIndex(x, y, z);
        assert(child == x + 4u * y + 16u * z);
        assert(child < kVoxelTraceChildCount);
        assert(!visited[child]);
        visited[child] = true;
      }
    }
  }
  for (const bool value : visited)
    assert(value);
}

void testOctantGrouping()
{
  uint64_t unionMask = 0u;
  for (uint32_t octant = 0u; octant < 8u; ++octant)
  {
    const uint64_t mask = voxelTraceNode64OctantMask(octant);
    assert(voxelTracePopulationCount64(mask) == 8u);
    assert((unionMask & mask) == 0u);
    unionMask |= mask;
    for (uint32_t child = 0u; child < kVoxelTraceChildCount; ++child)
    {
      const bool inMask = (mask & (uint64_t{1u} << child)) != 0u;
      assert(inMask == (voxelTraceNode64ChildOctant(child) == octant));
    }
  }
  assert(unionMask == std::numeric_limits<uint64_t>::max());

  const std::array<float, 3> origin{1.0f, 2.0f, 3.0f};
  const auto octant7 = voxelTraceNode64OctantOrigin(origin, 8.0f, 7u);
  assert(octant7[0] == 5.0f && octant7[1] == 6.0f && octant7[2] == 7.0f);
  const auto octant0 = voxelTraceNode64OctantOrigin(origin, 8.0f, 0u);
  assert(octant0 == origin);
}

void testOccupancyMaskRoundTrips()
{
  for (uint32_t child = 0u; child < kVoxelTraceChildCount; ++child)
  {
    VoxelTraceNode64GPU node{};
    const uint64_t mask = uint64_t{1u} << child;
    voxelTraceNode64SetPopulationMask(node, mask);
    assert(voxelTraceNode64PopulationMask(node) == mask);
    for (uint32_t other = 0u; other < kVoxelTraceChildCount; ++other)
      assert(voxelTraceNode64PopulationContains(node, other) == (other == child));
    assert(voxelTraceNode64PopulationCountBefore(node, child) == 0u);
    assert(voxelTraceNode64PopulationCountBefore(node, child + 1u) == 1u);
  }

  const uint64_t mixedMask =
      (uint64_t{1u} << 0u) |
      (uint64_t{1u} << 7u) |
      (uint64_t{1u} << 31u) |
      (uint64_t{1u} << 32u) |
      (uint64_t{1u} << 48u) |
      (uint64_t{1u} << 63u);
  const VoxelTraceNode64GPU node = voxelTraceNode64MakeInternal(123u, mixedMask, 456u);
  assert(!voxelTraceNode64Terminal(node));
  assert(voxelTraceNode64ChildBase(node) == 123u);
  assert(node.payloadOrSidecar == 456u);
  assert(voxelTraceNode64PopulationMask(node) == mixedMask);
  assert(voxelTraceNode64PopulationCountBefore(node, 0u) == 0u);
  assert(voxelTraceNode64PopulationCountBefore(node, 8u) == 2u);
  assert(voxelTraceNode64PopulationCountBefore(node, 32u) == 3u);
  assert(voxelTraceNode64PopulationCountBefore(node, 33u) == 4u);
  assert(voxelTraceNode64PopulationCountBefore(node, 64u) == 6u);
  assert(!voxelTraceNode64PopulationContains(node, 64u));

  const VoxelTraceNode64GPU terminal = voxelTraceNode64MakeTerminal(mixedMask, 99u);
  assert(voxelTraceNode64Terminal(terminal));
  assert(voxelTraceNode64ChildBase(terminal) == 0u);
  assert(voxelTraceNode64PopulationMask(terminal) == mixedMask);
  assert(terminal.payloadOrSidecar == 99u);
}

void testQ8Packing()
{
  for (uint32_t wordIndex = 0u; wordIndex < 16u; ++wordIndex)
  {
    const uint8_t a = static_cast<uint8_t>(wordIndex * 4u + 0u);
    const uint8_t b = static_cast<uint8_t>(wordIndex * 4u + 1u);
    const uint8_t c = static_cast<uint8_t>(wordIndex * 4u + 2u);
    const uint8_t d = static_cast<uint8_t>(wordIndex * 4u + 3u);
    const uint32_t packed = voxelTracePackQ8Error4(a, b, c, d);
    assert(voxelTraceUnpackQ8Error(packed, 0u) == a);
    assert(voxelTraceUnpackQ8Error(packed, 1u) == b);
    assert(voxelTraceUnpackQ8Error(packed, 2u) == c);
    assert(voxelTraceUnpackQ8Error(packed, 3u) == d);
    assert(voxelTraceUnpackQ8Error(packed, 4u) == a);
  }
}

void testQ8Quantization()
{
  assert(voxelTraceEncodeQ8Error(10.0f, 5.0f, 5.0f) == 0u);
  assert(voxelTraceDecodeQ8Error(128u, 5.0f, 5.0f) == 5.0f);
  assert(voxelTraceEncodeQ8Error(-1.0f, 0.0f, 10.0f) == 0u);
  assert(voxelTraceEncodeQ8Error(11.0f, 0.0f, 10.0f) == 255u);
  assert(voxelTraceDecodeQ8Error(0u, 0.0f, 10.0f) == 0.0f);
  assert(voxelTraceDecodeQ8Error(255u, 0.0f, 10.0f) == 10.0f);

  const uint8_t encodedMiddle = voxelTraceEncodeQ8Error(15.0f, 10.0f, 20.0f);
  assert(encodedMiddle == 128u);
  const float decodedMiddle = voxelTraceDecodeQ8Error(encodedMiddle, 10.0f, 20.0f);
  assert(std::fabs(decodedMiddle - 15.019608f) < 0.0001f);

  assert(voxelTraceEncodeQ8Error(
             std::numeric_limits<float>::quiet_NaN(),
             0.0f,
             1.0f) == 0u);
  assert(voxelTraceDecodeQ8Error(255u, 0.0f, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

void testCullErrorSidecar()
{
  std::vector<float> errors(kVoxelTraceChildCount);
  for (uint32_t child = 0u; child < kVoxelTraceChildCount; ++child)
    errors[child] = static_cast<float>(child);

  VoxelTraceCullErrorSidecarCPU sidecar{};
  std::string error;
  assert(voxelTraceBuildCullErrorSidecar(errors, sidecar, &error));
  assert(sidecar.errorMin == 0.0f);
  assert(sidecar.errorMax == 63.0f);
  assert(sidecar.childErrorWords.size() == 16u);
  assert(voxelTraceQ8ErrorWordCount(kVoxelTraceChildCount) == 16u);
  assert(voxelTraceQ8ErrorWordCount(1u) == 1u);
  assert(voxelTraceQ8ErrorWordCount(5u) == 2u);

  assert(voxelTraceCullSidecarEncodedError(sidecar, 0u) == 0u);
  assert(voxelTraceCullSidecarEncodedError(sidecar, 63u) == 255u);
  const float decodedChild32 = voxelTraceCullSidecarDecodedError(sidecar, 32u);
  const float q8HalfStep = (sidecar.errorMax - sidecar.errorMin) / 510.0f;
  assert(std::fabs(decodedChild32 - 32.0f) <= q8HalfStep);

  assert(
      voxelTraceCullExpansionDecision(&sidecar, 0u, 0.2f) ==
      VoxelTraceCullExpansionDecision::DefinitelySkip);
  assert(
      voxelTraceCullExpansionDecision(&sidecar, 63u, 62.8f) ==
      VoxelTraceCullExpansionDecision::MustExpand);
  assert(
      voxelTraceCullExpansionDecision(&sidecar, 32u, voxelTraceCullSidecarDecodedError(sidecar, 32u)) ==
      VoxelTraceCullExpansionDecision::ExactFallback);
  assert(
      voxelTraceCullExpansionDecision(nullptr, 0u, 1.0f) ==
      VoxelTraceCullExpansionDecision::ExactFallback);
  assert(
      voxelTraceCullExpansionDecision(&sidecar, 64u, 1.0f) ==
      VoxelTraceCullExpansionDecision::ExactFallback);

  VoxelTraceCullErrorSidecarCPU flat{};
  assert(voxelTraceBuildCullErrorSidecar(std::vector<float>{4.0f, 4.0f, 4.0f, 4.0f}, flat, &error));
  assert(
      voxelTraceCullExpansionDecision(&flat, 0u, 4.0f) ==
      VoxelTraceCullExpansionDecision::DefinitelySkip);
  assert(
      voxelTraceCullExpansionDecision(&flat, 0u, 3.99f) ==
      VoxelTraceCullExpansionDecision::MustExpand);

  VoxelTraceCullErrorSidecarCPU rejected{};
  assert(!voxelTraceBuildCullErrorSidecar({}, rejected, &error));
  assert(!voxelTraceBuildCullErrorSidecar(
      std::vector<float>{std::numeric_limits<float>::infinity()},
      rejected,
      &error));
}

VoxelTracePageGPU twoLeafPage()
{
  VoxelTracePageGPU page{};
  page.pageId = 17u;
  page.generation = 5u;
  page.geometryRevisionLo = 44u;
  page.format = kVoxelTraceFormatCubic64;
  page.rootNode = 0u;
  page.traceNodeOffset = 0u;
  page.traceNodeCount = 3u;
  page.localOriginX = 0.0f;
  page.localOriginY = 0.0f;
  page.localOriginZ = 0.0f;
  page.rootSide = 4.0f;
  page.flags = kVoxelTracePageValid;
  return page;
}

std::vector<VoxelTraceNode64GPU> twoLeafNodes()
{
  const uint64_t rootMask =
      (uint64_t{1u} << voxelTraceNode64ChildIndex(0u, 0u, 0u)) |
      (uint64_t{1u} << voxelTraceNode64ChildIndex(3u, 0u, 0u));
  const uint64_t leafMask = uint64_t{1u} << voxelTraceNode64ChildIndex(0u, 0u, 0u);
  return {
    voxelTraceNode64MakeInternal(1u, rootMask),
    voxelTraceNode64MakeTerminal(leafMask, 1000u),
    voxelTraceNode64MakeTerminal(leafMask, 2000u),
  };
}

VoxelTracePageGPU terminalFullPage()
{
  VoxelTracePageGPU page{};
  page.pageId = 23u;
  page.generation = 6u;
  page.format = kVoxelTraceFormatCubic64;
  page.rootNode = 0u;
  page.traceNodeOffset = 0u;
  page.traceNodeCount = 1u;
  page.localOriginX = 0.0f;
  page.localOriginY = 0.0f;
  page.localOriginZ = 0.0f;
  page.rootSide = 4.0f;
  page.flags = kVoxelTracePageValid;
  return page;
}

std::vector<VoxelTraceCullErrorSidecarCPU> nodeSidecarsWithRootError(const float rootChildError)
{
  std::vector<float> errors(kVoxelTraceChildCount, rootChildError);
  VoxelTraceCullErrorSidecarCPU rootSidecar{};
  std::string error;
  assert(voxelTraceBuildCullErrorSidecar(errors, rootSidecar, &error));
  return {rootSidecar};
}

std::vector<VoxelTraceCullErrorSidecarCPU> nodeSidecarsForRootErrors(
    const float defaultError,
    const std::vector<std::pair<uint32_t, float>> &overrides)
{
  std::vector<float> errors(kVoxelTraceChildCount, defaultError);
  for (const auto &overrideValue : overrides)
  {
    assert(overrideValue.first < errors.size());
    errors[overrideValue.first] = overrideValue.second;
  }
  VoxelTraceCullErrorSidecarCPU rootSidecar{};
  std::string error;
  assert(voxelTraceBuildCullErrorSidecar(errors, rootSidecar, &error));
  return {rootSidecar};
}

VoxelTracePageGPU bounds8Page(const uint32_t nodeCount = 1u)
{
  VoxelTracePageGPU page{};
  page.pageId = 31u;
  page.generation = 7u;
  page.geometryRevisionLo = 55u;
  page.format = kVoxelTraceFormatBounds8;
  page.rootNode = 0u;
  page.traceNodeOffset = 0u;
  page.traceNodeCount = nodeCount;
  page.flags = kVoxelTracePageValid;
  return page;
}

VoxelTracePageGPU bounds16Page(const uint32_t nodeCount = 1u)
{
  VoxelTracePageGPU page = bounds8Page(nodeCount);
  page.pageId = 32u;
  page.generation = 8u;
  page.geometryRevisionLo = 56u;
  page.format = kVoxelTraceFormatBounds16;
  return page;
}

std::vector<VoxelTraceBoundsChildCPU> boundsFixtureChildren()
{
  return {
    VoxelTraceBoundsChildCPU{{1.25f, 1.25f, 1.25f}, {2.25f, 2.25f, 2.25f}, false, true},
    VoxelTraceBoundsChildCPU{{4.10f, 1.00f, 1.00f}, {5.20f, 2.00f, 2.00f}, false, true},
    VoxelTraceBoundsChildCPU{{6.00f, 6.00f, 6.00f}, {7.00f, 7.00f, 7.00f}, false, false},
  };
}

void assertDecodedBoundsAreConservative(
    const VoxelTraceBounds8NodeGPU &node,
    const std::vector<VoxelTraceBoundsChildCPU> &source)
{
  for (uint32_t slot = 0u; slot < static_cast<uint32_t>(source.size()); ++slot)
  {
    VoxelTraceBoundsDecodedChildCPU decoded{};
    const bool decodedOccupied = voxelTraceBoundsDecodeChild(node, 8u, slot, decoded);
    assert(decodedOccupied == source[slot].occupied);
    if (!source[slot].occupied)
      continue;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      assert(decoded.minimum[axis] <= source[slot].minimum[axis] + 0.0001f);
      assert(decoded.maximum[axis] + 0.0001f >= source[slot].maximum[axis]);
    }
  }
}

void assertDecodedBoundsAreConservative(
    const VoxelTraceBounds16NodeGPU &node,
    const std::vector<VoxelTraceBoundsChildCPU> &source)
{
  for (uint32_t slot = 0u; slot < static_cast<uint32_t>(source.size()); ++slot)
  {
    VoxelTraceBoundsDecodedChildCPU decoded{};
    const bool decodedOccupied = voxelTraceBoundsDecodeChild(node, 16u, slot, decoded);
    assert(decodedOccupied == source[slot].occupied);
    if (!source[slot].occupied)
      continue;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      assert(decoded.minimum[axis] <= source[slot].minimum[axis] + 0.0001f);
      assert(decoded.maximum[axis] + 0.0001f >= source[slot].maximum[axis]);
    }
  }
}

void testBoundsQuantizationConservative()
{
  const auto children = boundsFixtureChildren();
  VoxelTraceBounds8NodeGPU node8{};
  std::string error;
  assert(voxelTraceBuildBoundsNode(
      {0.0f, 0.0f, 0.0f},
      {8.0f, 8.0f, 8.0f},
      children,
      8u,
      10u,
      100u,
      node8,
      &error));
  assert(voxelTraceBoundsChildCount(node8.scaleExponentsAndChildCount) == children.size());
  assert((node8.childMask & 0x3u) == 0x3u);
  assert((node8.childMask & 0x4u) == 0u);
  assert(node8.internalMask == 0u);
  assertDecodedBoundsAreConservative(node8, children);

  VoxelTraceBounds16NodeGPU node16{};
  assert(voxelTraceBuildBoundsNode(
      {0.0f, 0.0f, 0.0f},
      {16.0f, 16.0f, 16.0f},
      children,
      16u,
      20u,
      200u,
      node16,
      &error));
  assert(voxelTraceBoundsChildCount(node16.scaleExponentsAndChildCount) == children.size());
  assertDecodedBoundsAreConservative(node16, children);

  VoxelTraceBounds8NodeGPU rejected{};
  assert(!voxelTraceBuildBoundsNode(
      {0.0f, 0.0f, 0.0f},
      {8.0f, 8.0f, 8.0f},
      std::vector<VoxelTraceBoundsChildCPU>{
        VoxelTraceBoundsChildCPU{{-0.1f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false, true},
      },
      8u,
      0u,
      0u,
      rejected,
      &error));
}

void testBounds8ReferenceTraversalMatchesAabbFallback()
{
  const auto children = boundsFixtureChildren();
  VoxelTraceBounds8NodeGPU node{};
  std::string error;
  assert(voxelTraceBuildBoundsNode(
      {0.0f, 0.0f, 0.0f},
      {8.0f, 8.0f, 8.0f},
      children,
      8u,
      0u,
      900u,
      node,
      &error));

  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 1.5f, 1.5f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMax = 16.0f;

  VoxelTraceTraversalOptionsCPU options{};
  options.expectedGeneration = bounds8Page().generation;
  VoxelTraceHitCPU hit{};
  assert(voxelTraceBounds8(bounds8Page(), std::vector<VoxelTraceBounds8NodeGPU>{node}, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Hit);
  assert(hit.payload == 900u);
  assert(hit.childIndex == 0u);
  assert(hit.depth == 1u);

  float fallbackNear = 0.0f;
  float fallbackFar = 0.0f;
  assert(voxelTraceRayAabb(ray, children[0].minimum, children[0].maximum, fallbackNear, fallbackFar));
  assert(hit.t <= fallbackNear + 0.0001f);
  assert(hit.t + 1.0f >= fallbackNear);

  options.skipPayload = 900u;
  assert(voxelTraceBounds8(bounds8Page(), std::vector<VoxelTraceBounds8NodeGPU>{node}, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Hit);
  assert(hit.payload == 901u);
}

void testBounds8InternalTraversal()
{
  VoxelTraceBounds8NodeGPU root{};
  VoxelTraceBounds8NodeGPU leafNode{};
  std::string error;
  assert(voxelTraceBuildBoundsNode(
      {0.0f, 0.0f, 0.0f},
      {8.0f, 8.0f, 8.0f},
      std::vector<VoxelTraceBoundsChildCPU>{
        VoxelTraceBoundsChildCPU{{1.0f, 1.0f, 1.0f}, {3.0f, 3.0f, 3.0f}, true, true},
        VoxelTraceBoundsChildCPU{{5.0f, 5.0f, 5.0f}, {6.0f, 6.0f, 6.0f}, false, true},
      },
      8u,
      1u,
      700u,
      root,
      &error));
  assert(voxelTraceBuildBoundsNode(
      {1.0f, 1.0f, 1.0f},
      {3.0f, 3.0f, 3.0f},
      std::vector<VoxelTraceBoundsChildCPU>{
        VoxelTraceBoundsChildCPU{{1.25f, 1.25f, 1.25f}, {1.75f, 1.75f, 1.75f}, false, true},
      },
      8u,
      0u,
      800u,
      leafNode,
      &error));

  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 1.5f, 1.5f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMax = 16.0f;

  VoxelTraceTraversalOptionsCPU options{};
  options.expectedGeneration = bounds8Page(2u).generation;
  VoxelTraceHitCPU hit{};
  assert(voxelTraceBounds8(bounds8Page(2u), std::vector<VoxelTraceBounds8NodeGPU>{root, leafNode}, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Hit);
  assert(hit.payload == 800u);
  assert(hit.depth == 2u);
  assert(hit.nodeVisits == 2u);
}

void testBounds16ReferenceTraversal()
{
  std::vector<VoxelTraceBoundsChildCPU> children(16u);
  for (uint32_t slot = 0u; slot < 16u; ++slot)
  {
    const float x = 0.5f + static_cast<float>(slot);
    children[slot] = VoxelTraceBoundsChildCPU{{x, 0.5f, 0.5f}, {x + 0.25f, 1.5f, 1.5f}, false, true};
  }
  VoxelTraceBounds16NodeGPU node{};
  std::string error;
  assert(voxelTraceBuildBoundsNode(
      {0.0f, 0.0f, 0.0f},
      {17.0f, 4.0f, 4.0f},
      children,
      16u,
      0u,
      1200u,
      node,
      &error));
  assertDecodedBoundsAreConservative(node, children);

  VoxelTraceRayCPU ray{};
  ray.origin = {13.6f, -1.0f, 1.0f};
  ray.direction = {0.0f, 1.0f, 0.0f};
  ray.tMax = 16.0f;

  VoxelTraceTraversalOptionsCPU options{};
  options.expectedGeneration = bounds16Page().generation;
  VoxelTraceHitCPU hit{};
  assert(voxelTraceBounds16(bounds16Page(), std::vector<VoxelTraceBounds16NodeGPU>{node}, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Hit);
  assert(hit.childIndex == 13u);
  assert(hit.payload == 1213u);
}

void testMixedCubic64AndBoundsPagesStayFormatSeparated()
{
  const auto cubicPage = twoLeafPage();
  const auto cubicNodes = twoLeafNodes();

  const auto children = boundsFixtureChildren();
  VoxelTraceBounds8NodeGPU boundsNode{};
  std::string error;
  assert(voxelTraceBuildBoundsNode(
      {0.0f, 0.0f, 0.0f},
      {8.0f, 8.0f, 8.0f},
      children,
      8u,
      0u,
      900u,
      boundsNode,
      &error));
  const auto clusterPage = bounds8Page();

  VoxelTraceRayCPU cubicRay{};
  cubicRay.origin = {-1.0f, 0.125f, 0.125f};
  cubicRay.direction = {1.0f, 0.0f, 0.0f};
  cubicRay.tMax = 16.0f;
  VoxelTraceTraversalOptionsCPU cubicOptions{};
  cubicOptions.expectedGeneration = cubicPage.generation;
  VoxelTraceHitCPU cubicHit{};
  assert(voxelTraceCubic64(cubicPage, cubicNodes, cubicRay, cubicOptions, cubicHit));
  assert(cubicHit.status == VoxelTraceTraversalStatus::Hit);
  assert(cubicHit.payload == 1000u);

  VoxelTraceRayCPU boundsRay{};
  boundsRay.origin = {-1.0f, 1.5f, 1.5f};
  boundsRay.direction = {1.0f, 0.0f, 0.0f};
  boundsRay.tMax = 16.0f;
  VoxelTraceTraversalOptionsCPU boundsOptions{};
  boundsOptions.expectedGeneration = clusterPage.generation;
  VoxelTraceHitCPU boundsHit{};
  assert(voxelTraceBounds8(clusterPage, std::vector<VoxelTraceBounds8NodeGPU>{boundsNode}, boundsRay, boundsOptions, boundsHit));
  assert(boundsHit.status == VoxelTraceTraversalStatus::Hit);
  assert(boundsHit.payload == 900u);

  VoxelTracePageGPU wrongCubicPage = cubicPage;
  wrongCubicPage.format = kVoxelTraceFormatBounds8;
  assert(!voxelTraceCubic64(wrongCubicPage, cubicNodes, cubicRay, cubicOptions, cubicHit));
  assert(cubicHit.status == VoxelTraceTraversalStatus::Invalid);

  VoxelTracePageGPU wrongBoundsPage = clusterPage;
  wrongBoundsPage.format = kVoxelTraceFormatCubic64;
  assert(!voxelTraceBounds8(wrongBoundsPage, std::vector<VoxelTraceBounds8NodeGPU>{boundsNode}, boundsRay, boundsOptions, boundsHit));
  assert(boundsHit.status == VoxelTraceTraversalStatus::Invalid);
}

bool sameHitContract(const VoxelTraceHitCPU &left, const VoxelTraceHitCPU &right)
{
  return left.status == right.status &&
      std::fabs(left.t - right.t) < 0.0001f &&
      left.nodeIndex == right.nodeIndex &&
      left.childIndex == right.childIndex &&
      left.payload == right.payload &&
      left.depth == right.depth &&
      left.coarseFallback == right.coarseFallback;
}

void testCubic64ReferenceTraversal()
{
  const auto page = twoLeafPage();
  const auto nodes = twoLeafNodes();
  VoxelTraceTraversalOptionsCPU options{};
  options.expectedGeneration = page.generation;

  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 0.125f, 0.125f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMin = 0.0f;
  ray.tMax = 16.0f;

  VoxelTraceHitCPU hit{};
  assert(voxelTraceCubic64(page, nodes, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Hit);
  assert(hit.payload == 1000u);
  assert(hit.childIndex == voxelTraceNode64ChildIndex(0u, 0u, 0u));
  assert(hit.depth == 2u);
  assert(std::fabs(hit.t - 1.0f) < 0.0001f);
  assert(hit.nodeVisits >= 2u);

  options.skipPayload = 1000u;
  assert(voxelTraceCubic64(page, nodes, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Hit);
  assert(hit.payload == 2000u);
  assert(std::fabs(hit.t - 4.0f) < 0.0001f);

  options.skipPayload = kVoxelTraceInvalidOffset;
  ray.origin = {-1.0f, 2.5f, 0.125f};
  assert(voxelTraceCubic64(page, nodes, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Miss);

  ray.origin = {-1.0f, 0.125f, 0.125f};
  options.maxNodeVisits = 1u;
  assert(voxelTraceCubic64(page, nodes, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::QueryLimitExceeded);
}

void testCubic64ExactTraceMatchesInstalledRasterOccupancy()
{
  struct DenseCell
  {
    uint32_t x = 0u;
    uint32_t y = 0u;
    uint32_t z = 0u;
    uint32_t payload = 0u;
  };

  const uint32_t rootChildA = voxelTraceNode64ChildIndex(0u, 0u, 0u);
  const uint32_t rootChildB = voxelTraceNode64ChildIndex(1u, 2u, 0u);
  const uint32_t rootChildC = voxelTraceNode64ChildIndex(3u, 3u, 3u);
  const uint64_t rootMask =
      (uint64_t{1u} << rootChildA) |
      (uint64_t{1u} << rootChildB) |
      (uint64_t{1u} << rootChildC);
  const uint64_t leafMaskA =
      (uint64_t{1u} << voxelTraceNode64ChildIndex(0u, 0u, 0u)) |
      (uint64_t{1u} << voxelTraceNode64ChildIndex(2u, 1u, 0u));
  const uint64_t leafMaskB =
      (uint64_t{1u} << voxelTraceNode64ChildIndex(0u, 0u, 0u)) |
      (uint64_t{1u} << voxelTraceNode64ChildIndex(3u, 1u, 2u));
  const uint64_t leafMaskC =
      (uint64_t{1u} << voxelTraceNode64ChildIndex(1u, 1u, 1u));
  const std::vector<VoxelTraceNode64GPU> nodes{
    voxelTraceNode64MakeInternal(1u, rootMask),
    voxelTraceNode64MakeTerminal(leafMaskA, 1000u),
    voxelTraceNode64MakeTerminal(leafMaskB, 2000u),
    voxelTraceNode64MakeTerminal(leafMaskC, 3000u),
  };
  VoxelTracePageGPU page = terminalFullPage();
  page.pageId = 41u;
  page.generation = 9u;
  page.traceNodeCount = static_cast<uint32_t>(nodes.size());

  std::vector<DenseCell> rasterCells;
  const auto appendLeaf = [&rasterCells](
                              const uint32_t rootChild,
                              const uint64_t leafMask,
                              const uint32_t payloadBase)
  {
    const auto rootCoord = voxelTraceNode64ChildCoord(rootChild);
    for (uint32_t child = 0u; child < kVoxelTraceChildCount; ++child)
    {
      if ((leafMask & (uint64_t{1u} << child)) == 0u)
        continue;
      const auto leafCoord = voxelTraceNode64ChildCoord(child);
      rasterCells.push_back({
        rootCoord[0] * kVoxelTraceBranching + leafCoord[0],
        rootCoord[1] * kVoxelTraceBranching + leafCoord[1],
        rootCoord[2] * kVoxelTraceBranching + leafCoord[2],
        payloadBase + child,
      });
    }
  };
  appendLeaf(rootChildA, leafMaskA, 1000u);
  appendLeaf(rootChildB, leafMaskB, 2000u);
  appendLeaf(rootChildC, leafMaskC, 3000u);

  constexpr uint32_t denseSide = kVoxelTraceBranching * kVoxelTraceBranching;
  const float cellSide = page.rootSide / static_cast<float>(denseSide);
  VoxelTraceTraversalOptionsCPU options{};
  options.expectedGeneration = page.generation;
  options.variant = VoxelTraceTraversalVariant::Child64;

  for (uint32_t z = 0u; z < denseSide; ++z)
  {
    for (uint32_t y = 0u; y < denseSide; ++y)
    {
      const DenseCell *expected = nullptr;
      for (const auto &cell : rasterCells)
      {
        if (cell.y == y && cell.z == z &&
            (expected == nullptr || cell.x < expected->x))
        {
          expected = &cell;
        }
      }

      VoxelTraceRayCPU ray{};
      ray.origin = {-1.0f, (static_cast<float>(y) + 0.5f) * cellSide, (static_cast<float>(z) + 0.5f) * cellSide};
      ray.direction = {1.0f, 0.0f, 0.0f};
      ray.tMax = 8.0f;

      VoxelTraceHitCPU hit{};
      assert(voxelTraceCubic64(page, nodes, ray, options, hit));
      if (expected == nullptr)
      {
        assert(hit.status == VoxelTraceTraversalStatus::Miss);
      }
      else
      {
        assert(hit.status == VoxelTraceTraversalStatus::Hit);
        assert(hit.payload == expected->payload);
        assert(!hit.coarseFallback);
        const float expectedT = 1.0f + static_cast<float>(expected->x) * cellSide;
        assert(std::fabs(hit.t - expectedT) < 0.0001f);
      }
    }
  }

  VoxelTraceTraversalOptionsCPU grouped = options;
  grouped.variant = VoxelTraceTraversalVariant::GroupedOctant;
  for (const auto &cell : rasterCells)
  {
    VoxelTraceRayCPU ray{};
    ray.origin = {
      (static_cast<float>(cell.x) + 0.5f) * cellSide,
      (static_cast<float>(cell.y) + 0.5f) * cellSide,
      -1.0f};
    ray.direction = {0.0f, 0.0f, 1.0f};
    ray.tMax = 8.0f;

    VoxelTraceHitCPU baseline{};
    VoxelTraceHitCPU groupedHit{};
    assert(voxelTraceCubic64(page, nodes, ray, options, baseline));
    assert(voxelTraceCubic64(page, nodes, ray, grouped, groupedHit));
    assert(sameHitContract(baseline, groupedHit));
  }
}

void testCubic64ValidationRejectsStaleOrMalformedPages()
{
  auto page = twoLeafPage();
  auto nodes = twoLeafNodes();
  VoxelTraceTraversalOptionsCPU options{};
  options.expectedGeneration = page.generation + 1u;
  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 0.125f, 0.125f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMax = 16.0f;

  VoxelTraceHitCPU hit{};
  assert(!voxelTraceCubic64(page, nodes, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Invalid);

  options.expectedGeneration = page.generation;
  page.flags = 0u;
  assert(!voxelTraceCubic64(page, nodes, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Invalid);

  page = twoLeafPage();
  nodes[0] = voxelTraceNode64MakeInternal(100u, uint64_t{1u} << voxelTraceNode64ChildIndex(0u, 0u, 0u));
  assert(!voxelTraceCubic64(page, nodes, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Invalid);
}

void testCubic64ConservativeEarlyReturn()
{
  const auto page = twoLeafPage();
  const auto nodes = twoLeafNodes();
  const auto sidecars = nodeSidecarsWithRootError(0.01f);

  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 0.125f, 0.125f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMin = 0.0f;
  ray.tMax = 16.0f;

  VoxelTraceTraversalOptionsCPU exact{};
  exact.expectedGeneration = page.generation;
  VoxelTraceHitCPU exactHit{};
  assert(voxelTraceCubic64(page, nodes, ray, exact, exactHit));
  assert(exactHit.status == VoxelTraceTraversalStatus::Hit);
  assert(!exactHit.coarseFallback);

  VoxelTraceTraversalOptionsCPU disabled = exact;
  disabled.nodeErrorSidecars = &sidecars;
  disabled.projectedErrorThreshold = 100.0f;
  disabled.cameraPosition = {100.0f, 100.0f, 100.0f};
  VoxelTraceHitCPU disabledHit{};
  assert(voxelTraceCubic64(page, nodes, ray, disabled, disabledHit));
  assert(sameHitContract(exactHit, disabledHit));

  VoxelTraceTraversalOptionsCPU conservative = exact;
  conservative.mode = VoxelTraceTraversalMode::ConservativeVisibility;
  conservative.nodeErrorSidecars = &sidecars;
  conservative.projectedErrorThreshold = 1.0f;
  conservative.cameraPosition = {100.0f, 100.0f, 100.0f};
  VoxelTraceHitCPU conservativeHit{};
  assert(voxelTraceCubic64(page, nodes, ray, conservative, conservativeHit));
  assert(conservativeHit.status == VoxelTraceTraversalStatus::Hit);
  assert(conservativeHit.coarseFallback);
  assert(conservativeHit.payload == kVoxelTraceInvalidOffset);
  assert(conservativeHit.nodeIndex == 0u);
  assert(conservativeHit.childIndex == voxelTraceNode64ChildIndex(0u, 0u, 0u));
  assert(conservativeHit.depth == 1u);
  assert(conservativeHit.t <= exactHit.t + 0.0001f);
}

void testCubic64ExactModeRefusesImplicitCoarseFallback()
{
  const auto page = twoLeafPage();
  const auto nodes = twoLeafNodes();
  const auto sidecars = nodeSidecarsWithRootError(0.01f);

  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 0.125f, 0.125f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMax = 16.0f;

  VoxelTraceTraversalOptionsCPU exact{};
  exact.expectedGeneration = page.generation;
  exact.mode = VoxelTraceTraversalMode::Exact;
  exact.nodeErrorSidecars = &sidecars;
  exact.projectedErrorThreshold = 100.0f;
  exact.cameraPosition = {100.0f, 100.0f, 100.0f};

  VoxelTraceHitCPU hit{};
  assert(voxelTraceCubic64(page, nodes, ray, exact, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Hit);
  assert(!hit.coarseFallback);
  assert(hit.payload == 1000u);

  exact.allowExactCoarseFallback = true;
  assert(voxelTraceCubic64(page, nodes, ray, exact, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Hit);
  assert(hit.coarseFallback);
  assert(hit.payload == kVoxelTraceInvalidOffset);
}

void testCubic64ProjectedErrorUsesCameraDistance()
{
  const auto page = twoLeafPage();
  const auto nodes = twoLeafNodes();
  const auto sidecars = nodeSidecarsWithRootError(1.0f);

  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 0.125f, 0.125f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMax = 16.0f;

  VoxelTraceTraversalOptionsCPU options{};
  options.expectedGeneration = page.generation;
  options.mode = VoxelTraceTraversalMode::ConservativeVisibility;
  options.nodeErrorSidecars = &sidecars;
  options.projectedErrorThreshold = 0.1f;
  options.minimumCameraDistance = 0.001f;

  VoxelTraceHitCPU nearCameraHit{};
  options.cameraPosition = {0.5f, 0.5f, 0.5f};
  assert(voxelTraceCubic64(page, nodes, ray, options, nearCameraHit));
  assert(nearCameraHit.status == VoxelTraceTraversalStatus::Hit);
  assert(!nearCameraHit.coarseFallback);
  assert(nearCameraHit.payload == 1000u);

  VoxelTraceHitCPU farCameraHit{};
  options.cameraPosition = {100.0f, 100.0f, 100.0f};
  assert(voxelTraceCubic64(page, nodes, ray, options, farCameraHit));
  assert(farCameraHit.status == VoxelTraceTraversalStatus::Hit);
  assert(farCameraHit.coarseFallback);
  assert(farCameraHit.payload == kVoxelTraceInvalidOffset);
  assert(std::fabs(farCameraHit.t - nearCameraHit.t) < 0.0001f);
}

void testCubic64LowerErrorCoarseHitOverridesNearerHigherError()
{
  const auto page = twoLeafPage();
  const auto nodes = twoLeafNodes();
  const uint32_t nearChild = voxelTraceNode64ChildIndex(0u, 0u, 0u);
  const uint32_t farChild = voxelTraceNode64ChildIndex(3u, 0u, 0u);
  const auto sidecars = nodeSidecarsForRootErrors(10.0f, {{nearChild, 10.0f}, {farChild, 1.0f}});

  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 0.125f, 0.125f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMax = 16.0f;

  VoxelTraceTraversalOptionsCPU options{};
  options.expectedGeneration = page.generation;
  options.mode = VoxelTraceTraversalMode::ConservativeVisibility;
  options.nodeErrorSidecars = &sidecars;
  options.projectedErrorThreshold = 0.1f;
  options.cameraPosition = {100.0f, 100.0f, 100.0f};

  VoxelTraceHitCPU hit{};
  assert(voxelTraceCubic64(page, nodes, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Hit);
  assert(hit.coarseFallback);
  assert(hit.childIndex == farChild);
  assert(hit.payload == kVoxelTraceInvalidOffset);
  assert(hit.error < 1.1f);
  assert(std::fabs(hit.t - 4.0f) < 0.0001f);
}

void testCubic64ErrorSortedStackPrunesKnownWorseSubtree()
{
  const auto page = twoLeafPage();
  const auto nodes = twoLeafNodes();
  const uint32_t nearChild = voxelTraceNode64ChildIndex(0u, 0u, 0u);
  const uint32_t farChild = voxelTraceNode64ChildIndex(3u, 0u, 0u);
  const auto sidecars = nodeSidecarsForRootErrors(10.0f, {{nearChild, 10.0f}, {farChild, 1.0f}});

  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 0.125f, 0.125f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMax = 16.0f;

  VoxelTraceTraversalOptionsCPU options{};
  options.expectedGeneration = page.generation;
  options.mode = VoxelTraceTraversalMode::ConservativeVisibility;
  options.nodeErrorSidecars = &sidecars;
  options.projectedErrorThreshold = 0.1f;
  options.cameraPosition = {0.0f, 0.0f, 0.0f};

  VoxelTraceHitCPU hit{};
  assert(voxelTraceCubic64(page, nodes, ray, options, hit));
  assert(hit.status == VoxelTraceTraversalStatus::Hit);
  assert(!hit.coarseFallback);
  assert(hit.payload == 2000u);
  assert(hit.error == 0.0f);
  assert(hit.nodeVisits == 2u);
}

void testCubic64GroupedOctantTraversalMatchesChild64AndSkipsGroups()
{
  const auto page = terminalFullPage();
  const std::vector<VoxelTraceNode64GPU> nodes{
    voxelTraceNode64MakeTerminal(std::numeric_limits<uint64_t>::max(), 5000u),
  };

  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 0.125f, 0.125f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMax = 16.0f;

  VoxelTraceTraversalOptionsCPU child64{};
  child64.expectedGeneration = page.generation;
  VoxelTraceHitCPU baseline{};
  assert(voxelTraceCubic64(page, nodes, ray, child64, baseline));
  assert(baseline.status == VoxelTraceTraversalStatus::Hit);
  assert(baseline.payload == 5000u);
  assert(baseline.groupTests == 0u);
  assert(baseline.childTests == kVoxelTraceChildCount);

  VoxelTraceTraversalOptionsCPU grouped = child64;
  grouped.variant = VoxelTraceTraversalVariant::GroupedOctant;
  VoxelTraceHitCPU groupedHit{};
  assert(voxelTraceCubic64(page, nodes, ray, grouped, groupedHit));
  assert(groupedHit.status == baseline.status);
  assert(groupedHit.payload == baseline.payload);
  assert(groupedHit.childIndex == baseline.childIndex);
  assert(std::fabs(groupedHit.t - baseline.t) < 0.0001f);
  assert(groupedHit.groupTests == 8u);
  assert(groupedHit.skippedChildGroups == 48u);
  assert(groupedHit.childTests == 16u);
  assert(groupedHit.childTests < baseline.childTests);
}

void testCubic64GroupedOctantTraversalHandlesInternalNodes()
{
  const auto page = twoLeafPage();
  const auto nodes = twoLeafNodes();
  VoxelTraceRayCPU ray{};
  ray.origin = {-1.0f, 0.125f, 0.125f};
  ray.direction = {1.0f, 0.0f, 0.0f};
  ray.tMax = 16.0f;

  VoxelTraceTraversalOptionsCPU baselineOptions{};
  baselineOptions.expectedGeneration = page.generation;
  VoxelTraceHitCPU baseline{};
  assert(voxelTraceCubic64(page, nodes, ray, baselineOptions, baseline));

  VoxelTraceTraversalOptionsCPU groupedOptions = baselineOptions;
  groupedOptions.variant = VoxelTraceTraversalVariant::GroupedOctant;
  VoxelTraceHitCPU grouped{};
  assert(voxelTraceCubic64(page, nodes, ray, groupedOptions, grouped));
  assert(sameHitContract(baseline, grouped));
  assert(grouped.groupTests >= 1u);
}
} // namespace

int main()
{
  testABI();
  testConstantsAndDefaults();
  testChildIndexOrder();
  testOctantGrouping();
  testOccupancyMaskRoundTrips();
  testQ8Packing();
  testQ8Quantization();
  testCullErrorSidecar();
  testBoundsQuantizationConservative();
  testBounds8ReferenceTraversalMatchesAabbFallback();
  testBounds8InternalTraversal();
  testBounds16ReferenceTraversal();
  testMixedCubic64AndBoundsPagesStayFormatSeparated();
  testCubic64ReferenceTraversal();
  testCubic64ExactTraceMatchesInstalledRasterOccupancy();
  testCubic64ValidationRejectsStaleOrMalformedPages();
  testCubic64ConservativeEarlyReturn();
  testCubic64ExactModeRefusesImplicitCoarseFallback();
  testCubic64ProjectedErrorUsesCameraDistance();
  testCubic64LowerErrorCoarseHitOverridesNearerHigherError();
  testCubic64ErrorSortedStackPrunesKnownWorseSubtree();
  testCubic64GroupedOctantTraversalMatchesChild64AndSkipsGroups();
  testCubic64GroupedOctantTraversalHandlesInternalNodes();
  std::cout << "SparseVoxelVirtualTraceTests: passed\n";
  return 0;
}
