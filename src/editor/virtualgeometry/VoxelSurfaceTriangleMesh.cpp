#include "editor/virtualgeometry/VoxelSurfaceTriangleMesh.hpp"
#include "editor/virtualgeometry/VoxelSurfaceCellGrid.hpp"

#include "meshoptimizer.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <unordered_map>

namespace virtualgeometry
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error)
    *error = message;
  return false;
}

// Direction d: axis d / 2, positive when d is even.
constexpr int32_t kStep[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
uint32_t directionOf(uint32_t axis, bool negative) { return axis * 2u + (negative ? 1u : 0u); }
uint32_t opposite(uint32_t direction) { return direction ^ 1u; }

using Lattice = std::array<int32_t, 3>;

Lattice step(const Lattice &p, uint32_t direction)
{
  return {p[0] + kStep[direction][0], p[1] + kStep[direction][1], p[2] + kStep[direction][2]};
}

// 21 bits an axis, after shifting the object so every coordinate it or its
// one-cell border can reach is non-negative.
struct LatticeKeys
{
  Lattice offset{};
  uint64_t operator()(const Lattice &p) const
  {
    return uint64_t(uint32_t(p[0] - offset[0])) | (uint64_t(uint32_t(p[1] - offset[1])) << 21) |
           (uint64_t(uint32_t(p[2] - offset[2])) << 42);
  }
};

struct UnionFind
{
  std::vector<uint32_t> parent;
  explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
  uint32_t find(uint32_t x)
  {
    while (parent[x] != x)
      x = parent[x] = parent[parent[x]];
    return x;
  }
  void unite(uint32_t a, uint32_t b)
  {
    a = find(a);
    b = find(b);
    if (a != b)
      parent[std::max(a, b)] = std::min(a, b);
  }
};

struct Face
{
  uint32_t cell = 0u;
  uint32_t direction = 0u;
};

// Corner k of the face of `cell` facing `direction`, counter-clockwise seen
// from outside for a positive direction: (u, v) = (0,0), (1,0), (1,1), (0,1)
// with u = (axis + 1) % 3 and v = (axis + 2) % 3, whose cross product is +axis.
Lattice faceCorner(const Lattice &cell, uint32_t direction, uint32_t k)
{
  const uint32_t axis = direction / 2u, ua = (axis + 1u) % 3u, va = (axis + 2u) % 3u;
  static constexpr int32_t du[4] = {0, 1, 1, 0}, dv[4] = {0, 0, 1, 1};
  Lattice p = cell;
  p[axis] += (direction & 1u) ? 0 : 1;
  p[ua] += du[k];
  p[va] += dv[k];
  return p;
}

// The in-plane direction from the face centre across edge k (corner k to
// corner k + 1).
uint32_t edgeOutward(uint32_t direction, uint32_t k)
{
  const uint32_t axis = direction / 2u, ua = (axis + 1u) % 3u, va = (axis + 2u) % 3u;
  switch (k)
  {
  case 0: return directionOf(va, true);
  case 1: return directionOf(ua, false);
  case 2: return directionOf(va, false);
  default: return directionOf(ua, true);
  }
}

using Float3 = std::array<float, 3>;

Float3 sub(const Float3 &a, const Float3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Float3 cross(const Float3 &a, const Float3 &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float dot(const Float3 &a, const Float3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

} // namespace

bool buildVoxelSurfaceTriangleMesh(
    const std::vector<VoxelSurfaceCell> &input,
    const VoxelSurfaceTriangleMeshSettings &settings,
    VoxelSurfaceTriangleMesh &result,
    std::string *error)
{
  result = {};
  if (input.empty())
    return fail(error, "no voxel cells");
  if (!(settings.pitchMetres > 0.0))
    return fail(error, "voxel pitch must be positive");

  LatticeKeys key;
  Lattice lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
  for (const VoxelSurfaceCell &cell : input)
    for (uint32_t a = 0; a < 3u; ++a)
    {
      lo[a] = std::min(lo[a], cell.position[a]);
      hi[a] = std::max(hi[a], cell.position[a]);
    }
  for (uint32_t a = 0; a < 3u; ++a)
  {
    if (int64_t(hi[a]) - int64_t(lo[a]) + 3 >= (int64_t(1) << 21))
      return fail(error, "voxel object spans more than 2^21 cells on an axis");
    key.offset[a] = lo[a] - 1;
  }

  // Sorted by key and deduplicated, so everything numbered below follows
  // from the cell set alone, not from the order it arrived in.
  std::vector<std::pair<uint64_t, uint32_t>> order;
  order.reserve(input.size());
  for (uint32_t i = 0; i < input.size(); ++i)
    order.emplace_back(key(input[i].position), i);
  std::sort(order.begin(), order.end());
  order.erase(std::unique(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first == b.first; }), order.end());

  std::vector<Lattice> cells(order.size());
  for (uint32_t i = 0; i < order.size(); ++i)
    cells[i] = input[order[i].second].position;
  VoxelSurfaceCellGrid cellIndex;
  cellIndex.build(cells.size(), [&](size_t i) { return cells[i]; });
  const auto solid = [&](const Lattice &p) { return cellIndex.find(p) != VoxelSurfaceCellGrid::None; };
  const auto inDomain = [&](const Lattice &p)
  {
    if (!settings.hasDomain)
      return true;
    for (uint32_t a = 0; a < 3u; ++a)
      if (p[a] < settings.domainMin[a] || p[a] >= settings.domainMax[a])
        return false;
    return true;
  };

  // Exposed faces, and each one's index by (cell, direction).
  std::vector<Face> faces;
  std::vector<uint32_t> faceOf(cells.size() * 6u, UINT32_MAX);
  for (uint32_t c = 0; c < cells.size(); ++c)
    for (uint32_t d = 0; d < 6u; ++d)
      if (inDomain(cells[c]) && !solid(step(cells[c], d)))
      {
        faceOf[size_t(c) * 6u + d] = uint32_t(faces.size());
        faces.push_back({c, d});
      }
  const auto faceAt = [&](const Lattice &cell, uint32_t direction)
  {
    const uint32_t c = cellIndex.find(cell);
    return c == VoxelSurfaceCellGrid::None ? UINT32_MAX : faceOf[size_t(c) * 6u + direction];
  };

  // Weld corners face to face. Across each edge a face pairs with exactly one
  // other: its own cell's face round the edge when the cell beside it is
  // empty (convex, and also the diagonal case - solids are 6-connected), else
  // the face of the cell beyond when that is solid (concave), else the face
  // of the cell beside it (flat). The rule is symmetric, so each pass of the
  // surface round a lattice point becomes one closed fan: one vertex.
  UnionFind corners(faces.size() * 4u);
  // Lattice edges with two diagonal solid cells: (face, edge) of all four.
  std::map<std::pair<uint64_t, uint32_t>, std::vector<std::pair<uint32_t, uint32_t>>> diagonalEdges;
  for (uint32_t f = 0; f < faces.size(); ++f)
  {
    const Lattice &c = cells[faces[f].cell];
    const uint32_t d = faces[f].direction;
    for (uint32_t k = 0; k < 4u; ++k)
    {
      const uint32_t s = edgeOutward(d, k);
      const Lattice beside = step(c, s);
      uint32_t g;
      if (!solid(beside) && solid(step(beside, d)))
      {
        // Along the axis that is neither the normal's nor the outward one's.
        const Lattice p0 = faceCorner(c, d, k), p1 = faceCorner(c, d, (k + 1u) & 3u);
        diagonalEdges[{std::min(key(p0), key(p1)), 3u - d / 2u - s / 2u}].push_back({f, k});
      }
      if (!solid(beside))
        g = faceOf[size_t(faces[f].cell) * 6u + s];
      else if (const Lattice beyond = step(beside, d); solid(beyond))
        g = faceAt(beyond, opposite(s));
      else
        g = faceAt(beside, d);
      if (g == UINT32_MAX && settings.hasDomain)
      {
        // The partner face belongs to a cell outside the domain: an open
        // edge, for the neighbouring chunk to close.
        ++result.borderEdges;
        continue;
      }
      if (g == UINT32_MAX)
        return fail(error, "voxel surface edge without a partner face");
      if (g < f)
        continue; // Welded from the other side already.
      const Lattice &gc = cells[faces[g].cell];
      for (const uint32_t endpoint : {k, (k + 1u) & 3u})
      {
        const Lattice p = faceCorner(c, d, endpoint);
        uint32_t match = 4u;
        for (uint32_t j = 0; j < 4u; ++j)
          if (faceCorner(gc, faces[g].direction, j) == p)
            match = j;
        if (match == 4u)
          return fail(error, "voxel surface partner face does not share the edge");
        corners.unite(f * 4u + endpoint, g * 4u + match);
      }
    }
  }

  // One vertex per fan, numbered in face order.
  std::vector<uint32_t> vertexOfRoot(corners.parent.size(), UINT32_MAX);
  std::vector<Lattice> vertexLattice;
  std::vector<std::array<double, 3>> normalSum, faceCentreSum;
  std::vector<uint32_t> cornerVertex(corners.parent.size());
  for (uint32_t f = 0; f < faces.size(); ++f)
  {
    const Lattice &c = cells[faces[f].cell];
    const uint32_t d = faces[f].direction;
    std::array<double, 3> centre{};
    for (uint32_t a = 0; a < 3u; ++a)
      centre[a] = c[a] + 0.5 + 0.5 * kStep[d][a];
    for (uint32_t k = 0; k < 4u; ++k)
    {
      const uint32_t root = corners.find(f * 4u + k);
      if (vertexOfRoot[root] == UINT32_MAX)
      {
        vertexOfRoot[root] = uint32_t(vertexLattice.size());
        vertexLattice.push_back(faceCorner(c, d, k));
        normalSum.push_back({});
        faceCentreSum.push_back({});
      }
      const uint32_t v = vertexOfRoot[root];
      cornerVertex[f * 4u + k] = v;
      for (uint32_t a = 0; a < 3u; ++a)
      {
        normalSum[v][a] += kStep[d][a];
        faceCentreSum[v][a] += centre[a] - vertexLattice[v][a];
      }
    }
  }

  // Positions: lattice points, and where several fans share one, each fan
  // moved a thousandth of a voxel towards its own faces.
  std::unordered_map<uint64_t, uint32_t> fansAtPoint;
  for (const Lattice &p : vertexLattice)
    ++fansAtPoint[key(p)];
  result.splitVertices = uint32_t(vertexLattice.size() - fansAtPoint.size());
  const double nudge = 1e-3;
  result.vertices.resize(vertexLattice.size());
  result.pinned.assign(vertexLattice.size(), 0u);
  for (uint32_t v = 0; v < vertexLattice.size(); ++v)
  {
    std::array<double, 3> p{double(vertexLattice[v][0]), double(vertexLattice[v][1]), double(vertexLattice[v][2])};
    if (fansAtPoint[key(vertexLattice[v])] > 1u)
    {
      result.pinned[v] = 1u;
      const double length = std::sqrt(faceCentreSum[v][0] * faceCentreSum[v][0] + faceCentreSum[v][1] * faceCentreSum[v][1] +
                                      faceCentreSum[v][2] * faceCentreSum[v][2]);
      if (length > 0.0)
        for (uint32_t a = 0; a < 3u; ++a)
          p[a] += nudge * faceCentreSum[v][a] / length;
    }
    Vertex &vertex = result.vertices[v];
    const double normalLength = std::sqrt(normalSum[v][0] * normalSum[v][0] + normalSum[v][1] * normalSum[v][1] + normalSum[v][2] * normalSum[v][2]);
    for (uint32_t a = 0; a < 3u; ++a)
    {
      vertex.pos[a] = float(settings.origin[a] + settings.pitchMetres * p[a]);
      vertex.norm[a] = normalLength > 0.0 ? float(normalSum[v][a] / normalLength) : 0.f;
    }
  }

  const auto faceCentre = [&](uint32_t f)
  {
    std::array<double, 3> centre{};
    for (uint32_t a = 0; a < 3u; ++a)
      centre[a] = cells[faces[f].cell][a] + 0.5 + 0.5 * kStep[faces[f].direction][a];
    return centre;
  };
  const auto addVertex = [&](const std::array<double, 3> &p, const std::array<double, 3> &normal)
  {
    const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    Vertex vertex;
    for (uint32_t a = 0; a < 3u; ++a)
    {
      vertex.pos[a] = float(settings.origin[a] + settings.pitchMetres * p[a]);
      vertex.norm[a] = length > 0.0 ? float(normal[a] / length) : 0.f;
    }
    result.vertices.push_back(vertex);
    result.pinned.push_back(0u);
    return uint32_t(result.vertices.size() - 1u);
  };

  // A pinched edge: two diagonal cells whose surface is one fan at both ends
  // of the edge they share - joined round both ends - so both pairs of faces
  // would weld to the same two vertices and the edge would carry four
  // triangles. Each pair gets its own midpoint on the edge, moved into its
  // own faces, so each sheet passes the edge on its own.
  std::vector<uint32_t> edgeMidpoint(faces.size() * 4u, UINT32_MAX);
  for (const auto &[edge, entries] : diagonalEdges)
  {
    if (entries.size() != 4u && settings.hasDomain)
      continue; // On the domain boundary, half outside: left as it is.
    if (entries.size() != 4u)
      return fail(error, "diagonal voxel edge without four faces");
    std::vector<std::pair<uint32_t, uint32_t>> sheets[2];
    for (const auto &entry : entries)
      sheets[faces[entry.first].cell == faces[entries[0].first].cell ? 0 : 1].push_back(entry);
    if (sheets[0].size() != 2u || sheets[1].size() != 2u)
      return fail(error, "diagonal voxel edge faces do not pair by cell");
    const auto ends = [&](const std::pair<uint32_t, uint32_t> &e)
    {
      const uint32_t a = cornerVertex[e.first * 4u + e.second], b = cornerVertex[e.first * 4u + ((e.second + 1u) & 3u)];
      return std::make_pair(std::min(a, b), std::max(a, b));
    };
    if (ends(sheets[0][0]) != ends(sheets[1][0]))
      continue;
    for (const auto &sheet : sheets)
    {
      const auto &[f, k] = sheet[0];
      const Lattice p0 = faceCorner(cells[faces[f].cell], faces[f].direction, k);
      const Lattice p1 = faceCorner(cells[faces[f].cell], faces[f].direction, (k + 1u) & 3u);
      std::array<double, 3> mid{}, towards{}, normal{};
      for (uint32_t a = 0; a < 3u; ++a)
        mid[a] = 0.5 * (p0[a] + p1[a]);
      for (const auto &[g, edgeIndex] : sheet)
      {
        const auto centre = faceCentre(g);
        for (uint32_t a = 0; a < 3u; ++a)
        {
          towards[a] += centre[a] - mid[a];
          normal[a] += kStep[faces[g].direction][a];
        }
      }
      const double length = std::sqrt(towards[0] * towards[0] + towards[1] * towards[1] + towards[2] * towards[2]);
      for (uint32_t a = 0; a < 3u; ++a)
        mid[a] += nudge * towards[a] / length;
      const uint32_t m = addVertex(mid, normal);
      result.pinned[m] = 1u;
      for (const auto &[g, edgeIndex] : sheet)
        edgeMidpoint[g * 4u + edgeIndex] = m;
    }
    ++result.splitEdges;
  }

  result.indices.reserve(faces.size() * 6u);
  for (uint32_t f = 0; f < faces.size(); ++f)
  {
    const uint32_t *q = &cornerVertex[f * 4u];
    const bool positive = (faces[f].direction & 1u) == 0u;
    const uint32_t *mids = &edgeMidpoint[f * 4u];
    if (mids[0] == UINT32_MAX && mids[1] == UINT32_MAX && mids[2] == UINT32_MAX && mids[3] == UINT32_MAX)
    {
      if (positive)
        result.indices.insert(result.indices.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
      else
        result.indices.insert(result.indices.end(), {q[0], q[2], q[1], q[0], q[3], q[2]});
      continue;
    }
    // A face with a split edge is a fan round its centre; the centre is
    // coplanar, so the simplifier removes it for nothing.
    std::vector<uint32_t> ring;
    for (uint32_t k = 0; k < 4u; ++k)
    {
      ring.push_back(q[k]);
      if (mids[k] != UINT32_MAX)
        ring.push_back(mids[k]);
    }
    std::array<double, 3> normal{};
    for (uint32_t a = 0; a < 3u; ++a)
      normal[a] = kStep[faces[f].direction][a];
    const uint32_t centre = addVertex(faceCentre(f), normal);
    for (size_t i = 0; i < ring.size(); ++i)
    {
      const uint32_t a = ring[i], b = ring[(i + 1u) % ring.size()];
      if (positive)
        result.indices.insert(result.indices.end(), {centre, a, b});
      else
        result.indices.insert(result.indices.end(), {centre, b, a});
    }
  }
  result.faceCount = faces.size();
  return true;
}

void mergeVoxelSurfaceCoplanar(VoxelSurfaceTriangleMesh &mesh, const VoxelSurfaceTriangleMeshSettings &settings)
{
  if (mesh.indices.empty())
    return;
  // meshoptimizer measures in positions rescaled to the extent it is given,
  // and a coplanar collapse comes back as about 2e-4 of that extent instead
  // of zero: millimetres on a whole house, more than a real collapse costs.
  // So merge in blocks of 16 voxels with their borders locked - noise about
  // 0.3 mm, while any collapse that leaves the lattice surface costs at least
  // about pitch / 32, 3 mm - and run a second pass on blocks offset by half,
  // which frees the first pass's borders.
  // Pinned vertices, and their one-ring: a collapse beside two sheets that
  // pass a lattice point a ten-thousandth of a voxel apart can fold one onto
  // the other at a cost under the threshold.
  std::vector<uint8_t> frozen(mesh.vertices.size(), 0u);
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
  {
    bool touches = false;
    for (int k = 0; k < 3; ++k)
      touches = touches || (mesh.indices[t + k] < mesh.pinned.size() && mesh.pinned[mesh.indices[t + k]]);
    if (touches)
      for (int k = 0; k < 3; ++k)
        frozen[mesh.indices[t + k]] = 1u;
  }
  constexpr int32_t kBlock = 16;
  const double threshold = 1e-2 * settings.pitchMetres;
  for (const int32_t shift : {0, kBlock / 2})
  {
    const size_t triangleCount = mesh.indices.size() / 3u;
    std::vector<uint64_t> blockOf(triangleCount);
    for (size_t t = 0; t < triangleCount; ++t)
    {
      int32_t cell[3];
      for (int a = 0; a < 3; ++a)
      {
        const double centroid = (double(mesh.vertices[mesh.indices[t * 3]].pos[a]) + mesh.vertices[mesh.indices[t * 3 + 1]].pos[a] +
                                 mesh.vertices[mesh.indices[t * 3 + 2]].pos[a]) / 3.0;
        const int32_t lattice = int32_t(std::floor((centroid - settings.origin[a]) / settings.pitchMetres)) + shift;
        cell[a] = (lattice >= 0 ? lattice : lattice - kBlock + 1) / kBlock + (1 << 20);
      }
      blockOf[t] = (uint64_t(uint32_t(cell[0])) << 42) | (uint64_t(uint32_t(cell[1])) << 21) | uint64_t(uint32_t(cell[2]));
    }
    // A vertex used by two blocks is locked in both.
    std::vector<uint64_t> vertexBlock(mesh.vertices.size(), UINT64_MAX);
    std::vector<uint8_t> shared(mesh.vertices.size(), 0u);
    for (size_t t = 0; t < triangleCount; ++t)
      for (int k = 0; k < 3; ++k)
      {
        const uint32_t v = mesh.indices[t * 3 + k];
        if (vertexBlock[v] == UINT64_MAX)
          vertexBlock[v] = blockOf[t];
        else if (vertexBlock[v] != blockOf[t])
          shared[v] = 1u;
      }
    std::vector<uint32_t> order(triangleCount);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t l, uint32_t r) { return blockOf[l] < blockOf[r]; });

    std::vector<uint32_t> result;
    result.reserve(mesh.indices.size());
    std::vector<uint32_t> local(mesh.vertices.size(), UINT32_MAX);
    for (size_t begin = 0; begin < order.size();)
    {
      size_t end = begin;
      while (end < order.size() && blockOf[order[end]] == blockOf[order[begin]])
        ++end;
      std::vector<uint32_t> globals, indices;
      std::vector<float> positions;
      std::vector<uint8_t> locks;
      for (size_t i = begin; i < end; ++i)
        for (int k = 0; k < 3; ++k)
        {
          const uint32_t v = mesh.indices[order[i] * 3 + k];
          if (local[v] == UINT32_MAX)
          {
            local[v] = uint32_t(globals.size());
            globals.push_back(v);
            positions.insert(positions.end(), mesh.vertices[v].pos, mesh.vertices[v].pos + 3);
            locks.push_back(uint8_t(shared[v] | frozen[v]));
          }
          indices.push_back(local[v]);
        }
      std::vector<uint32_t> merged(indices.size());
      float error = 0.f;
      merged.resize(meshopt_simplifyWithAttributes(merged.data(), indices.data(), indices.size(), positions.data(), globals.size(),
                                                   sizeof(float) * 3, nullptr, 0, nullptr, 0, locks.data(), 0, float(threshold),
                                                   meshopt_SimplifyErrorAbsolute | meshopt_SimplifyLockBorder, &error));
      for (uint32_t index : merged)
        result.push_back(globals[index]);
      for (uint32_t v : globals)
        local[v] = UINT32_MAX;
      begin = end;
    }
    mesh.indices = std::move(result);
  }

  std::vector<uint32_t> remap(mesh.vertices.size(), UINT32_MAX);
  std::vector<Vertex> vertices;
  std::vector<uint8_t> pinned;
  for (uint32_t &index : mesh.indices)
  {
    if (remap[index] == UINT32_MAX)
    {
      remap[index] = uint32_t(vertices.size());
      vertices.push_back(mesh.vertices[index]);
      pinned.push_back(index < mesh.pinned.size() ? mesh.pinned[index] : 0u);
    }
    index = remap[index];
  }
  mesh.vertices = std::move(vertices);
  mesh.pinned = std::move(pinned);
}

VirtualGeometryBuildSettings voxelSurfaceBuildSettings()
{
  VirtualGeometryBuildSettings settings;
  settings.clusterizer = VirtualGeometryClusterizer::Meshopt;
  settings.simplifyPositionsOnly = true;
  // A third of MaxVertices: the atlas copies every vertex on a chart border,
  // and the merged LOD0's long creased triangles put most vertices on two or
  // three charts. Measured on a dwelling: at 128, 132 of 269 clusters
  // overflowed and lost their shared view charts; at 64, 3 of 397 did, and
  // the atlas fell from 2048x711 to 1024x657.
  settings.clusterMaxVertices = 64u;
  return settings;
}

bool assignVoxelSurfaceViewUVs(VirtualGeometryBuildData &build, const VoxelSurfaceTriangleMeshSettings &settings,
                               uint32_t *maxClusterVertices, std::string *error, bool storeUVs)
{
  uint32_t largest = 0u;
  for (VirtualGeometryCluster &cluster : build.clusters)
  {
    const size_t triangleCount = cluster.indices.size() / 3u;
    std::vector<uint32_t> viewOf(triangleCount);
    // Per (view, source vertex): area-weighted normal sum, then the copy.
    std::unordered_map<uint64_t, std::array<double, 3>> normals;
    for (size_t t = 0; t < triangleCount; ++t)
    {
      const float *a = cluster.vertices[cluster.indices[t * 3]].pos, *b = cluster.vertices[cluster.indices[t * 3 + 1]].pos,
                  *c = cluster.vertices[cluster.indices[t * 3 + 2]].pos;
      const Float3 n = cross(Float3{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, Float3{c[0] - a[0], c[1] - a[1], c[2] - a[2]});
      uint32_t axis = 0u;
      for (uint32_t k = 1; k < 3u; ++k)
        if (std::fabs(n[k]) > std::fabs(n[axis]))
          axis = k;
      viewOf[t] = axis * 2u + (n[axis] < 0.f ? 1u : 0u);
      for (uint32_t k = 0; k < 3u; ++k)
      {
        auto &sum = normals[(uint64_t(viewOf[t]) << 32) | cluster.indices[t * 3 + k]];
        for (int d = 0; d < 3; ++d)
          sum[d] += 0.5 * n[d]; // |n| is twice the area.
      }
    }
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices(cluster.indices.size());
    std::unordered_map<uint64_t, uint32_t> copy;
    for (size_t t = 0; t < triangleCount; ++t)
      for (uint32_t k = 0; k < 3u; ++k)
      {
        const uint32_t source = cluster.indices[t * 3 + k];
        const uint64_t key = (uint64_t(viewOf[t]) << 32) | source;
        auto [it, inserted] = copy.emplace(key, uint32_t(vertices.size()));
        if (inserted)
        {
          Vertex vertex = cluster.vertices[source];
          const uint32_t axis = viewOf[t] / 2u, ua = (axis + 1u) % 3u, va = (axis + 2u) % 3u;
          vertex.uv[0] = storeUVs ? float((double(vertex.pos[ua]) - settings.origin[ua]) / settings.pitchMetres) : 0.f;
          vertex.uv[1] = storeUVs ? float((double(vertex.pos[va]) - settings.origin[va]) / settings.pitchMetres) : 0.f;
          const auto &n = normals[key];
          const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          for (int d = 0; d < 3; ++d)
            vertex.norm[d] = length > 0.0 ? float(n[d] / length) : (d == int(axis) ? ((viewOf[t] & 1u) ? -1.f : 1.f) : 0.f);
          vertices.push_back(vertex);
        }
        indices[t * 3 + k] = it->second;
      }
    largest = std::max(largest, uint32_t(vertices.size()));
    cluster.vertices = std::move(vertices);
    cluster.indices = std::move(indices);
  }
  if (maxClusterVertices)
    *maxClusterVertices = largest;
  if (largest > MaxVertices)
    return fail(error, "a cluster exceeds MaxVertices after view UV copies");
  return true;
}

std::vector<std::array<float, 3>> voxelSurfaceCutTriangles(const VirtualGeometryBuildData &build, float cutError,
                                                           std::vector<std::array<float, 2>> *uvs)
{
  std::vector<std::array<float, 3>> corners;
  if (uvs)
    uvs->clear();
  for (const VirtualGeometryCluster &cluster : build.clusters)
  {
    if (!(cluster.self.error <= cutError && cutError < cluster.parent.error))
      continue;
    for (uint32_t index : cluster.indices)
    {
      const Vertex &v = cluster.vertices[index];
      corners.push_back({v.pos[0], v.pos[1], v.pos[2]});
      if (uvs)
        uvs->push_back({v.uv[0], v.uv[1]});
    }
  }
  return corners;
}

VoxelSurfaceTopology measureVoxelSurfaceTopology(const std::vector<std::array<float, 3>> &triangleCorners)
{
  VoxelSurfaceTopology topology;
  // Exact-position weld: the same test the simplifier's own position remap
  // makes, so a seam there is a border or a crack here.
  struct PositionHash
  {
    size_t operator()(const Float3 &p) const
    {
      uint32_t bits[3];
      std::memcpy(bits, p.data(), sizeof(bits));
      return size_t(bits[0]) * 73856093u ^ size_t(bits[1]) * 19349663u ^ size_t(bits[2]) * 83492791u;
    }
  };
  std::unordered_map<Float3, uint32_t, PositionHash> ids;
  std::vector<uint32_t> welded(triangleCorners.size());
  for (size_t i = 0; i < triangleCorners.size(); ++i)
    welded[i] = ids.emplace(triangleCorners[i], uint32_t(ids.size())).first->second;
  topology.vertices = ids.size();
  topology.triangles = triangleCorners.size() / 3u;

  std::unordered_map<uint64_t, uint32_t> directed, undirected;
  for (size_t t = 0; t < topology.triangles; ++t)
  {
    const uint32_t *w = &welded[t * 3u];
    const Float3 &a = triangleCorners[t * 3u], &b = triangleCorners[t * 3u + 1u], &c = triangleCorners[t * 3u + 2u];
    const Float3 n = cross(sub(b, a), sub(c, a));
    const double area2 = std::sqrt(double(dot(n, n)));
    topology.area += 0.5 * area2;
    topology.signedVolume += (double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) -
                              double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) +
                              double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0])) / 6.0;
    // A zero-area triangle with three distinct corners (collinear lattice
    // points, which simplification can leave) still joins its edges.
    topology.zeroAreaTriangles += area2 == 0.0;
    if (w[0] == w[1] || w[1] == w[2] || w[0] == w[2])
    {
      ++topology.degenerateTriangles;
      continue;
    }
    for (uint32_t e = 0; e < 3u; ++e)
    {
      const uint32_t from = w[e], to = w[(e + 1u) % 3u];
      if (++directed[(uint64_t(from) << 32) | to] == 2u)
        ++topology.misorientedEdges;
      ++undirected[(uint64_t(std::min(from, to)) << 32) | std::max(from, to)];
    }
  }
  topology.edges = undirected.size();
  for (const auto &[edge, count] : undirected)
  {
    if (count == 1u)
      ++topology.borderEdges;
    else if (count > 2u)
      ++topology.nonManifoldEdges;
  }
  return topology;
}

VoxelSurfaceTopology measureVoxelSurfaceTopology(const VoxelSurfaceTriangleMesh &mesh)
{
  std::vector<std::array<float, 3>> corners;
  corners.reserve(mesh.indices.size());
  for (uint32_t index : mesh.indices)
    corners.push_back({mesh.vertices[index].pos[0], mesh.vertices[index].pos[1], mesh.vertices[index].pos[2]});
  return measureVoxelSurfaceTopology(corners);
}

namespace
{

// Orthographic z-buffered raster shared by the previews; `colour` returns
// the unlit colour of a covered pixel, or false for magenta.
template <typename Colour>
VoxelSurfacePreviewImage rasterPreview(const std::vector<std::array<float, 3>> &triangleCorners, const VoxelSurfacePreviewView &view,
                                       Colour colour)
{
  VoxelSurfacePreviewImage image;
  image.size = view.size;
  image.rgba.assign(size_t(view.size) * view.size, 0xff302820u);
  std::vector<float> depth(image.rgba.size(), FLT_MAX);
  std::vector<uint8_t> missed(image.rgba.size(), 0u);

  // View: yaw about +Y, then pitch about the view X axis; Y is up.
  const float cy = std::cos(view.yaw), sy = std::sin(view.yaw), cp = std::cos(view.pitch), sp = std::sin(view.pitch);
  const auto toView = [&](const Float3 &w) -> Float3
  {
    const float x = cy * w[0] - sy * w[2];
    const float z0 = sy * w[0] + cy * w[2];
    const float y = cp * w[1] - sp * z0;
    const float z = sp * w[1] + cp * z0;
    return {x, y, z};
  };
  Float3 vlo{FLT_MAX, FLT_MAX, FLT_MAX}, vhi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
  for (uint32_t i = 0; i < 8u; ++i)
  {
    const Float3 corner{(i & 1u) ? view.boundsMax[0] : view.boundsMin[0], (i & 2u) ? view.boundsMax[1] : view.boundsMin[1],
                        (i & 4u) ? view.boundsMax[2] : view.boundsMin[2]};
    const Float3 v = toView(corner);
    for (uint32_t a = 0; a < 3u; ++a)
    {
      vlo[a] = std::min(vlo[a], v[a]);
      vhi[a] = std::max(vhi[a], v[a]);
    }
  }
  const float extent = std::max(vhi[0] - vlo[0], vhi[1] - vlo[1]) * 1.04f;
  const float scale = float(view.size) / std::max(extent, 1e-6f);
  const float cx0 = 0.5f * (vlo[0] + vhi[0]), cy0 = 0.5f * (vlo[1] + vhi[1]);
  const Float3 light = [] { Float3 l{0.45f, 0.8f, 0.35f}; const float n = std::sqrt(dot(l, l)); return Float3{l[0] / n, l[1] / n, l[2] / n}; }();

  const uint32_t size = view.size;
  for (size_t t = 0; t + 2u < triangleCorners.size(); t += 3u)
  {
    const Float3 &a = triangleCorners[t], &b = triangleCorners[t + 1u], &c = triangleCorners[t + 2u];
    Float3 n = cross(sub(b, a), sub(c, a));
    const float nl = std::sqrt(dot(n, n));
    if (nl == 0.f)
      continue;
    n = {n[0] / nl, n[1] / nl, n[2] / nl};
    Float3 s[3];
    const Float3 *w[3] = {&a, &b, &c};
    for (uint32_t i = 0; i < 3u; ++i)
    {
      const Float3 v = toView(*w[i]);
      s[i] = {(v[0] - cx0) * scale + 0.5f * size, (cy0 - v[1]) * scale + 0.5f * size, v[2]};
    }
    const float area = (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[2][0] - s[0][0]) * (s[1][1] - s[0][1]);
    if (std::fabs(area) < 1e-12f)
      continue;
    const int x0 = std::max(0, int(std::floor(std::min({s[0][0], s[1][0], s[2][0]}))));
    const int x1 = std::min(int(size) - 1, int(std::ceil(std::max({s[0][0], s[1][0], s[2][0]}))));
    const int y0 = std::max(0, int(std::floor(std::min({s[0][1], s[1][1], s[2][1]}))));
    const int y1 = std::min(int(size) - 1, int(std::ceil(std::max({s[0][1], s[1][1], s[2][1]}))));
    const float shade = 0.35f + 0.65f * std::fabs(dot(n, light));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
      {
        const float px = x + 0.5f, py = y + 0.5f;
        const float w0 = ((s[1][0] - px) * (s[2][1] - py) - (s[2][0] - px) * (s[1][1] - py)) / area;
        const float w1 = ((s[2][0] - px) * (s[0][1] - py) - (s[0][0] - px) * (s[2][1] - py)) / area;
        const float w2 = 1.f - w0 - w1;
        if (w0 < 0.f || w1 < 0.f || w2 < 0.f)
          continue;
        // Smaller view z is nearer the camera (it looks down for pitch < 0).
        const float z = w0 * s[0][2] + w1 * s[1][2] + w2 * s[2][2];
        const size_t pixel = size_t(y) * size + size_t(x);
        if (z >= depth[pixel])
          continue;
        depth[pixel] = z;
        std::array<double, 3> q;
        for (uint32_t i = 0; i < 3u; ++i)
          q[i] = double(w0) * a[i] + double(w1) * b[i] + double(w2) * c[i];
        uint32_t rgba = 0u;
        const bool found = colour(t / 3u, w0, w1, w2, q, n, rgba);
        missed[pixel] = found ? 0u : 1u;
        if (!found)
          rgba = 0xffff00ffu;
        const uint32_t r = uint32_t(float(rgba & 0xffu) * shade), g = uint32_t(float((rgba >> 8) & 0xffu) * shade),
                       bl = uint32_t(float((rgba >> 16) & 0xffu) * shade);
        image.rgba[pixel] = 0xff000000u | (bl << 16) | (g << 8) | r;
      }
  }
  for (size_t pixel = 0; pixel < depth.size(); ++pixel)
    if (depth[pixel] != FLT_MAX)
    {
      ++image.coveredPixels;
      image.missedPixels += missed[pixel];
    }
  return image;
}

} // namespace

VoxelSurfacePreviewImage renderVoxelSurfacePreview(
    const std::vector<std::array<float, 3>> &triangleCorners,
    const std::vector<VoxelSurfaceCell> &cells,
    const VoxelSurfaceTriangleMeshSettings &settings,
    const VoxelSurfacePreviewView &view,
    float reachMetres)
{
  LatticeKeys key;
  key.offset = {INT32_MAX, INT32_MAX, INT32_MAX};
  for (const VoxelSurfaceCell &cell : cells)
    for (uint32_t a = 0; a < 3u; ++a)
      key.offset[a] = std::min(key.offset[a], cell.position[a] - 8);
  std::unordered_map<uint64_t, uint32_t> colours;
  colours.reserve(cells.size() * 2u);
  for (const VoxelSurfaceCell &cell : cells)
    colours.emplace(key(cell.position), cell.rgba);
  const double pitch = settings.pitchMetres;
  const auto colourAt = [&](const std::array<double, 3> &q, uint32_t &rgba)
  {
    Lattice p;
    for (uint32_t a = 0; a < 3u; ++a)
    {
      p[a] = int32_t(std::floor((q[a] - settings.origin[a]) / pitch));
      if (p[a] < key.offset[a])
        return false;
    }
    const auto it = colours.find(key(p));
    if (it == colours.end())
      return false;
    rgba = it->second;
    return true;
  };
  return rasterPreview(triangleCorners, view,
                       [&](size_t, float, float, float, const std::array<double, 3> &q, const Float3 &n, uint32_t &rgba)
                       {
                         for (double tt = 0.25 * pitch; tt >= -double(reachMetres); tt -= 0.5 * pitch)
                           if (colourAt({q[0] + n[0] * tt, q[1] + n[1] * tt, q[2] + n[2] * tt}, rgba))
                             return true;
                         return false;
                       });
}

VoxelSurfacePreviewImage renderVoxelSurfaceTexturedPreview(
    const std::vector<std::array<float, 3>> &triangleCorners,
    const std::vector<std::array<float, 2>> &cornerUVs,
    const std::vector<uint32_t> &textureRGBA,
    uint32_t textureWidth,
    uint32_t textureHeight,
    const VoxelSurfacePreviewView &view)
{
  return rasterPreview(triangleCorners, view,
                       [&](size_t t, float w0, float w1, float w2, const std::array<double, 3> &, const Float3 &, uint32_t &rgba)
                       {
                         const auto &a = cornerUVs[t * 3], &b = cornerUVs[t * 3 + 1], &c = cornerUVs[t * 3 + 2];
                         const float u = w0 * a[0] + w1 * b[0] + w2 * c[0], v = w0 * a[1] + w1 * b[1] + w2 * c[1];
                         const uint32_t x = std::min(textureWidth - 1u, uint32_t(std::max(0.f, u * float(textureWidth))));
                         const uint32_t y = std::min(textureHeight - 1u, uint32_t(std::max(0.f, v * float(textureHeight))));
                         rgba = textureRGBA[size_t(y) * textureWidth + x];
                         return true;
                       });
}

bool writeVoxelSurfacePreviewSheet(const std::vector<VoxelSurfacePreviewImage> &panels, const std::string &path, std::string *error)
{
  if (panels.empty())
    return fail(error, "no preview panels");
  const uint32_t size = panels.front().size;
  const uint32_t width = size * uint32_t(panels.size());
  std::vector<uint32_t> sheet(size_t(width) * size, 0xff000000u);
  for (size_t i = 0; i < panels.size(); ++i)
  {
    if (panels[i].size != size)
      return fail(error, "preview panels differ in size");
    for (uint32_t y = 0; y < size; ++y)
      std::copy_n(&panels[i].rgba[size_t(y) * size], size, &sheet[size_t(y) * width + i * size]);
  }
  if (!stbi_write_png(path.c_str(), int(width), int(size), 4, sheet.data(), int(width * 4u)))
    return fail(error, "cannot write " + path);
  return true;
}

bool writeVoxelSurfaceTriangleMeshGLTF(
    const VoxelSurfaceTriangleMesh &mesh,
    const std::string &basePath,
    std::string *error)
{
  if (mesh.vertices.empty() || mesh.indices.empty())
    return fail(error, "empty voxel surface mesh");
  const size_t slash = basePath.find_last_of('/');
  const std::string name = slash == std::string::npos ? basePath : basePath.substr(slash + 1u);

  std::vector<float> positions, normals;
  positions.reserve(mesh.vertices.size() * 3u);
  normals.reserve(mesh.vertices.size() * 3u);
  float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
  for (const auto &vertex : mesh.vertices)
    for (unsigned a = 0; a < 3; ++a)
    {
      positions.push_back(vertex.pos[a]);
      normals.push_back(vertex.norm[a]);
      lo[a] = std::min(lo[a], vertex.pos[a]);
      hi[a] = std::max(hi[a], vertex.pos[a]);
    }
  const size_t positionBytes = positions.size() * 4u, normalBytes = normals.size() * 4u, indexBytes = mesh.indices.size() * 4u;
  {
    std::ofstream bin(basePath + ".bin", std::ios::binary);
    if (!bin)
      return fail(error, "cannot write " + basePath + ".bin");
    bin.write(reinterpret_cast<const char *>(positions.data()), std::streamsize(positionBytes));
    bin.write(reinterpret_cast<const char *>(normals.data()), std::streamsize(normalBytes));
    bin.write(reinterpret_cast<const char *>(mesh.indices.data()), std::streamsize(indexBytes));
  }
  std::FILE *file = std::fopen((basePath + ".gltf").c_str(), "wb");
  if (file == nullptr)
    return fail(error, "cannot write " + basePath + ".gltf");
  std::fprintf(file,
               "{\"asset\":{\"version\":\"2.0\",\"generator\":\"VoxelSurfaceTriangleMesh\"},\"scene\":0,"
               "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
               "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"material\":0}]}],"
               "\"materials\":[{\"name\":\"%s\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.8,0.8,1.0],"
               "\"metallicFactor\":0.0,\"roughnessFactor\":0.9}}],"
               "\"buffers\":[{\"uri\":\"%s.bin\",\"byteLength\":%zu}],"
               "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%zu,\"target\":34962},"
               "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962},"
               "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}],"
               "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\","
               "\"min\":[%.9g,%.9g,%.9g],\"max\":[%.9g,%.9g,%.9g]},"
               "{\"bufferView\":1,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\"},"
               "{\"bufferView\":2,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}]}\n",
               name.c_str(), name.c_str(), positionBytes + normalBytes + indexBytes,
               positionBytes, positionBytes, normalBytes, positionBytes + normalBytes, indexBytes,
               mesh.vertices.size(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], mesh.vertices.size(), mesh.indices.size());
  std::fclose(file);
  return true;
}

} // namespace virtualgeometry
