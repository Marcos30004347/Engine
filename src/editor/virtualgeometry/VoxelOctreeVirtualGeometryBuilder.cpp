#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"

#include "editor/virtualgeometry/VirtualGeometryHierarchyBuilder.hpp"
#include "voxel/svraster/SparseVoxelBrickSurfaceMesh.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>

namespace vg = virtualgeometry;

namespace voxel::svraster
{
namespace
{

constexpr uint32_t kInvalid = 0xffffffffu;
constexpr uint32_t kMaximumBlockDepth = 3u;

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

struct PageNode
{
  uint32_t globalIndex = kInvalid;
  uint32_t level = 0u;
  SparseVoxelBrickFaceMasks exposed{};
  std::array<uint8_t, kSparseVoxelBrickVoxelCount> cellMaterials{};
  uint32_t exposedFaces = 0u;
  std::array<uint32_t, 8> children{
      kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid};
  // First cluster slot for this node's blocks; blocks index it linearly.
  uint32_t firstBlockCluster = kInvalid;
};

struct VertexKey
{
  int32_t cell[3];
  uint32_t direction;

  bool operator==(const VertexKey &other) const
  {
    return cell[0] == other.cell[0] && cell[1] == other.cell[1] &&
        cell[2] == other.cell[2] && direction == other.direction;
  }
};

struct VertexKeyHash
{
  size_t operator()(const VertexKey &key) const noexcept
  {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const uint64_t value)
    {
      hash = (hash ^ value) * 1099511628211ull;
    };
    mix(static_cast<uint64_t>(static_cast<uint32_t>(key.cell[0])));
    mix(static_cast<uint64_t>(static_cast<uint32_t>(key.cell[1])));
    mix(static_cast<uint64_t>(static_cast<uint32_t>(key.cell[2])));
    mix(key.direction);
    return static_cast<size_t>(hash);
  }
};

vg::LODBounds boundsFromPoints(
    const std::array<float, 3> &minimum,
    const std::array<float, 3> &maximum,
    const float error)
{
  vg::LODBounds bounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    bounds.center[axis] = (minimum[axis] + maximum[axis]) * 0.5f;
  const float dx = maximum[0] - minimum[0];
  const float dy = maximum[1] - minimum[1];
  const float dz = maximum[2] - minimum[2];
  bounds.radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
  bounds.error = error;
  return bounds;
}

// Sphere that contains both inputs, matching the hierarchy builder's join so a
// merged group bound stays a true superset of every member.
vg::LODBounds joinBounds(const vg::LODBounds &a, const vg::LODBounds &b)
{
  if (a.radius <= 0.0f)
    return b;
  if (b.radius <= 0.0f)
    return a;

  float toOther[3];
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    toOther[axis] = b.center[axis] - a.center[axis];
  const float distanceSquared = toOther[0] * toOther[0] +
      toOther[1] * toOther[1] + toOther[2] * toOther[2];

  const float radiusDelta = a.radius - b.radius;
  if (radiusDelta * radiusDelta + 1e-4f >= distanceSquared)
    return a.radius < b.radius ? b : a;

  const float distance = std::sqrt(distanceSquared);
  vg::LODBounds result = a;
  result.radius = (distance + a.radius + b.radius) * 0.5f;
  if (distance > 1e-4f)
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      result.center[axis] += toOther[axis] * (result.radius - a.radius) / distance;
  return result;
}

vg::ClusterCone coneFromQuadDirections(const uint32_t directionMask)
{
  vg::ClusterCone cone{};
  float axis[3] = {0.0f, 0.0f, 0.0f};
  uint32_t used = 0u;
  for (uint32_t direction = 0u; direction < kSparseVoxelBrickFaceDirections;
       ++direction)
  {
    if ((directionMask & (1u << direction)) == 0u)
      continue;
    axis[direction >> 1u] += (direction & 1u) != 0u ? 1.0f : -1.0f;
    ++used;
  }
  const float length =
      std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  if (used == 0u || length <= 1e-6f)
  {
    cone.axis[0] = 0.0f;
    cone.axis[1] = 0.0f;
    cone.axis[2] = 1.0f;
    // Never cull: the block faces in enough directions that no cone is useful.
    cone.cutoff = 1.0f;
    return cone;
  }
  for (uint32_t component = 0u; component < 3u; ++component)
    cone.axis[component] = axis[component] / length;
  // Axis-aligned face normals are at most 90 degrees from the averaged axis
  // whenever more than one direction is present, so only a single-direction
  // block can claim a tight cone.
  cone.cutoff = used == 1u ? 0.0f : 1.0f;
  return cone;
}

} // namespace

bool buildSparseVoxelOctreeVirtualGeometry(
    const SparseVoxelBrickHierarchy &hierarchy,
    const uint32_t firstNode,
    const uint32_t nodeCount,
    const SparseVoxelOctreeVirtualGeometrySettings &settings,
    const vg::VirtualGeometryBuildSettings &buildSettings,
    vg::VirtualGeometryBuildData &result,
    SparseVoxelOctreeVirtualGeometryStats *stats,
    std::string *error)
{
  result = {};
  if (stats != nullptr)
    *stats = {};
  if (error != nullptr)
    error->clear();

  if (firstNode > hierarchy.nodes.size() ||
      nodeCount > hierarchy.nodes.size() - firstNode)
    return fail(error, "Voxel octree VG build received an out of range node span");
  if (nodeCount == 0u)
    return true;
  if (settings.suppressedFaces != nullptr &&
      settings.suppressedFaces->size() != hierarchy.nodes.size())
    return fail(error, "Voxel octree VG suppression count does not match nodes");

  const uint32_t maxTriangles =
      std::clamp(settings.maxTrianglesPerCluster, 2u, vg::ClusterSize);
  const uint32_t maxVertices = std::clamp(
      settings.maxVerticesPerCluster, 4u, static_cast<uint32_t>(vg::MaxVertices));
  const uint32_t maxClustersPerGroup =
      std::clamp(settings.maxClustersPerGroup, 1u, vg::MaxBVHChildren);

  const auto meshingStart = std::chrono::steady_clock::now();
  const auto position = [&](const SparseVoxelBrickNodeGPU &node,
                            const uint32_t axis, const uint32_t cell)
  {
    if (settings.canonicalWorldGrid[3] > 0.0)
    {
      const int64_t coordinate = static_cast<int64_t>(node.canonicalGridOriginScale[axis]) +
          static_cast<int64_t>(cell) * node.canonicalGridOriginScale[3];
      return static_cast<float>(settings.canonicalWorldGrid[axis] - settings.originOffset[axis] +
          static_cast<double>(coordinate) * settings.canonicalWorldGrid[3]);
    }
    return static_cast<float>(static_cast<double>(node.minimumVoxelSize[axis]) -
        settings.originOffset[axis] + static_cast<double>(cell) * node.minimumVoxelSize[3]);
  };

  // Preserve the authored octree topology. World-space grid division cannot
  // recover it after a page translation that is not aligned to every level.
  std::vector<PageNode> nodes(nodeCount);
  uint32_t maximumLevel = 0u;
  for (uint32_t local = 0u; local < nodeCount; ++local)
  {
    auto &node = nodes[local];
    node.globalIndex = firstNode + local;
    node.level = hierarchy.nodes[node.globalIndex].level & 0xffu;
    maximumLevel = std::max(maximumLevel, node.level);
  }
  for (uint32_t local = 0u; local < nodeCount; ++local)
  {
    const auto &source = hierarchy.nodes[firstNode + local];
    if (source.parentIndex < firstNode || source.parentIndex - firstNode >= nodeCount)
      continue;
    auto &parent = nodes[source.parentIndex - firstNode];
    const auto &parentSource = hierarchy.nodes[source.parentIndex];
    const int64_t scale = source.canonicalGridOriginScale[3];
    if (scale <= 0 || parentSource.canonicalGridOriginScale[3] != scale * 2 ||
        parent.level != nodes[local].level + 1u)
      return fail(error, "Voxel octree VG build found an invalid parent scale or level");
    uint32_t octant = 0u;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t offset = static_cast<int64_t>(source.canonicalGridOriginScale[axis]) -
          parentSource.canonicalGridOriginScale[axis];
      if (offset != 0 && offset != kSparseVoxelBrickSide * scale)
        return fail(error, "Voxel octree VG child does not occupy a parent octant");
      if (offset != 0)
        octant |= 1u << axis;
    }
    if (parent.children[octant] != kInvalid)
      return fail(error, "Voxel octree VG parent has duplicate children in an octant");
    parent.children[octant] = local;
  }

  using BrickKey = std::array<int64_t, 4>;
  std::map<BrickKey, uint32_t> geometryBrickNodes;
  if (settings.geometryOnly)
    for (uint32_t local=0u;local<nodeCount;++local)
    {
      const auto &source=hierarchy.nodes[nodes[local].globalIndex];
      const auto &grid=source.canonicalGridOriginScale;
      const BrickKey key={int64_t(nodes[local].level),grid[0],grid[1],grid[2]};
      if (!geometryBrickNodes.emplace(key,local).second)
        return fail(error,"Voxel VG geometry has duplicate same-level brick origins");
    }

  // City geometry builds never read the palette. The legacy terrain path
  // retains its material-aware partition until its resolve migrates.
  uint64_t totalExposedFaces = 0u;
  for (uint32_t local = 0u; local < nodeCount; ++local)
  {
    PageNode &node = nodes[local];
    const auto &source = hierarchy.nodes[node.globalIndex];
    if (source.occupancyIndex >= hierarchy.occupancy.size())
      return fail(error, "Voxel octree VG build found an invalid occupancy index");
    if (!settings.geometryOnly && !sparseVoxelBrickCellMaterials(
            hierarchy.occupancy[source.occupancyIndex],
            hierarchy.materialIndices, source.materialBase, node.cellMaterials))
      return fail(error, "Voxel octree VG build found an invalid material address");
  }

  // ── choose the global block depth and emit clusters ───────────────────────
  struct EmittedCluster
  {
    uint32_t nodeLocal = kInvalid;
    uint32_t clusterIndex = kInvalid;
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    uint32_t directionMask = 0u;
  };

  std::vector<vg::Vertex> vertices;
  std::vector<vg::VirtualGeometryCluster> clusters;
  std::vector<EmittedCluster> clusterInfo;
  uint32_t blockDepth = 0u;
  uint32_t quadTotal = 0u;

  for (;; ++blockDepth)
  {
    vertices.clear();
    clusters.clear();
    clusterInfo.clear();
    quadTotal = 0u;
    totalExposedFaces = 0u;

    const uint32_t blockCells = kSparseVoxelBrickSide >> blockDepth;
    const uint32_t blocksPerSide = 1u << blockDepth;
    const uint32_t blocksPerNode = blocksPerSide * blocksPerSide * blocksPerSide;
    bool overflowed = false;

    std::vector<SparseVoxelBrickSurfaceQuad> quads;
    std::vector<std::vector<SparseVoxelBrickSurfaceQuad>> blockQuads(blocksPerNode);
    std::unordered_map<VertexKey, uint32_t, VertexKeyHash> weld;

    for (uint32_t local = 0u; local < nodeCount && !overflowed; ++local)
    {
      PageNode &node = nodes[local];
      node.firstBlockCluster = static_cast<uint32_t>(clusterInfo.size());
      for (uint32_t block = 0u; block < blocksPerNode; ++block)
        clusterInfo.push_back(EmittedCluster{});

      const auto &source = hierarchy.nodes[node.globalIndex];
      if (settings.geometryOnly)
      {
        SparseVoxelBrickNeighbourOccupancy neighbours{};
        const auto &grid=source.canonicalGridOriginScale;
        for (uint32_t direction=0u;direction<6u;++direction)
        {
          BrickKey key={int64_t(node.level),grid[0],grid[1],grid[2]};
          key[1u+direction/2u]+=(direction&1u?1:-1)*
              int64_t(kSparseVoxelBrickSide)*grid[3];
          const auto found=geometryBrickNodes.find(key);
          if (found!=geometryBrickNodes.end())
          {
            const auto &other=hierarchy.nodes[nodes[found->second].globalIndex];
            neighbours.neighbours[direction]=&hierarchy.occupancy[other.occupancyIndex];
          }
        }
        node.exposedFaces=sparseVoxelBrickExposedFaces(
            hierarchy.occupancy[source.occupancyIndex],neighbours,node.exposed);
      }
      else
        node.exposedFaces = sparseVoxelBrickClosedBlockFaces(
            hierarchy.occupancy[source.occupancyIndex], blockCells, node.exposed);
      if (settings.suppressedFaces != nullptr)
      {
        const auto &suppressed = (*settings.suppressedFaces)[node.globalIndex];
        node.exposedFaces = 0u;
        for (uint32_t direction=0u;direction<kSparseVoxelBrickFaceDirections;++direction)
          for (uint32_t z=0u;z<kSparseVoxelBrickPlanes;++z)
          {
            node.exposed[direction][z] &= ~suppressed[direction][z];
            node.exposedFaces += static_cast<uint32_t>(
                __builtin_popcountll(node.exposed[direction][z]));
          }
      }
      totalExposedFaces += node.exposedFaces;
      if (node.exposedFaces == 0u)
        continue;

      quads.clear();
      if (settings.geometryOnly)
        sparseVoxelBrickGreedyGeometryQuads(
            node.exposed, settings.mergeCoplanarFaces ? blockCells : 1u, quads);
      else
        sparseVoxelBrickGreedyQuads(
            node.exposed, node.cellMaterials,
            settings.mergeCoplanarFaces ? blockCells : 1u, true, quads);
      quadTotal += static_cast<uint32_t>(quads.size());

      for (auto &bucket : blockQuads)
        bucket.clear();
      for (const auto &quad : quads)
      {
        const uint32_t block = (quad.origin[0] / blockCells) +
            blocksPerSide * (quad.origin[1] / blockCells) +
            blocksPerSide * blocksPerSide * (quad.origin[2] / blockCells);
        blockQuads[block].push_back(quad);
      }


      for (uint32_t block = 0u; block < blocksPerNode && !overflowed; ++block)
      {
        if (blockQuads[block].empty())
          continue;
        if (blockQuads[block].size() * 2u > maxTriangles)
        {
          overflowed = true;
          break;
        }

        vg::VirtualGeometryCluster cluster{};
        cluster.groupId = UINT32_MAX;
        cluster.meshPartIndex = UINT32_MAX;
        EmittedCluster info{};
        info.nodeLocal = local;
        info.minimum = {
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
        info.maximum = {
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};

        weld.clear();
        const uint32_t clusterVertexBase = static_cast<uint32_t>(vertices.size());
        for (const auto &quad : blockQuads[block])
        {
          info.directionMask |= 1u << quad.direction;
          const auto corners = sparseVoxelBrickQuadCorners(quad);
          const auto normal = sparseVoxelBrickQuadNormal(quad);
          const auto axes = sparseVoxelBrickFaceAxes(quad.direction);
          std::array<uint32_t, 4> globalIndices{};
          for (uint32_t corner = 0u; corner < 4u; ++corner)
          {
            VertexKey key{};
            for (uint32_t axis = 0u; axis < 3u; ++axis)
              key.cell[axis] = static_cast<int32_t>(corners[corner][axis]);
            key.direction = settings.weldClusterVertices ? quad.direction
                                                         : static_cast<uint32_t>(
                                                               vertices.size() + corner);
            const auto found = weld.find(key);
            if (found != weld.end())
            {
              globalIndices[corner] = found->second;
              continue;
            }

            vg::Vertex vertex{};
            for (uint32_t axis = 0u; axis < 3u; ++axis)
              vertex.pos[axis] = position(source, axis, corners[corner][axis]);
            vertex.norm[0] = normal[0];
            vertex.norm[1] = normal[1];
            vertex.norm[2] = normal[2];
            // Voxel-tiling UV: one unit per cell along the face's own axes, so
            // coplanar neighbours agree and the corner welds.
            vertex.uv[0] = static_cast<float>(corners[corner][axes[0]]);
            vertex.uv[1] = static_cast<float>(corners[corner][axes[1]]);

            globalIndices[corner] = static_cast<uint32_t>(vertices.size());
            weld.emplace(key, globalIndices[corner]);
            vertices.push_back(vertex);
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
              info.minimum[axis] = std::min(info.minimum[axis], vertex.pos[axis]);
              info.maximum[axis] = std::max(info.maximum[axis], vertex.pos[axis]);
            }
          }

          cluster.indices.insert(
              cluster.indices.end(),
              {globalIndices[0],
               globalIndices[1],
               globalIndices[2],
               globalIndices[0],
               globalIndices[2],
               globalIndices[3]});
        }

        if (static_cast<uint32_t>(vertices.size()) - clusterVertexBase > maxVertices)
        {
          overflowed = true;
          break;
        }

        cluster.cone = coneFromQuadDirections(info.directionMask);
        info.clusterIndex = static_cast<uint32_t>(clusters.size());
        // The (node, block) slot is the block lookup table the group pass walks;
        // it holds the cluster index so both directions of the map are O(1).
        clusterInfo[node.firstBlockCluster + block] = info;
        clusters.push_back(std::move(cluster));
      }
    }

    if (!overflowed)
      break;
    if (blockDepth >= kMaximumBlockDepth)
      return fail(error, "Voxel octree VG build could not fit a single cell into a cluster");
  }

  if (clusters.empty())
    return true;

  const uint32_t blocksPerSide = 1u << blockDepth;
  const uint32_t blockCells = kSparseVoxelBrickSide >> blockDepth;
  const uint32_t blocksPerNode = blocksPerSide * blocksPerSide * blocksPerSide;

  // clusterInfo is indexed by (node, block); `block` holds the cluster index of
  // that slot once it has been emitted. An unused slot keeps nodeLocal invalid.
  const auto clusterAt = [&](const uint32_t nodeLocal, const uint32_t block) -> uint32_t
  {
    const uint32_t slot = nodes[nodeLocal].firstBlockCluster + block;
    if (slot >= clusterInfo.size() || clusterInfo[slot].nodeLocal == kInvalid)
      return kInvalid;
    return clusterInfo[slot].clusterIndex;
  };

  const auto meshingEnd = std::chrono::steady_clock::now();

  // ── self bounds: the tight bounds of each cluster's own geometry ──────────
  //
  // The error starts at zero and is overwritten by the group bound for every
  // cluster that *is* a coarsening of something. A cluster with nothing finer
  // under it introduces no approximation inside this DAG, so its self error has
  // to be zero: the culler drops a cluster whose projected self error exceeds
  // the budget, and a finest cluster carrying the brick error floor is dropped
  // the moment the camera gets close enough - with nothing finer to take its
  // place, that is a hole. (The brick raster path wants that floor for its own
  // reasons; virtual geometry needs the LOD0 convention the generic builder
  // uses, which passes 0.f to computeBounds.) `settings.leafSelfError` is the
  // exception for chunks sharing one runtime object, where the frontier's
  // "always draw" comes from the streaming-leaf flag instead.
  std::vector<vg::LODBounds> tightBounds(clusters.size());
  for (uint32_t slot = 0u; slot < clusterInfo.size(); ++slot)
  {
    if (clusterInfo[slot].nodeLocal == kInvalid)
      continue;
    const EmittedCluster &info = clusterInfo[slot];
    tightBounds[info.clusterIndex] =
        boundsFromPoints(info.minimum, info.maximum, std::max(settings.leafSelfError, 0.0f));
  }
  for (uint32_t cluster = 0u; cluster < clusters.size(); ++cluster)
  {
    clusters[cluster].self = tightBounds[cluster];
    clusters[cluster].parent = tightBounds[cluster];
    clusters[cluster].parent.error = std::numeric_limits<float>::max();
  }

  // ── groups: one parent block against the eight child blocks under it ──────
  std::vector<vg::ClusterGroupInfo> groupInfos;
  std::vector<std::vector<std::vector<int32_t>>> groupsPerLevel(maximumLevel + 2u);
  uint32_t vanishingGroups = 0u;

  // Coarse to fine is not required, but building level by level keeps the group
  // list ordered the way the hierarchy builder reports it.
  for (uint32_t level = 1u; level <= maximumLevel; ++level)
  {
    for (uint32_t local = 0u; local < nodeCount; ++local)
    {
      const PageNode &parent = nodes[local];
      if (parent.level != level)
        continue;

      for (uint32_t block = 0u; block < blocksPerNode; ++block)
      {
        const std::array<uint32_t, 3> parentBlock{
            block % blocksPerSide,
            (block / blocksPerSide) % blocksPerSide,
            block / (blocksPerSide * blocksPerSide)};

        std::vector<int32_t> originals;
        vg::LODBounds groupBounds{};
        bool first = true;
        for (uint32_t combination = 0u; combination < 8u; ++combination)
        {
          // The parent block spans 2 * blockCells child cells per axis, which is
          // exactly two child blocks per axis.
          uint32_t childOctant = 0u;
          uint32_t childBlock = 0u;
          uint32_t stride = 1u;
          for (uint32_t axis = 0u; axis < 3u; ++axis)
          {
            const uint32_t step = (combination >> axis) & 1u;
            const uint32_t childCell =
                parentBlock[axis] * 2u * blockCells + step * blockCells;
            childOctant |= (childCell / kSparseVoxelBrickSide) << axis;
            childBlock +=
                ((childCell % kSparseVoxelBrickSide) / blockCells) * stride;
            stride *= blocksPerSide;
          }

          const uint32_t child = parent.children[childOctant];
          if (child == kInvalid)
            continue;
          const uint32_t childCluster = clusterAt(child, childBlock);
          if (childCluster == kInvalid)
            continue;
          originals.push_back(static_cast<int32_t>(childCluster));
          groupBounds = first ? clusters[childCluster].self
                              : joinBounds(groupBounds, clusters[childCluster].self);
          first = false;
        }

        if (originals.empty())
          continue;
        if (originals.size() > maxClustersPerGroup)
          return fail(error, "Voxel octree VG build produced an oversized group");

        const uint32_t parentCluster = clusterAt(local, block);
        // A coarse cell can stick out past the fine geometry under it, so the
        // shared bound has to contain the parent block's own vertices too or the
        // projected error of the coarse cut is computed from too small a sphere.
        if (parentCluster != kInvalid)
          groupBounds = joinBounds(groupBounds, tightBounds[parentCluster]);

        const auto &parentSource = hierarchy.nodes[parent.globalIndex];
        // The cut test compares this error, projected to pixels, against a
        // pixel budget - the Nanite formulation. What it has to measure is the
        // surface deviation introduced by drawing this parent block instead of
        // the eight child blocks under it, and for a voxel lattice that is
        // bounded by the parent cell: a face can move a whole coarse voxel.
        //
        // SparseVoxelBrickNodeGPU::selfError floors at a *quarter* of the voxel
        // (kSparseVoxelBrickErrorFloor) because the brick DDA raster wants that
        // floor for its own reasons. Carrying it into the mesh path let the
        // culler keep voxel faces on screen at four times the intended pixel
        // size - two whole LOD levels too coarse - which reads as terrain that
        // stays blocky no matter how close the camera gets. Flat ground is the
        // worst case: its measured deviation is ~0, so the floor is the whole
        // metric there.
        //
        // Monotonic by construction: a parent cell is exactly twice its child's,
        // and the max below clamps against the children anyway, so a hand-built
        // hierarchy can never produce a non-monotonic cut.
        const float parentVoxelSize = parentSource.minimumVoxelSize[3];
        float groupError = std::max(parentSource.selfError, parentVoxelSize);
        for (const int32_t original : originals)
          groupError = std::max(groupError, clusters[original].self.error);
        groupBounds.error = groupError;

        // A group with no coarse replacement must not consume its children.
        //
        // The parent block can carry no surface at all while its children do -
        // a ridge or a spike that is interior once coarsened - and the old
        // triangle builder's rule for exactly this case is `retry`: a group it
        // cannot simplify is dissolved and its clusters are left unconsumed,
        // which is what `assert(clusters[ci].parent.error == max())` enforces
        // one level up. Consuming them here instead handed them a *finite*
        // parent error with nothing to replace them, so at any budget past that
        // error the culler rejected them and drew nothing in their place.
        // These clusters stay unconsumed and fall through to the root pass,
        // which gives them the "nothing coarser than me" parent error.
        if (parentCluster == kInvalid)
        {
          ++vanishingGroups;
          if (!settings.consumeVanishingGroups)
            continue;
        }

        const uint32_t groupId = static_cast<uint32_t>(groupInfos.size());
        for (const int32_t original : originals)
        {
          clusters[original].parent = groupBounds;
          clusters[original].groupId = groupId;
        }

        vg::ClusterGroupInfo info{};
        info.lodLevel = level - 1u;
        info.originalClusterIndices = originals;
        if (parentCluster != kInvalid)
        {
          clusters[parentCluster].self = groupBounds;
          info.simplifiedClusterIndices.push_back(
              static_cast<int32_t>(parentCluster));
        }

        groupInfos.push_back(std::move(info));
        groupsPerLevel[level - 1u].push_back(originals);
      }
    }
  }

  // ── roots: clusters no group consumes keep an infinite parent error ───────
  uint32_t rootClusters = 0u;
  for (uint32_t slot = 0u; slot < clusterInfo.size(); ++slot)
  {
    if (clusterInfo[slot].nodeLocal == kInvalid)
      continue;
    const uint32_t cluster = clusterInfo[slot].clusterIndex;
    if (clusters[cluster].groupId != UINT32_MAX)
      continue;

    clusters[cluster].parent = clusters[cluster].self;
    clusters[cluster].parent.error = settings.parentChunkErrorAppliesToRoots
        ? settings.parentChunkError
        : std::numeric_limits<float>::max();
    clusters[cluster].groupId = static_cast<uint32_t>(groupInfos.size());

    const uint32_t level = nodes[clusterInfo[slot].nodeLocal].level;
    vg::ClusterGroupInfo info{};
    info.lodLevel = level;
    info.originalClusterIndices = {static_cast<int32_t>(cluster)};
    groupInfos.push_back(std::move(info));
    groupsPerLevel[level].push_back({static_cast<int32_t>(cluster)});
    ++rootClusters;
  }

  // ── the cross-chunk edge: clamp every parent error to the coarser chunk ──
  //
  // Phase 10c. The parent chunk covers this chunk's whole volume one level
  // coarser, so every level of this chunk at or above the parent chunk's
  // finest error draws the same ground the parent chunk already draws. A
  // cluster draws iff `self.error <= budget < parent.error`; clamping the
  // parent error to the parent chunk's finest error makes that range empty for
  // exactly those duplicate levels and leaves the finest level owning the band
  // below the parent chunk. The partition is then exact by construction, with
  // no coverage test, no quadrant mask and no vertical containment test.
  //
  // Nothing is clamped when the caller leaves `parentChunkError` infinite:
  // that is a chunk with nothing coarser over it, which has to keep its whole
  // chain or the cut has no coarse representation to fall back to.
  if (settings.parentChunkError < std::numeric_limits<float>::max())
  {
    for (auto &cluster : clusters)
    {
      if (cluster.parent.error == std::numeric_limits<float>::max())
        continue;
      cluster.parent.error =
          std::min(cluster.parent.error, settings.parentChunkError);
    }
  }

  // ── compact empty LOD levels so the hierarchy builder never sees one ──────
  std::vector<uint32_t> levelRemap(groupsPerLevel.size(), kInvalid);
  std::vector<std::vector<std::vector<int32_t>>> compactedLevels;
  for (uint32_t level = 0u; level < groupsPerLevel.size(); ++level)
  {
    if (groupsPerLevel[level].empty())
      continue;
    levelRemap[level] = static_cast<uint32_t>(compactedLevels.size());
    compactedLevels.push_back(std::move(groupsPerLevel[level]));
  }
  for (auto &info : groupInfos)
    info.lodLevel = levelRemap[info.lodLevel];

  const auto hierarchyStart = std::chrono::steady_clock::now();

  const bool previousLogging = vg::virtualGeometryBuildLoggingEnabled();
  vg::setVirtualGeometryBuildLogging(!settings.quiet);
  result = vg::VirtualGeometryHierarchyBuilder::buildHierarchy(
      vertices, clusters, groupInfos, compactedLevels, nullptr);
  const auto hierarchyBuilt = std::chrono::steady_clock::now();

  vg::VirtualGeometryShapeInfo shapeInfo{};
  shapeInfo.root_node_index = 0u;
  shapeInfo.root_page_index = 0u;
  shapeInfo.hierarchy_node_count =
      static_cast<uint32_t>(result.lodLevelHierarchy.size());
  result.shapes.push_back(shapeInfo);
  result.meshParts.push_back(vg::MeshPartInfo{UINT32_MAX});
  // A real finest error only works with the finest clusters marked streaming
  // leaves: the mark is what keeps the frontier drawing until a finer chunk
  // is resident, and residency clears it per quadrant when one arrives.
  auto rewriteSettings = buildSettings;
  rewriteSettings.finestClustersAreStreamingLeaves = settings.leafSelfError > 0.0f;
  vg::VirtualGeometryHierarchyBuilder::finalizePagesAndRewrites(result, rewriteSettings);
  vg::setVirtualGeometryBuildLogging(previousLogging);

  const auto hierarchyEnd = std::chrono::steady_clock::now();
  // Phase 9b: the voxel route's front half is meshing plus grouping, reported
  // under `partition`; the BVH build is `hierarchy`; packPages recorded its
  // own inside finalizePagesAndRewrites.
  result.timings.partition =
      std::chrono::duration<double, std::milli>(hierarchyStart - meshingStart).count();
  result.timings.hierarchy =
      std::chrono::duration<double, std::milli>(hierarchyBuilt - hierarchyStart).count();

  if (stats != nullptr)
  {
    stats->levelCount = maximumLevel + 1u;
    stats->nodeCount = nodeCount;
    stats->blockDepth = blockDepth;
    stats->clusterCount = static_cast<uint32_t>(result.clusters.size());
    stats->groupCount = static_cast<uint32_t>(result.groupInfos.size());
    stats->vertexCount = static_cast<uint32_t>(vertices.size());
    stats->quadCount = quadTotal;
    stats->exposedFaceCount = static_cast<uint32_t>(totalExposedFaces);
    stats->exposedFacesPerLevel.assign(maximumLevel+1u,0u);
    for (const auto &node:nodes)
      stats->exposedFacesPerLevel[node.level]+=node.exposedFaces;
    stats->rootClusterCount = rootClusters;
    stats->vanishingGroupCount = vanishingGroups;
    stats->clustersPerLevel.assign(maximumLevel + 1u, 0u);
    stats->trianglesPerLevel.assign(maximumLevel + 1u, 0u);
    stats->clusterSources.assign(
        result.clusters.size(), SparseVoxelOctreeVirtualGeometryClusterSource{});
    for (uint32_t slot = 0u; slot < clusterInfo.size(); ++slot)
    {
      if (clusterInfo[slot].nodeLocal == kInvalid)
        continue;
      const uint32_t local = clusterInfo[slot].nodeLocal;
      const uint32_t level = nodes[local].level;
      const uint32_t buildCluster = clusterInfo[slot].clusterIndex;
      const uint32_t triangles = static_cast<uint32_t>(
          clusters[buildCluster].indices.size() / 3u);
      stats->clustersPerLevel[level] += 1u;
      stats->trianglesPerLevel[level] += triangles;
      stats->triangleCount += triangles;

      const uint32_t block = slot - nodes[local].firstBlockCluster;
      const auto &source = hierarchy.nodes[nodes[local].globalIndex];
      const double cellSize = static_cast<double>(source.minimumVoxelSize[3]);
      SparseVoxelOctreeVirtualGeometryClusterSource entry{};
      entry.brickNodeIndex = nodes[local].globalIndex;
      entry.blockIndex = block;
      entry.level = level;
      entry.triangleCount = triangles;
      entry.blockExtent = static_cast<float>(cellSize * blockCells);
      const std::array<uint32_t, 3> blockCoordinate{
          block % blocksPerSide,
          (block / blocksPerSide) % blocksPerSide,
          block / (blocksPerSide * blocksPerSide)};
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        entry.blockMinimum[axis] = position(source, axis, blockCoordinate[axis] * blockCells);

      entry.surfaceMinimum = clusterInfo[slot].minimum;
      entry.surfaceMaximum = clusterInfo[slot].maximum;
      entry.surfaceValid =
          clusterInfo[slot].minimum[1] <= clusterInfo[slot].maximum[1];

      const uint32_t linearized =
          buildCluster < result.buildToLinearizedClusterIndex.size()
          ? result.buildToLinearizedClusterIndex[buildCluster]
          : kInvalid;
      if (linearized < stats->clusterSources.size())
        stats->clusterSources[linearized] = entry;
    }
    stats->meshingMilliseconds =
        std::chrono::duration<double, std::milli>(meshingEnd - meshingStart).count();
    stats->hierarchyMilliseconds =
        std::chrono::duration<double, std::milli>(hierarchyEnd - hierarchyStart)
            .count();
  }

  return true;
}

} // namespace voxel::svraster
