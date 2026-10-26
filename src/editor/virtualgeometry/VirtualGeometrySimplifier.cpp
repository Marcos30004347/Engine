#include "VirtualGeometrySimplifier.hpp"
#include "meshoptimizer.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <limits>
#include <unordered_map>

using namespace virtualgeometry;

namespace
{

constexpr size_t kBaseAttributeCount = 5u;
constexpr size_t kMaxSimplifierInfluences = (32u - kBaseAttributeCount) / 2u;
constexpr float kNormalAttributeWeight = 0.5f;
constexpr float kUVAttributeWeight = 1.0f;
constexpr float kBoneIndexAttributeWeight = 4.0f;
constexpr float kBoneWeightAttributeWeight = 2.0f;
constexpr float kSloppyErrorFactor = 2.0f;

using Point3 = std::array<float, 3>;

static float pointTriangleDistanceSquared(const Point3 &p, const Point3 &a, const Point3 &b, const Point3 &c)
{
  // Ericson, Real-Time Collision Detection 5.1.5.
  const auto sub = [](const Point3 &x, const Point3 &y) { return Point3{x[0] - y[0], x[1] - y[1], x[2] - y[2]}; };
  const auto dot = [](const Point3 &x, const Point3 &y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
  const auto at = [&](float v, float w) {
    return Point3{a[0] + (b[0] - a[0]) * v + (c[0] - a[0]) * w, a[1] + (b[1] - a[1]) * v + (c[1] - a[1]) * w,
                  a[2] + (b[2] - a[2]) * v + (c[2] - a[2]) * w};
  };
  const auto d2 = [&](const Point3 &q) { const Point3 d = sub(p, q); return dot(d, d); };
  const Point3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
  const float d1 = dot(ab, ap), e1 = dot(ac, ap);
  if (d1 <= 0.f && e1 <= 0.f) return d2(a);
  const Point3 bp = sub(p, b);
  const float d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0.f && d4 <= d3) return d2(b);
  const float vc = d1 * d4 - d3 * e1;
  if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) return d2(at(d1 / (d1 - d3), 0.f));
  const Point3 cp = sub(p, c);
  const float d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0.f && d5 <= d6) return d2(c);
  const float vb = d5 * e1 - d1 * d6;
  if (vb <= 0.f && e1 >= 0.f && d6 <= 0.f) return d2(at(0.f, e1 / (e1 - d6)));
  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
  {
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return d2(at(1.f - w, w));
  }
  const float denom = 1.f / (va + vb + vc);
  return d2(at(vb * denom, vc * denom));
}

// Nearest-surface queries against one triangle set, through a uniform grid
// whose cells list every triangle whose box they overlap. A query that finds a
// triangle within one cell of the point is exact (nothing nearer can sit
// outside the 3x3x3 block); farther than that it falls back to every triangle.
class TriangleDistanceGrid
{
public:
  TriangleDistanceGrid(const std::vector<Point3> &positions, const std::vector<uint32_t> &indices)
      : positions_(positions), indices_(indices)
  {
    lo_ = {FLT_MAX, FLT_MAX, FLT_MAX};
    Point3 hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (uint32_t index : indices)
      for (int a = 0; a < 3; ++a)
      {
        lo_[a] = std::min(lo_[a], positions[index][a]);
        hi[a] = std::max(hi[a], positions[index][a]);
      }
    const float extent = std::max({hi[0] - lo_[0], hi[1] - lo_[1], hi[2] - lo_[2], 1e-6f});
    const size_t triangles = indices.size() / 3u;
    const float cells = std::clamp(std::cbrt(float(triangles)) * 2.f, 1.f, 32.f);
    cell_ = extent / cells;
    for (int a = 0; a < 3; ++a)
      size_[a] = std::max(1, int(std::ceil((hi[a] - lo_[a]) / cell_)) + 1);
    start_.assign(size_t(size_[0]) * size_[1] * size_[2] + 1u, 0u);
    const auto forCells = [&](size_t t, auto &&visit) {
      int cmin[3], cmax[3];
      for (int a = 0; a < 3; ++a)
      {
        float tlo = FLT_MAX, thi = -FLT_MAX;
        for (int k = 0; k < 3; ++k)
        {
          tlo = std::min(tlo, positions[indices[t * 3u + k]][a]);
          thi = std::max(thi, positions[indices[t * 3u + k]][a]);
        }
        cmin[a] = cellOf(tlo, a);
        cmax[a] = cellOf(thi, a);
      }
      for (int z = cmin[2]; z <= cmax[2]; ++z)
        for (int y = cmin[1]; y <= cmax[1]; ++y)
          for (int x = cmin[0]; x <= cmax[0]; ++x)
            visit((size_t(z) * size_[1] + y) * size_[0] + x);
    };
    for (size_t t = 0; t < triangles; ++t)
      forCells(t, [&](size_t c) { ++start_[c + 1u]; });
    for (size_t c = 1; c < start_.size(); ++c)
      start_[c] += start_[c - 1u];
    items_.resize(start_.back());
    std::vector<uint32_t> fill(start_.begin(), start_.end() - 1);
    for (size_t t = 0; t < triangles; ++t)
      forCells(t, [&](size_t c) { items_[fill[c]++] = uint32_t(t); });
  }

  float distance(const Point3 &p) const
  {
    // Shells of cells at growing Chebyshev radius r around the point's cell;
    // after shell r every triangle within r cells is seen, so a best distance
    // under r cell widths is exact.
    float best = FLT_MAX;
    int c[3];
    for (int a = 0; a < 3; ++a)
      c[a] = cellOf(p[a], a);
    const int reach = std::max({size_[0], size_[1], size_[2]});
    for (int r = 0; r <= reach; ++r)
    {
      for (int z = std::max(0, c[2] - r); z <= std::min(size_[2] - 1, c[2] + r); ++z)
        for (int y = std::max(0, c[1] - r); y <= std::min(size_[1] - 1, c[1] + r); ++y)
        {
          const auto visit = [&](int x) {
            const size_t cell = (size_t(z) * size_[1] + y) * size_[0] + x;
            for (uint32_t i = start_[cell]; i < start_[cell + 1u]; ++i)
              best = std::min(best, triangle(items_[i], p));
          };
          if (r == 0 || std::abs(z - c[2]) == r || std::abs(y - c[1]) == r)
          {
            for (int x = std::max(0, c[0] - r); x <= std::min(size_[0] - 1, c[0] + r); ++x)
              visit(x);
          }
          else
          {
            if (c[0] - r >= 0)
              visit(c[0] - r);
            if (c[0] + r <= size_[0] - 1)
              visit(c[0] + r);
          }
        }
      if (best <= float(r) * cell_ * float(r) * cell_)
        break;
    }
    return std::sqrt(best);
  }

private:
  int cellOf(float v, int axis) const
  {
    return std::clamp(int((v - lo_[axis]) / cell_), 0, size_[axis] - 1);
  }
  float triangle(uint32_t t, const Point3 &p) const
  {
    return pointTriangleDistanceSquared(p, positions_[indices_[t * 3u]], positions_[indices_[t * 3u + 1u]],
                                        positions_[indices_[t * 3u + 2u]]);
  }
  const std::vector<Point3> &positions_;
  const std::vector<uint32_t> &indices_;
  Point3 lo_{};
  float cell_ = 1.f;
  int size_[3] = {1, 1, 1};
  std::vector<uint32_t> start_, items_;
};

// How far apart two surfaces are, both ways (a symmetric Hausdorff distance
// estimated at vertices and centroids). The quadric error
// meshoptimizer reports is not this: a voxel staircase collapsed to the slope
// it stands for reported a fraction of how far its step corners moved, and
// the cut drew those levels visibly early.
static float surfaceDeviation(const std::vector<Point3> &aPositions, const std::vector<uint32_t> &aIndices,
                              const std::vector<Point3> &bPositions, const std::vector<uint32_t> &bIndices)
{
  if (aIndices.empty() || bIndices.empty())
    return 0.f;
  float result = 0.f;
  const auto oneWay = [&](const std::vector<Point3> &from, const std::vector<uint32_t> &fromIndices,
                          const TriangleDistanceGrid &to) {
    std::vector<uint8_t> seen(from.size(), 0u);
    for (size_t t = 0; t + 2 < fromIndices.size(); t += 3)
    {
      const Point3 &p0 = from[fromIndices[t]], &p1 = from[fromIndices[t + 1]], &p2 = from[fromIndices[t + 2]];
      for (int k = 0; k < 3; ++k)
        if (!seen[fromIndices[t + k]])
        {
          seen[fromIndices[t + k]] = 1u;
          result = std::max(result, to.distance(from[fromIndices[t + k]]));
        }
      const Point3 samples[1] = {
          {(p0[0] + p1[0] + p2[0]) / 3.f, (p0[1] + p1[1] + p2[1]) / 3.f, (p0[2] + p1[2] + p2[2]) / 3.f}};
      for (const Point3 &q : samples)
        result = std::max(result, to.distance(q));
    }
  };
  oneWay(aPositions, aIndices, TriangleDistanceGrid(bPositions, bIndices));
  oneWay(bPositions, bIndices, TriangleDistanceGrid(aPositions, aIndices));
  return result;
}

static size_t getMaxBoneInfluenceCount(const std::vector<Vertex> &vertices)
{
  size_t maxCount = 0u;
  for (const Vertex &vertex : vertices)
    maxCount = std::max(maxCount, vertex.boneWeights.size());
  return std::min(maxCount, kMaxSimplifierInfluences);
}

static bool containsBoneWeights(const std::vector<Vertex> &vertices)
{
  return getMaxBoneInfluenceCount(vertices) > 0u;
}

static uint32_t getMaxBoneIndex(const std::vector<Vertex> &vertices)
{
  uint32_t maxBoneIndex = 0u;
  for (const Vertex &vertex : vertices)
    for (const BoneWeight &boneWeight : vertex.boneWeights)
      maxBoneIndex = std::max(maxBoneIndex, boneWeight.boneIndex);
  return maxBoneIndex;
}

static void buildSimplifierAttributes(
    const std::vector<Vertex> &vertices,
    size_t influenceCount,
    float boneIndexScale,
    std::vector<float> &attributes,
    std::vector<float> &attributeWeights)
{
  const size_t attributeCount = kBaseAttributeCount + influenceCount * 2u;
  attributes.resize(vertices.size() * attributeCount, 0.0f);
  attributeWeights.assign(attributeCount, 0.0f);

  attributeWeights[0] = kNormalAttributeWeight;
  attributeWeights[1] = kNormalAttributeWeight;
  attributeWeights[2] = kNormalAttributeWeight;
  attributeWeights[3] = kUVAttributeWeight;
  attributeWeights[4] = kUVAttributeWeight;

  for (size_t influenceIndex = 0u; influenceIndex < influenceCount; ++influenceIndex)
  {
    attributeWeights[kBaseAttributeCount + influenceIndex * 2u + 0u] = kBoneIndexAttributeWeight;
    attributeWeights[kBaseAttributeCount + influenceIndex * 2u + 1u] = kBoneWeightAttributeWeight;
  }

  for (size_t vertexIndex = 0u; vertexIndex < vertices.size(); ++vertexIndex)
  {
    const Vertex &vertex = vertices[vertexIndex];
    const size_t attributeBase = vertexIndex * attributeCount;
    attributes[attributeBase + 0u] = vertex.norm[0];
    attributes[attributeBase + 1u] = vertex.norm[1];
    attributes[attributeBase + 2u] = vertex.norm[2];
    attributes[attributeBase + 3u] = vertex.uv[0];
    attributes[attributeBase + 4u] = vertex.uv[1];

    for (size_t influenceIndex = 0u; influenceIndex < influenceCount; ++influenceIndex)
    {
      const size_t influenceBase = attributeBase + kBaseAttributeCount + influenceIndex * 2u;
      if (influenceIndex < vertex.boneWeights.size())
      {
        attributes[influenceBase + 0u] = static_cast<float>(vertex.boneWeights[influenceIndex].boneIndex) * boneIndexScale;
        attributes[influenceBase + 1u] = vertex.boneWeights[influenceIndex].weight;
      }
    }
  }
}

static void rebuildBoneWeightsFromAttributes(
    std::vector<Vertex> &vertices,
    const std::vector<float> &attributes,
    size_t influenceCount,
    float boneIndexScale,
    uint32_t maxBoneIndex)
{
  const size_t attributeCount = kBaseAttributeCount + influenceCount * 2u;
  const float inverseBoneIndexScale = boneIndexScale > 0.0f ? (1.0f / boneIndexScale) : 0.0f;

  for (size_t vertexIndex = 0u; vertexIndex < vertices.size(); ++vertexIndex)
  {
    Vertex &vertex = vertices[vertexIndex];
    const std::vector<BoneWeight> originalWeights = vertex.boneWeights;
    const size_t attributeBase = vertexIndex * attributeCount;

    vertex.norm[0] = attributes[attributeBase + 0u];
    vertex.norm[1] = attributes[attributeBase + 1u];
    vertex.norm[2] = attributes[attributeBase + 2u];
    vertex.uv[0] = attributes[attributeBase + 3u];
    vertex.uv[1] = attributes[attributeBase + 4u];

    if (influenceCount == 0u)
      continue;

    std::unordered_map<uint32_t, float> mergedWeights;
    for (size_t influenceIndex = 0u; influenceIndex < influenceCount; ++influenceIndex)
    {
      const size_t influenceBase = attributeBase + kBaseAttributeCount + influenceIndex * 2u;
      const float weight = std::max(0.0f, attributes[influenceBase + 1u]);
      if (weight <= 0.0f)
        continue;

      const float encodedBoneIndex = attributes[influenceBase + 0u];
      const uint32_t boneIndex = boneIndexScale > 0.0f
                                     ? std::min(maxBoneIndex, static_cast<uint32_t>(std::lround(encodedBoneIndex * inverseBoneIndexScale)))
                                     : 0u;
      mergedWeights[boneIndex] += weight;
    }

    std::vector<BoneWeight> rebuiltWeights;
    rebuiltWeights.reserve(mergedWeights.size());
    for (const auto &entry : mergedWeights)
      rebuiltWeights.push_back(BoneWeight{entry.second, entry.first});

    if (rebuiltWeights.empty())
      rebuiltWeights = originalWeights;

    if (rebuiltWeights.empty())
    {
      vertex.boneWeights.clear();
      continue;
    }

    std::sort(
        rebuiltWeights.begin(),
        rebuiltWeights.end(),
        [](const BoneWeight &lhs, const BoneWeight &rhs)
        {
          if (lhs.weight != rhs.weight)
            return lhs.weight > rhs.weight;
          return lhs.boneIndex < rhs.boneIndex;
        });

    if (rebuiltWeights.size() > influenceCount)
      rebuiltWeights.resize(influenceCount);

    float totalWeight = 0.0f;
    for (const BoneWeight &boneWeight : rebuiltWeights)
      totalWeight += boneWeight.weight;

    if (totalWeight > 0.0f)
      for (BoneWeight &boneWeight : rebuiltWeights)
        boneWeight.weight /= totalWeight;

    rebuiltWeights.resize(influenceCount, BoneWeight{0.0f, rebuiltWeights.front().boneIndex});
    vertex.boneWeights = std::move(rebuiltWeights);
  }
}

} // namespace

std::vector<uint32_t> VirtualGeometrySimplifier::simplify(
    const std::vector<Vertex>  &vertices,
    std::vector<Vertex>        &outVertices,
    const std::vector<uint32_t> &indices,
    const std::vector<uint8_t> *locks,
    uint64_t                    targetCount,
    float                      *outError,
    bool                        positionsOnly,
    float                       positionGrid)
{
  // ── 1. Build a compact local vertex buffer from the indices ───────────────
  // Sort + unique gives us a sorted list of referenced global indices, which
  // lets us use binary search for the global→local remap (no hash map needed).
  std::vector<uint32_t> uniqueGlobal = indices;
  std::sort(uniqueGlobal.begin(), uniqueGlobal.end());
  uniqueGlobal.erase(std::unique(uniqueGlobal.begin(), uniqueGlobal.end()), uniqueGlobal.end());

  std::vector<Vertex>  localVerts(uniqueGlobal.size());
  std::vector<uint8_t> localLocks;

  for (size_t i = 0; i < uniqueGlobal.size(); ++i)
    localVerts[i] = vertices[uniqueGlobal[i]];

  if (locks)
  {
    localLocks.resize(uniqueGlobal.size());
    for (size_t i = 0; i < uniqueGlobal.size(); ++i)
      localLocks[i] = (*locks)[uniqueGlobal[i]];
  }

  std::vector<uint32_t> localIndices(indices.size());
  for (size_t i = 0; i < indices.size(); ++i)
  {
    auto it       = std::lower_bound(uniqueGlobal.begin(), uniqueGlobal.end(), indices[i]);
    localIndices[i] = static_cast<uint32_t>(it - uniqueGlobal.begin());
  }

  // Early out — target already satisfied.
  if (targetCount >= localIndices.size())
  {
    outVertices = std::move(localVerts);
    return localIndices;
  }

  // ── 2. Simplify with vertex update ───────────────────────────────────────
  // meshopt_SimplifyErrorAbsolute: error is returned in world-space units,
  // which maps directly to a projection error at runtime:
  //   screenError = worldError / distance * cotHalfFov * screenHeight * 0.5
  //
  // UV weight is raised to 1.0 as recommended when using WithUpdate so that
  // texture coordinates are updated to avoid distortion. Skinning attributes
  // are packed into the same stream so simplification respects bone affinity.
  const uint32_t options = meshopt_SimplifyErrorAbsolute;
  if (positionsOnly)
  {
    // Geometry alone: normals and UVs neither steer the collapses nor add to
    // the error. Weld by position first. Clusters that came from different groups of
    // the level below carry their own copies of the vertices they share; with
    // no attributes to tell them apart meshoptimizer takes each such pair for
    // a seam and only collapses along it, which stalled a voxel house's chain
    // at 10k triangles.
    std::vector<uint32_t> weld(localVerts.size());
    const meshopt_Stream position = {&localVerts[0].pos[0], sizeof(float) * 3, sizeof(Vertex)};
    const size_t weldedCount = meshopt_generateVertexRemapMulti(weld.data(), localIndices.data(), localIndices.size(), localVerts.size(), &position, 1);
    std::vector<Vertex> weldedVerts(weldedCount);
    std::vector<uint8_t> weldedLocks(weldedCount, 0u);
    for (size_t i = 0; i < localVerts.size(); ++i)
    {
      weldedVerts[weld[i]] = localVerts[i];
      if (locks)
        weldedLocks[weld[i]] |= localLocks[i];
    }
    for (uint32_t &idx : localIndices)
      idx = weld[idx];
    localVerts = std::move(weldedVerts);
    localLocks = std::move(weldedLocks);

    // Surviving vertices move to their quadric optimum: a voxel staircase
    // becomes the slope it stands for (a hip roof's 1 m cut, 2066 terraced
    // triangles pinned to the lattice, 186 sloped), and the farthest sample of
    // a dwelling's 0.1 m cut fell from 0.33 to 0.08 m. Locked vertices never
    // move, so group borders stay bit-identical on both sides.
    // An open edge of the group that no other group shares is the mesh's own
    // border - a terrain chunk's edge - and stays put, as a neighbouring
    // chunk meshed alone has to meet it at any level.
    float resultError = 0.f;
    std::vector<Point3> inputPositions(localVerts.size());
    for (size_t i = 0; i < localVerts.size(); ++i)
      inputPositions[i] = {localVerts[i].pos[0], localVerts[i].pos[1], localVerts[i].pos[2]};
    std::vector<uint32_t> simplified = localIndices;
    const size_t resultSize = meshopt_simplifyWithUpdate(
        simplified.data(), simplified.size(),
        &localVerts[0].pos[0], localVerts.size(), sizeof(Vertex),
        nullptr, 0, nullptr, 0,
        locks ? localLocks.data() : nullptr,
        targetCount, std::numeric_limits<float>::max(), options | meshopt_SimplifyLockBorder, &resultError);
    simplified.resize(resultSize);

    // Then drop whole free-floating pieces - no vertex on the group border, so
    // nothing outside the group can open - smallest first, until the target
    // is met. Edge collapse alone cannot remove a closed piece, and a house
    // has many (frames, fittings); each costs its radius in error, as
    // meshopt_SimplifyPrune counts it. That option is not used because it
    // prunes pieces that touch locked vertices too.
    if (simplified.size() > targetCount)
    {
      std::vector<uint32_t> component(localVerts.size());
      for (uint32_t i = 0; i < component.size(); ++i)
        component[i] = i;
      const auto find = [&](uint32_t x)
      {
        while (component[x] != x)
          x = component[x] = component[component[x]];
        return x;
      };
      for (size_t t = 0; t + 2 < simplified.size(); t += 3)
        for (uint32_t e = 1; e < 3u; ++e)
        {
          const uint32_t a = find(simplified[t]), b = find(simplified[t + e]);
          if (a != b)
            component[std::max(a, b)] = std::min(a, b);
        }
      struct Piece
      {
        double centre[3] = {0.0, 0.0, 0.0};
        double count = 0.0;
        float radius = 0.f;
        bool anchored = false;
        uint64_t triangles = 0u;
      };
      std::unordered_map<uint32_t, Piece> pieces;
      std::vector<uint8_t> used(localVerts.size(), 0u);
      for (uint32_t idx : simplified)
        used[idx] = 1u;
      for (uint32_t v = 0; v < localVerts.size(); ++v)
        if (used[v])
        {
          Piece &piece = pieces[find(v)];
          for (int a = 0; a < 3; ++a)
            piece.centre[a] += localVerts[v].pos[a];
          piece.count += 1.0;
          piece.anchored = piece.anchored || (locks && localLocks[v]);
        }
      for (auto &[root, piece] : pieces)
        for (int a = 0; a < 3; ++a)
          piece.centre[a] /= piece.count;
      for (uint32_t v = 0; v < localVerts.size(); ++v)
        if (used[v])
        {
          Piece &piece = pieces[find(v)];
          double r2 = 0.0;
          for (int a = 0; a < 3; ++a)
            r2 += (localVerts[v].pos[a] - piece.centre[a]) * (localVerts[v].pos[a] - piece.centre[a]);
          piece.radius = std::max(piece.radius, float(std::sqrt(r2)));
        }
      for (size_t t = 0; t + 2 < simplified.size(); t += 3)
        ++pieces[find(simplified[t])].triangles;
      // A piece with an open edge reaches outside the group: anchored too.
      std::unordered_map<uint64_t, uint32_t> edgeUse;
      for (size_t t = 0; t + 2 < simplified.size(); t += 3)
        for (uint32_t e = 0; e < 3u; ++e)
        {
          const uint32_t a = simplified[t + e], b = simplified[t + (e + 1u) % 3u];
          ++edgeUse[(uint64_t(std::min(a, b)) << 32) | std::max(a, b)];
        }
      for (const auto &[edge, count] : edgeUse)
        if (count == 1u)
          pieces[find(uint32_t(edge >> 32))].anchored = true;

      std::vector<std::pair<float, uint32_t>> free;
      for (const auto &[root, piece] : pieces)
        if (!piece.anchored)
          free.emplace_back(piece.radius, root);
      std::sort(free.begin(), free.end());
      std::vector<uint8_t> drop(localVerts.size(), 0u);
      uint64_t remaining = simplified.size() / 3u;
      for (const auto &[radius, root] : free)
      {
        // Never the last piece: a group keeps some surface.
        if (remaining * 3u <= targetCount || remaining == pieces[root].triangles)
          break;
        drop[root] = 1u;
        remaining -= pieces[root].triangles;
        resultError = std::max(resultError, radius);
      }
      size_t write = 0;
      for (size_t t = 0; t + 2 < simplified.size(); t += 3)
        if (!drop[find(simplified[t])])
        {
          simplified[write++] = simplified[t];
          simplified[write++] = simplified[t + 1];
          simplified[write++] = simplified[t + 2];
        }
      simplified.resize(write);
    }
    // Moved vertices land on the encoder's grid here, so the measured error
    // below covers the rounding. Locked vertices are already on it: the
    // lattice is, and every earlier level snapped what it moved.
    if (positionGrid > 0.f)
      for (Vertex &vertex : localVerts)
        for (int a = 0; a < 3; ++a)
          vertex.pos[a] = std::round(vertex.pos[a] / positionGrid) * positionGrid;
    // The level's error is how far its surface really is from the input
    // (pruned pieces included: their triangles are missing from the output).
    if (outError)
    {
      std::vector<Point3> outputPositions(localVerts.size());
      for (size_t i = 0; i < localVerts.size(); ++i)
        outputPositions[i] = {localVerts[i].pos[0], localVerts[i].pos[1], localVerts[i].pos[2]};
      resultError = std::max(resultError, surfaceDeviation(inputPositions, localIndices, outputPositions, simplified));
    }
    localIndices = std::move(simplified);
    if (outError) *outError = resultError;
  }
  else
  {
    const size_t influenceCount = getMaxBoneInfluenceCount(localVerts);
    const uint32_t maxBoneIndex = getMaxBoneIndex(localVerts);
    const float boneIndexScale = maxBoneIndex > 0u ? (1.0f / static_cast<float>(maxBoneIndex)) : 0.0f;
    std::vector<float> vertexAttributes;
    std::vector<float> attributeWeights;
    buildSimplifierAttributes(localVerts, influenceCount, boneIndexScale, vertexAttributes, attributeWeights);
    float resultError = 0.f;

    size_t resultSize = meshopt_simplifyWithUpdate(
        localIndices.data(), localIndices.size(),
        &localVerts[0].pos[0],  localVerts.size(), sizeof(Vertex),
        vertexAttributes.data(), sizeof(float) * (kBaseAttributeCount + influenceCount * 2u),
        attributeWeights.data(), attributeWeights.size(),
        locks ? localLocks.data() : nullptr,
        targetCount, std::numeric_limits<float>::max(),
        options, &resultError);

    localIndices.resize(resultSize);
    if (outError) *outError = resultError;

    rebuildBoneWeightsFromAttributes(localVerts, vertexAttributes, influenceCount, boneIndexScale, maxBoneIndex);

    // ── 3. Renormalize normals (required after WithUpdate) ───────────────────
    for (auto &v : localVerts)
    {
      float len = std::sqrt(v.norm[0] * v.norm[0] + v.norm[1] * v.norm[1] + v.norm[2] * v.norm[2]);
      if (len > 1e-6f) { v.norm[0] /= len; v.norm[1] /= len; v.norm[2] /= len; }
    }
  }

  // ── 4. Compact: drop unreferenced vertices, vertex-cache-optimise first ──
  // We cannot use meshopt_optimizeVertexFetch because Vertex contains a
  // std::vector member; we do the remap manually instead.
  meshopt_optimizeVertexCache(localIndices.data(), localIndices.data(), localIndices.size(), localVerts.size());

  std::vector<uint32_t> oldToNew(localVerts.size(), UINT32_MAX);
  outVertices.clear();
  for (uint32_t idx : localIndices)
    if (oldToNew[idx] == UINT32_MAX)
    {
      oldToNew[idx] = static_cast<uint32_t>(outVertices.size());
      outVertices.push_back(std::move(localVerts[idx]));
    }
  for (uint32_t &idx : localIndices)
    idx = oldToNew[idx];

  return localIndices;
}

std::vector<uint32_t> VirtualGeometrySimplifier::simplifySloppy(
    const std::vector<Vertex>  &vertices,
    std::vector<Vertex>        &outVertices,
    const std::vector<uint32_t> &indices,
    const std::vector<uint8_t> *locks,
    uint64_t                    targetCount,
    float                      *outError)
{
  // ── 1. Extract local vertex buffer ───────────────────────────────────────
  std::vector<uint32_t> uniqueGlobal = indices;
  std::sort(uniqueGlobal.begin(), uniqueGlobal.end());
  uniqueGlobal.erase(std::unique(uniqueGlobal.begin(), uniqueGlobal.end()), uniqueGlobal.end());

  std::vector<Vertex> localVerts(uniqueGlobal.size());
  std::vector<uint8_t> localLocks(uniqueGlobal.size(), 0);
  bool hasLockedVertex = false;
  for (size_t i = 0; i < uniqueGlobal.size(); ++i)
  {
    localVerts[i] = vertices[uniqueGlobal[i]];
    if (locks)
    {
      localLocks[i] = (*locks)[uniqueGlobal[i]];
      hasLockedVertex = hasLockedVertex || (localLocks[i] != 0);
    }
  }

  std::vector<uint32_t> localIndices(indices.size());
  for (size_t i = 0; i < indices.size(); ++i)
  {
    auto it         = std::lower_bound(uniqueGlobal.begin(), uniqueGlobal.end(), indices[i]);
    localIndices[i] = static_cast<uint32_t>(it - uniqueGlobal.begin());
  }

  if (targetCount >= localIndices.size())
  {
    outVertices = std::move(localVerts);
    return localIndices;
  }

  // Locks must always be respected; sloppy has no lock support.
  if (hasLockedVertex || containsBoneWeights(localVerts))
    return simplify(vertices, outVertices, indices, locks, targetCount, outError);

  // ── 2. Sloppy simplification (no locked vertices in this set) ─────────────
  float resultErrorRel = 0.f;
  std::vector<uint32_t> simplified(localIndices.size());
  size_t resultSize = meshopt_simplifySloppy(
      simplified.data(),
      localIndices.data(), localIndices.size(),
      &localVerts[0].pos[0], localVerts.size(), sizeof(Vertex),
      targetCount, 1.0f, &resultErrorRel);

  simplified.resize(resultSize);

  // Convert relative error → absolute (world-space units) for consistency
  // with simplify(), so callers can accumulate errors in the same units.
  if (outError)
    *outError = resultErrorRel * meshopt_simplifyScale(
        &localVerts[0].pos[0], localVerts.size(), sizeof(Vertex)) * kSloppyErrorFactor;

  // ── 3. Compact ───────────────────────────────────────────────────────────
  meshopt_optimizeVertexCache(simplified.data(), simplified.data(), simplified.size(), localVerts.size());

  std::vector<uint32_t> oldToNew(localVerts.size(), UINT32_MAX);
  outVertices.clear();
  for (uint32_t idx : simplified)
    if (oldToNew[idx] == UINT32_MAX)
    {
      oldToNew[idx] = static_cast<uint32_t>(outVertices.size());
      outVertices.push_back(std::move(localVerts[idx]));
    }
  for (uint32_t &idx : simplified)
    idx = oldToNew[idx];

  return simplified;
}
