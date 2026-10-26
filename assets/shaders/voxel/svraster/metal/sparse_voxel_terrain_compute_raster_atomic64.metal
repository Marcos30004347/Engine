// Native Metal terrain compute raster prototype.
//
// This is the Metal-only escape hatch for Nanite-style primitive-parallel
// rasterization with a single packed 64-bit visibility key:
//
//   atomicMin(depth32 << 32 | payload32)
//
// The portable Vulkan/MoltenVK path must keep using the WGSL tiled 32-bit route
// unless the Vulkan device reports shaderBufferInt64Atomics. This shader exists
// specifically for a native Metal encoder that shares the MoltenVK device/queue
// and exported resources.

#include <metal_stdlib>
using namespace metal;

constant uint kTerrainComputeRasterTileSize = 16u;
constant ulong kTerrainComputeRasterEmptyKey = 0xfffffffffffffffful;

struct TerrainComputeRasterUniforms
{
  uint viewportWidth;
  uint viewportHeight;
  uint tileGridWidth;
  uint tileGridHeight;
  uint triangleCount;
  uint tileReferenceCapacity;
  uint payloadMask;
  uint tileCandidateCapacity;
};

struct TerrainComputeRasterTriangle
{
  packed_float2 p0;
  packed_float2 p1;
  packed_float2 p2;
  float z0;
  float z1;
  float z2;
  uint payload;
  uint flags;
};

struct TerrainComputeRasterTileRange
{
  atomic_uint count;
  atomic_uint cursor;
  uint offset;
  uint reserved0;
};

struct TerrainComputeRasterPixelPayload
{
  uint payload;
  // Reverse-Z device depth quantized to 32 bits; 0 means "no software hit"
  // and can never beat a hardware pixel.
  uint depthBits;
  // Oct-encoded world normal (2x16 snorm), averaged from the triangle's
  // decoded vertex normals - the same value the hardware path interpolates on
  // a flat voxel face.
  uint normalOct;
  // Dense instance index at resolve time, mapped from the owning object's
  // stable sparse id through instanceSparseToDense.
  uint denseInstance;
};

struct VGVisibleClusterInfo
{
  uint pageIndex;
  uint pageLocalClusterIndex;
  uint instanceIndex;
  uint padding;
};

struct VGPageTableEntry
{
  uint bufferOffset;
  uint size;
  uint clusterOffset;
  uint clusterCount;
  uint isInstalled;
  uint prioritySlot;
  uint unitScaleBits;
  uint decodedVertexOffset;
  uint decodedIndexOffset;
  uint decodedBoneWeightOffset;
  uint padding0;
  uint padding1;
};

struct VGInstanceData
{
  float4x4 modelMatrix;
  uint hierarchyStartOffset;
  uint quantizationFactor;
  uint unitScaleBits;
  uint pageTableOffset;
  uint materialIndex;
  uint meshPartTransformsOffset;
  uint padding0;
  uint padding1;
  float4x4 previousModelMatrix;
  uint previousMeshPartTransformsOffset;
  uint temporalFlags;
  uint padding2;
  uint padding3;
};

struct VGDecodedClusterCullData
{
  float localMinX;
  float localMinY;
  float localMinZ;
  uint triangleCount;
  float localMaxX;
  float localMaxY;
  float localMaxZ;
  uint padding0;
  float selfCenterX;
  float selfCenterY;
  float selfCenterZ;
  float selfRadius;
  float parentCenterX;
  float parentCenterY;
  float parentCenterZ;
  float parentRadius;
  float coneAxisX;
  float coneAxisY;
  float coneAxisZ;
  float coneCutoff;
  float selfError;
  float parentError;
  uint padding1;
  uint padding2;
};

struct VGDecodedClusterGeometryInfo
{
  uint vertexOffset;
  uint indexOffset;
  uint vertexCount;
  uint triangleCount;
  uint pageLocalGroupIndex;
  uint clusterIndexInGroup;
  uint padding0;
  uint padding1;
};

struct VGDecodedVertexData
{
  float positionX;
  float positionY;
  float positionZ;
  uint boneWeightOffset;
  float normalX;
  float normalY;
  float normalZ;
  uint boneWeightCount;
  float uvX;
  float uvY;
  uint padding0;
  uint padding1;
};

struct VGCullingUniforms
{
  float4x4 view;
  float4x4 proj;
  float4 viewPosition;
  uint viewportX;
  uint viewportY;
  float error;
  uint instancesCount;
  uint clustersCount;
  float nearPlane;
  float farPlane;
  uint hiZLevels;
  uint cullingFlags;
  uint registeredPages;
  uint streamingSelectionCount;
  uint padding0;
  uint padding1;
};

struct VGProjectedBounds
{
  uint2 firstTile;
  uint2 lastTile;
  bool visible;
};

constant uint kVGSoftwareVisibleClusterCounter = 4u;

static inline float edgeFunction(float2 a, float2 b, float2 p)
{
  return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

static inline uint clampDepthToUint(float depth)
{
  const float clamped = clamp(depth, 0.0f, 1.0f);
  return uint(clamped * 4294967295.0f);
}

static inline ulong packVisibilityKey(float depth, uint payload)
{
  // The engine projects reverse-Z (near = 1), but the visibility key resolves
  // overdraw with atomicMin. Packing 1-z orders keys near-first; the merge
  // kernel converts back to reverse-Z device depth for the consumer.
  const uint depthBits = clampDepthToUint(1.0f - depth);
  return (ulong(depthBits) << 32ul) | ulong(payload);
}

static inline uint2 tileFromPixel(float2 pixel)
{
  return uint2(pixel) / kTerrainComputeRasterTileSize;
}

static inline uint tileCount(const TerrainComputeRasterUniforms uniforms)
{
  return uniforms.tileGridWidth * uniforms.tileGridHeight;
}

static inline uint candidatesPerTile(const TerrainComputeRasterUniforms uniforms)
{
  const uint count = tileCount(uniforms);
  if (uniforms.tileCandidateCapacity != 0u)
    return uniforms.tileCandidateCapacity;
  if (count == 0u)
    return 0u;
  return uniforms.tileReferenceCapacity / count;
}

static inline bool triangleCoversPixel(
    const TerrainComputeRasterTriangle triangle,
    float2 pixel,
    thread float &depth)
{
  const float2 p0 = float2(triangle.p0);
  const float2 p1 = float2(triangle.p1);
  const float2 p2 = float2(triangle.p2);
  const float area = edgeFunction(p0, p1, p2);
  if (abs(area) <= 1.0e-8f)
    return false;

  const float w0 = edgeFunction(p1, p2, pixel);
  const float w1 = edgeFunction(p2, p0, pixel);
  const float w2 = edgeFunction(p0, p1, pixel);
  const bool positive = area > 0.0f;
  if (positive)
  {
    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
      return false;
  }
  else
  {
    if (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f)
      return false;
  }

  const float invArea = 1.0f / area;
  const float b0 = w0 * invArea;
  const float b1 = w1 * invArea;
  const float b2 = w2 * invArea;
  depth = b0 * triangle.z0 + b1 * triangle.z1 + b2 * triangle.z2;
  return true;
}

static inline bool screenTriangleCoversPixel(
    float2 p0,
    float2 p1,
    float2 p2,
    float z0,
    float z1,
    float z2,
    float2 pixel,
    thread float &depth)
{
  const float area = edgeFunction(p0, p1, p2);
  if (abs(area) <= 1.0e-8f)
    return false;

  const float w0 = edgeFunction(p1, p2, pixel);
  const float w1 = edgeFunction(p2, p0, pixel);
  const float w2 = edgeFunction(p0, p1, pixel);
  const bool positive = area > 0.0f;
  if (positive)
  {
    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
      return false;
  }
  else
  {
    if (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f)
      return false;
  }

  const float invArea = 1.0f / area;
  const float b0 = w0 * invArea;
  const float b1 = w1 * invArea;
  const float b2 = w2 * invArea;
  depth = b0 * z0 + b1 * z1 + b2 * z2;
  return true;
}

static inline uint globalClusterIndex(
    const VGPageTableEntry entry,
    const uint pageLocalClusterIndex)
{
  return entry.clusterOffset + pageLocalClusterIndex;
}

// Mirrors virtualgeometry::packVisibilityPayload in VirtualGeometryData.hpp and
// packVisibilityPayload in virtualgeometrydata.wgsl. Keep the three in step.
//
//   bits [6:0]   triangle index within its cluster
//   bits [25:7]  global decoded cluster index
//
// The previous packing here squeezed (clusterSlot, pageIndex, localCluster,
// triangle) into 12/8/6/6 bits against real ranges of 524288/65536/128/128, so
// every field truncated and the payload could not be resolved back to geometry.
// That is what stopped the software rasterizer from being authoritative.
constant uint kVisibilityPayloadTriangleBits = 7u;
constant uint kVisibilityPayloadTriangleMask = 0x7fu;
constant uint kVisibilityPayloadClusterMask = 0x7ffffu;

static inline uint packVisibilityPayload(
    const uint globalCluster,
    const uint triangleIndex)
{
  return ((globalCluster & kVisibilityPayloadClusterMask)
          << kVisibilityPayloadTriangleBits) |
      (triangleIndex & kVisibilityPayloadTriangleMask);
}

static inline float4 projectDecodedVGPosition(
    constant VGCullingUniforms &cullingUniforms,
    const VGInstanceData instance,
    const float3 localPosition)
{
  return cullingUniforms.proj * cullingUniforms.view *
      instance.modelMatrix * float4(localPosition, 1.0f);
}

static inline bool clipToPixel(
    const float4 clip,
    const TerrainComputeRasterUniforms uniforms,
    thread float2 &pixel,
    thread float &depth)
{
  if (clip.w <= 1.0e-6f)
    return false;
  const float3 ndc = clip.xyz / clip.w;
  pixel = float2(
      (ndc.x * 0.5f + 0.5f) * float(uniforms.viewportWidth),
      (ndc.y * 0.5f + 0.5f) * float(uniforms.viewportHeight));
  depth = ndc.z;
  return true;
}

static inline VGProjectedBounds decodedVGClusterProjectedBounds(
    const TerrainComputeRasterUniforms uniforms,
    constant VGCullingUniforms &cullingUniforms,
    const VGInstanceData instance,
    const VGDecodedClusterCullData cull)
{
  float2 minPixel = float2(1.0e20f);
  float2 maxPixel = float2(-1.0e20f);
  bool anyVisible = false;
  const float3 localMin = float3(cull.localMinX, cull.localMinY, cull.localMinZ);
  const float3 localMax = float3(cull.localMaxX, cull.localMaxY, cull.localMaxZ);

  for (uint corner = 0u; corner < 8u; ++corner)
  {
    const float3 p = float3(
        (corner & 1u) != 0u ? localMax.x : localMin.x,
        (corner & 2u) != 0u ? localMax.y : localMin.y,
        (corner & 4u) != 0u ? localMax.z : localMin.z);
    float2 pixel;
    float depth;
    if (!clipToPixel(projectDecodedVGPosition(cullingUniforms, instance, p), uniforms, pixel, depth))
      continue;
    anyVisible = true;
    minPixel = min(minPixel, pixel);
    maxPixel = max(maxPixel, pixel);
  }

  VGProjectedBounds result;
  result.visible = anyVisible;
  result.firstTile = uint2(0u);
  result.lastTile = uint2(0u);
  if (!anyVisible)
    return result;

  const float2 viewportMax =
      float2(float(uniforms.viewportWidth - 1u), float(uniforms.viewportHeight - 1u));
  minPixel = clamp(floor(minPixel), float2(0.0f), viewportMax);
  maxPixel = clamp(ceil(maxPixel), float2(0.0f), viewportMax);
  result.firstTile = min(
      tileFromPixel(minPixel),
      uint2(uniforms.tileGridWidth - 1u, uniforms.tileGridHeight - 1u));
  result.lastTile = min(
      tileFromPixel(maxPixel),
      uint2(uniforms.tileGridWidth - 1u, uniforms.tileGridHeight - 1u));
  return result;
}

kernel void terrainComputeRasterAtomic64Clear(
    device ulong *visibilityKeys [[buffer(0)]],
    device TerrainComputeRasterPixelPayload *resolvedPayloads [[buffer(1)]],
    constant TerrainComputeRasterUniforms &uniforms [[buffer(2)]],
    uint id [[thread_position_in_grid]])
{
  const uint pixelCount = uniforms.viewportWidth * uniforms.viewportHeight;
  if (id >= pixelCount)
    return;
  visibilityKeys[id] = kTerrainComputeRasterEmptyKey;
  resolvedPayloads[id].payload = 0u;
  resolvedPayloads[id].depthBits = 0u;
  resolvedPayloads[id].normalOct = 0u;
  resolvedPayloads[id].denseInstance = 0xffffffffu;
}

kernel void terrainComputeRasterAtomic64ClearTiles(
    constant TerrainComputeRasterUniforms &uniforms [[buffer(0)]],
    device TerrainComputeRasterTileRange *tileRanges [[buffer(1)]],
    device atomic_uint &overflowCounter [[buffer(2)]],
    uint id [[thread_position_in_grid]])
{
  if (id == 0u)
    atomic_store_explicit(&overflowCounter, 0u, memory_order_relaxed);

  const uint count = tileCount(uniforms);
  if (id >= count)
    return;

  const uint perTile = candidatesPerTile(uniforms);
  atomic_store_explicit(&tileRanges[id].count, 0u, memory_order_relaxed);
  atomic_store_explicit(&tileRanges[id].cursor, 0u, memory_order_relaxed);
  tileRanges[id].offset = id * perTile;
  tileRanges[id].reserved0 = 0u;
}

kernel void terrainComputeRasterAtomic64CountTiles(
    constant TerrainComputeRasterUniforms &uniforms [[buffer(0)]],
    const device TerrainComputeRasterTriangle *triangles [[buffer(1)]],
    device TerrainComputeRasterTileRange *tileRanges [[buffer(2)]],
    uint triangleIndex [[thread_position_in_grid]])
{
  if (triangleIndex >= uniforms.triangleCount)
    return;

  const TerrainComputeRasterTriangle triangle = triangles[triangleIndex];
  const float2 minimumPixel =
      floor(min(min(float2(triangle.p0), float2(triangle.p1)), float2(triangle.p2)));
  const float2 maximumPixel =
      ceil(max(max(float2(triangle.p0), float2(triangle.p1)), float2(triangle.p2)));
  const uint2 firstTile = min(
      tileFromPixel(max(minimumPixel, float2(0.0f))),
      uint2(uniforms.tileGridWidth - 1u, uniforms.tileGridHeight - 1u));
  const uint2 lastTile = min(
      tileFromPixel(max(maximumPixel, float2(0.0f))),
      uint2(uniforms.tileGridWidth - 1u, uniforms.tileGridHeight - 1u));

  for (uint y = firstTile.y; y <= lastTile.y; ++y)
  {
    for (uint x = firstTile.x; x <= lastTile.x; ++x)
    {
      const uint tile = y * uniforms.tileGridWidth + x;
      atomic_fetch_add_explicit(
          &tileRanges[tile].count, 1u, memory_order_relaxed);
    }
  }
}

kernel void terrainComputeRasterAtomic64ScatterTiles(
    constant TerrainComputeRasterUniforms &uniforms [[buffer(0)]],
    const device TerrainComputeRasterTriangle *triangles [[buffer(1)]],
    device TerrainComputeRasterTileRange *tileRanges [[buffer(2)]],
    device uint *tileTriangleIndices [[buffer(3)]],
    device atomic_uint &overflowCounter [[buffer(4)]],
    uint triangleIndex [[thread_position_in_grid]])
{
  if (triangleIndex >= uniforms.triangleCount)
    return;

  const TerrainComputeRasterTriangle triangle = triangles[triangleIndex];
  const float2 minimumPixel =
      floor(min(min(float2(triangle.p0), float2(triangle.p1)), float2(triangle.p2)));
  const float2 maximumPixel =
      ceil(max(max(float2(triangle.p0), float2(triangle.p1)), float2(triangle.p2)));
  const uint2 firstTile = min(
      tileFromPixel(max(minimumPixel, float2(0.0f))),
      uint2(uniforms.tileGridWidth - 1u, uniforms.tileGridHeight - 1u));
  const uint2 lastTile = min(
      tileFromPixel(max(maximumPixel, float2(0.0f))),
      uint2(uniforms.tileGridWidth - 1u, uniforms.tileGridHeight - 1u));

  for (uint y = firstTile.y; y <= lastTile.y; ++y)
  {
    for (uint x = firstTile.x; x <= lastTile.x; ++x)
    {
      const uint tile = y * uniforms.tileGridWidth + x;
      const uint slot = atomic_fetch_add_explicit(
          &tileRanges[tile].cursor, 1u, memory_order_relaxed);
      const uint address = tileRanges[tile].offset + slot;
      const uint perTile = candidatesPerTile(uniforms);
      if (slot < perTile && address < uniforms.tileReferenceCapacity)
      {
        tileTriangleIndices[address] = triangleIndex;
      }
      else
      {
        atomic_fetch_add_explicit(&overflowCounter, 1u, memory_order_relaxed);
      }
    }
  }
}

kernel void terrainComputeRasterAtomic64RasterizeTiles(
    constant TerrainComputeRasterUniforms &uniforms [[buffer(0)]],
    const device TerrainComputeRasterTriangle *triangles [[buffer(1)]],
    const device TerrainComputeRasterTileRange *tileRanges [[buffer(2)]],
    const device uint *tileTriangleIndices [[buffer(3)]],
    device atomic_ulong *visibilityKeys [[buffer(4)]],
    uint2 pixel [[thread_position_in_grid]])
{
  if (pixel.x >= uniforms.viewportWidth || pixel.y >= uniforms.viewportHeight)
    return;

  const uint tileX = pixel.x / kTerrainComputeRasterTileSize;
  const uint tileY = pixel.y / kTerrainComputeRasterTileSize;
  const uint tile = tileY * uniforms.tileGridWidth + tileX;
  const uint count =
      atomic_load_explicit(&tileRanges[tile].count, memory_order_relaxed);
  const uint offset = tileRanges[tile].offset;
  const uint perTile = candidatesPerTile(uniforms);
  const float2 pixelCenter = float2(pixel) + 0.5f;
  const uint pixelIndex = pixel.y * uniforms.viewportWidth + pixel.x;

  for (uint i = 0u; i < min(count, perTile); ++i)
  {
    const uint address = offset + i;
    if (address >= uniforms.tileReferenceCapacity)
      break;
    const uint triangleIndex = tileTriangleIndices[address];
    if (triangleIndex >= uniforms.triangleCount)
      continue;

    float depth = 1.0f;
    const TerrainComputeRasterTriangle triangle = triangles[triangleIndex];
    if (!triangleCoversPixel(triangle, pixelCenter, depth))
      continue;

    const ulong key =
        packVisibilityKey(depth, triangle.payload & uniforms.payloadMask);
    atomic_min_explicit(
        &visibilityKeys[pixelIndex], key, memory_order_relaxed);
  }
}

kernel void terrainComputeRasterAtomic64CountDecodedVGClusters(
    constant TerrainComputeRasterUniforms &uniforms [[buffer(0)]],
    constant VGCullingUniforms &cullingUniforms [[buffer(1)]],
    device atomic_uint *cullingCounters [[buffer(2)]],
    const device VGVisibleClusterInfo *swVisibleClusterInfos [[buffer(3)]],
    const device VGInstanceData *instances [[buffer(4)]],
    const device VGPageTableEntry *pageTable [[buffer(5)]],
    const device VGDecodedClusterCullData *decodedClusterCull [[buffer(6)]],
    device TerrainComputeRasterTileRange *tileRanges [[buffer(7)]],
    uint clusterSlot [[thread_position_in_grid]])
{
  const uint swClusterCount =
      atomic_load_explicit(&cullingCounters[kVGSoftwareVisibleClusterCounter], memory_order_relaxed);
  if (clusterSlot >= min(swClusterCount, uniforms.triangleCount))
    return;

  const VGVisibleClusterInfo clusterInfo = swVisibleClusterInfos[clusterSlot];
  const VGPageTableEntry entry = pageTable[clusterInfo.pageIndex];
  if (entry.isInstalled == 0u ||
      clusterInfo.pageLocalClusterIndex >= entry.clusterCount)
    return;
  const VGInstanceData instance = instances[clusterInfo.instanceIndex];
  const VGDecodedClusterCullData cull =
      decodedClusterCull[globalClusterIndex(entry, clusterInfo.pageLocalClusterIndex)];
  if (cull.triangleCount == 0u)
    return;

  const VGProjectedBounds bounds =
      decodedVGClusterProjectedBounds(uniforms, cullingUniforms, instance, cull);
  if (!bounds.visible)
    return;

  for (uint y = bounds.firstTile.y; y <= bounds.lastTile.y; ++y)
  {
    for (uint x = bounds.firstTile.x; x <= bounds.lastTile.x; ++x)
    {
      const uint tile = y * uniforms.tileGridWidth + x;
      atomic_fetch_add_explicit(
          &tileRanges[tile].count, 1u, memory_order_relaxed);
    }
  }
}

kernel void terrainComputeRasterAtomic64ScatterDecodedVGClusters(
    constant TerrainComputeRasterUniforms &uniforms [[buffer(0)]],
    constant VGCullingUniforms &cullingUniforms [[buffer(1)]],
    device atomic_uint *cullingCounters [[buffer(2)]],
    const device VGVisibleClusterInfo *swVisibleClusterInfos [[buffer(3)]],
    const device VGInstanceData *instances [[buffer(4)]],
    const device VGPageTableEntry *pageTable [[buffer(5)]],
    const device VGDecodedClusterCullData *decodedClusterCull [[buffer(6)]],
    device TerrainComputeRasterTileRange *tileRanges [[buffer(7)]],
    device uint *tileTriangleIndices [[buffer(8)]],
    device atomic_uint &overflowCounter [[buffer(9)]],
    uint clusterSlot [[thread_position_in_grid]])
{
  const uint swClusterCount =
      atomic_load_explicit(&cullingCounters[kVGSoftwareVisibleClusterCounter], memory_order_relaxed);
  if (clusterSlot >= min(swClusterCount, uniforms.triangleCount))
    return;

  const VGVisibleClusterInfo clusterInfo = swVisibleClusterInfos[clusterSlot];
  const VGPageTableEntry entry = pageTable[clusterInfo.pageIndex];
  if (entry.isInstalled == 0u ||
      clusterInfo.pageLocalClusterIndex >= entry.clusterCount)
    return;
  const VGInstanceData instance = instances[clusterInfo.instanceIndex];
  const VGDecodedClusterCullData cull =
      decodedClusterCull[globalClusterIndex(entry, clusterInfo.pageLocalClusterIndex)];
  if (cull.triangleCount == 0u)
    return;

  const VGProjectedBounds bounds =
      decodedVGClusterProjectedBounds(uniforms, cullingUniforms, instance, cull);
  if (!bounds.visible)
    return;

  const uint perTile = candidatesPerTile(uniforms);
  for (uint y = bounds.firstTile.y; y <= bounds.lastTile.y; ++y)
  {
    for (uint x = bounds.firstTile.x; x <= bounds.lastTile.x; ++x)
    {
      const uint tile = y * uniforms.tileGridWidth + x;
      const uint slot = atomic_fetch_add_explicit(
          &tileRanges[tile].cursor, 1u, memory_order_relaxed);
      const uint address = tileRanges[tile].offset + slot;
      if (slot < perTile && address < uniforms.tileReferenceCapacity)
      {
        tileTriangleIndices[address] = clusterSlot;
      }
      else
      {
        atomic_fetch_add_explicit(&overflowCounter, 1u, memory_order_relaxed);
      }
    }
  }
}

kernel void terrainComputeRasterAtomic64RasterizeDecodedVG(
    constant TerrainComputeRasterUniforms &uniforms [[buffer(0)]],
    constant VGCullingUniforms &cullingUniforms [[buffer(1)]],
    const device VGVisibleClusterInfo *swVisibleClusterInfos [[buffer(2)]],
    const device VGInstanceData *instances [[buffer(3)]],
    const device VGPageTableEntry *pageTable [[buffer(4)]],
    const device VGDecodedClusterGeometryInfo *decodedClusterGeometry [[buffer(5)]],
    const device VGDecodedVertexData *decodedVertices [[buffer(6)]],
    const device uint *decodedIndices [[buffer(7)]],
    const device TerrainComputeRasterTileRange *tileRanges [[buffer(8)]],
    const device uint *tileTriangleIndices [[buffer(9)]],
    device atomic_ulong *visibilityKeys [[buffer(10)]],
    uint2 pixel [[thread_position_in_grid]])
{
  if (pixel.x >= uniforms.viewportWidth || pixel.y >= uniforms.viewportHeight)
    return;

  const uint tileX = pixel.x / kTerrainComputeRasterTileSize;
  const uint tileY = pixel.y / kTerrainComputeRasterTileSize;
  const uint tile = tileY * uniforms.tileGridWidth + tileX;
  const uint count =
      atomic_load_explicit(&tileRanges[tile].count, memory_order_relaxed);
  const uint offset = tileRanges[tile].offset;
  const uint perTile = candidatesPerTile(uniforms);
  const float2 pixelCenter = float2(pixel) + 0.5f;
  const uint pixelIndex = pixel.y * uniforms.viewportWidth + pixel.x;

  for (uint i = 0u; i < min(count, perTile); ++i)
  {
    const uint address = offset + i;
    if (address >= uniforms.tileReferenceCapacity)
      break;
    const uint clusterSlot = tileTriangleIndices[address];
    if (clusterSlot >= uniforms.triangleCount)
      continue;

    const VGVisibleClusterInfo clusterInfo = swVisibleClusterInfos[clusterSlot];
    const VGPageTableEntry entry = pageTable[clusterInfo.pageIndex];
    if (entry.isInstalled == 0u ||
        clusterInfo.pageLocalClusterIndex >= entry.clusterCount)
      continue;
    const uint globalCluster =
        globalClusterIndex(entry, clusterInfo.pageLocalClusterIndex);
    const VGDecodedClusterGeometryInfo geometry =
        decodedClusterGeometry[globalCluster];
    const VGInstanceData instance = instances[clusterInfo.instanceIndex];
    const uint triCount = min(geometry.triangleCount, 128u);

    for (uint tri = 0u; tri < triCount; ++tri)
    {
      const uint cornerBase = geometry.indexOffset + tri * 3u;
      const uint local0 = decodedIndices[cornerBase + 0u];
      const uint local1 = decodedIndices[cornerBase + 1u];
      const uint local2 = decodedIndices[cornerBase + 2u];
      if (local0 >= geometry.vertexCount ||
          local1 >= geometry.vertexCount ||
          local2 >= geometry.vertexCount)
        continue;

      const VGDecodedVertexData v0 = decodedVertices[geometry.vertexOffset + local0];
      const VGDecodedVertexData v1 = decodedVertices[geometry.vertexOffset + local1];
      const VGDecodedVertexData v2 = decodedVertices[geometry.vertexOffset + local2];

      float2 p0;
      float2 p1;
      float2 p2;
      float z0;
      float z1;
      float z2;
      if (!clipToPixel(
              projectDecodedVGPosition(
                  cullingUniforms,
                  instance,
                  float3(v0.positionX, v0.positionY, v0.positionZ)),
              uniforms,
              p0,
              z0) ||
          !clipToPixel(
              projectDecodedVGPosition(
                  cullingUniforms,
                  instance,
                  float3(v1.positionX, v1.positionY, v1.positionZ)),
              uniforms,
              p1,
              z1) ||
          !clipToPixel(
              projectDecodedVGPosition(
                  cullingUniforms,
                  instance,
                  float3(v2.positionX, v2.positionY, v2.positionZ)),
              uniforms,
              p2,
              z2))
        continue;

      float depth = 1.0f;
      if (!screenTriangleCoversPixel(p0, p1, p2, z0, z1, z2, pixelCenter, depth))
        continue;

      const uint payload = packVisibilityPayload(globalCluster, tri) &
          uniforms.payloadMask;
      atomic_min_explicit(
          &visibilityKeys[pixelIndex],
          packVisibilityKey(depth, payload),
          memory_order_relaxed);
    }
  }
}

static inline uint packOctNormal(float3 n)
{
  const float len = abs(n.x) + abs(n.y) + abs(n.z);
  float2 o = len > 1.0e-6f ? n.xy / len : float2(0.0f, 0.0f);
  if (n.z < 0.0f)
  {
    const float2 signs = float2(
        o.x >= 0.0f ? 1.0f : -1.0f, o.y >= 0.0f ? 1.0f : -1.0f);
    o = (1.0f - abs(o.yx)) * signs;
  }
  const uint qx = uint(int(clamp(o.x, -1.0f, 1.0f) * 32767.0f)) & 0xffffu;
  const uint qy = uint(int(clamp(o.y, -1.0f, 1.0f) * 32767.0f)) & 0xffffu;
  return qx | (qy << 16u);
}

kernel void terrainComputeRasterAtomic64Merge(
    constant TerrainComputeRasterUniforms &uniforms [[buffer(0)]],
    const device ulong *visibilityKeys [[buffer(1)]],
    device TerrainComputeRasterPixelPayload *resolvedPayloads [[buffer(2)]],
    uint id [[thread_position_in_grid]])
{
  const uint pixelCount = uniforms.viewportWidth * uniforms.viewportHeight;
  if (id >= pixelCount)
    return;

  const ulong key = visibilityKeys[id];
  if (key == kTerrainComputeRasterEmptyKey)
    return;

  resolvedPayloads[id].depthBits = 0xffffffffu - uint(key >> 32ul);
  resolvedPayloads[id].payload = uint(key & 0xfffffffful);
}

// The decoded-VG flow resolves the winning payload all the way to a surface:
// world normal from the triangle's decoded vertex normals and the dense
// instance from the owning object's stable sparse id, so the G-buffer adapter
// can merge software pixels without binding the geometry buffers itself.
kernel void terrainComputeRasterAtomic64MergeDecodedVG(
    constant TerrainComputeRasterUniforms &uniforms [[buffer(0)]],
    const device ulong *visibilityKeys [[buffer(1)]],
    device TerrainComputeRasterPixelPayload *resolvedPayloads [[buffer(2)]],
    const device VGDecodedClusterGeometryInfo *decodedClusterGeometry [[buffer(3)]],
    const device VGDecodedVertexData *decodedVertices [[buffer(4)]],
    const device uint *decodedIndices [[buffer(5)]],
    const device VGInstanceData *instances [[buffer(6)]],
    const device uint *instanceSparseToDense [[buffer(7)]],
    uint id [[thread_position_in_grid]])
{
  const uint pixelCount = uniforms.viewportWidth * uniforms.viewportHeight;
  if (id >= pixelCount)
    return;

  const ulong key = visibilityKeys[id];
  if (key == kTerrainComputeRasterEmptyKey)
    return;

  const uint payload = uint(key & 0xfffffffful);
  const uint cluster = payload >> 7u;
  const uint triangle = payload & 0x7fu;
  const VGDecodedClusterGeometryInfo info = decodedClusterGeometry[cluster];
  if (triangle >= info.triangleCount)
    return;
  const uint triangleBase = info.indexOffset + triangle * 3u;
  const VGDecodedVertexData v0 =
      decodedVertices[info.vertexOffset + decodedIndices[triangleBase + 0u]];
  const VGDecodedVertexData v1 =
      decodedVertices[info.vertexOffset + decodedIndices[triangleBase + 1u]];
  const VGDecodedVertexData v2 =
      decodedVertices[info.vertexOffset + decodedIndices[triangleBase + 2u]];
  const float3 localNormal =
      float3(v0.normalX, v0.normalY, v0.normalZ) +
      float3(v1.normalX, v1.normalY, v1.normalZ) +
      float3(v2.normalX, v2.normalY, v2.normalZ);

  const uint sparseOwner = info.padding1;
  const uint denseInstance = sparseOwner != 0xffffffffu
      ? instanceSparseToDense[sparseOwner]
      : 0xffffffffu;
  float3 worldNormal = localNormal;
  if (denseInstance != 0xffffffffu)
    worldNormal =
        (instances[denseInstance].modelMatrix * float4(localNormal, 0.0f)).xyz;
  const float worldLen = length(worldNormal);
  worldNormal = worldLen > 1.0e-6f
      ? worldNormal / worldLen
      : float3(0.0f, 0.0f, 1.0f);

  // Keys pack 1-z so atomicMin keeps the nearest fragment; hand the consumer
  // reverse-Z device depth again.
  resolvedPayloads[id].depthBits = 0xffffffffu - uint(key >> 32ul);
  resolvedPayloads[id].payload = payload;
  resolvedPayloads[id].normalOct = packOctNormal(worldNormal);
  resolvedPayloads[id].denseInstance = denseInstance;
}

// Unified VG visibility backend. One workgroup per software-routed cluster,
// one thread per triangle, and one indivisible depth/payload winner per pixel.
kernel void vgVisibilityClear(device ulong *keys [[buffer(0)]],
    constant uint2 &viewport [[buffer(1)]], uint pixel [[thread_position_in_grid]])
{
  if (pixel < viewport.x * viewport.y) keys[pixel] = 0ul;
}
inline float vgVisibilityEdge(float2 a, float2 b, float2 p)
{ return (p.x-a.x)*(b.y-a.y)-(p.y-a.y)*(b.x-a.x); }
inline bool vgVisibilityTopLeft(float2 a, float2 b)
{ float2 d=b-a; return d.y < 0.0f || (d.y == 0.0f && d.x < 0.0f); }
inline uint vgExtractBits(device const uint *words, uint base, uint offset, uint bits) {
  if (!bits) return 0u;
  uint wi=base+offset/32u, bi=offset%32u;
  uint v=words[wi]>>bi;
  if (bi+bits>32u) v |= words[wi+1u] << (32u-bi);
  return v & (bits==32u ? 0xffffffffu : ((1u<<bits)-1u));
}
inline uint vgEncodedIndex(device const uint *words, VGDecodedClusterGeometryInfo g, uint corner) {
  uint p=g.indexOffset, d=g.vertexOffset;
  uint base=p+72u+words[p]*34u+words[p+1u]+words[p+2u]+words[p+3u]+words[d+6u];
  return (words[base+corner/4u] >> ((corner%4u)*8u)) & 255u;
}
inline float3 vgEncodedPosition(device const uint *words, VGDecodedClusterGeometryInfo g, uint index) {
  uint p=g.indexOffset, d=g.vertexOffset;
  uint3 bits=uint3(32u)-clz(uint3(words[d+10u],words[d+11u],words[d+12u]));
  uint base=p+72u+words[p]*34u+words[d], off=index*(bits.x+bits.y+bits.z);
  uint3 q=uint3(vgExtractBits(words,base,off,bits.x),
               vgExtractBits(words,base,off+bits.x,bits.y),
               vgExtractBits(words,base,off+bits.x+bits.y,bits.z));
  float3 minimum=as_type<float3>(uint3(words[d+14u],words[d+15u],words[d+16u]));
  float scale=float(1u<<words[d+13u])*as_type<float>(g.padding0);
  return (minimum+float3(q))/scale;
}
kernel void vgVisibilityRaster(
    constant VGCullingUniforms &view [[buffer(0)]],
    constant uint2 &viewport [[buffer(1)]],
    device const VGInstanceData *instances [[buffer(2)]],
    device const VGPageTableEntry *pages [[buffer(3)]],
    device const VGVisibleClusterInfo *visible [[buffer(4)]],
    device const uint *counters [[buffer(5)]],
    device const VGDecodedClusterGeometryInfo *geometries [[buffer(6)]],
    device const VGDecodedVertexData *vertices [[buffer(7)]],
    device const uint *indices [[buffer(8)]],
    device atomic_ulong *keys [[buffer(9)]],
    device const uint *encoded [[buffer(10)]],
    uint clusterSlot [[threadgroup_position_in_grid]],
    uint triangle [[thread_index_in_threadgroup]])
{
  if (clusterSlot >= counters[4]) return;
  VGVisibleClusterInfo item = visible[clusterSlot];
  VGPageTableEntry page = pages[item.pageIndex];
  uint cluster = page.clusterOffset + item.pageLocalClusterIndex;
  VGDecodedClusterGeometryInfo geometry = geometries[cluster];
  if (triangle >= geometry.triangleCount) return;
  float3 screen[3];
  for (uint corner=0u; corner<3u; ++corner)
  {
    uint index=geometry.padding0 ? vgEncodedIndex(encoded, geometry, triangle*3u+corner) : indices[geometry.indexOffset+triangle*3u+corner];
    if (index >= geometry.vertexCount) return;
    float3 position;
    if (geometry.padding0) position=vgEncodedPosition(encoded, geometry, index);
    else {
      VGDecodedVertexData vtx=vertices[geometry.vertexOffset+index];
      position=float3(vtx.positionX,vtx.positionY,vtx.positionZ);
    }
    float4 world=instances[item.instanceIndex].modelMatrix*float4(position,1.0f);
    float4 clip=view.proj*view.view*float4(world.xyz,1.0f);
    if (clip.w <= 1.0e-6f) return;
    // Same reverse-Z clip convention and top-left viewport as the WGSL path.
    screen[corner]=float3((clip.x/clip.w*.5f+.5f)*viewport.x,
                         (-clip.y/clip.w*.5f+.5f)*viewport.y,clip.z/clip.w);
  }
  float2 p0=screen[0].xy,p1=screen[1].xy,p2=screen[2].xy;
  float z0=screen[0].z,z1=screen[1].z,z2=screen[2].z;
  float area=vgVisibilityEdge(p0,p1,p2);
  if (area == 0.0f) return;
  if (area < 0.0f) { float2 p=p1; p1=p2; p2=p; float z=z1; z1=z2; z2=z; area=-area; }
  float2 lo=max(floor(min(min(p0,p1),p2)),float2(0));
  float2 hi=min(ceil(max(max(p0,p1),p2)),float2(viewport));
  if (any(hi<=lo)) return;
  bool t0=vgVisibilityTopLeft(p0,p1),t1=vgVisibilityTopLeft(p1,p2),t2=vgVisibilityTopLeft(p2,p0);
  uint payload=packVisibilityPayload(cluster,triangle);
  for (uint y=uint(lo.y); y<uint(hi.y); ++y)
    for (uint x=uint(lo.x); x<uint(hi.x); ++x)
    {
      float2 p=float2(x,y)+.5f;
      float w0=vgVisibilityEdge(p1,p2,p),w1=vgVisibilityEdge(p2,p0,p),w2=vgVisibilityEdge(p0,p1,p);
      if (!((w0>0 || (w0==0 && t1)) && (w1>0 || (w1==0 && t2)) && (w2>0 || (w2==0 && t0)))) continue;
      float z=(w0*z0+w1*z1+w2*z2)/area;
      if (z<0 || z>1) continue;
      ulong key=(ulong(as_type<uint>(max(z,0.0f)))<<32u)|ulong(payload);
      atomic_max_explicit(&keys[y*viewport.x+x],key,memory_order_relaxed);
    }
}
kernel void vgVisibilityResolve(device const ulong *keys [[buffer(0)]],
    constant uint2 &viewport [[buffer(1)]], device uint *depth [[buffer(2)]],
    device uint *payload [[buffer(3)]], uint pixel [[thread_position_in_grid]])
{
  if (pixel>=viewport.x*viewport.y) return;
  ulong key=keys[pixel]; depth[pixel]=uint(key>>32u); payload[pixel]=uint(key);
}
