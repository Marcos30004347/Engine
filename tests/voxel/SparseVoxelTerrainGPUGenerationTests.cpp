// GPU contract of the GPU-generated terrain virtual-geometry pages.
//
// Synthetic fine-raster bricks (a height field on the 8-cubed brick lattice)
// go through the real emit chain on the GPU and are checked against a CPU
// enumeration of the same exposed faces:
//   [count]    the count transaction reports the exposed faces per tile;
//   [emit]     the emit transaction writes exactly those faces, tile by tile,
//              into the reservation the count sized, with the page's spheres,
//              errors and per-quadrant streaming flags on every cluster, and
//              the page installs into the scene;
//   [flags]    the flag rewrite follows the resident child mask;
//   [lod]      a parent page and its four children through the real culler:
//              far draws the parent alone, near draws the children alone, and
//              a missing child leaves exactly the parent's quadrant drawn -
//              never both levels, never a hole;
//   [feedback] the culler requests the missing children only when close.
#include "SparseVoxelVGTestFixture.hpp"

#include "os/Logger.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryGeneratedPayloadManager.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include <thread>
#include <chrono>
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
#include "virtualgeometry/raytracing/metal/MetalPageBlasGate.hpp"
#endif
#include "virtualgeometry/rendering/VirtualGeometryRendererPass.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/TerrainGPUV2FineRaster.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2GeneratedVGEmitPass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace rendering;
using namespace virtualgeometry;
using namespace virtualgeometry::gpgpu;
namespace svr = voxel::svraster;
namespace svrender = voxel::svraster::rendering;

namespace
{
int failures = 0;
void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  if (!condition)
    ++failures;
}

constexpr uint32_t kImageSide = 256u;
constexpr float kNear = 0.1f;
constexpr float kFar = 4000.0f;
constexpr float kErrorBudget = 1.5f;
constexpr uint32_t kStatsBytes = 128u;
constexpr uint32_t kMaxQueueElements = 1u << 18u;
constexpr uint32_t kPageSlots = 8u;
constexpr const char *kObjectName = "terrain-gpu-generated";

// ---------------------------------------------------------------------------
// Synthetic source: a height field voxelised on the brick lattice, the way
// the fine raster leaves it for the emit (occupied shell under the surface).
// ---------------------------------------------------------------------------
struct SyntheticPage
{
  uint32_t side = 64u;
  float voxel = 1.0f;
  std::array<float, 3u> origin{};
  std::vector<uint32_t> heights; // top voxel row per column, exclusive
  std::vector<svr::SparseVoxelBrickNodeGPU> nodes;
  std::vector<svr::SparseVoxelBrickOccupancyGPU> occupancy;
  // brick (bx, by, bz) -> node index
  std::map<std::array<uint32_t, 3u>, uint32_t> brickNodes;
  uint32_t bricksPerAxis() const { return side / 8u; }
};

// World-space height in metres of the shared surface the pages sample.
float surfaceHeight(const float x, const float z)
{
  float h = 6.0f + 3.0f * std::sin(x * 0.11f) + 2.5f * std::cos(z * 0.07f + x * 0.03f);
  if (x > 40.0f && x < 52.0f && z > -20.0f && z < 30.0f)
    h += 9.0f;
  if (z > 8.0f && z < 14.0f)
    h += 4.0f + std::fmod(std::fabs(x), 3.0f);
  return h;
}

SyntheticPage buildSyntheticPage(const uint32_t side, const float voxel, const std::array<float, 3u> &origin)
{
  SyntheticPage page{};
  page.side = side;
  page.voxel = voxel;
  page.origin = origin;
  page.heights.assign(side * side, 0u);
  for (uint32_t z = 0u; z < side; ++z)
    for (uint32_t x = 0u; x < side; ++x)
    {
      const float worldX = origin[0] + (static_cast<float>(x) + 0.5f) * voxel;
      const float worldZ = origin[2] + (static_cast<float>(z) + 0.5f) * voxel;
      const float h = (surfaceHeight(worldX, worldZ) - origin[1]) / voxel;
      page.heights[x + z * side] = static_cast<uint32_t>(std::max(1.0f, std::round(h)));
    }
  constexpr uint32_t kShell = 3u;
  const uint32_t bricks = side / 8u;
  std::map<std::array<uint32_t, 3u>, svr::SparseVoxelBrickOccupancyGPU> occupied;
  for (uint32_t z = 0u; z < side; ++z)
    for (uint32_t x = 0u; x < side; ++x)
    {
      const uint32_t top = page.heights[x + z * side];
      uint32_t lowest = top;
      const std::array<std::array<int32_t, 2u>, 4u> offsets{{{{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}}}};
      for (const auto &offset : offsets)
      {
        const int32_t nx = static_cast<int32_t>(x) + offset[0];
        const int32_t nz = static_cast<int32_t>(z) + offset[1];
        if (nx < 0 || nz < 0 || nx >= static_cast<int32_t>(side) || nz >= static_cast<int32_t>(side))
          continue;
        lowest = std::min(lowest, page.heights[static_cast<uint32_t>(nx) + static_cast<uint32_t>(nz) * side]);
      }
      const uint32_t floor = std::min(top > kShell ? top - kShell : 0u, lowest > 0u ? lowest - 1u : 0u);
      for (uint32_t y = floor; y < top; ++y)
      {
        const std::array<uint32_t, 3u> brick{x / 8u, y / 8u, z / 8u};
        auto &cells = occupied[brick];
        const uint32_t cell = (x & 7u) + (y & 7u) * 8u + (z & 7u) * 64u;
        cells.cells[cell >> 5u].bits |= 1u << (cell & 31u);
      }
    }
  for (const auto &[brick, cells] : occupied)
  {
    svr::SparseVoxelBrickNodeGPU node{};
    node.minimumVoxelSize = {
      origin[0] + static_cast<float>(brick[0] * 8u) * voxel,
      origin[1] + static_cast<float>(brick[1] * 8u) * voxel,
      origin[2] + static_cast<float>(brick[2] * 8u) * voxel,
      voxel};
    node.occupancyIndex = static_cast<uint32_t>(page.nodes.size());
    node.canonicalGridOriginScale = {
      static_cast<int32_t>(brick[0] * 8u), static_cast<int32_t>(brick[1] * 8u),
      static_cast<int32_t>(brick[2] * 8u), 1};
    node.selfError = voxel * 0.25f;
    node.parentError = std::numeric_limits<float>::max();
    page.brickNodes.emplace(brick, node.occupancyIndex);
    page.nodes.push_back(node);
    page.occupancy.push_back(cells);
  }
  (void)bricks;
  return page;
}

bool cellOccupied(const SyntheticPage &page, const int32_t x, const int32_t y, const int32_t z)
{
  if (x < 0 || y < 0 || z < 0)
    return false;
  const std::array<uint32_t, 3u> brick{static_cast<uint32_t>(x) / 8u, static_cast<uint32_t>(y) / 8u, static_cast<uint32_t>(z) / 8u};
  const auto found = page.brickNodes.find(brick);
  if (found == page.brickNodes.end())
    return false;
  const uint32_t cell = (static_cast<uint32_t>(x) & 7u) + (static_cast<uint32_t>(y) & 7u) * 8u + (static_cast<uint32_t>(z) & 7u) * 64u;
  return (page.occupancy[found->second].cells[cell >> 5u].bits & (1u << (cell & 31u))) != 0u;
}

// A face as the emit writes it: its four corners, in order.
struct FaceKey
{
  std::array<std::array<int32_t, 3u>, 4u> corners{};
  bool operator<(const FaceKey &other) const { return corners < other.corners; }
  bool operator==(const FaceKey &other) const { return corners == other.corners; }
};

struct ReferenceFaces
{
  std::array<uint32_t, svrender::kTerrainGPUV2GeneratedVGEmitTileCount> tileFaces{};
  std::multiset<FaceKey> faces;
  std::map<FaceKey, uint32_t> faceTile;
  uint32_t total = 0u;
};

std::array<int32_t, 3u> faceCornerOffset(const uint32_t face, const uint32_t corner)
{
  static const int32_t table[6][4][3] = {
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},
    {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}},
    {{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}},
    {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},
    {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}},
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},
  };
  return {table[face][corner][0], table[face][corner][1], table[face][corner][2]};
}

uint32_t tileOfBrick(const SyntheticPage &page, const uint32_t bx, const uint32_t bz)
{
  const uint32_t bricks = std::max(page.bricksPerAxis(), 8u);
  const uint32_t tx = std::min(bx * 8u / bricks, 7u);
  const uint32_t tz = std::min(bz * 8u / bricks, 7u);
  return tx + tz * 8u;
}

ReferenceFaces enumerateReferenceFaces(const SyntheticPage &page, const bool omitNegativeY)
{
  ReferenceFaces reference{};
  const std::array<std::array<int32_t, 3u>, 6u> normals{{{{-1, 0, 0}}, {{1, 0, 0}}, {{0, -1, 0}}, {{0, 1, 0}}, {{0, 0, -1}}, {{0, 0, 1}}}};
  for (const auto &[brick, nodeIndex] : page.brickNodes)
    for (uint32_t cell = 0u; cell < 512u; ++cell)
    {
      if ((page.occupancy[nodeIndex].cells[cell >> 5u].bits & (1u << (cell & 31u))) == 0u)
        continue;
      const int32_t x = static_cast<int32_t>(brick[0] * 8u + (cell & 7u));
      const int32_t y = static_cast<int32_t>(brick[1] * 8u + ((cell >> 3u) & 7u));
      const int32_t z = static_cast<int32_t>(brick[2] * 8u + ((cell >> 6u) & 7u));
      for (uint32_t face = 0u; face < 6u; ++face)
      {
        if (omitNegativeY && face == 2u)
          continue;
        if (cellOccupied(page, x + normals[face][0], y + normals[face][1], z + normals[face][2]))
          continue;
        FaceKey key{};
        for (uint32_t corner = 0u; corner < 4u; ++corner)
        {
          const auto offset = faceCornerOffset(face, corner);
          key.corners[corner] = {x + offset[0], y + offset[1], z + offset[2]};
        }
        const uint32_t tile = tileOfBrick(page, brick[0], brick[2]);
        reference.tileFaces[tile] += 1u;
        reference.faces.insert(key);
        reference.faceTile[key] = tile;
        ++reference.total;
      }
    }
  return reference;
}

// ---------------------------------------------------------------------------
// The GPU rig: a scene with one generated-terrain object of kPageSlots parked
// leaves, the emit chain over synthetic source buffers, and the renderer.
// ---------------------------------------------------------------------------
std::vector<VirtualGeometryHierarchy> parkedHierarchy(const uint32_t slots, const float extent)
{
  std::vector<VirtualGeometryHierarchy> hierarchy;
  VirtualGeometryHierarchy root{};
  root.min_x = -extent; root.min_y = -extent; root.min_z = -extent;
  root.max_x = extent; root.max_y = extent; root.max_z = extent;
  root.max_center_x = 0.0f; root.max_center_y = 0.0f; root.max_center_z = 0.0f;
  root.max_radius = extent * 1.8f;
  root.min_lod_error = 0.0f;
  root.max_parent_lod_error = std::numeric_limits<float>::max();
  root.child_start = 1u;
  root.child_count = slots;
  root.pageIndex = UINT32_MAX;
  root.meshPartIndex = UINT32_MAX;
  root.flags = HIERARCHY_FORCE_TRAVERSAL_FLAG;
  hierarchy.push_back(root);
  for (uint32_t slot = 0u; slot < slots; ++slot)
  {
    VirtualGeometryHierarchy leaf{};
    leaf.max_parent_lod_error = 0.0f;
    leaf.child_start = 0u;
    leaf.child_count = 0u;
    leaf.pageIndex = PAGE_NOT_INSTALLED_BIT | slot;
    leaf.meshPartIndex = UINT32_MAX;
    leaf.flags = HIERARCHY_LEAF_FLAG | HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG;
    hierarchy.push_back(leaf);
  }
  return hierarchy;
}

struct CullingCounters
{
  uint32_t hierarchyQueueSize;
  uint32_t clusterQueueSize;
  uint32_t visibleClusterHardwareCount;
  uint32_t readQueueSize;
  uint32_t visibleClusterSoftwareCount;
};

struct FrameResult
{
  uint32_t phaseOneHardware = 0u;
  uint32_t phaseTwoHardware = 0u;
  std::array<uint32_t, 32u> preStats{};
  std::array<uint32_t, 32u> finalStats{};
  std::string statsText() const
  {
    std::string text = "pre[";
    for (uint32_t word = 0u; word < 32u; ++word)
      text += (word ? "," : "") + std::to_string(preStats[word]);
    text += "] final[";
    for (uint32_t word = 0u; word < 32u; ++word)
      text += (word ? "," : "") + std::to_string(finalStats[word]);
    return text + "]";
  }
  // (page slot, local cluster) of every drawn cluster.
  std::set<std::pair<uint32_t, uint32_t>> drawn;
};

class Rig
{
public:
  explicit Rig(backend::vulkan::VulkanRHI *rhi, const uint32_t nodeCapacity, const std::string &prefix,
      std::shared_ptr<virtualgeometry::raytracing::PageBlasGate> gate = nullptr)
      : prefix_(prefix), nodeCapacity_(nodeCapacity)
  {
    graphStorage_ = std::make_unique<RenderGraph>(rhi, RenderGraph::Settings{.maxFramesInFlight = 1u});
    graph_ = graphStorage_.get();
    VirtualGeometryScene::Settings settings;
    settings.pageBlasGate=std::move(gate); settings.metalExportableBuffers=bool(settings.pageBlasGate);
    scene_ = std::make_unique<VirtualGeometryScene>(graph_, 256u * 1024u, 96ull * 1024u * 1024u, settings);
    VirtualGeometryScene::RuntimeObjectCreateInfo createInfo{};
    createInfo.name = kObjectName;
    createInfo.pageCount = kPageSlots;
    createInfo.hierarchy = parkedHierarchy(kPageSlots, 512.0f);
    createInfo.quantizationConfig.unit_scale = 1.0f;
    createInfo.quantizationConfig.padMeshlets = true;
    const float minPoint[3] = {-512.0f, -512.0f, -512.0f};
    const float maxPoint[3] = {512.0f, 512.0f, 512.0f};
    createInfo.localBounds.expandBy(minPoint);
    createInfo.localBounds.expandBy(maxPoint);
    std::string error;
    registered_ = scene_->registerRuntimeObject(createInfo, nullptr, &error);
    if (!registered_)
      os::Logger::errorf("%s: register failed: %s", prefix.c_str(), error.c_str());
    scene_->updateInstanceBuffer();
    manager_ = std::make_unique<VirtualGeometryGeneratedPayloadManager>(*scene_);

    const auto storage = [&](const char *name, const uint64_t bytes, const BufferUsage extra)
    {
      return graph_->createBuffer(BufferInfo{
        .name = prefix_ + name,
        .size = bytes,
        .usage = BufferUsage::BufferUsage_Storage | extra,
      });
    };
    fineResults_ = storage("FineResults", sizeof(svr::TerrainGPUV2FineRasterResultGPU), BufferUsage::BufferUsage_Push);
    nodes_ = storage("Nodes", uint64_t{nodeCapacity_} * sizeof(svr::SparseVoxelBrickNodeGPU), BufferUsage::BufferUsage_Push);
    occupancy_ = storage("Occupancy", uint64_t{nodeCapacity_} * sizeof(svr::SparseVoxelBrickOccupancyGPU), BufferUsage::BufferUsage_Push);
    materialWords_ = storage("MaterialWords", 16u, BufferUsage::BufferUsage_Push);
    svrender::TerrainGPUV2GeneratedVGEmitPass::Capacities capacities{};
    capacities.requests = 1u;
    capacities.sourceNodeLaneStride = nodeCapacity_;
    capacities.sourceLanes = 1u;
    capacities.maxClustersPerRequest = VirtualGeometryScene::MAX_CLUSTERS_PER_PAGE;
    capacities.decodedClusterCapacity = VirtualGeometryScene::MAX_DECODED_CLUSTERS;
    capacities.decodedVertexCapacity = static_cast<uint32_t>(scene_->decodedVertexBufferSize / sizeof(DecodedVertexData));
    capacities.decodedIndexCapacity = static_cast<uint32_t>(scene_->decodedIndexBufferSize / sizeof(uint32_t));
    capacities.materialWordCapacity = 4u;
    capacities.flagRequests = 8u;
    capacities.workgroup = 128u;
    uint32_t passIndex = 0u;
    emit_ = std::make_unique<svrender::TerrainGPUV2GeneratedVGEmitChain>(
        *graph_, passIndex,
        svrender::TerrainGPUV2GeneratedVGEmitChain::SourceBuffers{fineResults_, nodes_, occupancy_, materialWords_},
        svrender::TerrainGPUV2GeneratedVGEmitChain::DestinationBuffers{
          scene_->decodedClusterCullBuffer, scene_->decodedClusterGeometryBuffer, scene_->decodedVertexBuffer, scene_->decodedIndexBuffer},
        capacities, prefix_ + "Emit");
    const auto staging = [&](const char *name, const uint64_t bytes)
    {
      return graph_->createBuffer(BufferInfo{
        .name = prefix_ + name,
        .size = bytes,
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
      });
    };
    resultStaging_ = staging("EmitResult.staging", sizeof(svrender::TerrainGPUV2GeneratedVGEmitResultGPU));
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(
        prefix_ + "copyEmitResult", passIndex++, emit_->results(), 0u,
        sizeof(svrender::TerrainGPUV2GeneratedVGEmitResultGPU), resultStaging_, 0u,
        sizeof(svrender::TerrainGPUV2GeneratedVGEmitResultGPU));
    // The scene's decoded buffers are device-local: read them through a
    // staged window over the front of each buffer, where the pages of one
    // test land.
    const auto window = [&](const char *name, const Buffer &source, const uint64_t bytes)
    {
      const Buffer target = staging(name, bytes);
      graph_->registerPass<rendering::gpgpu::CopyBufferPass>(
          prefix_ + std::string("copy") + name, passIndex++, source, 0u, bytes, target, 0u, bytes);
      return target;
    };
    geometryWindowBytes_ = uint64_t{kDecodedWindowClusters} * sizeof(DecodedClusterGeometryInfo);
    cullWindowBytes_ = uint64_t{kDecodedWindowClusters} * sizeof(DecodedClusterCullData);
    vertexWindowBytes_ = uint64_t{kDecodedWindowClusters} * svrender::kTerrainGPUV2GeneratedVGEmitFacesPerCluster *
        svrender::kTerrainGPUV2GeneratedVGEmitVerticesPerFace * sizeof(DecodedVertexData);
    indexWindowBytes_ = uint64_t{kDecodedWindowClusters} * svrender::kTerrainGPUV2GeneratedVGEmitFacesPerCluster *
        svrender::kTerrainGPUV2GeneratedVGEmitIndicesPerFace * sizeof(uint32_t);
    geometryStaging_ = window("Geometry.staging", scene_->decodedClusterGeometryBuffer, geometryWindowBytes_);
    cullStaging_ = window("Cull.staging", scene_->decodedClusterCullBuffer, cullWindowBytes_);
    vertexStaging_ = window("Vertex.staging", scene_->decodedVertexBuffer, vertexWindowBytes_);
    indexStaging_ = window("Index.staging", scene_->decodedIndexBuffer, indexWindowBytes_);
    setupRenderer(passIndex);
  }

  bool valid() const { return registered_ && renderer_ != nullptr; }
  VirtualGeometryScene &scene() { return *scene_; }
  VirtualGeometryGeneratedPayloadManager &manager() { return *manager_; }
  RenderGraph &graph() { return *graph_; }

  void uploadSource(const SyntheticPage &page, const uint32_t generation)
  {
    svr::TerrainGPUV2FineRasterResultGPU fine{};
    fine.status = 2u; // ready
    fine.nodeCount = static_cast<uint32_t>(page.nodes.size());
    fine.terrainGeneration = generation;
    graph_->bufferWrite(fineResults_, 0u, sizeof(fine), &fine);
    graph_->bufferWrite(nodes_, 0u, page.nodes.size() * sizeof(page.nodes[0]), const_cast<svr::SparseVoxelBrickNodeGPU *>(page.nodes.data()));
    graph_->bufferWrite(occupancy_, 0u, page.occupancy.size() * sizeof(page.occupancy[0]), const_cast<svr::SparseVoxelBrickOccupancyGPU *>(page.occupancy.data()));
  }

  // One graph run: emit chain (with the given requests) and the renderer.
  FrameResult run(
      const std::vector<svrender::TerrainGPUV2GeneratedVGEmitRequestGPU> &requests,
      const std::vector<svrender::TerrainGPUV2GeneratedVGFlagRequestGPU> &flags,
      const vgfixture::CameraState &camera)
  {
    std::string error;
    if (!emit_->update(requests, flags, &error))
      os::Logger::errorf("%s: emit update failed: %s", prefix_.c_str(), error.c_str());
    graph_->setCurrentFrameIndex(0u);
    updateUniforms(camera);
    scene_->prepareFrame(0u);
    RenderGraph::Overrides overrides;
    renderer_->appendFrameOverrides(overrides, 0u);
    RenderGraph::Frame frame{};
    frame.frameIndex = 0u;
    graph_->run(frame, overrides);
    graph_->waitFrame(frame);
    graph_->setCurrentFrameIndex(0u);
    return collect();
  }

  svrender::TerrainGPUV2GeneratedVGEmitResultGPU emitResult()
  {
    svrender::TerrainGPUV2GeneratedVGEmitResultGPU result{};
    graph_->bufferRead(resultStaging_, 0u, sizeof(result), [&](const void *data) { std::memcpy(&result, data, sizeof(result)); });
    return result;
  }

  // Reads [offset, offset + count) elements of a decoded scene buffer from
  // its staged window (copied every run).
  template <typename T>
  std::vector<T> readDecoded(const Buffer &buffer, const uint32_t offset, const uint32_t count)
  {
    std::vector<T> values(count);
    if (count == 0u)
      return values;
    Buffer stagingBuffer{};
    uint64_t windowBytes = 0u;
    if (buffer.name == scene_->decodedClusterGeometryBuffer.name) { stagingBuffer = geometryStaging_; windowBytes = geometryWindowBytes_; }
    else if (buffer.name == scene_->decodedClusterCullBuffer.name) { stagingBuffer = cullStaging_; windowBytes = cullWindowBytes_; }
    else if (buffer.name == scene_->decodedVertexBuffer.name) { stagingBuffer = vertexStaging_; windowBytes = vertexWindowBytes_; }
    else if (buffer.name == scene_->decodedIndexBuffer.name) { stagingBuffer = indexStaging_; windowBytes = indexWindowBytes_; }
    else { stagingBuffer = buffer; windowBytes = (uint64_t{offset} + count) * sizeof(T); }
    const uint64_t begin = uint64_t{offset} * sizeof(T);
    const uint64_t bytes = uint64_t{count} * sizeof(T);
    if (begin + bytes > windowBytes)
    {
      os::Logger::errorf("%s: decoded read past the staged window (%llu + %llu > %llu)", prefix_.c_str(),
                         static_cast<unsigned long long>(begin), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(windowBytes));
      return values;
    }
    graph_->bufferRead(stagingBuffer, begin, bytes, [&](const void *data) { std::memcpy(values.data(), data, bytes); });
    return values;
  }

  uint32_t nodeCapacity() const { return nodeCapacity_; }

private:
  void setupRenderer(uint32_t &passIndex)
  {
    const auto color = [&](const char *name, const Format format)
    {
      return graph_->createTexture(TextureInfo{
        .name = prefix_ + name,
        .format = format,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
        .width = kImageSide,
        .height = kImageSide,
        .mipLevels = 1u,
      });
    };
    const auto colorView = [](const Texture texture)
    {
      return TextureView{
        .texture = texture, .index = 0u, .flags = ImageAspectFlags::Color, .baseMipLevel = 0u, .levelCount = 1u,
        .baseArrayLayer = 0u, .layerCount = 1u, .access = AccessPattern::COLOR_ATTACHMENT_WRITE, .layout = ResourceLayout::COLOR_ATTACHMENT,
      };
    };
    Texture depth = graph_->createTexture(TextureInfo{
      .name = prefix_ + "Depth.texture",
      .format = Format::Format_Depth32Float,
      .memoryProperties = BufferUsage::BufferUsage_None,
      .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
      .width = kImageSide,
      .height = kImageSide,
      .mipLevels = 1u,
    });
    const TextureView depthView{
      .texture = depth, .index = 0u, .flags = ImageAspectFlags::Depth, .baseMipLevel = 0u, .levelCount = 1u, .baseArrayLayer = 0u, .layerCount = 1u,
      .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE | AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ, .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
    };
    Texture idsLo = color("IdsLo.texture", Format::Format_R32Uint);
    Texture idsHi = color("IdsHi.texture", Format::Format_R32Uint);
    Texture materialId = color("MaterialId.texture", Format::Format_R32Uint);
    Texture materialUV = color("MaterialUV.texture", Format::Format_RG32Float);
    Texture geometryNormal = color("GeometryNormal.texture", Format::Format_RGBA16Float);
    Texture motion = color("Motion.texture", Format::Format_RG16Float);
    Texture baseColor = color("BaseColor.texture", Format::Format_RGBA16Float);
    Texture materialProperties = color("MaterialProperties.texture", Format::Format_RGBA16Float);
    Texture emissive = color("Emissive.texture", Format::Format_RGBA16Float);
    Texture normal = color("Normal.texture", Format::Format_RGBA16Float);
    Texture colorTexture = color("Color.texture", Format::Format_RGBA16Float);
    hzbMipCount_ = 1u;
    for (uint32_t dimension = kImageSide; dimension > 1u; dimension >>= 1u)
      ++hzbMipCount_;
    hzbMipCount_ = std::min(hzbMipCount_, 7u);
    Texture hzb = graph_->createTexture(TextureInfo{
      .name = prefix_ + "HZB.texture",
      .format = Format::Format_R32Float,
      .memoryProperties = BufferUsage::BufferUsage_Storage,
      .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
      .width = kImageSide,
      .height = kImageSide,
      .mipLevels = hzbMipCount_,
    });
    VirtualGeometryHardwareDrawPass::FrameTarget frameTarget{
      .packedGeometryIdsLoView = colorView(idsLo),
      .packedGeometryIdsHiView = colorView(idsHi),
      .materialIdView = colorView(materialId),
      .materialUVView = colorView(materialUV),
      .baseColorView = colorView(baseColor),
      .materialPropertiesView = colorView(materialProperties),
      .emissiveView = colorView(emissive),
      .geometryNormalView = colorView(geometryNormal),
      .normalView = colorView(normal),
      .motionVectorView = colorView(motion),
      .depthView = depthView,
      .depthTexture = depth,
      .baseColorTexture = baseColor,
      .materialPropertiesTexture = materialProperties,
      .emissiveTexture = emissive,
      .geometryNormalTexture = geometryNormal,
      .normalTexture = normal,
      .motionVectorTexture = motion,
      .colorView = colorView(colorTexture),
      .colorTexture = colorTexture,
    };
    VirtualGeometryDepthPrePassDrawPass::FrameTarget prePassTarget{.depthView = depthView, .depthTexture = depth};
    VirtualGeometryRendererPass::Settings settings{};
    settings.prepassCullingSettings.maxHierarchyLevels = 32u;
    settings.prepassCullingSettings.clustersQueueBufferSize = kMaxQueueElements * sizeof(uint32_t) * 4u;
    settings.prepassCullingSettings.errorTreshhold = kErrorBudget;
    settings.prepassCullingSettings.viewPortWidth = kImageSide;
    settings.prepassCullingSettings.viewPortHeight = kImageSide;
    settings.prepassCullingSettings.hiZMipLevels = hzbMipCount_;
    settings.prepassCullingSettings.useExternalHiZTexture = true;
    // The cut is what is under test: no frustum or occlusion rejection.
    settings.prepassCullingSettings.enableFrustumCulling = false;
    settings.prepassCullingSettings.enableOcclusionCulling = false;
    settings.prepassCullingSettings.enableStreamingPriorityUpdates = false;
    settings.prepassCullingSettings.swTriangleEdgePixelThreshold = 0.0f;
    settings.finalCullingSettings = settings.prepassCullingSettings;
    settings.finalCullingSettings.enableStreamingPriorityUpdates = true;
    settings.depthPrePassSettings.viewPortWidth = kImageSide;
    settings.depthPrePassSettings.viewPortHeight = kImageSide;
    settings.depthPrePassSettings.cullMode = CullMode::None;
    settings.depthPrePassSettings.directDrawWhenIndirectCountUnavailable = false;
    settings.depthPyramidSettings.width = kImageSide;
    settings.depthPyramidSettings.height = kImageSide;
    settings.depthPyramidSettings.outputWidth = kImageSide;
    settings.depthPyramidSettings.outputHeight = kImageSide;
    settings.depthPyramidSettings.mipCount = hzbMipCount_;
    settings.depthPyramidSettings.outputFormat = Format::Format_R32Float;
    settings.hardwareDrawSettings.viewPortWidth = kImageSide;
    settings.hardwareDrawSettings.viewPortHeight = kImageSide;
    settings.hardwareDrawSettings.colorFormat = Format::Format_RGBA16Float;
    settings.hardwareDrawSettings.motionVectorFormat = Format::Format_RG16Float;
    settings.hardwareDrawSettings.depthFormat = Format::Format_Depth32Float;
    settings.hardwareDrawSettings.cullMode = CullMode::None;
    settings.hardwareDrawSettings.directDrawWhenIndirectCountUnavailable = false;
    settings.computeRasterSettings.enabled = false;
    settings.twoPhaseOcclusion = true;
    settings.registerScreenSpaceRayTracePass = false;
    settings.registerMaterialPass = false;
    settings.unifiedVisibility = true;
    settings.softwareRaster.enabled = false;
    renderer_ = graph_->registerPass<VirtualGeometryRendererPass>(
        prefix_ + "renderer", passIndex++, *scene_, frameTarget, prePassTarget, Texture{}, hzb, 0.0f, settings);
    passIndex += VirtualGeometryRendererPass::childPassCount(settings);
    graph_->compile();
    preCulling_ = renderer_->getPreCullingPass();
    finalCulling_ = renderer_->getFinalCullingPass();
    phaseOneDraw_ = renderer_->getPhaseOneDrawPass();
    hardwareDraw_ = renderer_->getHardwareDrawPass();
    if (preCulling_ == nullptr || finalCulling_ == nullptr || phaseOneDraw_ == nullptr || hardwareDraw_ == nullptr)
    {
      renderer_ = nullptr;
      return;
    }
    const auto staging = [&](const char *name, const uint64_t bytes)
    {
      return graph_->createBuffer(BufferInfo{
        .name = prefix_ + name,
        .size = bytes,
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
      });
    };
    visibleBytes_ = static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * sizeof(VisibleClusterInfo);
    preStatsStaging_ = staging("PreStats.staging", kStatsBytes);
    finalStatsStaging_ = staging("FinalStats.staging", kStatsBytes);
    preHardwareStaging_ = staging("PreHardware.staging", visibleBytes_);
    finalHardwareStaging_ = staging("FinalHardware.staging", visibleBytes_);
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(prefix_ + "copyPreStats", passIndex++, preCulling_->getCullingStatisticsBuffer(), 0, kStatsBytes, preStatsStaging_, 0, kStatsBytes);
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(prefix_ + "copyFinalStats", passIndex++, finalCulling_->getCullingStatisticsBuffer(), 0, kStatsBytes, finalStatsStaging_, 0, kStatsBytes);
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(prefix_ + "copyPreHardware", passIndex++, preCulling_->getHWVisibleClusterInfosBuffer(), 0, visibleBytes_, preHardwareStaging_, 0, visibleBytes_);
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(prefix_ + "copyFinalHardware", passIndex++, finalCulling_->getHWVisibleClusterInfosBuffer(), 0, visibleBytes_, finalHardwareStaging_, 0, visibleBytes_);
    graph_->compile();
  }

  void updateUniforms(const vgfixture::CameraState &camera)
  {
    ++frameNumber_;
    scene_->setRuntimeStreamingFrame(frameNumber_);
    scene_->touchRuntimePagesFromGPUPriorities();
    const float *view = camera.view.data;
    const float *projection = camera.projection.data;
    float viewPosition[4] = {camera.eye[0], camera.eye[1], camera.eye[2], 1.0f};
    preCulling_->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_);
    finalCulling_->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_);
    phaseOneDraw_->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_, kErrorBudget);
    hardwareDraw_->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_, kErrorBudget);
    if (auto *pass = renderer_->getPhaseOneVisibilityResolvePass())
      pass->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_, kErrorBudget);
    if (auto *pass = renderer_->getVisibilityResolvePass())
      pass->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_, kErrorBudget);
  }

  FrameResult collect()
  {
    FrameResult result{};
    CullingCounters pre{};
    CullingCounters fin{};
    graph_->bufferRead(preStatsStaging_, 0, kStatsBytes, [&](const void *data) { std::memcpy(result.preStats.data(), data, kStatsBytes); });
    graph_->bufferRead(finalStatsStaging_, 0, kStatsBytes, [&](const void *data) { std::memcpy(result.finalStats.data(), data, kStatsBytes); });
    std::memcpy(&pre, result.preStats.data(), sizeof(pre));
    std::memcpy(&fin, result.finalStats.data(), sizeof(fin));
    result.phaseOneHardware = pre.visibleClusterHardwareCount;
    result.phaseTwoHardware = fin.visibleClusterHardwareCount;
    const auto readVisible = [&](const Buffer &stagingBuffer, const uint32_t count)
    {
      if (count == 0u || count > VirtualGeometryScene::MAX_VISIBLE_CLUSTERS)
        return;
      std::vector<VisibleClusterInfo> visible(count);
      graph_->bufferRead(stagingBuffer, 0, count * sizeof(VisibleClusterInfo),
                         [&](const void *data) { std::memcpy(visible.data(), data, count * sizeof(VisibleClusterInfo)); });
      for (const auto &info : visible)
        result.drawn.emplace(info.pageIndex, info.pageLocalClusterIndex);
    };
    readVisible(preHardwareStaging_, result.phaseOneHardware);
    readVisible(finalHardwareStaging_, result.phaseTwoHardware);
    return result;
  }

  std::string prefix_;
  uint32_t nodeCapacity_ = 0u;
  std::unique_ptr<RenderGraph> graphStorage_;
  RenderGraph *graph_ = nullptr;
  std::unique_ptr<VirtualGeometryScene> scene_;
  std::unique_ptr<VirtualGeometryGeneratedPayloadManager> manager_;
  std::unique_ptr<svrender::TerrainGPUV2GeneratedVGEmitChain> emit_;
  bool registered_ = false;
  Buffer fineResults_{};
  Buffer nodes_{};
  Buffer occupancy_{};
  Buffer materialWords_{};
  Buffer resultStaging_{};
  static constexpr uint32_t kDecodedWindowClusters = 2048u;
  Buffer geometryStaging_{};
  Buffer cullStaging_{};
  Buffer vertexStaging_{};
  Buffer indexStaging_{};
  uint64_t geometryWindowBytes_ = 0u;
  uint64_t cullWindowBytes_ = 0u;
  uint64_t vertexWindowBytes_ = 0u;
  uint64_t indexWindowBytes_ = 0u;
  uint32_t hzbMipCount_ = 1u;
  VirtualGeometryRendererPass *renderer_ = nullptr;
  VirtualGeometryCullingMultipleDispatchesPass *preCulling_ = nullptr;
  VirtualGeometryCullingMultipleDispatchesPass *finalCulling_ = nullptr;
  VirtualGeometryHardwareDrawPass *phaseOneDraw_ = nullptr;
  VirtualGeometryHardwareDrawPass *hardwareDraw_ = nullptr;
  uint64_t visibleBytes_ = 0u;
  Buffer preStatsStaging_{};
  Buffer finalStatsStaging_{};
  Buffer preHardwareStaging_{};
  Buffer finalHardwareStaging_{};
  uint64_t frameNumber_ = 0u;
};

// A page's description for the emit: bounds fixed for the whole test.
struct PageDescription
{
  uint32_t slot = 0u;
  uint32_t parentSlot = UINT32_MAX;
  SyntheticPage source;
  svrender::TerrainGPUV2GeneratedVGEmitPageInfo info{};
  svrender::TerrainGPUV2GeneratedVGClusterPlan plan{};
  VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor descriptor{};
  uint32_t clusterCount = 0u;
};

std::array<float, 3u> pageMin(const SyntheticPage &page) { return page.origin; }
std::array<float, 3u> pageMax(const SyntheticPage &page)
{
  uint32_t top = 0u;
  for (const uint32_t h : page.heights)
    top = std::max(top, h);
  return {page.origin[0] + static_cast<float>(page.side) * page.voxel,
          page.origin[1] + static_cast<float>(top) * page.voxel,
          page.origin[2] + static_cast<float>(page.side) * page.voxel};
}

vgfixture::CameraState cameraAt(const float distance, const std::array<float, 3u> &target)
{
  // Looking down at the page centre from the +x/+z side at 30 degrees.
  const math::Vec3f eye(target[0] + distance * 0.5f, target[1] + distance * 0.5f, target[2] + distance * 0.707f);
  return vgfixture::makeCamera(eye, math::Vec3f(target[0], target[1], target[2]), kNear, kFar);
}

// Runs the count then the emit transaction for one page, installs it, and
// returns the emit result. `camera` only feeds the renderer alongside.
bool generatePage(Rig &rig, PageDescription &page, const uint32_t generation, const vgfixture::CameraState &camera, std::string &error)
{
  rig.uploadSource(page.source, generation);
  svrender::TerrainGPUV2GeneratedVGEmitRequestGPU count{};
  if (!svrender::buildTerrainGPUV2GeneratedVGCountRequest(
          page.info, 0u, rig.nodeCapacity(), static_cast<uint32_t>(page.source.nodes.size()), generation, count, &error))
    return false;
  rig.run({count}, {}, camera);
  const auto counted = rig.emitResult();
  if (counted.status != static_cast<uint32_t>(svrender::TerrainGPUV2GeneratedVGEmitStatus::Counted))
  {
    error = "count status " + std::to_string(counted.status) + " validation " + std::to_string(counted.validationErrors);
    return false;
  }
  page.plan = svrender::terrainGPUV2GeneratedVGClusterPlan(counted.tileFaces);
  VirtualGeometryGeneratedPayloadManager::Request request{};
  request.objectName = kObjectName;
  request.localPageIndex = page.slot;
  request.parentLocalPageIndex = page.parentSlot;
  request.fallbackLocalPageIndex = page.parentSlot;
  request.pageGeneration = generation;
  request.geometryRevision = generation;
  request.materialRevision = generation;
  request.unitScaleBits = 0x3f800000u;
  svrender::terrainGPUV2GeneratedVGReservationForPlan(page.plan, request);
  request.rootPage = page.info.rootPage;
  request.overrideHierarchyLeafMetadata = true;
  request.hierarchyBoundsMin = page.info.boundsMin;
  request.hierarchyBoundsMax = page.info.boundsMax;
  request.hierarchySelfError = page.info.selfError;
  request.hierarchyParentError = page.info.parentError;
  request.hierarchyParentSphereValid = !page.info.rootPage;
  request.hierarchyParentBoundsMin = page.info.parentBoundsMin;
  request.hierarchyParentBoundsMax = page.info.parentBoundsMax;
  request.decodedClusterFlags = true;
  request.priority = 1u;
  if (!rig.manager().enqueueRequest(request, &error))
    return false;
  std::vector<VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor> descriptors;
  std::vector<std::string> errors;
  if (rig.manager().reserveReady(1u, &descriptors, &errors) != 1u)
  {
    error = errors.empty() ? "reservation failed" : errors.front();
    return false;
  }
  page.descriptor = descriptors.front();
  svrender::TerrainGPUV2GeneratedVGEmitRequestGPU emit{};
  if (!svrender::buildTerrainGPUV2GeneratedVGEmitRequest(
          page.descriptor, page.info, 0u, rig.nodeCapacity(), static_cast<uint32_t>(page.source.nodes.size()), generation, page.plan, emit, &error))
    return false;
  rig.run({emit}, {}, camera);
  const auto emitted = rig.emitResult();
  if (!svrender::publishTerrainGPUV2GeneratedVGEmitResult(rig.manager(), page.descriptor, emitted, &error))
    return false;
  page.clusterCount = emitted.emittedClusters;
  rig.scene().updateInstanceBuffer();
  return true;
}

// ---------------------------------------------------------------------------
// [count] and [emit]: one page against the CPU enumeration.
// ---------------------------------------------------------------------------
void testCountAndEmit(backend::vulkan::VulkanRHI *rhi,
    std::shared_ptr<virtualgeometry::raytracing::PageBlasGate> gate = nullptr)
{
  std::cout << "[count][emit] a page's faces per tile, and the emitted clusters\n";
  Rig rig(rhi, 1024u, "gen.emit.", gate);
  check(rig.valid(), "rig is valid");
  if (!rig.valid())
    return;
  PageDescription page{};
  page.slot = 0u;
  page.source = buildSyntheticPage(64u, 1.0f, {-32.0f, 0.0f, -32.0f});
  page.info.boundsMin = pageMin(page.source);
  page.info.boundsMax = pageMax(page.source);
  page.info.parentBoundsMin = {-64.0f, 0.0f, -64.0f};
  page.info.parentBoundsMax = {64.0f, 40.0f, 64.0f};
  page.info.selfError = 1.0f;
  page.info.parentError = 2.0f;
  page.info.rootPage = false;
  page.info.trueLeaf = false;
  page.info.omitNegativeYFace = true;
  page.info.residentChildMask = 0x5u;
  page.info.pageBricksPerAxis = 8u;
  const auto reference = enumerateReferenceFaces(page.source, true);
  const auto camera = cameraAt(300.0f, {0.0f, 8.0f, 0.0f});

  // Count.
  rig.uploadSource(page.source, 1u);
  svrender::TerrainGPUV2GeneratedVGEmitRequestGPU count{};
  std::string error;
  check(svrender::buildTerrainGPUV2GeneratedVGCountRequest(page.info, 0u, rig.nodeCapacity(), static_cast<uint32_t>(page.source.nodes.size()), 1u, count, &error),
        "count request builds: " + error);
  rig.run({count}, {}, camera);
  const auto counted = rig.emitResult();
  check(counted.status == static_cast<uint32_t>(svrender::TerrainGPUV2GeneratedVGEmitStatus::Counted) && counted.validationErrors == 0u,
        "count transaction reports Counted");
  check(counted.emittedFaces == reference.total, "counted faces equal the CPU enumeration: gpu=" + std::to_string(counted.emittedFaces) + " cpu=" + std::to_string(reference.total));
  bool tilesMatch = true;
  for (uint32_t tile = 0u; tile < 64u; ++tile)
    tilesMatch = tilesMatch && counted.tileFaces[tile] == reference.tileFaces[tile];
  check(tilesMatch, "per-tile face counts equal the CPU enumeration");
  check(reference.total > 4096u, "the synthetic page has more than its top faces (" + std::to_string(reference.total) + ")");

  // Emit into the exact reservation.
  page.plan = svrender::terrainGPUV2GeneratedVGClusterPlan(counted.tileFaces);
  check(page.plan.clusterCount > 64u && page.plan.clusterCount <= VirtualGeometryScene::MAX_CLUSTERS_PER_PAGE,
        "cluster plan spans the page: clusters=" + std::to_string(page.plan.clusterCount));
  VirtualGeometryGeneratedPayloadManager::Request request{};
  request.objectName = kObjectName;
  request.localPageIndex = page.slot;
  request.pageGeneration = 1u;
  request.geometryRevision = 1u;
  request.materialRevision = 1u;
  request.unitScaleBits = 0x3f800000u;
  svrender::terrainGPUV2GeneratedVGReservationForPlan(page.plan, request);
  request.rootPage = true;
  request.overrideHierarchyLeafMetadata = true;
  request.hierarchyBoundsMin = page.info.boundsMin;
  request.hierarchyBoundsMax = page.info.boundsMax;
  request.hierarchySelfError = page.info.selfError;
  request.hierarchyParentError = page.info.parentError;
  request.decodedClusterFlags = true;
  request.priority = 1u;
  check(rig.manager().enqueueRequest(request, &error), "payload request enqueues: " + error);
  std::vector<VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor> descriptors;
  check(rig.manager().reserveReady(1u, &descriptors) == 1u, "the exact reservation succeeds");
  if (descriptors.empty())
    return;
  page.descriptor = descriptors.front();
  check(page.descriptor.clusterCount == page.plan.clusterCount, "the reservation is the plan's cluster count");
  svrender::TerrainGPUV2GeneratedVGEmitRequestGPU emit{};
  check(svrender::buildTerrainGPUV2GeneratedVGEmitRequest(page.descriptor, page.info, 0u, rig.nodeCapacity(), static_cast<uint32_t>(page.source.nodes.size()), 1u, page.plan, emit, &error),
        "emit request builds: " + error);
  rig.run({emit}, {}, camera);
  const auto emitted = rig.emitResult();
  check(emitted.status == static_cast<uint32_t>(svrender::TerrainGPUV2GeneratedVGEmitStatus::Ready) && emitted.validationErrors == 0u,
        "emit transaction reports Ready (status " + std::to_string(emitted.status) + " validation " + std::to_string(emitted.validationErrors) + ")");
  check(emitted.emittedFaces == reference.total && emitted.emittedClusters == page.plan.clusterCount, "emit wrote every counted face into the planned clusters");
  check(emitted.reserved0 == reference.total && emitted.reserved1 == page.plan.clusterCount,
        "the tile counters at finalize hold every face and imply the planned clusters (" + std::to_string(emitted.reserved0) + "/" + std::to_string(emitted.reserved1) + ")");

  // Read the page back.
  const auto geometry = rig.readDecoded<DecodedClusterGeometryInfo>(rig.scene().decodedClusterGeometryBuffer, page.descriptor.clusterOffset, page.plan.clusterCount);
  const auto cull = rig.readDecoded<DecodedClusterCullData>(rig.scene().decodedClusterCullBuffer, page.descriptor.clusterOffset, page.plan.clusterCount);
  const auto vertices = rig.readDecoded<DecodedVertexData>(rig.scene().decodedVertexBuffer, page.descriptor.decodedVertexOffset, page.descriptor.decodedVertexCount);
  const auto indices = rig.readDecoded<uint32_t>(rig.scene().decodedIndexBuffer, page.descriptor.decodedIndexOffset, page.descriptor.decodedIndexCount);
  std::multiset<FaceKey> emittedFaces;
  bool clusterTilesConsistent = true;
  bool clusterBoundsTight = true;
  bool clusterSpheres = true;
  bool clusterErrors = true;
  bool clusterFlags = true;
  bool clusterSizes = true;
  uint32_t totalTriangles = 0u;
  // The LOD test runs per tile: a cluster's sphere is its tile's square with
  // the page's height range, and its parent sphere is the parent page tile
  // covering the same ground.
  const auto tileSquare = [](const std::array<float, 3u> &boundsMin, const std::array<float, 3u> &boundsMax, const uint32_t tileX, const uint32_t tileZ)
  {
    const float spanX = (boundsMax[0] - boundsMin[0]) / 8.0f;
    const float spanZ = (boundsMax[2] - boundsMin[2]) / 8.0f;
    const std::array<float, 3u> minimum{boundsMin[0] + spanX * static_cast<float>(tileX), boundsMin[1], boundsMin[2] + spanZ * static_cast<float>(tileZ)};
    const std::array<float, 3u> maximum{minimum[0] + spanX, boundsMax[1], minimum[2] + spanZ};
    return std::pair<std::array<float, 3u>, std::array<float, 3u>>{minimum, maximum};
  };
  const auto sphereOf = [](const std::pair<std::array<float, 3u>, std::array<float, 3u>> &box)
  {
    const std::array<float, 3u> center{(box.first[0] + box.second[0]) * 0.5f, (box.first[1] + box.second[1]) * 0.5f, (box.first[2] + box.second[2]) * 0.5f};
    const float dx = box.second[0] - box.first[0];
    const float dy = box.second[1] - box.first[1];
    const float dz = box.second[2] - box.first[2];
    return std::pair<std::array<float, 3u>, float>{center, std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f};
  };
  for (uint32_t cluster = 0u; cluster < page.plan.clusterCount; ++cluster)
  {
    const auto &info = geometry[cluster];
    const auto &data = cull[cluster];
    const uint32_t faces = info.triangleCount / 2u;
    totalTriangles += info.triangleCount;
    clusterSizes = clusterSizes && faces <= svrender::kTerrainGPUV2GeneratedVGEmitFacesPerCluster && info.vertexCount == faces * 4u && data.triangleCount == info.triangleCount;
    const uint32_t tile = info.pageLocalGroupIndex;
    clusterTilesConsistent = clusterTilesConsistent && tile < 64u && page.plan.tileClusterBase[tile] + info.clusterIndexInGroup == cluster;
    const uint32_t vertexBase = info.vertexOffset - page.descriptor.decodedVertexOffset;
    const uint32_t indexBase = info.indexOffset - page.descriptor.decodedIndexOffset;
    for (uint32_t face = 0u; face < faces; ++face)
    {
      FaceKey key{};
      for (uint32_t corner = 0u; corner < 4u; ++corner)
      {
        const auto &vertex = vertices[vertexBase + face * 4u + corner];
        key.corners[corner] = {
          static_cast<int32_t>(std::lround((vertex.positionX - page.source.origin[0]) / page.source.voxel)),
          static_cast<int32_t>(std::lround((vertex.positionY - page.source.origin[1]) / page.source.voxel)),
          static_cast<int32_t>(std::lround((vertex.positionZ - page.source.origin[2]) / page.source.voxel))};
        clusterBoundsTight = clusterBoundsTight &&
            vertex.positionX >= data.localMinX - 1e-3f && vertex.positionX <= data.localMaxX + 1e-3f &&
            vertex.positionY >= data.localMinY - 1e-3f && vertex.positionY <= data.localMaxY + 1e-3f &&
            vertex.positionZ >= data.localMinZ - 1e-3f && vertex.positionZ <= data.localMaxZ + 1e-3f;
      }
      // Two triangles per face on the four private vertices.
      const uint32_t *tri = &indices[indexBase + face * 6u];
      clusterSizes = clusterSizes && tri[0] == face * 4u && tri[1] == face * 4u + 1u && tri[2] == face * 4u + 2u && tri[3] == face * 4u && tri[4] == face * 4u + 2u && tri[5] == face * 4u + 3u;
      emittedFaces.insert(key);
      const auto referenceTile = reference.faceTile.find(key);
      clusterTilesConsistent = clusterTilesConsistent && referenceTile != reference.faceTile.end() && referenceTile->second == tile;
    }
    {
      const uint32_t tileX = tile % 8u;
      const uint32_t tileZ = tile / 8u;
      const auto own = sphereOf(tileSquare(page.info.boundsMin, page.info.boundsMax, tileX, tileZ));
      const uint32_t quadrantX = (page.info.boundsMin[0] + page.info.boundsMax[0]) * 0.5f >= (page.info.parentBoundsMin[0] + page.info.parentBoundsMax[0]) * 0.5f ? 1u : 0u;
      const uint32_t quadrantZ = (page.info.boundsMin[2] + page.info.boundsMax[2]) * 0.5f >= (page.info.parentBoundsMin[2] + page.info.parentBoundsMax[2]) * 0.5f ? 1u : 0u;
      const auto parent = sphereOf(tileSquare(page.info.parentBoundsMin, page.info.parentBoundsMax, quadrantX * 4u + tileX / 2u, quadrantZ * 4u + tileZ / 2u));
      clusterSpheres = clusterSpheres &&
          std::fabs(data.selfCenterX - own.first[0]) < 1e-2f && std::fabs(data.selfCenterY - own.first[1]) < 1e-2f && std::fabs(data.selfCenterZ - own.first[2]) < 1e-2f &&
          std::fabs(data.selfRadius - own.second) < 1e-2f &&
          std::fabs(data.parentCenterX - parent.first[0]) < 1e-2f && std::fabs(data.parentCenterZ - parent.first[2]) < 1e-2f &&
          std::fabs(data.parentRadius - parent.second) < 1e-2f;
    }
    clusterErrors = clusterErrors && data.selfError == page.info.selfError && data.parentError == page.info.parentError;
    const uint32_t quadrant = (tile % 8u >= 4u ? 1u : 0u) | (tile / 8u >= 4u ? 2u : 0u);
    const bool expectStreaming = ((page.info.residentChildMask >> quadrant) & 1u) == 0u;
    clusterFlags = clusterFlags && (((data.flags & DECODED_CLUSTER_FLAG_STREAMING_LEAF) != 0u) == expectStreaming);
  }
  check(totalTriangles == reference.total * 2u, "cluster triangle counts sum to the page's faces");
  check(clusterSizes, "every cluster holds at most 48 faces of four private vertices and two triangles");
  check(emittedFaces == reference.faces, "the emitted quads are exactly the CPU enumeration");
  check(clusterTilesConsistent, "every cluster's faces belong to its tile and the cluster sits in its tile's range");
  check(clusterBoundsTight, "cluster bounds contain their vertices");
  check(clusterSpheres, "every cluster carries its tile sphere and the parent page tile sphere covering it");
  check(clusterErrors, "every cluster carries the page's self and parent error");
  check(clusterFlags, "streaming-leaf flags follow the resident child mask per quadrant");

  // Publish: the page installs with exactly the planned clusters.
  if (gate)
  {
    bool ready=svrender::publishTerrainGPUV2GeneratedVGEmitResult(rig.manager(),page.descriptor,emitted,&error,true);
    check(!ready && rig.manager().isPublishPending(page.descriptor.ticketId) &&
        !rig.scene().getRuntimeObjectPageStatus(kObjectName,page.slot)->installed,
        "GPU emitted page remains uninstalled during native BLAS builds");
    for (uint32_t frame=0;frame<2000 && !ready;++frame)
    {
      gate->pump();
      ready=svrender::publishTerrainGPUV2GeneratedVGEmitResult(rig.manager(),page.descriptor,emitted,&error,true);
      if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(ready,"GPU terrain cluster manifest completed native BLAS builds: "+error);
  }
  check(svrender::publishTerrainGPUV2GeneratedVGEmitResult(rig.manager(), page.descriptor, emitted, &error), "emit result publishes: " + error);
  if (gate) check(rig.scene().runtimePageBlasHandles(kObjectName,page.slot).size()==emitted.emittedClusters,
      "published terrain page has one completed BLAS per emitted cluster");
  if (gate) {
    const auto handles=rig.scene().runtimePageBlasHandles(kObjectName,page.slot);
    const auto gpu=rig.readDecoded<std::array<uint32_t,2>>(rig.scene().rayTraceClusterHandlesBuffer,
        page.descriptor.clusterOffset,emitted.emittedClusters);
    bool matches=gpu.size()==handles.size();
    for (size_t i=0;i<handles.size();++i)
      matches=matches && gpu[i][0]==handles[i] && gpu[i][1]==page.descriptor.globalPageTableIndex;
    check(matches,"GPU cut handle table matches installed terrain manifest and page ownership");
  }
  const auto *status = rig.scene().getRuntimeObjectPageStatus(kObjectName, page.slot);
  check(status != nullptr && status->installed && status->pageTableEntry.clusterCount == page.plan.clusterCount, "the page is installed with the planned cluster count");

  // [flags] rewrite.
  std::cout << "[flags] streaming-leaf flags follow child installs\n";
  svrender::TerrainGPUV2GeneratedVGFlagRequestGPU flag{};
  flag.clusterOffset = page.descriptor.clusterOffset;
  flag.clusterCount = page.plan.clusterCount;
  flag.residentChildMask = 0xfu;
  rig.run({}, {flag}, camera);
  auto flagged = rig.readDecoded<DecodedClusterCullData>(rig.scene().decodedClusterCullBuffer, page.descriptor.clusterOffset, page.plan.clusterCount);
  bool none = true;
  for (const auto &data : flagged)
    none = none && (data.flags & DECODED_CLUSTER_FLAG_STREAMING_LEAF) == 0u;
  check(none, "all four children resident: no cluster is a streaming leaf");
  flag.residentChildMask = 0x2u;
  rig.run({}, {flag}, camera);
  flagged = rig.readDecoded<DecodedClusterCullData>(rig.scene().decodedClusterCullBuffer, page.descriptor.clusterOffset, page.plan.clusterCount);
  bool quadrantOne = true;
  for (uint32_t cluster = 0u; cluster < page.plan.clusterCount; ++cluster)
  {
    const uint32_t tile = geometry[cluster].pageLocalGroupIndex;
    const uint32_t quadrant = (tile % 8u >= 4u ? 1u : 0u) | (tile / 8u >= 4u ? 2u : 0u);
    quadrantOne = quadrantOne && (((flagged[cluster].flags & DECODED_CLUSTER_FLAG_STREAMING_LEAF) != 0u) == (quadrant != 1u));
  }
  check(quadrantOne, "one child resident: only its quadrant's clusters lose the flag");
  flag.residentChildMask = 0xfu;
  flag.flags = svrender::kTerrainGPUV2GeneratedVGEmitTrueLeaf;
  rig.run({}, {flag}, camera);
  flagged = rig.readDecoded<DecodedClusterCullData>(rig.scene().decodedClusterCullBuffer, page.descriptor.clusterOffset, page.plan.clusterCount);
  bool all = true;
  for (const auto &data : flagged)
    all = all && (data.flags & DECODED_CLUSTER_FLAG_STREAMING_LEAF) != 0u;
  check(all, "a true leaf keeps every cluster a streaming leaf whatever the mask");

  // A camera-centered directory can remove and reintroduce the parent while
  // retaining this installed page. Update its GPU cut metric without a BLAS
  // rebuild; a stale finite parent error would cull a newly promoted root.
  flag.updateCullMetadata = 1u;
  for (uint32_t axis = 0; axis < 3; ++axis)
  {
    flag.boundsMin[axis] = page.info.boundsMin[axis];
    flag.boundsMax[axis] = page.info.boundsMax[axis];
    flag.parentBoundsMin[axis] = page.info.parentBoundsMin[axis];
    flag.parentBoundsMax[axis] = page.info.parentBoundsMax[axis];
  }
  flag.selfError = page.info.selfError;
  flag.parentError = page.info.parentError;
  flag.rootPage = 1u;
  rig.run({}, {flag}, camera);
  flagged = rig.readDecoded<DecodedClusterCullData>(rig.scene().decodedClusterCullBuffer, page.descriptor.clusterOffset, page.plan.clusterCount);
  check(std::all_of(flagged.begin(), flagged.end(), [&](const auto &data) {
    return data.parentError > 1.0e20f && data.selfError == page.info.selfError && data.parentRadius == data.selfRadius;
  }), "a promoted resident root has an unconditional GPU parent test");
  flag.rootPage = 0u;
  rig.run({}, {flag}, camera);
  flagged = rig.readDecoded<DecodedClusterCullData>(rig.scene().decodedClusterCullBuffer, page.descriptor.clusterOffset, page.plan.clusterCount);
  check(std::all_of(flagged.begin(), flagged.end(), [&](const auto &data) {
    return data.parentError == page.info.parentError && data.selfError == page.info.selfError;
  }), "a reattached resident child recovers its finite GPU parent error");
}

// ---------------------------------------------------------------------------
// [lod] and [feedback]: a parent page and its four children through the
// culler.
// ---------------------------------------------------------------------------
void testLODCut(backend::vulkan::VulkanRHI *rhi)
{
  std::cout << "[lod][feedback] parent and children through the culler\n";
  Rig rig(rhi, 2048u, "gen.lod.");
  check(rig.valid(), "rig is valid");
  if (!rig.valid())
    return;
  // Parent: 64 columns of 2 m at (-64, -64); children: 64 columns of 1 m.
  PageDescription parent{};
  parent.slot = 0u;
  parent.source = buildSyntheticPage(64u, 2.0f, {-64.0f, 0.0f, -64.0f});
  parent.info.boundsMin = {-64.0f, 0.0f, -64.0f};
  parent.info.boundsMax = {64.0f, 32.0f, 64.0f};
  parent.info.parentBoundsMin = parent.info.boundsMin;
  parent.info.parentBoundsMax = parent.info.boundsMax;
  parent.info.selfError = 2.0f;
  parent.info.parentError = 4.0f;
  parent.info.rootPage = true;
  parent.info.omitNegativeYFace = true;
  parent.info.residentChildMask = 0u;
  std::array<PageDescription, 4u> children{};
  for (uint32_t quadrant = 0u; quadrant < 4u; ++quadrant)
  {
    auto &child = children[quadrant];
    child.slot = 1u + quadrant;
    child.parentSlot = 0u;
    const float originX = (quadrant & 1u) != 0u ? 0.0f : -64.0f;
    const float originZ = (quadrant & 2u) != 0u ? 0.0f : -64.0f;
    child.source = buildSyntheticPage(64u, 1.0f, {originX, 0.0f, originZ});
    child.info.boundsMin = {originX, 0.0f, originZ};
    child.info.boundsMax = {originX + 64.0f, 32.0f, originZ + 64.0f};
    child.info.parentBoundsMin = parent.info.boundsMin;
    child.info.parentBoundsMax = parent.info.boundsMax;
    child.info.selfError = 1.0f;
    child.info.parentError = 2.0f;
    child.info.rootPage = false;
    child.info.trueLeaf = true;
    child.info.omitNegativeYFace = true;
  }
  // Leaves of the children: requestable, parent sphere = the parent's.
  for (const auto &child : children)
  {
    VirtualGeometryScene::RuntimePageLeafInfo leaf{};
    leaf.boundsMin = child.info.boundsMin;
    leaf.boundsMax = child.info.boundsMax;
    leaf.parentSphereBoundsMin = parent.info.boundsMin;
    leaf.parentSphereBoundsMax = parent.info.boundsMax;
    leaf.selfError = 1.0f;
    leaf.parentError = 2.0f;
    leaf.requestable = true;
    std::string leafError;
    check(rig.scene().updateRuntimeObjectPageLeaf(kObjectName, child.slot, leaf, &leafError), "child leaf publishes: " + leafError);
  }
  // Camera distances from the projected error: the parent sphere radius is
  // ~96 m; k = proj11 * 0.5 * 256 ~= 221. e(2 m) >= 1.5 px within ~295 m of
  // the sphere, e(1 m) < 1.5 px beyond ~148 m of the child sphere.
  const std::array<float, 3u> centre{0.0f, 16.0f, 0.0f};
  const auto far = cameraAt(900.0f, centre);
  const auto near = cameraAt(330.0f, centre);
  const float parentRadius = std::sqrt(64.0f * 64.0f * 2.0f + 16.0f * 16.0f);
  const float parentCentre[3] = {0.0f, 16.0f, 0.0f};
  const float nearParentPx = vgfixture::projectedError(near, parentCentre, parentRadius, 2.0f, kNear, kImageSide);
  const float farParentPx = vgfixture::projectedError(far, parentCentre, parentRadius, 2.0f, kNear, kImageSide);
  check(nearParentPx >= kErrorBudget && farParentPx < kErrorBudget,
        "camera distances straddle the parent's error threshold: near=" + std::to_string(nearParentPx) + "px far=" + std::to_string(farParentPx) + "px");
  bool childrenFine = true;
  for (const auto &child : children)
  {
    const float childCentre[3] = {(child.info.boundsMin[0] + child.info.boundsMax[0]) * 0.5f, 16.0f, (child.info.boundsMin[2] + child.info.boundsMax[2]) * 0.5f};
    const float childRadius = std::sqrt(32.0f * 32.0f * 2.0f + 16.0f * 16.0f);
    childrenFine = childrenFine && vgfixture::projectedError(near, childCentre, childRadius, 1.0f, kNear, kImageSide) < kErrorBudget;
  }
  check(childrenFine, "near the children's own voxel projects under the budget");

  std::string error;
  check(generatePage(rig, parent, 1u, far, error), "parent generates and installs: " + error);
  const auto drawnPages = [&](const FrameResult &frame)
  {
    std::map<uint32_t, uint32_t> counts;
    for (const auto &[page, cluster] : frame.drawn)
      ++counts[page];
    return counts;
  };
  // The object's pages occupy consecutive page table slots from its offset.
  VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(rig.scene().getRuntimeObjectDiagnostics(kObjectName, diagnostics), "object diagnostics resolve");
  const auto globalPage = [&](const uint32_t slot) { return diagnostics.pageTableOffset + slot; };
  // A cluster that is one voxel wide projects under a pixel at these
  // distances and may be footprint-culled; the cut contract is about the
  // rest. Two such clusters per page are tolerated.
  const auto expectDrawn = [&](const FrameResult &frame, const uint32_t slot, const uint32_t clusters)
  {
    const auto counts = drawnPages(frame);
    const auto found = counts.find(globalPage(slot));
    return found != counts.end() && found->second <= clusters && found->second + 2u >= clusters;
  };
  const auto notDrawn = [&](const FrameResult &frame, const uint32_t slot)
  {
    return drawnPages(frame).count(globalPage(slot)) == 0u;
  };

  // The invariant the cut has to hold is per tile, not per page: for every
  // piece of ground exactly one level draws. A page is 8x8 tiles and a child
  // page covers one quadrant of its parent, so a child tile (tx,tz) sits
  // inside parent tile (4qx + tx/2, 4qz + tz/2).
  const auto parentTileOfChild = [&](const PageDescription &child, const uint32_t tile)
  {
    const uint32_t quadrantX = (child.info.boundsMin[0] + child.info.boundsMax[0]) * 0.5f >=
        (parent.info.boundsMin[0] + parent.info.boundsMax[0]) * 0.5f ? 1u : 0u;
    const uint32_t quadrantZ = (child.info.boundsMin[2] + child.info.boundsMax[2]) * 0.5f >=
        (parent.info.boundsMin[2] + parent.info.boundsMax[2]) * 0.5f ? 1u : 0u;
    const uint32_t tileX = tile % 8u;
    const uint32_t tileZ = tile / 8u;
    return (quadrantX * 4u + tileX / 2u) + (quadrantZ * 4u + tileZ / 2u) * 8u;
  };
  const auto tilesOf = [&](const PageDescription &page)
  {
    // tile -> clusters with geometry, from the decoded records.
    std::map<uint32_t, uint32_t> tiles;
    const auto geometry = rig.readDecoded<DecodedClusterGeometryInfo>(
        rig.scene().decodedClusterGeometryBuffer, page.descriptor.clusterOffset, page.clusterCount);
    for (const auto &info : geometry)
      if (info.triangleCount != 0u)
        ++tiles[info.pageLocalGroupIndex];
    return tiles;
  };
  const auto clusterTiles = [&](const PageDescription &page)
  {
    std::vector<uint32_t> tiles(page.clusterCount, UINT32_MAX);
    const auto geometry = rig.readDecoded<DecodedClusterGeometryInfo>(
        rig.scene().decodedClusterGeometryBuffer, page.descriptor.clusterOffset, page.clusterCount);
    for (uint32_t cluster = 0u; cluster < page.clusterCount; ++cluster)
      tiles[cluster] = geometry[cluster].triangleCount != 0u ? geometry[cluster].pageLocalGroupIndex : UINT32_MAX;
    return tiles;
  };
  // Ground covered by each level, in parent tiles.
  const auto coverage = [&](const FrameResult &frame)
  {
    struct Coverage
    {
      std::set<uint32_t> parentTiles;
      std::set<uint32_t> childTiles;
      uint32_t parentClusters = 0u;
      uint32_t childClusters = 0u;
    };
    Coverage result{};
    const auto parentTilesByCluster = clusterTiles(parent);
    std::map<uint32_t, std::vector<uint32_t>> childTilesByCluster;
    for (const auto &child : children)
      if (child.clusterCount != 0u)
        childTilesByCluster[child.slot] = clusterTiles(child);
    for (const auto &[pageIndex, cluster] : frame.drawn)
    {
      if (pageIndex == globalPage(0u))
      {
        ++result.parentClusters;
        if (cluster < parentTilesByCluster.size() && parentTilesByCluster[cluster] != UINT32_MAX)
          result.parentTiles.insert(parentTilesByCluster[cluster]);
        continue;
      }
      for (const auto &child : children)
      {
        if (pageIndex != globalPage(child.slot))
          continue;
        ++result.childClusters;
        const auto &tiles = childTilesByCluster[child.slot];
        if (cluster < tiles.size() && tiles[cluster] != UINT32_MAX)
          result.childTiles.insert(parentTileOfChild(child, tiles[cluster]));
      }
    }
    return result;
  };
  const auto disjoint = [](const std::set<uint32_t> &left, const std::set<uint32_t> &right)
  {
    for (const uint32_t value : left)
      if (right.count(value) != 0u)
        return false;
    return true;
  };

  // Parent alone: it draws at both distances, because with no child resident
  // every one of its clusters is a streaming leaf.
  FrameResult frame = rig.run({}, {}, far);
  auto covered = coverage(frame);
  const auto parentTilesWithGeometry = tilesOf(parent);
  check(covered.childClusters == 0u && covered.parentTiles.size() == parentTilesWithGeometry.size(),
        "far, parent alone: every parent tile with geometry draws (" + std::to_string(covered.parentTiles.size()) + "/" +
            std::to_string(parentTilesWithGeometry.size()) + ") " + frame.statsText());
  frame = rig.run({}, {}, near);
  covered = coverage(frame);
  check(covered.childClusters == 0u && covered.parentTiles.size() == parentTilesWithGeometry.size(),
        "near, parent alone: the streaming-leaf parent still covers every tile");
  // Feedback: near asks for the four children, far asks for nothing.
  auto requests = rig.scene().collectRuntimeObjectPageRequests(kObjectName, 8u);
  std::set<uint32_t> requested;
  for (const auto &request : requests)
    requested.insert(request.localPageIndex);
  check(requested == std::set<uint32_t>{1u, 2u, 3u, 4u}, "near, the culler requests exactly the four child pages (" + std::to_string(requested.size()) + ")");
  rig.run({}, {}, far);
  requests = rig.scene().collectRuntimeObjectPageRequests(kObjectName, 8u);
  check(requests.empty(), "far, the culler requests nothing");

  // Children arrive; the parent's flags follow.
  std::string error2;
  for (auto &child : children)
    check(generatePage(rig, child, 2u, far, error2), "child " + std::to_string(child.slot) + " generates and installs: " + error2);
  svrender::TerrainGPUV2GeneratedVGFlagRequestGPU flag{};
  flag.clusterOffset = parent.descriptor.clusterOffset;
  flag.clusterCount = parent.clusterCount;
  flag.residentChildMask = 0xfu;
  frame = rig.run({}, {flag}, near);
  covered = coverage(frame);
  check(disjoint(covered.parentTiles, covered.childTiles),
        "near, all children resident: no ground draws at two levels (parent tiles " + std::to_string(covered.parentTiles.size()) +
            ", child tiles " + std::to_string(covered.childTiles.size()) + ")");
  {
    std::set<uint32_t> union_;
    union_.insert(covered.parentTiles.begin(), covered.parentTiles.end());
    union_.insert(covered.childTiles.begin(), covered.childTiles.end());
    check(union_.size() == parentTilesWithGeometry.size(),
          "near, all children resident: every tile is covered by exactly one level (" + std::to_string(union_.size()) + "/" +
              std::to_string(parentTilesWithGeometry.size()) + ")");
  }
  check(covered.childClusters > covered.parentClusters,
        "near, all children resident: the fine level carries most of the ground (" + std::to_string(covered.childClusters) +
            " child clusters against " + std::to_string(covered.parentClusters) + ")");
  frame = rig.run({}, {}, far);
  covered = coverage(frame);
  check(covered.childClusters == 0u && covered.parentTiles.size() == parentTilesWithGeometry.size(),
        "far, all children resident: the parent draws alone");
  requests = rig.scene().collectRuntimeObjectPageRequests(kObjectName, 8u);
  check(requests.empty(), "installed children are not requested again");

  // One child leaves: its quadrant returns to the parent, the rest stays fine.
  check(rig.scene().uninstallRuntimeObjectPage(kObjectName, children[3].slot, 0u, &error2), "child 3 uninstalls: " + error2);
  rig.scene().updateInstanceBuffer();
  flag.residentChildMask = 0x7u;
  frame = rig.run({}, {flag}, near);
  covered = coverage(frame);
  check(disjoint(covered.parentTiles, covered.childTiles),
        "near, one child missing: no ground draws at two levels");
  {
    std::set<uint32_t> union_;
    union_.insert(covered.parentTiles.begin(), covered.parentTiles.end());
    union_.insert(covered.childTiles.begin(), covered.childTiles.end());
    check(union_.size() == parentTilesWithGeometry.size(),
          "near, one child missing: the parent covers the missing quadrant and nothing is lost (" +
              std::to_string(union_.size()) + "/" + std::to_string(parentTilesWithGeometry.size()) + ")");
  }
  {
    // The missing child's quadrant falls back to the parent in full. The
    // parent also keeps the tiles where its own voxel is already fine enough
    // on screen - that is the cut working per tile, and those tiles are the
    // same ones it kept while every child was resident.
    uint32_t missingQuadrantTiles = 0u;
    uint32_t missingQuadrantDrawn = 0u;
    for (const auto &[tile, clusters] : parentTilesWithGeometry)
    {
      if ((tile % 8u) < 4u || (tile / 8u) < 4u)
        continue;
      ++missingQuadrantTiles;
      missingQuadrantDrawn += covered.parentTiles.count(tile) != 0u ? 1u : 0u;
    }
    check(missingQuadrantTiles != 0u && missingQuadrantDrawn == missingQuadrantTiles,
          "near, one child missing: the parent covers that whole quadrant (" + std::to_string(missingQuadrantDrawn) + "/" +
              std::to_string(missingQuadrantTiles) + " tiles)");
    check(covered.childTiles.size() != 0u,
          "near, one child missing: the resident children still carry their ground (" + std::to_string(covered.childTiles.size()) + " tiles)");
  }
  requests = rig.scene().collectRuntimeObjectPageRequests(kObjectName, 8u);
  requested.clear();
  for (const auto &request : requests)
    requested.insert(request.localPageIndex);
  check(requested == std::set<uint32_t>{children[3].slot}, "near, one child missing: the culler asks for that child alone");
}

} // namespace

int main()
{
  os::Logger::start(100);
  DeviceRequiredLimits limits{0u, 0u, 0u};
  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute | DeviceFeatures::DeviceFeatures_Graphics |
      DeviceFeatures::DeviceFeatures_Subgroup_Basic | DeviceFeatures::DeviceFeatures_Subgroup_Shuffle |
      DeviceFeatures::DeviceFeatures_Timestamp | DeviceFeatures::DeviceFeatures_ClipDistances;
  std::unique_ptr<backend::vulkan::VulkanRHI> rhi;
  try
  {
    rhi = std::make_unique<backend::vulkan::VulkanRHI>(backend::vulkan::Vulkan_1_2, limits, features, std::vector<std::string>{});
    std::vector<VkSurfaceKHR> surfaces;
    rhi->init(surfaces);
  }
  catch (const std::exception &gpuError)
  {
    os::Logger::warningf("[Test] SparseVoxelTerrainGPUGeneration tests skipped: %s", gpuError.what());
    os::Logger::shutdown();
    std::cout << "SparseVoxelTerrainGPUGenerationTests passed (GPU backend skipped)\n";
    return 0;
  }
  std::cout << "Sparse voxel terrain GPU generation tests\n";
  testCountAndEmit(rhi.get());
  testLODCut(rhi.get());
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
  {
    using Backend=virtualgeometry::raytracing::VirtualGeometryMetalRayTraceBackend;
    Backend::Configuration config{VG_RT_METALLIB_PATH,1,1,1,64ull*1024*1024};
    config.maxBlasBuildsPerFrame=1;
    auto backend=std::make_shared<Backend>(config,rhi->exportMetalDevice());
    if (backend->capabilities().available)
      testCountAndEmit(rhi.get(),std::make_shared<virtualgeometry::raytracing::MetalPageBlasGate>(backend,0));
  }
#endif
  rhi.reset();
  os::Logger::shutdown();
  if (failures != 0)
  {
    std::cout << failures << " terrain GPU generation checks failed\n";
    return 1;
  }
  std::cout << "SparseVoxelTerrainGPUGenerationTests passed\n";
  return 0;
}
