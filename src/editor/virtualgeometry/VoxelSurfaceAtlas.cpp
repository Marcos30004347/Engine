#include "editor/virtualgeometry/VoxelSurfaceAtlas.hpp"
#include "editor/virtualgeometry/VoxelSurfaceCellGrid.hpp"

#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"

#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
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

using Float3 = std::array<float, 3>;

// Cell colours by lattice position.
class ColourField
{
public:
  ColourField(const std::vector<VoxelSurfaceCell> &cells, const VoxelSurfaceTriangleMeshSettings &settings) : cells_(cells), settings_(settings)
  {
    grid_.build(cells.size(), [&](size_t i) { return cells[i].position; });
  }

  bool at(const std::array<double, 3> &p, uint32_t &rgba) const
  {
    std::array<int32_t, 3> cell;
    for (int a = 0; a < 3; ++a)
      cell[a] = int32_t(std::floor((p[a] - settings_.origin[a]) / settings_.pitchMetres));
    const uint32_t i = grid_.find(cell);
    if (i == VoxelSurfaceCellGrid::None)
      return false;
    rgba = cells_[i].rgba;
    return true;
  }

  // The surface voxel along the line through p: marching inward from just
  // outside p, the first solid cell; when p is already inside the solid (a
  // coarse surface sunk below the voxels), the last solid cell before empty
  // walking outward. Within `reach` either way.
  bool surface(const std::array<double, 3> &p, const Float3 &n, double reach, uint32_t &rgba) const
  {
    const double step = 0.5 * settings_.pitchMetres;
    const auto along = [&](double t) { return std::array<double, 3>{p[0] + n[0] * t, p[1] + n[1] * t, p[2] + n[2] * t}; };
    const double start = 0.25 * settings_.pitchMetres;
    uint32_t found = 0u;
    if (!at(along(start), found))
    {
      for (double t = start - step; t >= -reach; t -= step)
        if (at(along(t), rgba))
          return true;
      return false;
    }
    for (double t = start + step; t <= reach; t += step)
    {
      uint32_t next = 0u;
      if (!at(along(t), next))
        break;
      found = next;
    }
    rgba = found;
    return true;
  }

private:
  const std::vector<VoxelSurfaceCell> &cells_;
  VoxelSurfaceTriangleMeshSettings settings_;
  VoxelSurfaceCellGrid grid_;
};

Float3 triangleNormal(const Vertex &a, const Vertex &b, const Vertex &c, float *area = nullptr)
{
  const Float3 e1{b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
  const Float3 e2{c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
  Float3 n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
  const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (area)
    *area = 0.5f * length;
  if (length > 0.f)
    for (float &x : n)
      x /= length;
  return n;
}

uint32_t viewOf(const Float3 &n)
{
  uint32_t axis = 0u;
  for (uint32_t a = 1; a < 3u; ++a)
    if (std::fabs(n[a]) > std::fabs(n[axis]))
      axis = a;
  return axis * 2u + (n[axis] < 0.f ? 1u : 0u);
}

// One (view, layer) piece of a cluster before packing.
struct Piece
{
  uint32_t view = 0u;
  uint32_t layer = 0u;
  std::vector<uint32_t> triangles;
  // Texel-space bounds of the projection, in whole texels.
  int64_t u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  uint32_t atlasX = 0u, atlasY = 0u;
};

struct ClusterUnwrap
{
  double texel = 0.0;
  std::vector<Piece> pieces;
};

// Projection of a vertex onto a view's plane, in texels from the lattice
// origin, so LOD0 texel edges are voxel edges.
std::array<double, 2> project(const Vertex &v, uint32_t view, double texel, const VoxelSurfaceTriangleMeshSettings &settings)
{
  const uint32_t axis = view / 2u, ua = (axis + 1u) % 3u, va = (axis + 2u) % 3u;
  return {(double(v.pos[ua]) - settings.origin[ua]) / texel, (double(v.pos[va]) - settings.origin[va]) / texel};
}

// Texel centres (i + 0.5, j + 0.5) inside the 2D triangle: strictly for the
// overlap test (neighbours share edges), edges included for the bake (the
// diagonal of a one-voxel quad runs through its only texel centre).
template <typename Visit>
void forTexelCentres(const std::array<double, 2> &a, const std::array<double, 2> &b, const std::array<double, 2> &c, Visit visit,
                     bool inclusive = false)
{
  const double area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
  if (std::fabs(area) < 1e-12)
    return;
  const int64_t x0 = int64_t(std::floor(std::min({a[0], b[0], c[0]}) - 0.5)), x1 = int64_t(std::ceil(std::max({a[0], b[0], c[0]}) - 0.5));
  const int64_t y0 = int64_t(std::floor(std::min({a[1], b[1], c[1]}) - 0.5)), y1 = int64_t(std::ceil(std::max({a[1], b[1], c[1]}) - 0.5));
  const double eps = inclusive ? -1e-7 : 1e-7;
  for (int64_t y = y0; y <= y1; ++y)
    for (int64_t x = x0; x <= x1; ++x)
    {
      const double px = double(x) + 0.5, py = double(y) + 0.5;
      const double w0 = ((b[0] - px) * (c[1] - py) - (c[0] - px) * (b[1] - py)) / area;
      const double w1 = ((c[0] - px) * (a[1] - py) - (a[0] - px) * (c[1] - py)) / area;
      const double w2 = 1.0 - w0 - w1;
      if (w0 > eps && w1 > eps && w2 > eps)
        visit(x, y, w0, w1, w2);
    }
}

uint64_t texelKey(int64_t x, int64_t y) { return (uint64_t(uint32_t(int32_t(x))) << 32) | uint32_t(int32_t(y)); }

// Charts of one cluster: by view, by connectivity, then peeled into layers
// wherever a view's projection covers a texel centre twice.
std::vector<Piece> unwrapCluster(const VirtualGeometryCluster &cluster, double texel, const VoxelSurfaceTriangleMeshSettings &settings,
                                 bool peel, const std::vector<uint8_t> *include = nullptr)
{
  const uint32_t triangleCount = uint32_t(cluster.indices.size() / 3u);
  std::vector<uint32_t> view(triangleCount);
  std::vector<float> area(triangleCount);
  for (uint32_t t = 0; t < triangleCount; ++t)
    view[t] = viewOf(triangleNormal(cluster.vertices[cluster.indices[t * 3]], cluster.vertices[cluster.indices[t * 3 + 1]],
                                    cluster.vertices[cluster.indices[t * 3 + 2]], &area[t]));

  // Triangles of one view sharing an edge.
  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;
  for (uint32_t t = 0; t < triangleCount; ++t)
    for (uint32_t e = 0; e < 3u; ++e)
    {
      const uint32_t a = cluster.indices[t * 3 + e], b = cluster.indices[t * 3 + (e + 1) % 3];
      edges[{std::min(a, b), std::max(a, b)}].push_back(t);
    }
  std::vector<std::vector<uint32_t>> neighbours(triangleCount);
  for (const auto &[edge, triangles] : edges)
    for (uint32_t i : triangles)
      for (uint32_t j : triangles)
        if (i != j && view[i] == view[j])
          neighbours[i].push_back(j);

  std::vector<Piece> pieces;
  // Unplaced triangles of one view are split into connected pieces; each is
  // laid down breadth-first from its largest triangle, and whatever would
  // cover a texel centre already covered goes round again as the next layer.
  std::vector<uint32_t> layerOf(triangleCount, UINT32_MAX);
  std::vector<uint8_t> pending(triangleCount, 1u);
  if (include)
    pending = *include;
  for (uint32_t layer = 0; std::find(pending.begin(), pending.end(), 1u) != pending.end(); ++layer)
  {
    std::vector<uint8_t> seen(triangleCount, 0u);
    std::vector<uint8_t> next(triangleCount, 0u);
    std::vector<uint32_t> order(triangleCount);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return area[a] > area[b]; });
    for (uint32_t seed : order)
    {
      if (!pending[seed] || seen[seed])
        continue;
      Piece piece;
      piece.view = view[seed];
      piece.layer = layer;
      std::unordered_map<uint64_t, uint32_t> taken;
      std::deque<uint32_t> queue{seed};
      seen[seed] = 1u;
      while (!queue.empty())
      {
        const uint32_t t = queue.front();
        queue.pop_front();
        const auto a = project(cluster.vertices[cluster.indices[t * 3]], piece.view, texel, settings);
        const auto b = project(cluster.vertices[cluster.indices[t * 3 + 1]], piece.view, texel, settings);
        const auto c = project(cluster.vertices[cluster.indices[t * 3 + 2]], piece.view, texel, settings);
        bool overlaps = false;
        std::vector<uint64_t> covered;
        forTexelCentres(a, b, c, [&](int64_t x, int64_t y, double, double, double)
                        {
                          const uint64_t k = texelKey(x, y);
                          covered.push_back(k);
                          overlaps = overlaps || taken.count(k) != 0u;
                        });
        if (overlaps && peel)
        {
          next[t] = 1u;
          continue;
        }
        for (uint64_t k : covered)
          taken.emplace(k, t);
        piece.triangles.push_back(t);
        layerOf[t] = layer;
        for (uint32_t n : neighbours[t])
          if (pending[n] && !seen[n])
          {
            seen[n] = 1u;
            queue.push_back(n);
          }
      }
      for (uint32_t t : piece.triangles)
        pending[t] = 0u;
      if (!piece.triangles.empty())
        pieces.push_back(std::move(piece));
    }
    for (uint32_t t = 0; t < triangleCount; ++t)
      if (next[t])
        pending[t] = 1u;
  }
  return pieces;
}

// One region per view and layer, as far as it pays: pieces of the same view
// and layer whose texels do not collide share a chart when the shared
// rectangle, gutter included, is no larger than the two apart. Small pieces
// are mostly gutter, so this is where the atlas area goes.
void mergeViewRegions(const VirtualGeometryCluster &cluster, double texel, const VoxelSurfaceTriangleMeshSettings &settings, uint32_t gutter,
                      std::vector<Piece> &pieces)
{
  struct Region
  {
    std::unordered_map<uint64_t, uint8_t> texels;
    double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
  };
  const auto rectArea = [&](const double lo[2], const double hi[2])
  {
    const double w = std::max(1.0, std::ceil(hi[0] - 1e-4) - std::floor(lo[0] + 1e-4)) + 2.0 * gutter;
    const double h = std::max(1.0, std::ceil(hi[1] - 1e-4) - std::floor(lo[1] + 1e-4)) + 2.0 * gutter;
    return w * h;
  };
  std::vector<Region> regions(pieces.size());
  for (size_t i = 0; i < pieces.size(); ++i)
    for (uint32_t t : pieces[i].triangles)
    {
      std::array<double, 2> q[3];
      for (int k = 0; k < 3; ++k)
      {
        q[k] = project(cluster.vertices[cluster.indices[t * 3 + k]], pieces[i].view, texel, settings);
        for (int a = 0; a < 2; ++a)
        {
          regions[i].lo[a] = std::min(regions[i].lo[a], q[k][a]);
          regions[i].hi[a] = std::max(regions[i].hi[a], q[k][a]);
        }
      }
      forTexelCentres(q[0], q[1], q[2], [&](int64_t x, int64_t y, double, double, double) { regions[i].texels.emplace(texelKey(x, y), 1u); });
    }
  for (size_t i = 0; i < pieces.size(); ++i)
  {
    if (pieces[i].triangles.empty())
      continue;
    for (size_t j = i + 1; j < pieces.size(); ++j)
    {
      if (pieces[j].triangles.empty() || pieces[j].view != pieces[i].view || pieces[j].layer != pieces[i].layer)
        continue;
      double lo[2], hi[2];
      for (int a = 0; a < 2; ++a)
      {
        lo[a] = std::min(regions[i].lo[a], regions[j].lo[a]);
        hi[a] = std::max(regions[i].hi[a], regions[j].hi[a]);
      }
      if (rectArea(lo, hi) > rectArea(regions[i].lo, regions[i].hi) + rectArea(regions[j].lo, regions[j].hi))
        continue;
      bool collide = false;
      for (const auto &[k, unused] : regions[j].texels)
        if (regions[i].texels.count(k))
        {
          collide = true;
          break;
        }
      if (collide)
        continue;
      regions[i].texels.insert(regions[j].texels.begin(), regions[j].texels.end());
      for (int a = 0; a < 2; ++a)
      {
        regions[i].lo[a] = lo[a];
        regions[i].hi[a] = hi[a];
      }
      pieces[i].triangles.insert(pieces[i].triangles.end(), pieces[j].triangles.begin(), pieces[j].triangles.end());
      pieces[j].triangles.clear();
    }
  }
  pieces.erase(std::remove_if(pieces.begin(), pieces.end(), [](const Piece &p) { return p.triangles.empty(); }), pieces.end());
}

size_t splitVertexCount(const VirtualGeometryCluster &cluster, const std::vector<Piece> &pieces)
{
  size_t total = 0u;
  for (const Piece &piece : pieces)
  {
    std::vector<uint32_t> used;
    for (uint32_t t : piece.triangles)
      for (uint32_t k = 0; k < 3u; ++k)
        used.push_back(cluster.indices[t * 3 + k]);
    std::sort(used.begin(), used.end());
    total += size_t(std::unique(used.begin(), used.end()) - used.begin());
  }
  return total;
}

// Over the vertex budget: fold the smallest piece into the piece it shares
// most vertices with, projected the way that one is. A fold may overlap; the
// texels it shares then show one of the two surfaces.
void foldPieces(const VirtualGeometryCluster &cluster, std::vector<Piece> &pieces)
{
  while (pieces.size() > 1u && splitVertexCount(cluster, pieces) > MaxVertices)
  {
    size_t smallest = 0u;
    for (size_t i = 1; i < pieces.size(); ++i)
      if (pieces[i].triangles.size() < pieces[smallest].triangles.size())
        smallest = i;
    std::vector<uint32_t> mine;
    for (uint32_t t : pieces[smallest].triangles)
      for (uint32_t k = 0; k < 3u; ++k)
        mine.push_back(cluster.indices[t * 3 + k]);
    std::sort(mine.begin(), mine.end());
    size_t best = smallest == 0u ? 1u : 0u, bestShared = 0u;
    for (size_t i = 0; i < pieces.size(); ++i)
    {
      if (i == smallest)
        continue;
      size_t shared = 0u;
      for (uint32_t t : pieces[i].triangles)
        for (uint32_t k = 0; k < 3u; ++k)
          shared += std::binary_search(mine.begin(), mine.end(), cluster.indices[t * 3 + k]);
      if (shared > bestShared)
      {
        bestShared = shared;
        best = i;
      }
    }
    pieces[best].triangles.insert(pieces[best].triangles.end(), pieces[smallest].triangles.begin(), pieces[smallest].triangles.end());
    pieces.erase(pieces.begin() + std::ptrdiff_t(smallest));
  }
}

// The object seen from each of the six views, layer by layer: for every
// column of the view's plane, the exposed faces facing that view, nearest the
// viewer first. A (view, layer) image is cut into connected regions - a
// facade, a roof slope, a window recess seen through - and each region is
// one chart that every level's triangles lying on it share.
class ObjectViews
{
public:
  struct Entry
  {
    int32_t plane = 0;
    uint32_t rgba = 0u;
    uint32_t region = UINT32_MAX;
  };
  struct Region
  {
    uint32_t view = 0u, layer = 0u;
    int64_t u0 = 0, w0 = 0, u1 = 0, w1 = 0; // Inclusive texel bounds.
    bool used = false;
    uint32_t atlasX = 0u, atlasY = 0u;
  };

  ObjectViews(const std::vector<VoxelSurfaceCell> &cells, const VoxelSurfaceTriangleMeshSettings &settings)
  {
    VoxelSurfaceCellGrid solid;
    solid.build(cells.size(), [&](size_t i) { return cells[i].position; });
    static constexpr int32_t step[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const auto inDomain = [&](const std::array<int32_t, 3> &p)
    {
      for (uint32_t a = 0; a < 3u; ++a)
        if (settings.hasDomain && (p[a] < settings.domainMin[a] || p[a] >= settings.domainMax[a]))
          return false;
      return true;
    };
    for (const VoxelSurfaceCell &cell : cells)
      for (uint32_t view = 0; view < 6u && inDomain(cell.position); ++view)
      {
        const std::array<int32_t, 3> next{cell.position[0] + step[view][0], cell.position[1] + step[view][1], cell.position[2] + step[view][2]};
        if (solid.find(next) != VoxelSurfaceCellGrid::None)
          continue;
        const uint32_t axis = view / 2u, ua = (axis + 1u) % 3u, va = (axis + 2u) % 3u;
        const int32_t plane = cell.position[axis] + ((view & 1u) ? 0 : 1);
        columns_[columnKey(view, cell.position[ua], cell.position[va])].push_back({plane, cell.rgba, UINT32_MAX});
      }
    for (auto &[key, column] : columns_)
    {
      const bool positive = ((key >> 42) & 7u) % 2u == 0u;
      std::sort(column.begin(), column.end(), [&](const Entry &l, const Entry &r) { return positive ? l.plane > r.plane : l.plane < r.plane; });
    }
    // Patches: same-view faces on one plane, 4-connected. Then regions:
    // patches merged greedily, largest first, into a neighbouring region of
    // the same view whose columns they do not overlap - a facade with its
    // recesses and trims, as one picture. What would overlap (a wall behind
    // an eave, a hole's far side) starts a region of its own.
    std::vector<uint64_t> keys;
    keys.reserve(columns_.size());
    for (const auto &[key, column] : columns_)
      keys.push_back(key);
    std::sort(keys.begin(), keys.end()); // Numbering independent of hashing.
    struct Patch
    {
      uint32_t view = 0u;
      std::vector<std::pair<uint64_t, uint32_t>> faces; // (column, index in column)
    };
    std::vector<Patch> patches;
    std::unordered_map<uint64_t, std::vector<uint32_t>> patchOf; // column -> patch per entry
    for (const uint64_t key : keys)
      patchOf[key].assign(columns_[key].size(), UINT32_MAX);
    const int64_t du[4] = {1, -1, 0, 0}, dw[4] = {0, 0, 1, -1};
    for (const uint64_t start : keys)
      for (uint32_t i = 0; i < columns_[start].size(); ++i)
      {
        if (patchOf[start][i] != UINT32_MAX)
          continue;
        const uint32_t id = uint32_t(patches.size());
        Patch patch;
        patch.view = uint32_t(start >> 42);
        const int32_t plane = columns_[start][i].plane;
        std::deque<std::pair<uint64_t, uint32_t>> queue{{start, i}};
        patchOf[start][i] = id;
        while (!queue.empty())
        {
          const auto [key, index] = queue.front();
          queue.pop_front();
          patch.faces.emplace_back(key, index);
          const int64_t u = unpack(key, 21), w = unpack(key, 0);
          for (int k = 0; k < 4; ++k)
          {
            const uint64_t next = columnKey(patch.view, u + du[k], w + dw[k]);
            const auto it = columns_.find(next);
            if (it == columns_.end())
              continue;
            for (uint32_t j = 0; j < it->second.size(); ++j)
              if (it->second[j].plane == plane && patchOf[next][j] == UINT32_MAX)
              {
                patchOf[next][j] = id;
                queue.emplace_back(next, j);
              }
          }
        }
        patches.push_back(std::move(patch));
      }

    std::vector<uint32_t> order(patches.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t l, uint32_t r) { return patches[l].faces.size() > patches[r].faces.size(); });
    std::vector<std::unordered_map<uint64_t, uint8_t>> occupied; // Per region: its columns.
    for (const uint32_t p : order)
    {
      const Patch &patch = patches[p];
      // Candidate regions: those owning a column next to one of the patch's.
      std::vector<uint32_t> candidates;
      for (const auto &[key, index] : patch.faces)
      {
        const int64_t u = unpack(key, 21), w = unpack(key, 0);
        for (int k = 0; k < 4; ++k)
        {
          const auto it = columns_.find(columnKey(patch.view, u + du[k], w + dw[k]));
          if (it == columns_.end())
            continue;
          for (const Entry &entry : it->second)
            if (entry.region != UINT32_MAX)
              candidates.push_back(entry.region);
        }
      }
      std::sort(candidates.begin(), candidates.end());
      candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
      uint32_t target = UINT32_MAX;
      for (const uint32_t r : candidates)
      {
        bool overlaps = false;
        for (const auto &[key, index] : patch.faces)
          if (occupied[r].count(key))
          {
            overlaps = true;
            break;
          }
        if (!overlaps)
        {
          target = r;
          break;
        }
      }
      if (target == UINT32_MAX)
      {
        target = uint32_t(regions.size());
        Region region;
        region.view = patch.view;
        region.layer = 0u;
        region.u0 = region.w0 = INT64_MAX;
        region.u1 = region.w1 = INT64_MIN;
        regions.push_back(region);
        occupied.emplace_back();
      }
      Region &region = regions[target];
      for (const auto &[key, index] : patch.faces)
      {
        columns_[key][index].region = target;
        occupied[target].emplace(key, 1u);
        const int64_t u = unpack(key, 21), w = unpack(key, 0);
        region.u0 = std::min(region.u0, u);
        region.u1 = std::max(region.u1, u);
        region.w0 = std::min(region.w0, w);
        region.w1 = std::max(region.w1, w);
      }
    }
    // Layer: how many regions of the same view were opened before it - the
    // hole-and-sub-hole depth, for the stats only.
    std::array<uint32_t, 6> opened{};
    for (Region &region : regions)
      region.layer = opened[region.view]++;
  }

  const std::vector<Entry> *column(uint32_t view, int64_t u, int64_t w) const
  {
    const auto it = columns_.find(columnKey(view, u, w));
    return it == columns_.end() ? nullptr : &it->second;
  }

  std::vector<Region> regions;

private:
  static uint64_t columnKey(uint32_t view, int64_t u, int64_t w)
  {
    return (uint64_t(view) << 42) | (uint64_t(uint32_t(u + (1 << 20)) & 0x1fffffu) << 21) | uint64_t(uint32_t(w + (1 << 20)) & 0x1fffffu);
  }
  static int64_t unpack(uint64_t key, int shift) { return int64_t((key >> shift) & 0x1fffffu) - (1 << 20); }

  std::unordered_map<uint64_t, std::vector<Entry>> columns_;
};

// The region a triangle lies on: at every texel centre it covers (edges
// included), the column's face nearest the triangle in depth is within
// `reach` voxels and in one and the same region.
uint32_t regionOf(const ObjectViews &views, const Vertex *v[3], uint32_t view, double reach, const VoxelSurfaceTriangleMeshSettings &settings)
{
  const uint32_t axis = view / 2u;
  std::array<double, 2> q[3];
  double depth[3];
  for (int k = 0; k < 3; ++k)
  {
    q[k] = project(*v[k], view, settings.pitchMetres, settings);
    depth[k] = (double(v[k]->pos[axis]) - settings.origin[axis]) / settings.pitchMetres;
  }
  uint32_t region = UINT32_MAX;
  bool ok = true, any = false;
  const auto test = [&](int64_t x, int64_t y, double w0, double w1, double w2)
  {
    if (!ok)
      return;
    any = true;
    const auto *column = views.column(view, x, y);
    if (!column)
    {
      ok = false;
      return;
    }
    const double d = w0 * depth[0] + w1 * depth[1] + w2 * depth[2];
    const ObjectViews::Entry *best = nullptr;
    for (const auto &entry : *column)
      if (!best || std::fabs(entry.plane - d) < std::fabs(best->plane - d))
        best = &entry;
    if (std::fabs(best->plane - d) > reach || (region != UINT32_MAX && best->region != region))
    {
      ok = false;
      return;
    }
    region = best->region;
  };
  forTexelCentres(q[0], q[1], q[2], test, true);
  if (!any)
  {
    // Smaller than a texel centre: the texel under its centroid.
    const double cu = (q[0][0] + q[1][0] + q[2][0]) / 3.0, cw = (q[0][1] + q[1][1] + q[2][1]) / 3.0;
    test(int64_t(std::floor(cu)), int64_t(std::floor(cw)), 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
  }
  return ok ? region : UINT32_MAX;
}

// A chart key per triangle: a shared region, or a piece of the cluster's own.
struct ClusterMapping
{
  double texel = 0.0;
  std::vector<uint32_t> region; // Per triangle; UINT32_MAX for the cluster's own pieces.
  std::vector<Piece> pieces;
};

size_t mappedVertexCount(const VirtualGeometryCluster &cluster, const ClusterMapping &mapping)
{
  std::vector<uint64_t> keys;
  for (uint32_t t = 0; t < mapping.region.size(); ++t)
    if (mapping.region[t] != UINT32_MAX)
      for (uint32_t k = 0; k < 3u; ++k)
        keys.push_back((uint64_t(mapping.region[t]) << 32) | cluster.indices[t * 3 + k]);
  for (size_t p = 0; p < mapping.pieces.size(); ++p)
    for (uint32_t t : mapping.pieces[p].triangles)
      for (uint32_t k = 0; k < 3u; ++k)
        keys.push_back((uint64_t(0xffffffffu - p) << 32) | cluster.indices[t * 3 + k]);
  std::sort(keys.begin(), keys.end());
  return size_t(std::unique(keys.begin(), keys.end()) - keys.begin());
}

} // namespace

bool bakeVoxelSurfaceAtlas(
    VirtualGeometryBuildData &build,
    const std::vector<VoxelSurfaceCell> &cells,
    const VoxelSurfaceTriangleMeshSettings &meshSettings,
    const VoxelSurfaceAtlasSettings &settings,
    VoxelSurfaceAtlas &atlas,
    std::string *error)
{
  atlas = {};
  if (cells.empty() || build.clusters.empty())
    return fail(error, "nothing to bake");
  const double pitch = meshSettings.pitchMetres;
  const uint32_t gutter = settings.gutterTexels;
  ObjectViews views(cells, meshSettings);

  // 1. Map every triangle onto a shared view region where it lies on one;
  // unwrap the rest per cluster.
  std::vector<ClusterMapping> mappings(build.clusters.size());
  for (size_t c = 0; c < build.clusters.size(); ++c)
  {
    const VirtualGeometryCluster &cluster = build.clusters[c];
    ClusterMapping &mapping = mappings[c];
    mapping.texel = std::max(pitch, double(cluster.self.error));
    const uint32_t triangleCount = uint32_t(cluster.indices.size() / 3u);
    mapping.region.assign(triangleCount, UINT32_MAX);
    const double reach = (pitch + 2.0 * double(cluster.self.error)) / pitch;
    std::vector<uint8_t> own(triangleCount, 0u);
    for (uint32_t t = 0; t < triangleCount; ++t)
    {
      const Vertex *v[3] = {&cluster.vertices[cluster.indices[t * 3]], &cluster.vertices[cluster.indices[t * 3 + 1]],
                            &cluster.vertices[cluster.indices[t * 3 + 2]]};
      mapping.region[t] = regionOf(views, v, viewOf(triangleNormal(*v[0], *v[1], *v[2])), reach, meshSettings);
      own[t] = mapping.region[t] == UINT32_MAX;
    }
    const auto unwrapOwn = [&](bool peel)
    {
      mapping.pieces = unwrapCluster(cluster, mapping.texel, meshSettings, peel, &own);
      mergeViewRegions(cluster, mapping.texel, meshSettings, gutter, mapping.pieces);
    };
    unwrapOwn(true);
    if (mappedVertexCount(cluster, mapping) > MaxVertices)
    {
      // Too many vertex copies: the cluster's own charts only, by view, folded.
      mapping.region.assign(triangleCount, UINT32_MAX);
      own.assign(triangleCount, 1u);
      unwrapOwn(false);
      foldPieces(cluster, mapping.pieces);
      ++atlas.mergedClusters;
    }
    for (uint32_t t = 0; t < triangleCount; ++t)
    {
      if (mapping.region[t] != UINT32_MAX)
      {
        views.regions[mapping.region[t]].used = true;
        ++atlas.sharedTriangles;
      }
      else
        ++atlas.ownTriangles;
    }
    for (Piece &piece : mapping.pieces)
    {
      double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
      for (uint32_t t : piece.triangles)
        for (uint32_t k = 0; k < 3u; ++k)
        {
          const auto p = project(cluster.vertices[cluster.indices[t * 3 + k]], piece.view, mapping.texel, meshSettings);
          for (int a = 0; a < 2; ++a)
          {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
          }
        }
      // A hair of slack so lattice-exact corners land on texel edges.
      piece.u0 = int64_t(std::floor(lo[0] + 1e-4));
      piece.v0 = int64_t(std::floor(lo[1] + 1e-4));
      piece.u1 = std::max(piece.u0 + 1, int64_t(std::ceil(hi[0] - 1e-4)));
      piece.v1 = std::max(piece.v0 + 1, int64_t(std::ceil(hi[1] - 1e-4)));
      atlas.peeledCharts += piece.layer > 0u;
    }
  }

  // 2. Shelf-pack the used regions and every own piece, tallest first.
  struct Slot
  {
    uint32_t cluster, piece, width, height; // cluster UINT32_MAX: piece is a region.
  };
  std::vector<Slot> slots;
  uint64_t area = 0u;
  uint32_t widest = 0u;
  const auto addSlot = [&](uint32_t cluster, uint32_t piece, uint64_t w, uint64_t h)
  {
    if (w > settings.maxAtlasWidth || h > settings.maxAtlasWidth)
      return false;
    slots.push_back({cluster, piece, uint32_t(w), uint32_t(h)});
    area += w * h;
    widest = std::max(widest, uint32_t(w));
    return true;
  };
  for (uint32_t r = 0; r < views.regions.size(); ++r)
  {
    const auto &region = views.regions[r];
    if (region.used && !addSlot(UINT32_MAX, r, uint64_t(region.u1 - region.u0 + 1) + 2u * gutter, uint64_t(region.w1 - region.w0 + 1) + 2u * gutter))
      return fail(error, "a view region is larger than the atlas");
  }
  for (uint32_t c = 0; c < mappings.size(); ++c)
    for (uint32_t p = 0; p < mappings[c].pieces.size(); ++p)
    {
      const Piece &piece = mappings[c].pieces[p];
      if (!addSlot(c, p, uint64_t(piece.u1 - piece.u0) + 2u * gutter, uint64_t(piece.v1 - piece.v0) + 2u * gutter))
        return fail(error, "a chart is larger than the atlas");
    }
  std::stable_sort(slots.begin(), slots.end(), [](const Slot &a, const Slot &b) { return a.height != b.height ? a.height > b.height : a.width > b.width; });
  uint32_t width = 64u;
  while (width < settings.maxAtlasWidth && (uint64_t(width) * width < area * 11u / 10u || width < widest))
    width *= 2u;
  uint32_t x = 0u, y = 0u, shelf = 0u;
  for (const Slot &slot : slots)
  {
    if (x + slot.width > width)
    {
      x = 0u;
      y += shelf;
      shelf = 0u;
    }
    if (slot.cluster == UINT32_MAX)
    {
      auto &region = views.regions[slot.piece];
      region.atlasX = x;
      region.atlasY = y;
      atlas.charts.push_back({UINT32_MAX, region.view, region.layer, x, y, slot.width, slot.height});
      ++atlas.sharedRegions;
    }
    else
    {
      Piece &piece = mappings[slot.cluster].pieces[slot.piece];
      piece.atlasX = x;
      piece.atlasY = y;
      atlas.charts.push_back({slot.cluster, piece.view, piece.layer, x, y, slot.width, slot.height});
    }
    x += slot.width;
    shelf = std::max(shelf, slot.height);
  }
  atlas.width = width;
  atlas.height = y + shelf;
  atlas.rgba.assign(size_t(atlas.width) * atlas.height, 0xff808080u);
  std::vector<uint8_t> filled(atlas.rgba.size(), 0u);

  // Every unfilled texel of a rectangle takes its nearest filled neighbour's
  // colour (gutter, edges, covered holes).
  const auto dilate = [&](uint32_t rx0, uint32_t ry0, uint32_t rw, uint32_t rh)
  {
    std::deque<std::pair<uint32_t, uint32_t>> queue;
    for (uint32_t ry = 0; ry < rh; ++ry)
      for (uint32_t rx = 0; rx < rw; ++rx)
        if (filled[size_t(ry0 + ry) * atlas.width + rx0 + rx])
          queue.emplace_back(rx, ry);
    while (!queue.empty())
    {
      const auto [rx, ry] = queue.front();
      queue.pop_front();
      const uint32_t colour = atlas.rgba[size_t(ry0 + ry) * atlas.width + rx0 + rx];
      const int32_t dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4; ++k)
      {
        const int64_t nx = int64_t(rx) + dx[k], ny = int64_t(ry) + dy[k];
        if (nx < 0 || ny < 0 || nx >= int64_t(rw) || ny >= int64_t(rh))
          continue;
        const size_t at = size_t(ry0 + uint32_t(ny)) * atlas.width + rx0 + uint32_t(nx);
        if (filled[at])
          continue;
        filled[at] = 1u;
        atlas.rgba[at] = colour;
        queue.emplace_back(uint32_t(nx), uint32_t(ny));
      }
    }
  };

  // 3a. Regions: the faces' own colours, one texel per voxel face.
  for (uint32_t r = 0; r < views.regions.size(); ++r)
  {
    const auto &region = views.regions[r];
    if (!region.used)
      continue;
    for (int64_t w = region.w0; w <= region.w1; ++w)
      for (int64_t u = region.u0; u <= region.u1; ++u)
      {
        const auto *column = views.column(region.view, u, w);
        const ObjectViews::Entry *entry = nullptr;
        if (column)
          for (const auto &candidate : *column)
            if (candidate.region == r)
              entry = &candidate;
        if (!entry)
          continue;
        const size_t at = size_t(region.atlasY + gutter + uint32_t(w - region.w0)) * atlas.width + region.atlasX + gutter + uint32_t(u - region.u0);
        atlas.rgba[at] = entry->rgba | 0xff000000u;
        filled[at] = 1u;
        ++atlas.coveredTexels;
      }
    dilate(region.atlasX, region.atlasY, uint32_t(region.u1 - region.u0 + 1) + 2u * gutter, uint32_t(region.w1 - region.w0 + 1) + 2u * gutter);
  }

  // 3b. The clusters' own pieces, from the voxels.
  const ColourField field(cells, meshSettings);
  for (uint32_t c = 0; c < mappings.size(); ++c)
  {
    const VirtualGeometryCluster &cluster = build.clusters[c];
    const ClusterMapping &mapping = mappings[c];
    const double reach = pitch + 2.0 * double(cluster.self.error);
    const uint32_t subsamples = std::clamp(uint32_t(std::lround(mapping.texel / pitch)), 1u, std::max(1u, settings.maxSubsamples));
    for (const Piece &piece : mapping.pieces)
    {
      const int64_t ox = int64_t(piece.atlasX + gutter) - piece.u0, oy = int64_t(piece.atlasY + gutter) - piece.v0;
      for (uint32_t t : piece.triangles)
      {
        const Vertex *v[3] = {&cluster.vertices[cluster.indices[t * 3]], &cluster.vertices[cluster.indices[t * 3 + 1]],
                              &cluster.vertices[cluster.indices[t * 3 + 2]]};
        // March along the chart's view axis, not the triangle normal: the
        // texel shows what that view sees. Along a tilted normal, stepped
        // terrain was left sideways through a riser and came back rock.
        Float3 n{0.f, 0.f, 0.f};
        n[piece.view / 2u] = (piece.view & 1u) ? -1.f : 1.f;
        std::array<double, 2> q[3];
        for (int k = 0; k < 3; ++k)
          q[k] = project(*v[k], piece.view, mapping.texel, meshSettings);
        const double area2 = (q[1][0] - q[0][0]) * (q[2][1] - q[0][1]) - (q[2][0] - q[0][0]) * (q[1][1] - q[0][1]);
        forTexelCentres(q[0], q[1], q[2], [&](int64_t tx, int64_t ty, double, double, double)
                        {
                          const int64_t ax = tx + ox, ay = ty + oy;
                          if (ax < 0 || ay < 0 || ax >= int64_t(atlas.width) || ay >= int64_t(atlas.height))
                            return;
                          const size_t at = size_t(ay) * atlas.width + size_t(ax);
                          if (filled[at])
                            return; // On an edge shared with a triangle already baked.
                          ++atlas.coveredTexels;
                          double sum[3] = {0.0, 0.0, 0.0};
                          uint32_t hits = 0u;
                          for (uint32_t sy = 0; sy < subsamples; ++sy)
                            for (uint32_t sx = 0; sx < subsamples; ++sx)
                            {
                              const double px = double(tx) + (sx + 0.5) / subsamples, py = double(ty) + (sy + 0.5) / subsamples;
                              const double w0 = ((q[1][0] - px) * (q[2][1] - py) - (q[2][0] - px) * (q[1][1] - py)) / area2;
                              const double w1 = ((q[2][0] - px) * (q[0][1] - py) - (q[0][0] - px) * (q[2][1] - py)) / area2;
                              const double w2 = 1.0 - w0 - w1;
                              std::array<double, 3> p;
                              for (int a = 0; a < 3; ++a)
                                p[a] = w0 * v[0]->pos[a] + w1 * v[1]->pos[a] + w2 * v[2]->pos[a];
                              uint32_t rgba = 0u;
                              if (!field.surface(p, n, reach, rgba))
                                continue;
                              for (int ch = 0; ch < 3; ++ch)
                                sum[ch] += double((rgba >> (8 * ch)) & 0xffu);
                              ++hits;
                            }
                          if (hits == 0u)
                          {
                            ++atlas.missedTexels;
                            return;
                          }
                          uint32_t rgba = 0xff000000u;
                          for (int ch = 0; ch < 3; ++ch)
                            rgba |= uint32_t(std::lround(sum[ch] / hits)) << (8 * ch);
                          atlas.rgba[at] = rgba;
                          filled[at] = 1u;
                        },
                        true);
      }
      dilate(piece.atlasX, piece.atlasY, uint32_t(piece.u1 - piece.u0) + 2u * gutter, uint32_t(piece.v1 - piece.v0) + 2u * gutter);
    }
  }

  // 4. UVs and normals: one vertex copy per (chart, vertex). The copy's
  // normal is its chart's own - area-weighted over the chart's triangles at
  // that vertex - so an axis view's faces shade flat, as voxels do, and the
  // welded mesh's smoothed normals never round a voxel edge.
  for (uint32_t c = 0; c < mappings.size(); ++c)
  {
    VirtualGeometryCluster &cluster = build.clusters[c];
    const ClusterMapping &mapping = mappings[c];
    const uint32_t triangleCount = uint32_t(cluster.indices.size() / 3u);
    // Chart key per triangle, and where its texels start.
    struct Chart
    {
      uint32_t view;
      double texel, ox, oy;
    };
    std::vector<uint64_t> chartKey(triangleCount);
    std::unordered_map<uint64_t, Chart> charts;
    for (uint32_t t = 0; t < triangleCount; ++t)
      if (mapping.region[t] != UINT32_MAX)
      {
        const auto &region = views.regions[mapping.region[t]];
        chartKey[t] = mapping.region[t];
        charts[chartKey[t]] = {region.view, pitch, double(region.atlasX + gutter) - double(region.u0), double(region.atlasY + gutter) - double(region.w0)};
      }
    for (size_t p = 0; p < mapping.pieces.size(); ++p)
    {
      const Piece &piece = mapping.pieces[p];
      const uint64_t key = (uint64_t(1) << 40) | p;
      charts[key] = {piece.view, mapping.texel, double(piece.atlasX + gutter) - double(piece.u0), double(piece.atlasY + gutter) - double(piece.v0)};
      for (uint32_t t : piece.triangles)
        chartKey[t] = key;
    }
    std::map<std::pair<uint64_t, uint32_t>, std::array<double, 3>> normals;
    for (uint32_t t = 0; t < triangleCount; ++t)
    {
      float area = 0.f;
      const Float3 n = triangleNormal(cluster.vertices[cluster.indices[t * 3]], cluster.vertices[cluster.indices[t * 3 + 1]],
                                      cluster.vertices[cluster.indices[t * 3 + 2]], &area);
      for (uint32_t k = 0; k < 3u; ++k)
      {
        auto &sum = normals[{chartKey[t], cluster.indices[t * 3 + k]}];
        for (int a = 0; a < 3; ++a)
          sum[a] += double(n[a]) * area;
      }
    }
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices(cluster.indices.size());
    std::map<std::pair<uint64_t, uint32_t>, uint32_t> copy;
    for (uint32_t t = 0; t < triangleCount; ++t)
      for (uint32_t k = 0; k < 3u; ++k)
      {
        const uint32_t source = cluster.indices[t * 3 + k];
        auto [it, inserted] = copy.emplace(std::make_pair(chartKey[t], source), uint32_t(vertices.size()));
        if (inserted)
        {
          const Chart &chart = charts[chartKey[t]];
          Vertex vertex = cluster.vertices[source];
          const auto p = project(vertex, chart.view, chart.texel, meshSettings);
          vertex.uv[0] = float((p[0] + chart.ox) / atlas.width);
          vertex.uv[1] = float((p[1] + chart.oy) / atlas.height);
          const auto &n = normals[{chartKey[t], source}];
          const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          if (length > 0.0)
            for (int a = 0; a < 3; ++a)
              vertex.norm[a] = float(n[a] / length);
          vertices.push_back(vertex);
        }
        indices[t * 3 + k] = it->second;
      }
    atlas.addedVertices += uint32_t(vertices.size() - cluster.vertices.size());
    atlas.maxClusterVertices = std::max(atlas.maxClusterVertices, uint32_t(vertices.size()));
    cluster.vertices = std::move(vertices);
    cluster.indices = std::move(indices);
  }
  if (atlas.maxClusterVertices > MaxVertices)
    return fail(error, "a cluster exceeds MaxVertices after unwrapping");
  return true;
}

bool writeVoxelSurfaceAsset(
    const VirtualGeometryBuildData &build,
    const VoxelSurfaceAtlas &atlas,
    const std::string &basePath,
    std::string *error)
{
  namespace fs = std::filesystem;
  const std::string png = basePath + ".png", vmat = basePath + ".vmat", vg = basePath + ".vg";
  if (!stbi_write_png(png.c_str(), int(atlas.width), int(atlas.height), 4, atlas.rgba.data(), int(atlas.width * 4u)))
    return fail(error, "cannot write " + png);

  VirtualTextureSystem::MaterialCreateInfo material{};
  auto &texture = material.textures[material.textureCount++];
  texture.name = "baseColor";
  texture.source.path = fs::absolute(png).lexically_normal().string();
  // Charts are packed edge to edge: clamp, and nearest so LOD0 shows voxels.
  texture.sampling.addressModeU = VirtualTextureSystem::TextureAddressMode::ClampToEdge;
  texture.sampling.addressModeV = VirtualTextureSystem::TextureAddressMode::ClampToEdge;
  texture.sampling.filterMode = VirtualTextureSystem::TextureFilterMode::Nearest;
  // No mips: a cluster's texels are as wide as its own error, and the cut
  // only draws it while its parent's error is over the pixel threshold, so a
  // texel never shrinks much below half a pixel. Mips would bleed across
  // charts packed edge to edge.
  texture.sampling.maxMip = 0u;
  if (!VirtualMaterialFile::saveFromCreateInfo(vmat, material))
    return fail(error, "cannot write " + vmat);

  VirtualGeometryBuildData withMaterial = build;
  withMaterial.materialFiles = {fs::absolute(vmat).lexically_normal().string()};
  for (VirtualGeometryShapeInfo &shape : withMaterial.shapes)
    shape.materialIndex = 0u;
  const VirtualGeometryEncodedData encoded = VirtualGeometryCompressor::encode(withMaterial);
  VirtualGeometryFile file(vg, true);
  if (!file.isOpen() || !file.write(encoded, withMaterial.pages, MESHLET_LZ4))
    return fail(error, "cannot write " + vg);
  return true;
}

} // namespace virtualgeometry
