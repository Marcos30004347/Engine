#include "procedural/geometry/TerrainRiverSurface.hpp"
#include "procedural/Geometry.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
namespace procedural::geometry
{
namespace
{
using Key = std::pair<int64_t, int64_t>;
Key key(Point2 p)
{
  return {int64_t(std::llround(p.x * 1000)), int64_t(std::llround(p.y * 1000))};
}
double cross(Point2 a, Point2 b, Point2 c)
{
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
} // namespace
size_t TerrainRiverSurface::bucket(Point2 p) const
{
  return size_t(std::clamp(p.y / extent_ * 128, 0.0, 127.0)) * 128 + size_t(std::clamp(p.x / extent_ * 128, 0.0, 127.0));
}
void TerrainRiverSurface::build(const std::vector<RiverSurfaceReach> &reaches, double extent, uint64_t seed)
{
  extent_ = extent;
  sections_.clear();
  buckets_.assign(128 * 128, {});
  if (extent <= 0)
    return;
  struct Node
  {
    double flow = 0, level = std::numeric_limits<double>::infinity(), depth = 0;
    bool fixed = false;
    uint32_t inflow = 0;
    std::vector<Key> outgoing;
    std::vector<size_t> edges;
  };
  std::map<Key, Node> nodes;
  for (size_t i = 0; i < reaches.size(); ++i)
  {
    const auto &r = reaches[i];
    if (distance(r.a, r.b) < 1e-6)
      continue;
    for (int end = 0; end < 2; ++end)
    {
      auto &n = nodes[key(end ? r.b : r.a)];
      n.flow = std::max(n.flow, r.flow);
      n.level = std::min(n.level, end ? r.levelB : r.levelA);
      n.depth = std::max(n.depth, end ? r.depthB : r.depthA);
      n.edges.push_back(i);
      n.fixed |= end ? r.outletB : r.outletA;
    }
    nodes[key(r.a)].outgoing.push_back(key(r.b));
    ++nodes[key(r.b)].inflow;
  }
  std::deque<Key> ready;
  std::vector<Key> order;
  for (const auto &[id, n] : nodes)
    if (!n.inflow)
      ready.push_back(id);
  while (!ready.empty())
  {
    auto id = ready.front();
    ready.pop_front();
    order.push_back(id);
    auto &n = nodes.at(id);
    for (auto next : n.outgoing)
    {
      auto &m = nodes.at(next);
      if (!m.fixed)
        m.level = std::min(m.level, n.level);
      if (!--m.inflow)
        ready.push_back(next);
    }
  }
  // Lake/ocean outlets impose backwater levels upstream; no water reach rises
  // downstream, including when depth changes at a confluence.
  for (auto it = order.rbegin(); it != order.rend(); ++it)
    for (auto next : nodes.at(*it).outgoing)
      nodes.at(*it).level = std::max(nodes.at(*it).level, nodes.at(next).level);
  const auto width = [&](Point2 p, const Node &n)
  {
    // Smooth spatial variation, shared at junctions, with discharge controlling
    // the scale. It never changes routing or adds an unrelated meander.
    const double phase = double(seed % 65536) * .001;
    const double variation = 1 + .10 * std::sin(p.x / 310 + p.y / 470 + phase) + .05 * std::sin(p.x / 730 - p.y / 530 - phase);
    // Must match the channel half width in TerrainMapGeometry, which is the
    // sqrt(Q) law scaled up so the network reads at map distance.
    return std::max(15.0, 6.0 * std::sqrt(std::max(0.0, n.flow))) * variation;
  };
  for (size_t i = 0; i < reaches.size(); ++i)
  {
    const auto &r = reaches[i];
    const double len = distance(r.a, r.b);
    if (len < 1e-6)
      continue;
    RiverSurfaceSection s;
    s.a = r.a;
    s.b = r.b;
    const auto &na = nodes.at(key(r.a)), &nb = nodes.at(key(r.b));
    s.widthA = width(r.a, na);
    s.widthB = width(r.b, nb);
    s.levelA = na.level;
    s.levelB = nb.level;
    s.depthA = na.depth;
    s.depthB = nb.depth;
    const auto offset = [&](bool end, double sign)
    {
      const auto p = end ? r.b : r.a;
      const auto &n = end ? nb : na;
      double tx = (r.b.x - r.a.x) / len, ty = (r.b.y - r.a.y) / len, w = end ? s.widthB : s.widthA;
      if (n.edges.size() == 2)
      {
        const auto &other = reaches[n.edges[0] == i ? n.edges[1] : n.edges[0]];
        if ((end && key(other.a) == key(p)) || (!end && key(other.b) == key(p)))
        {
          const double l = distance(other.a, other.b), dx = (other.b.x - other.a.x) / l, dy = (other.b.y - other.a.y) / l;
          const double sum = std::hypot(tx + dx, ty + dy);
          if (sum > .5)
          {
            const double cosine = (1 + tx * dx + ty * dy) / sum;
            tx = (tx + dx) / sum;
            ty = (ty + dy) / sum;
            w /= std::max(.5, cosine);
          }
        }
      }
      return Point2{p.x - ty * w * sign, p.y + tx * w * sign};
    };
    s.leftA = offset(false, 1);
    s.leftB = offset(true, 1);
    s.rightA = offset(false, -1);
    s.rightB = offset(true, -1);
    const uint32_t id = sections_.size();
    s.lo = {std::min({s.a.x, s.b.x, s.leftA.x, s.leftB.x, s.rightA.x, s.rightB.x}), std::min({s.a.y, s.b.y, s.leftA.y, s.leftB.y, s.rightA.y, s.rightB.y})};
    s.hi = {std::max({s.a.x, s.b.x, s.leftA.x, s.leftB.x, s.rightA.x, s.rightB.x}), std::max({s.a.y, s.b.y, s.leftA.y, s.leftB.y, s.rightA.y, s.rightB.y})};
    sections_.push_back(s);
    const size_t lo = bucket(s.lo);
    const size_t hi = bucket(s.hi);
    for (size_t y = lo / 128; y <= hi / 128; ++y)
      for (size_t x = lo % 128; x <= hi % 128; ++x)
        buckets_[y * 128 + x].push_back(id);
  }
}
RiverSurfaceSample TerrainRiverSurface::sample(Point2 p) const
{
  RiverSurfaceSample result;
  if (buckets_.empty() || extent_ <= 0)
    return result;
  // The union chooses a single surface at overlapping tributary ribbons.
  for (auto id : buckets_[bucket(p)])
  {
    const auto &s = sections_[id];
    // The barycentric tests admit a point only inside one of the four ribbon
    // triangles, and all four lie inside these bounds, so the same rejection
    // is reached here for a pair of comparisons instead of twelve crosses.
    // `epsilon` below is a barycentric slack, not a distance: it never exceeds
    // extent*2e-9, and a barycentric slack maps to at most that times the
    // triangle's own height. A thousandth of the longer side covers that with
    // margin to spare, and still rejects on the scale a bucket cell confuses.
    const double slack = 1e-3 * (std::max(s.hi.x - s.lo.x, s.hi.y - s.lo.y) + 1.0);
    if (p.x < s.lo.x - slack || p.x > s.hi.x + slack || p.y < s.lo.y - slack || p.y > s.hi.y + slack)
      continue;
    const std::array<Point2, 6> points{s.a, s.b, s.leftA, s.leftB, s.rightA, s.rightB};
    const std::array<double, 6> along{0, 1, 0, 1, 0, 1}, lateral{0, 0, 1, 1, -1, -1};
    for (const auto tri : std::array<std::array<int, 3>, 4>{{{0, 2, 1}, {2, 3, 1}, {0, 1, 4}, {4, 1, 5}}})
    {
      const auto a = points[tri[0]], b = points[tri[1]], c = points[tri[2]];
      const double d = cross(a, b, c);
      if (std::abs(d) < 1e-12)
        continue;
      const std::array<double, 3> w{cross(p, b, c) / d, cross(a, p, c) / d, cross(a, b, p) / d};
      // Accommodate the CDT world-coordinate weld lattice (micrometres).
      const double epsilon = std::max(1e-7, extent_ * 2e-9 / std::max(1.0, std::min(s.widthA, s.widthB)));
      if (*std::min_element(w.begin(), w.end()) < -epsilon)
        continue;
      double t = 0, side = 0;
      for (int k = 0; k < 3; ++k)
      {
        t += w[k] * along[tri[k]];
        side += w[k] * lateral[tri[k]];
      }
      const double level = s.levelA + (s.levelB - s.levelA) * t, depth = s.depthA + (s.depthB - s.depthA) * t;
      const double bed = level - depth * (1 - side * side);
      const double oldBed = result.level - result.depth * (1 - result.lateral * result.lateral);
      if (!result.inside || bed < oldBed)
        result = {true, level, depth, side};
    }
  }
  return result;
}
std::vector<Chain> TerrainRiverSurface::chains() const
{
  std::vector<Chain> out;
  for (const auto &s : sections_)
  {
    for (const auto pair : std::array<std::array<Point2, 2>, 3>{{{s.a, s.b}, {s.leftA, s.leftB}, {s.rightA, s.rightB}}})
    {
      Chain c;
      c.kind = ChainKind::Stroke;
      c.points = {pair[0], pair[1]};
      out.push_back(std::move(c));
    }
    for (bool end : {false, true})
    {
      Chain c;
      c.kind = ChainKind::Stroke | ChainKind::Slope;
      c.points = end ? std::vector<Point2>{s.leftB, s.b, s.rightB} : std::vector<Point2>{s.leftA, s.a, s.rightA};
      out.push_back(std::move(c));
    }
  }
  return out;
}
bool TerrainRiverSurface::buildMesh(TerrainMeshSettings settings, TerrainMesh &mesh, std::string *error, const std::vector<Chain> &shorelines, const std::function<bool(Point2)> &include) const
{
  if (sections_.empty())
  {
    mesh = {};
    mesh.extentMetres = extent_;
    return true;
  }
  settings.poissonLayers = 0;
  settings.maximumRefinementRounds = 0;
  settings.simplificationRounds = 0;
  settings.uniformSubdivisionRounds = 0;
  settings.beautifyEdges = false;
  settings.terraceStrength = 0;
  settings.minimumAngleDegrees = 0;
  settings.preserveChainVertices = true;
  const auto sink = settings.tileSink;
  settings.tileSink = [&, sink](MeshTile &tile, std::string &message)
  {
    std::vector<uint32_t> indices;
    std::vector<uint8_t> materials;
    for (size_t i = 0; i < tile.indices.size(); i += 3)
    {
      const auto a = tile.vertices[tile.indices[i]], b = tile.vertices[tile.indices[i + 1]], c = tile.vertices[tile.indices[i + 2]];
      const Point2 center{(a.x + b.x + c.x) / 3, (a.z + b.z + c.z) / 3};
      if (!sample(center).inside || (include && !include(center)))
        continue;
      indices.insert(indices.end(), {tile.indices[i], tile.indices[i + 1], tile.indices[i + 2]});
      materials.push_back(0);
    }
    std::vector<uint32_t> remap(tile.vertices.size(), UINT32_MAX);
    std::vector<MeshVertex> vertices;
    for (auto &id : indices)
    {
      if (remap[id] == UINT32_MAX)
      {
        remap[id] = vertices.size();
        vertices.push_back(tile.vertices[id]);
      }
      id = remap[id];
    }
    tile.indices.swap(indices);
    tile.materials.swap(materials);
    tile.vertices.swap(vertices);
    return !sink || sink(tile, message);
  };
  MaterialEntry water;
  water.role = SurfaceRole::Water;
  water.cover = world::Biome::Lake;
  mesh.palette.add(water);
  auto edges = chains();
  edges.insert(edges.end(), shorelines.begin(), shorelines.end());
  return buildTerrainMesh(
      edges,
      [this](Point2 p)
      {
        return sample(p).level;
      },
      [](Point2)
      {
        return uint8_t(0);
      },
      extent_,
      settings,
      mesh,
      error);
}
} // namespace procedural::geometry
