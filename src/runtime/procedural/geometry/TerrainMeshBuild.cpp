#include "procedural/geometry/TerrainMaterial.hpp"
#include "procedural/geometry/TerrainMeshBuild.hpp"
#include <set>
#include "procedural/geometry/TerrainMeshSimplify.hpp"

#include "procedural/ConstrainedDelaunay.hpp"
#include "procedural/Geometry.hpp"
#include "procedural/PoissonDiskSampler.hpp"
#include "procedural/world/MapEdges.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace procedural::geometry
{

namespace
{

constexpr double kWeld = 1.0e-6;

uint64_t mixBits(uint64_t value)
{
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33u;
  return value;
}

uint64_t hashDouble(const double value)
{
  // Hash the bit pattern of a canonicalised value, so a signed zero and a
  // negative zero cannot produce two signatures for one mesh.
  const double canonical = value == 0.0 ? 0.0 : value;
  uint64_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(canonical), "double is not 64 bits");
  std::memcpy(&bits, &canonical, sizeof(bits));
  return mixBits(bits);
}

struct Segment
{
  Point2 a;
  Point2 b;
  ChainKind kind = ChainKind::None;
  double elevationA = 0.0;
  double elevationB = 0.0;
  bool hasElevation = false;
};

Segment segmentSlice(const Segment &source, const Point2 &a, const Point2 &b)
{
  Segment piece = source;
  piece.a = a;
  piece.b = b;
  const double dx = source.b.x - source.a.x, dy = source.b.y - source.a.y;
  const auto elevation = [&](Point2 p)
  {
    const double t = std::clamp(((p.x - source.a.x) * dx + (p.y - source.a.y) * dy) /
        std::max(1e-20, dx * dx + dy * dy), 0.0, 1.0);
    return source.elevationA + (source.elevationB - source.elevationA) * t;
  };
  piece.elevationA = elevation(a);
  piece.elevationB = elevation(b);
  return piece;
}

bool insideRect(
    const Point2 &p, const double minX, const double minZ, const double size)
{
  return p.x >= minX - kWeld && p.x <= minX + size + kWeld &&
      p.y >= minZ - kWeld && p.y <= minZ + size + kWeld;
}

// Liang-Barsky. Returns the clipped span of `a`->`b` inside the rectangle, or
// false when the segment misses it entirely.
bool clipSegment(
    const Point2 &a, const Point2 &b, const double minX, const double minZ,
    const double size, Point2 &outA, Point2 &outB)
{
  const double maxX = minX + size;
  const double maxZ = minZ + size;
  double t0 = 0.0;
  double t1 = 1.0;
  const double dx = b.x - a.x;
  const double dz = b.y - a.y;
  const double p[4] = {-dx, dx, -dz, dz};
  const double q[4] = {a.x - minX, maxX - a.x, a.y - minZ, maxZ - a.y};
  for (int i = 0; i < 4; ++i)
  {
    if (std::abs(p[i]) < 1.0e-12)
    {
      if (q[i] < 0.0)
        return false;
      continue;
    }
    const double r = q[i] / p[i];
    if (p[i] < 0.0)
      t0 = std::max(t0, r);
    else
      t1 = std::min(t1, r);
    if (t0 > t1)
      return false;
  }
  outA = Point2(a.x + t0 * dx, a.y + t0 * dz);
  outB = Point2(a.x + t1 * dx, a.y + t1 * dz);
  return distance(outA, outB) > kWeld;
}

// A uniform grid over a tile, holding whatever is bucketed into it.
//
// Both planar-straight-line-graph passes are all-pairs without one, and on a
// tile carrying a few thousand contour points that is the whole build time -
// measured, a single tile went to twenty-four seconds. Bucketing by bounding
// box turns both into near-linear sweeps.
class UniformGrid
{
public:
  UniformGrid(const double minX, const double minZ, const double size,
      const size_t expected)
      : minX_(minX), minZ_(minZ)
  {
    side_ = static_cast<uint32_t>(std::clamp<int64_t>(
        static_cast<int64_t>(std::sqrt(static_cast<double>(expected)) / 2.0),
        1, 128));
    cell_ = size / static_cast<double>(side_);
    buckets_.resize(static_cast<size_t>(side_) * side_);
  }

  void insert(const Point2 &a, const Point2 &b, const uint32_t id)
  {
    forEachCell(a, b, [&](const size_t index) { buckets_[index].push_back(id); });
  }

  template <typename Visit>
  void query(const Point2 &a, const Point2 &b, const Visit &visit) const
  {
    forEachCell(a, b, [&](const size_t index)
        {
          for (const uint32_t id : buckets_[index])
            visit(id);
        });
  }

private:
  template <typename Apply>
  void forEachCell(const Point2 &a, const Point2 &b, const Apply &apply) const
  {
    const auto clampIndex = [this](const double value)
    {
      return static_cast<int64_t>(std::clamp<double>(
          std::floor(value / cell_), 0.0, static_cast<double>(side_) - 1.0));
    };
    const int64_t x0 = clampIndex(std::min(a.x, b.x) - minX_);
    const int64_t x1 = clampIndex(std::max(a.x, b.x) - minX_);
    const int64_t z0 = clampIndex(std::min(a.y, b.y) - minZ_);
    const int64_t z1 = clampIndex(std::max(a.y, b.y) - minZ_);
    for (int64_t z = z0; z <= z1; ++z)
      for (int64_t x = x0; x <= x1; ++x)
        apply(static_cast<size_t>(z) * side_ + static_cast<size_t>(x));
  }

  double minX_ = 0.0;
  double minZ_ = 0.0;
  double cell_ = 1.0;
  uint32_t side_ = 1u;
  std::vector<std::vector<uint32_t>> buckets_;
};

// Splits every segment at every crossing with every other, so what reaches the
// triangulator is a planar straight-line graph. Long constraints that merely
// cross are exactly the arrangement the triangulator's own header warns can
// never reach a fixed point.
void splitAtIntersections(std::vector<Segment> &segments, const double minX,
    const double minZ, const double size)
{
  const size_t count = segments.size();
  if (count < 2u)
    return;
  UniformGrid grid(minX, minZ, size, count);
  for (uint32_t index = 0u; index < static_cast<uint32_t>(count); ++index)
    grid.insert(segments[index].a, segments[index].b, index);
  std::vector<std::vector<double>> cuts(count);
  for (size_t i = 0u; i < count; ++i)
  {
    std::vector<uint32_t> candidates;
    grid.query(segments[i].a, segments[i].b,
        [&candidates](const uint32_t id) { candidates.push_back(id); });
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end()), candidates.end());
    for (const uint32_t j : candidates)
    {
      if (static_cast<size_t>(j) <= i)
        continue;
      Point2 hit;
      if (!segmentIntersection(
              segments[i].a, segments[i].b, segments[j].a, segments[j].b, hit))
        continue;
      const auto parameterOf = [&hit](const Segment &segment)
      {
        const double dx = segment.b.x - segment.a.x;
        const double dz = segment.b.y - segment.a.y;
        const double lengthSquared = dx * dx + dz * dz;
        if (lengthSquared <= 0.0)
          return 0.0;
        return ((hit.x - segment.a.x) * dx + (hit.y - segment.a.y) * dz) /
            lengthSquared;
      };
      const double ti = parameterOf(segments[i]);
      const double tj = parameterOf(segments[j]);
      if (ti > 1.0e-7 && ti < 1.0 - 1.0e-7)
        cuts[i].push_back(ti);
      if (tj > 1.0e-7 && tj < 1.0 - 1.0e-7)
        cuts[j].push_back(tj);
    }
  }
  std::vector<Segment> split;
  split.reserve(count);
  for (size_t i = 0u; i < count; ++i)
  {
    if (cuts[i].empty())
    {
      split.push_back(segments[i]);
      continue;
    }
    std::sort(cuts[i].begin(), cuts[i].end());
    cuts[i].erase(
        std::unique(cuts[i].begin(), cuts[i].end(),
            [](const double a, const double b) { return std::abs(a - b) < 1.0e-7; }),
        cuts[i].end());
    Point2 previous = segments[i].a;
    for (const double t : cuts[i])
    {
      const Point2 point(
          segments[i].a.x + t * (segments[i].b.x - segments[i].a.x),
          segments[i].a.y + t * (segments[i].b.y - segments[i].a.y));
      if (distance(previous, point) > kWeld)
      {
        Segment piece = segmentSlice(segments[i], previous, point);
        split.push_back(piece);
      }
      previous = point;
    }
    if (distance(previous, segments[i].b) > kWeld)
    {
      Segment piece = segmentSlice(segments[i], previous, segments[i].b);
      split.push_back(piece);
    }
  }
  segments.swap(split);
}

// Welds points to a quantised lattice so two chains meeting at a shared corner
// resolve to one vertex. Quantisation, not a distance search: a distance search
// is order dependent and this has to be identical from either side of a tile
// border.
struct PointSet
{
  explicit PointSet(const double quantum) : quantum_(quantum)
  {
  }

  uint32_t add(const Point2 &point)
  {
    const Key key{
        static_cast<int64_t>(std::llround(point.x / quantum_)),
        static_cast<int64_t>(std::llround(point.y / quantum_))};
    const auto found = index_.find(key);
    if (found != index_.end())
      return found->second;
    const uint32_t id = static_cast<uint32_t>(points.size());
    // Store the lattice position, not the request: every caller that asks for
    // a point in this cell must get the identical coordinate back, or two
    // tiles disagree about a shared border vertex by an ulp.
    points.push_back(Point2(
        static_cast<double>(key.x) * quantum_,
        static_cast<double>(key.y) * quantum_));
    index_.emplace(key, id);
    return id;
  }

  std::vector<Point2> points;

private:
  struct Key
  {
    int64_t x = 0;
    int64_t y = 0;
    bool operator<(const Key &other) const
    {
      return x != other.x ? x < other.x : y < other.y;
    }
  };

  double quantum_ = 1.0;
  std::map<Key, uint32_t> index_;
};

// Distance from a point to a segment. The §4.2 filter needs this for every
// candidate against every constraint, which is why it is kept branch-light.
double distanceToSegment(const Point2 &p, const Point2 &a, const Point2 &b)
{
  const double dx = b.x - a.x;
  const double dz = b.y - a.y;
  const double lengthSquared = dx * dx + dz * dz;
  if (lengthSquared <= 0.0)
    return distance(p, a);
  const double t = std::clamp(
      ((p.x - a.x) * dx + (p.y - a.y) * dz) / lengthSquared, 0.0, 1.0);
  return distance(p, Point2(a.x + t * dx, a.y + t * dz));
}

// Circumcentre of a triangle, or false when it is too near-degenerate to have
// a meaningful one. Delaunay refinement's only move.
bool circumcentre(
    const Point2 &a, const Point2 &b, const Point2 &c, Point2 &centre)
{
  const double dx1 = b.x - a.x;
  const double dz1 = b.y - a.y;
  const double dx2 = c.x - a.x;
  const double dz2 = c.y - a.y;
  const double determinant = 2.0 * (dx1 * dz2 - dz1 * dx2);
  if (std::abs(determinant) < 1.0e-12)
    return false;
  const double length1 = dx1 * dx1 + dz1 * dz1;
  const double length2 = dx2 * dx2 + dz2 * dz2;
  centre = Point2(a.x + (dz2 * length1 - dz1 * length2) / determinant,
      a.y + (dx1 * length2 - dx2 * length1) / determinant);
  return std::isfinite(centre.x) && std::isfinite(centre.y);
}

double triangleAreaTwice(const Point2 &a, const Point2 &b, const Point2 &c)
{
  return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

double smallestAngleDegrees(const Point2 &a, const Point2 &b, const Point2 &c)
{
  const double ab = distance(a, b);
  const double bc = distance(b, c);
  const double ca = distance(c, a);
  if (ab <= 0.0 || bc <= 0.0 || ca <= 0.0)
    return 0.0;
  const auto angle = [](const double opposite, const double p, const double q)
  {
    const double cosine =
        std::clamp((p * p + q * q - opposite * opposite) / (2.0 * p * q), -1.0, 1.0);
    return std::acos(cosine) * 180.0 / 3.14159265358979323846;
  };
  return std::min({angle(bc, ab, ca), angle(ca, ab, bc), angle(ab, bc, ca)});
}

} // namespace

uint64_t TerrainMesh::triangleCount() const
{
  uint64_t total = 0u;
  for (const MeshTile &tile : tiles)
    total += tile.triangleCount();
  return total;
}

uint64_t TerrainMesh::signature() const
{
  uint64_t value = 0x9e3779b97f4a7c15ull;
  const auto mix = [&value](const uint64_t entry)
  { value = mixBits(value ^ (entry + 0x9e3779b97f4a7c15ull + (value << 6u))); };
  for (const MeshTile &tile : tiles)
  {
    mix(tile.tileX);
    mix(tile.tileZ);
    mix(tile.vertices.size());
    mix(tile.indices.size());
    for (const MeshVertex &vertex : tile.vertices)
    {
      mix(hashDouble(vertex.x));
      mix(hashDouble(vertex.y));
      mix(hashDouble(vertex.z));
    }
    for (const uint32_t index : tile.indices)
      mix(index);
    for (const uint8_t materialId : tile.materials)
      mix(materialId);
  }
  return value;
}

std::vector<Chain> gatherWorldChains(
    const world::GeneratedWorld &world, const double weldToleranceMetres)
{
  std::vector<Chain> chains;
  const double extent = world.extentMetres;

  // 1. The world border. Four segments; everything else is clipped inside it.
  Chain border{};
  border.kind = ChainKind::WorldBorder;
  border.closed = true;
  border.points = {Point2(0.0, 0.0), Point2(extent, 0.0), Point2(extent, extent),
      Point2(0.0, extent)};
  chains.push_back(std::move(border));

  // 2. Coastline and lake shores, already traced by the map.
  for (const world::CoastPolyline &line : world.coastline)
  {
    if (line.points.size() < 2u)
      continue;
    Chain chain{};
    chain.kind = ChainKind::Coast;
    chain.points = line.points;
    chain.closed = line.closed;
    chains.push_back(std::move(chain));
  }

  // 3. Elevation contours. These carry their height, which is what lets a
  // vertex on one take an exact elevation instead of resampling the field.
  for (const world::ContourPolyline &line : world.contours.lines)
  {
    if (line.points.size() < 2u)
      continue;
    Chain chain{};
    chain.kind = ChainKind::Contour;
    chain.points = line.points;
    chain.closed = line.closed;
    chain.elevationMetres = line.elevationMetres;
    chain.hasElevation = true;
    chains.push_back(std::move(chain));
  }

  // 3B. Ridge lines and peaks. Low-poly terrain reads as blobby when triangles
  // straddle the crest, because a vertex just off a maximum samples below it
  // and the silhouette dents. An edge exactly on the divide fixes that, and a
  // pinned peak stops a summit being clipped flat by whichever triangle covers
  // it.
  for (const world::RidgeLine &ridge : world.ridges)
  {
    if (ridge.points.size() < 2u)
      continue;
    Chain chain{};
    chain.kind = ChainKind::Ridge;
    chain.points = ridge.points;
    chains.push_back(std::move(chain));
  }
  for (const Point2 &peak : world.peaks)
  {
    // A single point is not a chain, so a peak is a very short one: a degenerate
    // spur the triangulation is obliged to keep a vertex on.
    Chain chain{};
    chain.kind = ChainKind::Ridge;
    chain.points = {Point2(peak.x - 0.5, peak.y), Point2(peak.x + 0.5, peak.y)};
    chains.push_back(std::move(chain));
  }

  // 4. Region, biome, content and landform borders, from the welded edge set
  // the map already builds so both cells either side share one boundary.
  const double weld = weldToleranceMetres > 0.0
      ? weldToleranceMetres
      : std::max(extent * 1.0e-7, 1.0e-4);
  const world::MapEdgeSet edges = world::buildEdgeSet(world.graph, weld);
  for (const world::MapBoundaryEdge &edge : edges.edges)
  {
    if (edge.worldBorder || !edge.classificationBorder)
      continue;
    Chain chain{};
    chain.kind = ChainKind::RegionBorder;
    if (edge.path.size() >= 2u)
      chain.points = edge.path;
    else
      chain.points = {edges.vertices[edge.from].position,
          edges.vertices[edge.to].position};
    chains.push_back(std::move(chain));
  }
  return chains;
}

namespace
{

struct TileBuildContext
{
  const std::vector<Chain> *chains = nullptr;
  std::vector<std::vector<uint32_t>> tileChains;
  uint32_t tilesPerAxis = 0;
  const HeightFunction *height = nullptr;
  const MaterialFunction *material = nullptr;
  double extentMetres = 0.0;
  double chainSpacing = 0.0;
  double borderSpacing = 0.0;
  TerrainMeshSettings settings{};
};

// Height of a point, with the contour ladder applied.
//
// terraceStrength 0 leaves the sampled surface alone; 1 snaps every height down
// to its band floor, which is what makes the shelves flat and the contour a
// riser. The riser itself is geometry the step primitive will own; at this
// stage the band is what makes the facets read.
double terracedHeight(
    const double sampled, const TerrainMeshSettings &settings)
{
  if (settings.terraceStrength <= 0.0 || !(settings.terraceIntervalMetres > 0.0))
    return sampled;
  const double interval = settings.terraceIntervalMetres;
  const double band = std::floor(sampled / interval) * interval;
  return sampled + (band - sampled) * std::clamp(settings.terraceStrength, 0.0, 1.0);
}

bool buildTile(
    const TileBuildContext &context, const uint32_t tileX, const uint32_t tileZ,
    MeshTile &tile, std::string *error)
{
  const double size = context.settings.tileSizeMetres;
  const double minX = static_cast<double>(tileX) * size;
  const double minZ = static_cast<double>(tileZ) * size;
  const double maxX = minX + size;
  const double maxZ = minZ + size;
  tile.tileX = tileX;
  tile.tileZ = tileZ;
  tile.minX = minX;
  tile.minZ = minZ;
  tile.sizeMetres = size;

  const double quantum = std::max(size * 1.0e-9, 1.0e-6);
  PointSet points(quantum);
  std::vector<Segment> segments;

  // --- chain runs clipped into this tile, resampled by arc length ----------
  //
  // Resampling has to work in both directions. The contour extractor emits
  // curve samples at its own density - measured, about 980 points on a single
  // line - and only ever subdividing them leaves the mesh carrying every one.
  // That is where the cost and the slivers both come from: two contour lines a
  // few metres apart, each sampled every few metres, is a ladder of thin
  // triangles. Walking the run by arc length and emitting a point every
  // `chainSpacing` merges as readily as it splits.
  const auto emitRun = [&](const std::vector<Point2> &run,
                           const std::vector<double> &runHeights,
                           const Segment &proto)
  {
    if (run.size() < 2u)
      return;
    if (context.settings.preserveChainVertices)
    {
      for (size_t index = 0; index + 1 < run.size(); ++index)
      {
        Segment original = proto;
        original.a = run[index]; original.b = run[index + 1];
        if (runHeights.size() == run.size())
        {
          original.elevationA = runHeights[index];
          original.elevationB = runHeights[index + 1];
        }
        const double length = distance(original.a, original.b);
        const double maximum = context.settings.maximumConstraintSegmentMetres;
        const uint32_t count = maximum > 0.0 ? std::max(1u, static_cast<uint32_t>(std::ceil(length / maximum))) : 1u;
        for (uint32_t part = 0; part < count; ++part)
        {
          const auto point = [&](double t) { return Point2(original.a.x + (original.b.x - original.a.x) * t,
              original.a.y + (original.b.y - original.a.y) * t); };
          const auto piece = segmentSlice(original, point(double(part) / count), point(double(part + 1) / count));
          if (distance(piece.a, piece.b) > kWeld) segments.push_back(piece);
        }
      }
      return;
    }
    // Elevation is carried through the resampling by the same arc-length
    // parameter as position, so a bed that falls along its course keeps
    // falling after the chain is re-spaced.
    const auto heightAtParameter = [&](const size_t segment, const double t)
    {
      if (runHeights.size() != run.size())
        return proto.elevationA;
      const size_t next = std::min(run.size() - 1u, segment + 1u);
      return runHeights[segment] + (runHeights[next] - runHeights[segment]) * t;
    };
    double total = 0.0;
    for (size_t index = 0u; index + 1u < run.size(); ++index)
      total += distance(run[index], run[index + 1u]);
    if (total <= kWeld)
      return;
    const uint32_t steps = std::max(1u,
        static_cast<uint32_t>(std::llround(total / context.chainSpacing)));
    const double step = total / static_cast<double>(steps);
    Point2 previous = run.front();
    double previousHeight =
        runHeights.size() == run.size() ? runHeights.front() : proto.elevationA;
    double walked = 0.0;
    double target = step;
    size_t segment = 0u;
    double consumed = 0.0;
    for (uint32_t emitted = 1u; emitted < steps; ++emitted)
    {
      while (segment + 1u < run.size())
      {
        const double length = distance(run[segment], run[segment + 1u]);
        if (walked + (length - consumed) >= target || length <= kWeld)
          break;
        walked += length - consumed;
        consumed = 0.0;
        ++segment;
      }
      if (segment + 1u >= run.size())
        break;
      const double length = distance(run[segment], run[segment + 1u]);
      const double into = consumed + (target - walked);
      const double t = length > 0.0 ? std::clamp(into / length, 0.0, 1.0) : 0.0;
      const Point2 point(
          run[segment].x + t * (run[segment + 1u].x - run[segment].x),
          run[segment].y + t * (run[segment + 1u].y - run[segment].y));
      if (distance(previous, point) > kWeld)
      {
        Segment piece = proto;
        piece.a = previous;
        piece.b = point;
        piece.elevationA = previousHeight;
        piece.elevationB = heightAtParameter(segment, t);
        segments.push_back(piece);
        previous = point;
        previousHeight = piece.elevationB;
      }
      target += step;
    }
    if (distance(previous, run.back()) > kWeld)
    {
      Segment piece = proto;
      piece.a = previous;
      piece.b = run.back();
      piece.elevationA = previousHeight;
      piece.elevationB = runHeights.size() == run.size() ? runHeights.back()
                                                         : proto.elevationB;
      segments.push_back(piece);
    }
  };

  const auto &candidates=context.tileChains[size_t(tileZ)*context.tilesPerAxis+tileX];
  const size_t candidateCount=context.settings.indexConstraints?candidates.size():context.chains->size();
  for (size_t candidate=0;candidate<candidateCount;++candidate)
  {
    const Chain &chain=(*context.chains)[context.settings.indexConstraints?candidates[candidate]:candidate];
    if (chain.points.size() < 2u)
      continue;
    Segment proto{};
    proto.kind = chain.kind;
    proto.elevationA = chain.elevationMetres;
    proto.elevationB = chain.elevationMetres;
    proto.hasElevation = chain.hasElevation;
    const size_t spans =
        chain.closed ? chain.points.size() : chain.points.size() - 1u;
    // Consecutive clipped spans that stay inside the tile form one run; a span
    // that leaves it ends the run, so a chain crossing the tile twice
    // contributes two independent runs and never a chord between them.
    std::vector<Point2> run;
    std::vector<double> runHeights;
    for (size_t index = 0u; index < spans; ++index)
    {
      const size_t nextIndex = (index + 1u) % chain.points.size();
      const Point2 &a = chain.points[index];
      const Point2 &b = chain.points[nextIndex];
      Point2 clippedA;
      Point2 clippedB;
      if (!clipSegment(a, b, minX, minZ, size, clippedA, clippedB))
      {
        emitRun(run, runHeights, proto);
        run.clear();
        runHeights.clear();
        continue;
      }
      // Interpolate the chain's own heights to the clipped endpoints, so a
      // chain entering a tile part way along a span keeps the right elevation.
      const double span = distance(a, b);
      const auto heightAt = [&](const Point2 &point)
      {
        if (chain.elevations.size() != chain.points.size())
          return chain.elevationMetres;
        const double t = span > 0.0 ? std::clamp(distance(a, point) / span, 0.0, 1.0) : 0.0;
        return chain.elevationAt(index) +
            (chain.elevationAt(nextIndex) - chain.elevationAt(index)) * t;
      };
      if (run.empty() || distance(run.back(), clippedA) > kWeld)
      {
        emitRun(run, runHeights, proto);
        run.clear();
        runHeights.clear();
        run.push_back(clippedA);
        runHeights.push_back(heightAt(clippedA));
      }
      run.push_back(clippedB);
      runHeights.push_back(heightAt(clippedB));
    }
    emitRun(run, runHeights, proto);
  }

  // --- the border contract -------------------------------------------------
  // Border vertices are a pure function of position: a regular sampling whose
  // phase comes from the world origin, plus every place a chain crosses. Both
  // tiles either side compute the same set from the same inputs, so the seam
  // closes without either knowing the other exists.
  std::array<std::vector<double>, 4u> borderCuts;
  const auto addBorderCut = [&](const uint32_t side, const double coordinate)
  {
    const double lo = side < 2u ? minX : minZ;
    const double hi = side < 2u ? maxX : maxZ;
    if (coordinate > lo + kWeld && coordinate < hi - kWeld)
      borderCuts[side].push_back(coordinate);
  };
  for (const Segment &segment : segments)
  {
    const auto consider = [&](const Point2 &p)
    {
      if (std::abs(p.y - minZ) <= kWeld)
        addBorderCut(0u, p.x);
      if (std::abs(p.y - maxZ) <= kWeld)
        addBorderCut(1u, p.x);
      if (std::abs(p.x - minX) <= kWeld)
        addBorderCut(2u, p.y);
      if (std::abs(p.x - maxX) <= kWeld)
        addBorderCut(3u, p.y);
    };
    consider(segment.a);
    consider(segment.b);
  }
  for (uint32_t side = 0u; side < 4u; ++side)
  {
    const double lo = side < 2u ? minX : minZ;
    const double hi = side < 2u ? maxX : maxZ;
    // Phase from the world origin, not from the tile, so the samples land in
    // the same places for both owners of the edge.
    const double spacing = context.borderSpacing;
    const double first = std::ceil(lo / spacing) * spacing;
    for (double at = first; at < hi - kWeld; at += spacing)
      addBorderCut(side, at);
    borderCuts[side].push_back(lo);
    borderCuts[side].push_back(hi);
    std::sort(borderCuts[side].begin(), borderCuts[side].end());
    borderCuts[side].erase(
        std::unique(borderCuts[side].begin(), borderCuts[side].end(),
            [](const double a, const double b)
            { return std::abs(a - b) <= kWeld; }),
        borderCuts[side].end());
    for (size_t index = 0u; index + 1u < borderCuts[side].size(); ++index)
    {
      const double from = borderCuts[side][index];
      const double to = borderCuts[side][index + 1u];
      Segment piece{};
      piece.kind = ChainKind::WorldBorder;
      if (side == 0u)
      {
        piece.a = Point2(from, minZ);
        piece.b = Point2(to, minZ);
      }
      else if (side == 1u)
      {
        piece.a = Point2(from, maxZ);
        piece.b = Point2(to, maxZ);
      }
      else if (side == 2u)
      {
        piece.a = Point2(minX, from);
        piece.b = Point2(minX, to);
      }
      else
      {
        piece.a = Point2(maxX, from);
        piece.b = Point2(maxX, to);
      }
      segments.push_back(piece);
    }
  }

  splitAtIntersections(segments, minX, minZ, size);

  // Weld every endpoint first: from here on a segment is a pair of lattice
  // indices, and two chains that traced the same boundary from opposite sides
  // become the same pair rather than two segments a nanometre apart.
  std::vector<std::pair<uint32_t, uint32_t>> welded;
  welded.reserve(segments.size());
  for (const Segment &segment : segments)
  {
    if (!insideRect(segment.a, minX, minZ, size) ||
        !insideRect(segment.b, minX, minZ, size))
    {
      welded.emplace_back(0u, 0u);
      continue;
    }
    welded.emplace_back(points.add(segment.a), points.add(segment.b));
  }

  // Split each segment at any welded point lying in its interior. Without this
  // a chain vertex that lands mid-way along a tile border leaves two
  // constraints overlapping along part of their length, which is precisely the
  // arrangement the triangulator's header says can never reach a fixed point.
  // Bucket the welded points once, then each segment only tests the points in
  // the cells its bounding box touches.
  UniformGrid pointGrid(minX, minZ, size, points.points.size());
  for (uint32_t index = 0u; index < static_cast<uint32_t>(points.points.size());
       ++index)
    pointGrid.insert(points.points[index], points.points[index], index);

  std::vector<Segment> refined;
  std::vector<std::pair<uint32_t, uint32_t>> refinedWelded;
  refined.reserve(segments.size());
  for (size_t index = 0u; index < segments.size(); ++index)
  {
    const Segment &segment = segments[index];
    if (welded[index].first == welded[index].second)
      continue;
    const Point2 a = points.points[welded[index].first];
    const Point2 b = points.points[welded[index].second];
    const double dx = b.x - a.x;
    const double dz = b.y - a.y;
    const double lengthSquared = dx * dx + dz * dz;
    if (lengthSquared <= 0.0)
      continue;
    std::vector<std::pair<double, uint32_t>> interior;
    std::vector<uint32_t> nearby;
    pointGrid.query(a, b, [&nearby](const uint32_t id) { nearby.push_back(id); });
    std::sort(nearby.begin(), nearby.end());
    nearby.erase(std::unique(nearby.begin(), nearby.end()), nearby.end());
    for (const uint32_t candidate : nearby)
    {
      if (candidate == welded[index].first || candidate == welded[index].second)
        continue;
      const Point2 &p = points.points[candidate];
      const double t = ((p.x - a.x) * dx + (p.y - a.y) * dz) / lengthSquared;
      if (t <= 1.0e-9 || t >= 1.0 - 1.0e-9)
        continue;
      const Point2 projected(a.x + t * dx, a.y + t * dz);
      if (distance(projected, p) > kWeld * 4.0)
        continue;
      interior.emplace_back(t, candidate);
    }
    std::sort(interior.begin(), interior.end());
    uint32_t previous = welded[index].first;
    for (const auto &entry : interior)
    {
      if (entry.second == previous)
        continue;
      Segment piece = segmentSlice(segment, points.points[previous], points.points[entry.second]);
      refined.push_back(piece);
      refinedWelded.emplace_back(previous, entry.second);
      previous = entry.second;
    }
    if (previous != welded[index].second)
    {
      Segment piece = segmentSlice(segment, points.points[previous], points.points[welded[index].second]);
      refined.push_back(piece);
      refinedWelded.emplace_back(previous, welded[index].second);
    }
  }
  segments.swap(refined);
  welded.swap(refinedWelded);

  // Collapse duplicates. A segment is identified by its unordered index pair,
  // and a boundary traced by both a chain and the tile border is one
  // constraint carrying the union of what the two meant.
  std::map<std::pair<uint32_t, uint32_t>, size_t> unique;
  std::vector<Constraint> constraints;
  std::vector<ChainKind> constraintKinds;
  std::vector<double> constraintElevation;
  std::vector<double> constraintElevationB;
  std::vector<uint8_t> constraintHasElevation;
  constraints.reserve(segments.size());
  for (size_t index = 0u; index < segments.size(); ++index)
  {
    const uint32_t a = welded[index].first;
    const uint32_t b = welded[index].second;
    if (a == b)
      continue;
    const std::pair<uint32_t, uint32_t> key{std::min(a, b), std::max(a, b)};
    const auto found = unique.find(key);
    if (found != unique.end())
    {
      constraintKinds[found->second] =
          constraintKinds[found->second] | segments[index].kind;
      if (segments[index].hasElevation &&
          constraintHasElevation[found->second] == 0u)
      {
        constraintHasElevation[found->second] = 1u;
        constraintElevation[found->second] = segments[index].elevationA;
        constraintElevationB[found->second] = segments[index].elevationB;
      }
      continue;
    }
    unique.emplace(key, constraints.size());
    constraints.emplace_back(static_cast<int64_t>(a), static_cast<int64_t>(b));
    constraintKinds.push_back(segments[index].kind);
    constraintElevation.push_back(segments[index].elevationA);
    constraintElevationB.push_back(segments[index].elevationB);
    constraintHasElevation.push_back(segments[index].hasElevation ? 1u : 0u);
  }

  const uint32_t poissonLayers = context.settings.poissonSpacingMetres > 0.0
      ? context.settings.poissonLayers : 0u;
  std::vector<Point2> acceptedPoisson;
  const auto addPoissonLayer = [&](uint32_t layer)
  {
    PoissonDiskSampler sampler;
    PoissonDiskSampler::Settings sampling{};
    sampling.radius = context.settings.poissonSpacingMetres / std::pow(2.0, layer);
    sampling.width = size;
    sampling.height = size;
    sampling.method = PoissonDiskSampler::Method::RandomThrows;
    sampling.seed = mixBits(context.settings.poissonSeed ^
        (static_cast<uint64_t>(tileX) << 32u) ^ tileZ ^ mixBits(layer));
    const double clearance = std::max(quantum * 2.0,
        std::min(sampling.radius * 0.15, context.settings.minimumEdgeMetres));
    UniformGrid constraintGrid(minX, minZ, size, constraints.size());
    for (uint32_t index = 0; index < constraints.size(); ++index)
      constraintGrid.insert(points.points[constraints[index].origin],
          points.points[constraints[index].end], index);
    UniformGrid sampleGrid(minX, minZ, size, 1024);
    for (uint32_t index = 0; index < acceptedPoisson.size(); ++index)
      sampleGrid.insert(acceptedPoisson[index], acceptedPoisson[index], index);
    for (const Point2 &local : sampler.generate(sampling))
    {
      Point2 point(minX + local.x, minZ + local.y);
      if (context.settings.poissonPlacement)
        point = context.settings.poissonPlacement(point, sampling.radius);
      if (point.x < minX + clearance || point.y < minZ + clearance ||
          point.x > maxX - clearance || point.y > maxZ - clearance)
        continue;
      if (layer > 0 && (!context.settings.toleranceScale ||
          context.settings.toleranceScale(point) > (layer == 1 ? 1.0 : 0.6)))
        continue;
      // Borders retain their canonical samples, independent of tile RNG.
      if (local.x < clearance || local.y < clearance ||
          local.x > size - clearance || local.y > size - clearance)
        continue;
      bool near = false;
      constraintGrid.query(Point2(point.x - clearance, point.y - clearance),
          Point2(point.x + clearance, point.y + clearance), [&](uint32_t index)
          {
            const Point2 &a = points.points[constraints[index].origin];
            const Point2 &b = points.points[constraints[index].end];
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double t = std::clamp(((point.x - a.x) * dx +
                (point.y - a.y) * dy) / std::max(1e-20, dx * dx + dy * dy), 0.0, 1.0);
            near |= distance(point, Point2(a.x + t * dx, a.y + t * dy)) < clearance;
          });
      sampleGrid.query(Point2(point.x - sampling.radius, point.y - sampling.radius),
          Point2(point.x + sampling.radius, point.y + sampling.radius), [&](uint32_t index)
          { near |= distance(point, acceptedPoisson[index]) < sampling.radius; });
      if (!near)
      {
        points.add(point);
        sampleGrid.insert(point, point, static_cast<uint32_t>(acceptedPoisson.size()));
        acceptedPoisson.push_back(point);
      }
    }
  };

  // Elevation carried by a constraint vertex. A point on a contour is on the
  // isoline by definition, so it takes the contour's height exactly rather
  // than resampling a field that may disagree by a metre.
  std::vector<double> pinnedElevation(points.points.size(), 0.0);
  std::vector<uint8_t> pinned(points.points.size(), 0u);
  for (size_t index = 0u; index < constraints.size(); ++index)
  {
    if (constraintHasElevation[index] == 0u)
      continue;
    // A pinned vertex takes the *lowest* elevation claimed for it. Where a
    // carved bed meets untouched ground the channel has to win, or the
    // triangulation runs the surface across the trench.
    const auto claim = [&](const int64_t vertex, const double height)
    {
      const auto at = static_cast<size_t>(vertex);
      if (pinned[at] == 0u || height < pinnedElevation[at])
        pinnedElevation[at] = height;
      pinned[at] = 1u;
    };
    claim(constraints[index].origin, constraintElevation[index]);
    claim(constraints[index].end, constraintElevationB[index]);
  }

  // --- triangulate, then refine on measured error --------------------------
  ConstrainedDelaunayTriangulator triangulator;
  const auto triangulateLocal = [&](const ConstrainedDelaunayTriangulator::Settings &settings)
  {
    // Predicates operate on nearby coordinates. Subtracting the tile origin
    // avoids losing precision around tightly intersecting river/contour edges
    // far from the world origin; indices and world-space output stay identical.
    std::vector<Point2> local;
    local.reserve(points.points.size());
    for (const auto &point : points.points) local.emplace_back(point.x - minX, point.y - minZ);
    return triangulator.triangulate(local, constraints, settings);
  };
  ConstrainedDelaunayTriangulator::Settings cdt{};
  // Zero means "until it converges", which is safe only because the
  // triangulator detects its own stalls - and it is the right setting here: a
  // refined tile carries thousands of points, insertion legitimately takes many
  // passes, and a fixed cap turns a slow tile into a failed one.
  cdt.maximumIterations = 0u;
  cdt.ensureDelaunay = true;
  bool relaxed = false;
  uint32_t dropped = 0u;
  uint32_t degradedRounds = 0u;
  uint32_t splitConstraints = 0u;

  std::vector<int64_t> triangles;
  uint32_t round = 0u;
  const uint32_t totalRounds = poissonLayers + context.settings.maximumRefinementRounds;
  for (; round <= totalRounds; ++round)
  {
    // Restoring the empty-circumcircle property after each round of constraint
    // insertion can undo the marking that insertion depends on, and the
    // triangulator's own header says certain arrangements then never reach a
    // fixed point. Measured on real map data: one tile in a few hundred stalls
    // with a single constraint left out, and the nearest point to it is 2.4 m
    // from a 163 m segment - not a degeneracy, just the livelock. Relaxing the
    // Delaunay restoration inserts every constraint and converges. Quality is
    // the thing traded away, so the fallback is counted and gated on, never
    // silent.
    bool triangulated = false;
    for (uint32_t attempt = 0u; attempt < 6u && !triangulated; ++attempt)
    {
      cdt.ensureDelaunay = true;
      triangulated = triangulateLocal(cdt);
      if (!triangulated)
      {
        cdt.ensureDelaunay = false;
        triangulated = triangulateLocal(cdt);
        relaxed = relaxed || triangulated;
      }
      if (triangulated)
        break;

      // Whatever is left out is encroached: measured on real map data, every
      // residual failure is a single constraint with a point a couple of
      // metres from a 150 m segment - well inside its diametral circle. That
      // is Ruppert's case and Ruppert's answer, splitting the segment at its
      // midpoint, is what makes it insertable. Splitting halves the diametral
      // circle each time, so this terminates.
      std::vector<Constraint> split;
      std::vector<ChainKind> splitKinds;
      std::vector<double> splitElevation;
      std::vector<double> splitElevationB;
      std::vector<uint8_t> splitHasElevation;
      split.reserve(constraints.size() + 8u);
      bool anySplit = false;
      for (uint64_t index = 0u; index < constraints.size(); ++index)
      {
        const auto push = [&](const int64_t from, const int64_t to)
        {
          split.emplace_back(from, to);
          splitKinds.push_back(constraintKinds[index]);
          splitElevation.push_back(constraintElevation[index]);
          splitElevationB.push_back(constraintElevationB[index]);
          splitHasElevation.push_back(constraintHasElevation[index]);
        };
        if (triangulator.constraintIsIncluded(index))
        {
          push(constraints[index].origin, constraints[index].end);
          continue;
        }
        const Point2 &a = points.points[static_cast<size_t>(constraints[index].origin)];
        const Point2 &b = points.points[static_cast<size_t>(constraints[index].end)];
        if (std::hypot(b.x - a.x, b.y - a.y) < quantum * 4.0)
        { push(constraints[index].origin, constraints[index].end); continue; }
        const Point2 middle((a.x + b.x) * 0.5, (a.y + b.y) * 0.5);
        const uint32_t inserted = points.add(middle);
        if (inserted == static_cast<uint32_t>(constraints[index].origin) ||
            inserted == static_cast<uint32_t>(constraints[index].end))
        {
          push(constraints[index].origin, constraints[index].end);
          continue;
        }
        pinnedElevation.resize(points.points.size(), 0.0);
        pinned.resize(points.points.size(), 0u);
        if (constraintHasElevation[index] != 0u)
        {
          const double middleHeight = 0.5 *
              (constraintElevation[index] + constraintElevationB[index]);
          if (pinned[inserted] == 0u || middleHeight < pinnedElevation[inserted])
            pinnedElevation[inserted] = middleHeight;
          pinned[inserted] = 1u;
        }
        const double middleHeight = 0.5 * (constraintElevation[index] + constraintElevationB[index]);
        push(constraints[index].origin, static_cast<int64_t>(inserted));
        splitElevationB.back() = middleHeight;
        push(static_cast<int64_t>(inserted), constraints[index].end);
        splitElevation.back() = middleHeight;
        anySplit = true;
      }
      if (!anySplit)
        break;
      constraints.swap(split);
      constraintKinds.swap(splitKinds);
      constraintElevation.swap(splitElevation);
      constraintElevationB.swap(splitElevationB);
      constraintHasElevation.swap(splitHasElevation);
      ++splitConstraints;
    }
    // Last resort: build the tile without the constraints that will not go in.
    // A handful of unsatisfiable segments in one tile is a bounded, measurable
    // loss of fidelity; failing here loses the entire world. The count is
    // reported so the degradation cannot pass unnoticed.
    if (!triangulated)
    {
      std::vector<Constraint> viable;
      std::vector<ChainKind> viableKinds;
      std::vector<double> viableElevation, viableElevationB;
      std::vector<uint8_t> viableHas;
      for (size_t index = 0u; index < constraints.size(); ++index)
      {
        if (!triangulator.constraintIsIncluded(index)) { ++dropped; continue; }
        viable.push_back(constraints[index]);
        viableKinds.push_back(constraintKinds[index]);
        viableElevation.push_back(constraintElevation[index]);
        viableElevationB.push_back(constraintElevationB[index]);
        viableHas.push_back(constraintHasElevation[index]);
      }
      if (dropped > 0u && dropped < constraints.size())
      {
        constraints.swap(viable); constraintKinds.swap(viableKinds);
        constraintElevation.swap(viableElevation); constraintElevationB.swap(viableElevationB);
        constraintHasElevation.swap(viableHas);
        cdt.ensureDelaunay = false;
        triangulated = triangulateLocal(cdt);
        relaxed = relaxed || triangulated;
      }
      if (!triangulated) dropped = 0u;
    }
    // A stall with every constraint inserted is not a constraint conflict, so
    // splitting has nothing to work on and the attempts above cannot help. The
    // previous refinement round did triangulate, and its triangles are still
    // here: points are only ever appended, so those indices stay valid. Falling
    // back to it costs this one tile some refinement and keeps the world.
    if (!triangulated && round > 0u && !triangles.empty())
    {
      degradedRounds = totalRounds - round + 1u;
      break;
    }
    if (!triangulated)
    {
      if (std::getenv("DUMP_CDT") != nullptr)
      {
        char name[1024];
        std::snprintf(name, sizeof(name), "%s.%u_%u.txt",
            std::getenv("DUMP_CDT"), tileX, tileZ);
        std::FILE *f = std::fopen(name, "w");
        if (f != nullptr)
        {
          std::fprintf(f, "%zu %zu\n", points.points.size(), constraints.size());
          for (const Point2 &p : points.points)
            std::fprintf(f, "p %.17g %.17g\n", p.x, p.y);
          for (const Constraint &c : constraints)
            std::fprintf(f, "c %lld %lld\n", (long long)c.origin, (long long)c.end);
          std::fclose(f);
        }
      }
      if (error != nullptr)
      {
        std::ostringstream message;
        size_t missing = 0u;
        for (size_t index = 0u; index < constraints.size(); ++index)
          if (!triangulator.constraintIsIncluded(index)) ++missing;
        message << "tile " << tileX << "," << tileZ << " failed to triangulate "
                << points.points.size() << " points and " << constraints.size()
                << " constraints (" << missing << " not inserted, "
                << splitConstraints << " split rounds)";
        *error = message.str();
      }
      return false;
    }
    triangles = triangulator.collectTriangles();
    // collectTriangles() returns every live triangle, including the ones that
    // touch the three super-triangle vertices. Those occupy slots past the
    // input points, so indexing our own array with them reads out of bounds -
    // which shows up as non-determinism and as degenerate faces rather than as
    // a crash. Drop them here, once, rather than guarding every consumer.
    {
      const auto inputCount = static_cast<int64_t>(points.points.size());
      std::vector<int64_t> inside;
      inside.reserve(triangles.size());
      for (size_t index = 0u; index + 2u < triangles.size(); index += 3u)
      {
        if (triangles[index] >= inputCount || triangles[index + 1u] >= inputCount ||
            triangles[index + 2u] >= inputCount || triangles[index] < 0 ||
            triangles[index + 1u] < 0 || triangles[index + 2u] < 0)
          continue;
        inside.push_back(triangles[index]);
        inside.push_back(triangles[index + 1u]);
        inside.push_back(triangles[index + 2u]);
      }
      triangles.swap(inside);
    }
    // Establish the constraint-only CDT first. Each Poisson layer is then
    // inserted and retriangulated before the next layer or error refinement.
    if (round < poissonLayers)
    {
      addPoissonLayer(round);
      pinnedElevation.resize(points.points.size(), 0.0);
      pinned.resize(points.points.size(), 0u);
      continue;
    }
    if (round == totalRounds)
      break;

    // Insert where the flat triangle departs from the surface by more than the
    // tolerance. Flat ground produces nothing here, which is the entire reason
    // a plain costs almost no triangles.
    std::vector<Point2> insertions;
    // A vertex belongs to about six triangles, and each of them resamples the
    // surface at its three corners. Neither `pinned` nor the point set moves
    // between here and the end of the scan, so one query a vertex a round
    // answers all six identically.
    std::vector<double> cornerHeight(points.points.size());
    std::vector<uint8_t> cornerResolved(points.points.size(), 0u);
    // The proximity test below asks "is any constraint within `proximityLimit`
    // of this candidate", once per candidate, and answered it by walking every
    // constraint in the tile - a thousand of them on constrained ground. Two
    // boxes that come within the limit always share a grid cell, so bucketing
    // by bounding box returns a superset of the segments a linear scan would
    // have tested and the answer is the same one.
    UniformGrid candidateConstraints(minX, minZ, size, constraints.size());
    for (uint32_t entry = 0u; entry < static_cast<uint32_t>(constraints.size()); ++entry)
      candidateConstraints.insert(points.points[static_cast<size_t>(constraints[entry].origin)],
          points.points[static_cast<size_t>(constraints[entry].end)], entry);
    for (size_t index = 0u; index + 2u < triangles.size(); index += 3u)
    {
      const Point2 a = points.points[static_cast<size_t>(triangles[index])];
      const Point2 b = points.points[static_cast<size_t>(triangles[index + 1u])];
      const Point2 c = points.points[static_cast<size_t>(triangles[index + 2u])];
      const double longest = std::max({distance(a, b), distance(b, c), distance(c, a)});
      if (longest <= context.settings.minimumEdgeMetres * 2.0)
        continue;
      // Both thresholds scale with the triangle rather than with a global
      // spacing: a band measured in metres is meaningless against a mesh whose
      // triangles span three orders of magnitude, and set from the chain
      // spacing it swallows the whole interior of a tile.
      const double slivernessLimit = longest * 0.02;
      const double proximityLimit =
          longest * std::clamp(context.settings.snapFraction, 0.0, 0.9) * 0.1;
      // A sliver has no meaningful centroid: it lies on the sliver's own long
      // edge, which is a constraint whenever the sliver came from one.
      if (std::abs(triangleAreaTwice(a, b, c)) / longest < slivernessLimit)
        continue;
      const Point2 centroid(
          (a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0);
      if (!insideRect(centroid, minX, minZ, size))
        continue;
      const auto heightAt = [&](const size_t vertex) -> double
      {
        const auto at = static_cast<size_t>(triangles[index + vertex]);
        if (pinned[at] != 0u)
          return pinnedElevation[at];
        if (cornerResolved[at] == 0u)
        {
          cornerHeight[at] = (*context.height)(points.points[at]);
          cornerResolved[at] = 1u;
        }
        return cornerHeight[at];
      };
      // A narrow ridge can cross a triangle while its centroid happens to sit
      // in a valley. Probe toward all three corners as well and insert at the
      // largest error, rather than repeatedly subdividing only the centre.
      const std::array<double, 3> heights{heightAt(0u), heightAt(1u), heightAt(2u)};
      const std::array<std::array<double, 3>, 7> probes{{
          {1.0 / 3, 1.0 / 3, 1.0 / 3}, {0.6, 0.2, 0.2}, {0.2, 0.6, 0.2}, {0.2, 0.2, 0.6},
          {.48,.48,.04},{.48,.04,.48},{.04,.48,.48}}};
      Point2 candidate = centroid;
      double largestExcess = 0.0;
      for (const auto &weights : probes)
      {
        const Point2 probe(a.x * weights[0] + b.x * weights[1] + c.x * weights[2],
            a.y * weights[0] + b.y * weights[1] + c.y * weights[2]);
        const double interpolated = heights[0] * weights[0] + heights[1] * weights[1] + heights[2] * weights[2];
        const double tolerance = context.settings.verticalToleranceMetres * (context.settings.toleranceScale
            ? std::max(0.05, context.settings.toleranceScale(probe)) : 1.0);
        const double excess = std::abs((*context.height)(probe) - interpolated) - tolerance;
        if (excess > largestExcess) { largestExcess = excess; candidate = probe; }
      }
      // Area is the second criterion. Vertical error alone cannot ask for
      // geometry on flat ground, and flat ground is exactly where a river's
      // curve, a shoreline and a road's edge live.
      if (largestExcess <= 0.0)
      {
        if (!context.settings.maximumTriangleArea) continue;
        const double cap = context.settings.maximumTriangleArea(centroid);
        if (!(cap > 0)) continue;
        const double area = std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * .5;
        if (area <= cap) continue;
        candidate = centroid;
      }
      // A point that sits on, or beside, a constraint is worse than no point.
      // It buys nothing - the constraint already puts an edge there - and it
      // splits the constraint so the triangulator can never insert it, which
      // stalls the run. A collinear sliver's centroid lands exactly on the
      // chain it came from, so this is not a rare case.
      bool tooCloseToChain = false;
      candidateConstraints.query(
          Point2(candidate.x - proximityLimit, candidate.y - proximityLimit),
          Point2(candidate.x + proximityLimit, candidate.y + proximityLimit),
          [&](const uint32_t entry)
          {
            if (tooCloseToChain)
              return;
            const Point2 &ca = points.points[static_cast<size_t>(constraints[entry].origin)];
            const Point2 &cb = points.points[static_cast<size_t>(constraints[entry].end)];
            if (distanceToSegment(candidate, ca, cb) < proximityLimit)
              tooCloseToChain = true;
          });
      if (!tooCloseToChain)
        insertions.push_back(candidate);
    }
    if (insertions.empty())
      break;
    // Deterministic order: the point set quantises and the insertion list is
    // sorted, so the same tile produces the same points whatever order the
    // triangles came out in.
    sortPoints(insertions);
    const size_t before = points.points.size();
    for (const Point2 &point : insertions)
      points.add(point);
    pinnedElevation.resize(points.points.size(), 0.0);
    pinned.resize(points.points.size(), 0u);
    if (points.points.size() == before)
      break;
  }

  // --- quality: break up the thin triangles the error criterion leaves ------
  //
  // The error criterion measures height, not shape, so two constraint chains
  // running close together - two contour lines on gentle ground - leave a
  // ladder of long thin triangles it is perfectly happy with. Delaunay
  // refinement's answer is to insert the circumcentre of an offending
  // triangle, which is what actually improves the angle rather than merely
  // subdividing it.
  if (context.settings.minimumAngleDegrees > 0.0)
  {
    for (uint32_t qualityRound = 0u;
         qualityRound < context.settings.maximumQualityRounds; ++qualityRound)
    {
      std::vector<Point2> insertions;
      for (size_t index = 0u; index + 2u < triangles.size(); index += 3u)
      {
        const Point2 a = points.points[static_cast<size_t>(triangles[index])];
        const Point2 b = points.points[static_cast<size_t>(triangles[index + 1u])];
        const Point2 c = points.points[static_cast<size_t>(triangles[index + 2u])];
        const double shortest =
            std::min({distance(a, b), distance(b, c), distance(c, a)});
        // Never refine below the floor: a genuinely small feature is allowed
        // to be a thin triangle, and chasing it is how refinement runs away.
        if (shortest <= context.settings.minimumEdgeMetres)
          continue;
        if (smallestAngleDegrees(a, b, c) >= context.settings.minimumAngleDegrees)
          continue;
        Point2 centre;
        if (!circumcentre(a, b, c, centre))
          continue;
        if (!insideRect(centre, minX, minZ, size))
          continue;
        // A circumcentre that lands on a constraint would split it and make it
        // uninsertable, the same trap as the error candidates.
        const double band = shortest * 0.25;
        bool onChain = false;
        for (const Constraint &constraint : constraints)
        {
          const Point2 &ca = points.points[static_cast<size_t>(constraint.origin)];
          const Point2 &cb = points.points[static_cast<size_t>(constraint.end)];
          if (distanceToSegment(centre, ca, cb) < band)
          {
            onChain = true;
            break;
          }
        }
        if (!onChain)
          insertions.push_back(centre);
      }
      if (insertions.empty())
        break;
      sortPoints(insertions);
      const size_t before = points.points.size();
      for (const Point2 &point : insertions)
        points.add(point);
      pinnedElevation.resize(points.points.size(), 0.0);
      pinned.resize(points.points.size(), 0u);
      if (points.points.size() == before)
        break;
      cdt.ensureDelaunay = true;
      if (!triangulateLocal(cdt))
      {
        cdt.ensureDelaunay = false;
        if (!triangulateLocal(cdt))
          break;
        relaxed = true;
      }
      triangles = triangulator.collectTriangles();
      const auto inputCount = static_cast<int64_t>(points.points.size());
      std::vector<int64_t> inside;
      inside.reserve(triangles.size());
      for (size_t index = 0u; index + 2u < triangles.size(); index += 3u)
      {
        if (triangles[index] >= inputCount || triangles[index + 1u] >= inputCount ||
            triangles[index + 2u] >= inputCount || triangles[index] < 0 ||
            triangles[index + 1u] < 0 || triangles[index + 2u] < 0)
          continue;
        inside.push_back(triangles[index]);
        inside.push_back(triangles[index + 1u]);
        inside.push_back(triangles[index + 2u]);
      }
      triangles.swap(inside);
      ++tile.qualityRounds;
    }
  }

  tile.constraintCount = constraints.size();
  tile.constraintsIncluded = 0u;
  for (uint64_t index = 0u; index < constraints.size(); ++index)
    if (triangulator.constraintIsIncluded(index))
      ++tile.constraintsIncluded;
  tile.relaxedDelaunay = relaxed;
  tile.droppedConstraints = dropped + degradedRounds;
  tile.encroachmentSplits = splitConstraints;

  // --- emit ----------------------------------------------------------------
  tile.vertices.resize(points.points.size());
  for (size_t index = 0u; index < points.points.size(); ++index)
  {
    const Point2 &point = points.points[index];
    const double sampled = pinned[index] != 0u
        ? pinnedElevation[index]
        : terracedHeight((*context.height)(point), context.settings);
    tile.vertices[index] = MeshVertex{point.x, sampled, point.y};
  }
  tile.indices.clear();
  tile.materials.clear();
  tile.indices.reserve(triangles.size());
  tile.materials.reserve(triangles.size() / 3u);
  for (size_t index = 0u; index + 2u < triangles.size(); index += 3u)
  {
    const auto ia = static_cast<uint32_t>(triangles[index]);
    const auto ib = static_cast<uint32_t>(triangles[index + 1u]);
    const auto ic = static_cast<uint32_t>(triangles[index + 2u]);
    const Point2 &a = points.points[ia];
    const Point2 &b = points.points[ib];
    const Point2 &c = points.points[ic];
    const Point2 centroid((a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0);
    if (!insideRect(centroid, minX, minZ, size))
      continue;
    // A zero-area triangle has no orientation, so its winding is undefined and
    // every downstream sign test on it is meaningless. Drop it rather than
    // emit a face whose normal is a coin toss.
    const double twiceArea = triangleAreaTwice(a, b, c);
    const double longestEdge =
        std::max({distance(a, b), distance(b, c), distance(c, a)});
    if (longestEdge <= 0.0 || std::abs(twiceArea) / longestEdge < longestEdge * 1.0e-6)
      continue;
    // Counter-clockwise seen from above, so a front face is up.
    if (twiceArea < 0.0)
    {
      tile.indices.push_back(ia);
      tile.indices.push_back(ic);
      tile.indices.push_back(ib);
    }
    else
    {
      tile.indices.push_back(ia);
      tile.indices.push_back(ib);
      tile.indices.push_back(ic);
    }
    tile.materials.push_back((*context.material)(centroid));
  }

  for (uint32_t side = 0u; side < 4u; ++side)
  {
    tile.borders[side].clear();
    for (const double at : borderCuts[side])
    {
      if (side == 0u)
        tile.borders[side].push_back(Point2(at, minZ));
      else if (side == 1u)
        tile.borders[side].push_back(Point2(at, maxZ));
      else if (side == 2u)
        tile.borders[side].push_back(Point2(minX, at));
      else
        tile.borders[side].push_back(Point2(maxX, at));
    }
  }

  if(context.settings.simplificationRounds)
  {
    std::vector<uint8_t> locked=pinned;
    for(const auto &edge:constraints){locked[edge.origin]=1;locked[edge.end]=1;}
    simplifyTerrainTile(tile,std::move(locked),context.settings,*context.height,*context.material);
  }
  tile.vertices.shrink_to_fit();
  return true;
}

// Uniform red refinement preserves every source edge and gives both sides of
// a tile boundary identical dyadic samples. This is surface amplification after
// CDT, not a claim that the amplified triangles have been Delaunay-restored.
void subdivideTile(MeshTile &tile, const HeightFunction &height,
    const MaterialFunction &material, uint32_t rounds)
{
  for(uint32_t round=0;round<rounds;++round)
  {
    std::unordered_map<uint64_t,uint32_t> mids;
    std::vector<uint32_t> triangles; triangles.reserve(tile.indices.size()*4);
    std::vector<uint8_t> materials; materials.reserve(tile.materials.size()*4);
    const auto midpoint=[&](uint32_t a,uint32_t b)
    {
      const uint64_t key=(uint64_t(std::min(a,b))<<32)|std::max(a,b);
      const auto found=mids.find(key); if(found!=mids.end())return found->second;
      const auto va=tile.vertices[a],vb=tile.vertices[b];
      const Point2 p((va.x+vb.x)*.5,(va.z+vb.z)*.5);
      const auto id=uint32_t(tile.vertices.size());tile.vertices.push_back({p.x,height(p),p.y});
      mids.emplace(key,id);return id;
    };
    const auto triangle=[&](uint32_t a,uint32_t b,uint32_t c)
    {
      triangles.insert(triangles.end(),{a,b,c});
      const auto &va=tile.vertices[a],&vb=tile.vertices[b],&vc=tile.vertices[c];
      materials.push_back(material({(va.x+vb.x+vc.x)/3,(va.z+vb.z+vc.z)/3}));
    };
    for(size_t i=0;i<tile.indices.size();i+=3)
    {
      const auto a=tile.indices[i],b=tile.indices[i+1],c=tile.indices[i+2];
      const auto ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);
      triangle(a,ab,ca);triangle(ab,b,bc);triangle(ca,bc,c);triangle(ab,bc,ca);
    }
    tile.indices.swap(triangles);tile.materials.swap(materials);
    for(auto &border:tile.borders)
    {
      std::vector<Point2> refined;
      for(size_t i=0;i<border.size();++i)
      {
        if(i)refined.push_back({(border[i-1].x+border[i].x)*.5,(border[i-1].y+border[i].y)*.5});
        refined.push_back(border[i]);
      }
      border.swap(refined);
    }
  }
}

} // namespace

bool buildTerrainMesh(
    const std::vector<Chain> &chains,
    const std::vector<StepQuad> &steps,
    const HeightFunction &height,
    const MaterialFunction &material,
    const double extentMetres,
    const TerrainMeshSettings &settings,
    TerrainMesh &mesh,
    std::string *error)
{
  if (!(extentMetres > 0.0) || !std::isfinite(extentMetres))
  {
    if (error != nullptr)
      *error = "the world extent must be positive and finite";
    return false;
  }
  if (!(settings.tileSizeMetres > 0.0))
  {
    if (error != nullptr)
      *error = "the tile size must be positive";
    return false;
  }
  if (!height || !material)
  {
    if (error != nullptr)
      *error = "a height and a material function are both required";
    return false;
  }

  if ((!settings.retainTiles && !settings.tileSink) || (settings.tileSink && !steps.empty()))
  { if(error)*error="streaming requires a tile sink and does not support appended step faces"; return false; }
  if(settings.uniformSubdivisionRounds>4 || (settings.uniformSubdivisionRounds &&
      (!steps.empty() || std::any_of(chains.begin(),chains.end(),[](const Chain &c){return c.hasElevation;}))))
  { if(error)*error="uniform surface subdivision supports up to four rounds and unpinned height functions";return false; }
  // PSLG intersection tests run before the first CDT is constructed.
  initialisePredicates();
  const auto started = std::chrono::steady_clock::now();
  TileBuildContext context{};
  context.chains = &chains;
  context.height = &height;
  context.material = &material;
  context.extentMetres = extentMetres;
  context.settings = settings;
  context.chainSpacing = settings.chainSpacingMetres > 0.0
      ? settings.chainSpacingMetres
      : settings.tileSizeMetres / 32.0;
  context.borderSpacing = settings.borderSpacingMetres > 0.0
      ? settings.borderSpacingMetres
      : settings.tileSizeMetres / 8.0;

  const auto tilesPerAxis = static_cast<uint32_t>(
      std::ceil(extentMetres / settings.tileSizeMetres));
  mesh.tiles.assign(
      static_cast<size_t>(tilesPerAxis) * tilesPerAxis, MeshTile{});
  mesh.extentMetres = extentMetres;
  mesh.tileSizeMetres = settings.tileSizeMetres;
  mesh.tilesPerAxis = tilesPerAxis;

  const uint32_t workers = std::min<uint64_t>(mesh.tiles.size(),settings.workerCount > 0u
      ? settings.workerCount : std::max(1u, std::thread::hardware_concurrency()));
  if(settings.progress)
  {try{settings.progress({"indexing constraints",0,uint32_t(mesh.tiles.size()),0,workers,0,0});}
    catch(const std::exception &e){if(error)*error=e.what();return false;}
    catch(...){if(error)*error="mesh progress callback failed";return false;}}
  context.tilesPerAxis=tilesPerAxis;
  context.tileChains.resize(mesh.tiles.size());
  if(settings.indexConstraints)
  {
    // Keep complete chains and original order so arc-length resampling and
    // boundary vertices are identical to the unindexed reference path.
    for(uint32_t id=0;id<chains.size();++id)
    {
      const auto &chain=chains[id];if(chain.points.size()<2)continue;
      double minX=chain.points[0].x,maxX=minX,minY=chain.points[0].y,maxY=minY;
      for(const auto p:chain.points){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minY=std::min(minY,p.y);maxY=std::max(maxY,p.y);}
      const double extent=tilesPerAxis*settings.tileSizeMetres;
      if(maxX< -kWeld||maxY< -kWeld||minX>extent+kWeld||minY>extent+kWeld)continue;
      const auto cell=[&](double p){return uint32_t(std::clamp(std::floor(p/settings.tileSizeMetres),0.0,double(tilesPerAxis-1)));};
      for(uint32_t y=cell(minY-kWeld);y<=cell(maxY+kWeld);++y)
        for(uint32_t x=cell(minX-kWeld);x<=cell(maxX+kWeld);++x)context.tileChains[size_t(y)*tilesPerAxis+x].push_back(id);
    }
  }
  std::atomic<size_t> next{0u};
  std::atomic<uint32_t> failures{0u};
  std::vector<std::string> workerErrors(workers);
  std::vector<double> workerPeak(workers, 0.0);

  std::atomic<uint64_t> emittedTriangles{0};
  std::atomic<bool> cancelled{false};
  std::atomic<uint32_t> completed{0},active{0};
  std::atomic<uint64_t> reportedTriangles{0};
  std::mutex progressMutex;
  std::condition_variable progressWake;
  bool progressDone=false;
  std::string progressError;
  const auto reportProgress=[&]
  {
    if(!settings.progress)return;
    try{settings.progress({"meshing",completed.load(),uint32_t(mesh.tiles.size()),active.load(),workers,reportedTriangles.load(),
      std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()});}
    catch(const std::exception &e){progressError=e.what();cancelled=true;}
    catch(...){progressError="mesh progress callback failed";cancelled=true;}
  };
  reportProgress();
  std::thread reporter;
  if(settings.progress)reporter=std::thread([&]{std::unique_lock<std::mutex> lock(progressMutex);
    while(!progressWake.wait_for(lock,std::chrono::seconds(1),[&]{return progressDone;})){lock.unlock();reportProgress();lock.lock();}});
  std::vector<TerrainMeshStatistics> emitted(workers);
  const auto run = [&](const uint32_t worker)
  {
    for (;;)
    {
      const size_t index = next.fetch_add(1u);
      if (index >= mesh.tiles.size() || cancelled.load())
        return;
      ++active;
      const auto tileStart = std::chrono::steady_clock::now();
      const auto tileX = static_cast<uint32_t>(index % tilesPerAxis);
      const auto tileZ = static_cast<uint32_t>(index / tilesPerAxis);
      std::string tileError;
      bool ok = buildTile(context, tileX, tileZ, mesh.tiles[index], &tileError);
      auto &tile=mesh.tiles[index];
      const uint64_t predicted=uint64_t(tile.triangleCount())<<(2*settings.uniformSubdivisionRounds);
      if(ok && settings.maximumTriangleCount &&
          emittedTriangles.fetch_add(predicted)+predicted>settings.maximumTriangleCount)
      { ok=false; tileError="triangle budget exceeded; use a coarser spacing or a larger budget"; cancelled=true; }
      if(ok && settings.uniformSubdivisionRounds)subdivideTile(tile,height,material,settings.uniformSubdivisionRounds);
      if(ok && settings.tileSink)
      {
        try { ok=settings.tileSink(tile,tileError); }
        catch(const std::exception &e) { ok=false; tileError=e.what(); }
        if(!ok)cancelled=true;
      }
      if (!ok)
      {
        failures.fetch_add(1u);
        if (workerErrors[worker].empty()) workerErrors[worker] = tileError;
      }
      if(ok){reportedTriangles.fetch_add(tile.triangleCount());++completed;}
      --active;
      if(!settings.retainTiles)
      {
        auto &stats=emitted[worker];
        stats.vertexCount+=tile.vertices.size(); stats.triangleCount+=tile.triangleCount();
        stats.constraintCount+=tile.constraintCount; stats.constraintsIncluded+=tile.constraintsIncluded;
        stats.relaxedTiles+=tile.relaxedDelaunay; stats.encroachmentSplitRounds+=tile.encroachmentSplits;
        stats.droppedConstraints+=tile.droppedConstraints; stats.degradedTiles+=tile.droppedConstraints?1u:0u;
        for(size_t i=0;i+2<tile.indices.size();i+=3)
        {
          const auto &a=tile.vertices[tile.indices[i]],&b=tile.vertices[tile.indices[i+1]],&c=tile.vertices[tile.indices[i+2]];
          const Point2 pa(a.x,a.z),pb(b.x,b.z),pc(c.x,c.z);
          const double angle=smallestAngleDegrees(pa,pb,pc),area=.5*std::abs(triangleAreaTwice(pa,pb,pc));
          stats.minimumTriangleAngleDegrees=std::min(stats.minimumTriangleAngleDegrees,angle);
          stats.totalAreaSquareMetres+=area;
          if(angle<5){++stats.sliverTriangleCount;stats.sliverAreaSquareMetres+=area;}
        }
        // Keep only border/constraint metadata. Peak geometry memory is bounded
        // by active workers, not by the final world's triangle count.
        std::vector<MeshVertex>().swap(tile.vertices);
        std::vector<uint32_t>().swap(tile.indices);
        std::vector<uint8_t>().swap(tile.materials);
      }
      workerPeak[worker] = std::max(workerPeak[worker],
          std::chrono::duration<double>(
              std::chrono::steady_clock::now() - tileStart)
              .count());
    }
  };

  if (workers <= 1u)
  {
    run(0u);
  }
  else
  {
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (uint32_t worker = 0u; worker < workers; ++worker)
      pool.emplace_back(run, worker);
    for (std::thread &thread : pool)
      thread.join();
  }

  {std::lock_guard<std::mutex> lock(progressMutex);progressDone=true;}
  progressWake.notify_one();
  if(reporter.joinable())reporter.join();
  reportProgress();
  if(!progressError.empty()){if(error)*error=progressError;return false;}

  // --- step faces ----------------------------------------------------------
  //
  // A vertical discontinuity is two heights at one plan position, which a 2D
  // triangulation cannot express, so the faces are appended rather than
  // triangulated. Their corners are the brink and toe constraint points, which
  // the triangulation already carries at the same coordinates, so the strip
  // meets the terraces above and below it exactly.
  for (const StepQuad &original : steps)
  {
    StepQuad quad = original;
    {
      MaterialEntry entry{};
      entry.role = quad.role;
      entry.lithology = quad.lithology;
      entry.cover = quad.cover;
      // A gentle face keeps whatever grows on it; a sheer one cannot.
      const double slope = quad.walkable ? 0.2 : 0.9;
      entry.weathering = weatheringFor(entry.cover, entry.role, slope);
      quad.material = mesh.palette.add(entry);
    }
    const Point2 centroid(
        (quad.brinkA.x + quad.brinkB.x + quad.toeA.x + quad.toeB.x) * 0.25,
        (quad.brinkA.y + quad.brinkB.y + quad.toeA.y + quad.toeB.y) * 0.25);
    const auto tileX = static_cast<int64_t>(
        std::floor(centroid.x / settings.tileSizeMetres));
    const auto tileZ = static_cast<int64_t>(
        std::floor(centroid.y / settings.tileSizeMetres));
    if (tileX < 0 || tileZ < 0 || tileX >= tilesPerAxis || tileZ >= tilesPerAxis)
      continue;
    MeshTile &tile = mesh.tiles[static_cast<size_t>(tileZ) * tilesPerAxis + tileX];
    const auto base = static_cast<uint32_t>(tile.vertices.size());
    tile.vertices.push_back({quad.brinkA.x, quad.brinkAHeight, quad.brinkA.y});
    tile.vertices.push_back({quad.brinkB.x, quad.brinkBHeight, quad.brinkB.y});
    tile.vertices.push_back({quad.toeB.x, quad.toeBHeight, quad.toeB.y});
    tile.vertices.push_back({quad.toeA.x, quad.toeAHeight, quad.toeA.y});
    // Two triangles, wound so the face points away from the block.
    const uint32_t order[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    for (const uint32_t corner : order)
      tile.indices.push_back(base + corner);
    tile.materials.push_back(quad.material);
    tile.materials.push_back(quad.material);
    (void)quad.role;
    tile.stepFaceCount += 2u;
  }

  TerrainMeshStatistics statistics{};
  statistics.tileCount = static_cast<uint32_t>(mesh.tiles.size());
  statistics.stepCount = static_cast<uint32_t>(steps.size());
  statistics.failedTiles = failures.load();
  for (const MeshTile &tile : mesh.tiles)
  {
    statistics.vertexCount += tile.vertices.size();
    statistics.triangleCount += tile.triangleCount();
    statistics.constraintCount += tile.constraintCount;
    statistics.constraintsIncluded += tile.constraintsIncluded;
    statistics.simplifiedVertices += tile.simplifiedVertices;
    statistics.relaxedTiles += tile.relaxedDelaunay ? 1u : 0u;
    statistics.droppedConstraints += tile.droppedConstraints;
    statistics.degradedTiles += tile.droppedConstraints ? 1u : 0u;
    statistics.encroachmentSplitRounds += tile.encroachmentSplits;
    statistics.stepFaceTriangles += tile.stepFaceCount;
    for (size_t index = 0u; index + 2u < tile.indices.size(); index += 3u)
    {
      const MeshVertex &a = tile.vertices[tile.indices[index]];
      const MeshVertex &b = tile.vertices[tile.indices[index + 1u]];
      const MeshVertex &c = tile.vertices[tile.indices[index + 2u]];
      const double angle = smallestAngleDegrees(
          Point2(a.x, a.z), Point2(b.x, b.z), Point2(c.x, c.z));
      statistics.minimumTriangleAngleDegrees =
          std::min(statistics.minimumTriangleAngleDegrees, angle);
      const double area = 0.5 * std::abs(triangleAreaTwice(
          Point2(a.x, a.z), Point2(b.x, b.z), Point2(c.x, c.z)));
      statistics.totalAreaSquareMetres += area;
      if (angle < 5.0)
      {
        ++statistics.sliverTriangleCount;
        statistics.sliverAreaSquareMetres += area;
      }
    }
  }
  if(!settings.retainTiles)
  {
    statistics.vertexCount=statistics.triangleCount=statistics.constraintCount=statistics.constraintsIncluded=0;
    statistics.relaxedTiles=0;statistics.encroachmentSplitRounds=0;
    statistics.droppedConstraints=0;statistics.degradedTiles=0;
    for(const auto &st:emitted)
    {
      statistics.vertexCount+=st.vertexCount;statistics.triangleCount+=st.triangleCount;
      statistics.constraintCount+=st.constraintCount;statistics.constraintsIncluded+=st.constraintsIncluded;
      statistics.relaxedTiles+=st.relaxedTiles;statistics.encroachmentSplitRounds+=st.encroachmentSplitRounds;
      statistics.droppedConstraints+=st.droppedConstraints;statistics.degradedTiles+=st.degradedTiles;
      statistics.minimumTriangleAngleDegrees=std::min(statistics.minimumTriangleAngleDegrees,st.minimumTriangleAngleDegrees);
      statistics.sliverTriangleCount+=st.sliverTriangleCount;statistics.sliverAreaSquareMetres+=st.sliverAreaSquareMetres;
      statistics.totalAreaSquareMetres+=st.totalAreaSquareMetres;
    }
  }
  for (const double peak : workerPeak)
    statistics.peakTileSeconds = std::max(statistics.peakTileSeconds, peak);
  statistics.buildSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  mesh.statistics = statistics;

  if (failures.load() != 0u)
  {
    if (error != nullptr)
    {
      for (const std::string &message : workerErrors)
        if (!message.empty())
        {
          *error = message;
          break;
        }
    }
    return false;
  }
  return true;
}

bool buildTerrainMesh(
    const world::GeneratedWorld &world,
    const TerrainMeshSettings &settings,
    TerrainMesh &mesh,
    std::string *error)
{
  if (world.surface.empty())
  {
    if (error != nullptr)
      *error = "the world has no continuous surface to sample";
    return false;
  }
  std::vector<Chain> chains = gatherWorldChains(world);
  // Cliffs, when the map has plateau blocks to cut them from. The brink and toe
  // become constraints like any other line; the faces between them are the one
  // thing the triangulator cannot make.
  std::vector<StepQuad> steps;
  uint32_t strokeCourses = 0u;
  uint32_t strokeJunctions = 0u;
  uint32_t strokeDuplicates = 0u;
  if (settings.carveRivers && !world.rivers.segments.empty())
  {
    StrokeSettings strokes{};
    strokes.seed = world.graph.brief().seed ^ 0x2545f4914f6cdd1dull;
    strokes.spacingMetres = std::max(30.0, settings.tileSizeMetres / 48.0);
    strokes.reachIntervalMetres = settings.terraceIntervalMetres > 0.0
        ? std::max(1.0, settings.terraceIntervalMetres * 0.25)
        : 4.0;
    const StrokeGeometry water = buildStrokes(world, strokes);
    chains.insert(chains.end(), water.chains.begin(), water.chains.end());
    steps.insert(steps.end(), water.quads.begin(), water.quads.end());
    strokeCourses = water.courseCount;
    strokeJunctions = water.junctionCount;
    strokeDuplicates = water.duplicateCells;
  }
  if (!world.plateaus.blocks.empty())
  {
    CliffSettings cliffs{};
    cliffs.seed = world.graph.brief().seed;
    cliffs.spacingMetres = std::max(20.0, settings.tileSizeMetres / 64.0);
    cliffs.ramps.seed = world.graph.brief().seed ^ 0x51ed270bull;
    cliffs.beautify = settings.beautifyEdges;
    cliffs.detail.seed = world.graph.brief().seed ^ 0x6a09e667ull;
    const CliffGeometry geometry = buildCliffs(world, cliffs);
    chains.insert(chains.end(), geometry.chains.begin(), geometry.chains.end());
    steps.insert(steps.end(), geometry.quads.begin(), geometry.quads.end());
    mesh.terraces = geometry.terraces;
  }
  const world::MapSurfaceField *surface = &world.surface;
  const world::MapGraph *graph = &world.graph;
  const HeightFunction height = [surface](const Point2 point)
  { return surface->sample(point).elevationMetres; };
  // Ground faces resolve through the palette: role first, then the rock the
  // ground stands on and the cover growing on it. The palette de-duplicates, so
  // a world with a handful of biomes over a handful of rock types costs a
  // handful of entries however many million faces it has.
  MaterialPalette *palette = &mesh.palette;
  MaterialEntry ocean{};
  ocean.role = SurfaceRole::Water;
  ocean.cover = world::Biome::Ocean;
  const uint8_t oceanMaterial = palette->add(ocean);
  std::vector<uint8_t> cellMaterials(graph->nodeCount());
  for (uint32_t node = 0; node < graph->nodeCount(); ++node)
  {
    const world::MapNode &cell = graph->node(node);
    MaterialEntry entry{};
    entry.role = cell.land ? SurfaceRole::Ground : SurfaceRole::Water;
    entry.cover = world::biomeOf(cell);
    entry.lithology = lithologyForCell(cell);
    entry.weathering = weatheringFor(entry.cover, entry.role, 0.0);
    cellMaterials[node] = palette->add(entry);
  }
  const MaterialFunction material =
      [graph, &cellMaterials, oceanMaterial](const Point2 point) -> uint8_t
  {
    const uint32_t node = graph->nodeAt(point);
    return node == world::kInvalidNode ? oceanMaterial : cellMaterials[node];
  };
  if (!buildTerrainMesh(chains, steps, height, material, world.extentMetres,
          settings, mesh, error))
    return false;
  mesh.statistics.strokeCourses = strokeCourses;
  mesh.statistics.strokeJunctions = strokeJunctions;
  mesh.statistics.strokeDuplicates = strokeDuplicates;
  return true;
}

} // namespace procedural::geometry
