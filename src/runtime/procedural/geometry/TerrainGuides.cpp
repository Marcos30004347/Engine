#include "procedural/geometry/TerrainGuides.hpp"
#include "procedural/Geometry.hpp"
#include <algorithm>
#include <cmath>
#include <map>
namespace procedural::geometry
{
namespace
{
Point2 mix(Point2 a, Point2 b, double t)
{
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}
void emit(std::vector<Chain> &out, std::vector<Point2> points, ChainKind kind)
{
  if (points.size() < 2)
    return;
  Chain c;
  c.kind = kind;
  c.points = std::move(points);
  out.push_back(std::move(c));
}
uint64_t hash(uint64_t n)
{
  n ^= n >> 30;
  n *= 0xbf58476d1ce4e5b9ull;
  n ^= n >> 27;
  n *= 0x94d049bb133111ebull;
  return n ^ (n >> 31);
}
} // namespace
std::vector<Chain> ridgeSurfaceGuides(Point2 a, Point2 b, double width, double spacing, const HeightFunction &height, const std::function<bool(Point2)> &valid)
{
  std::vector<Chain> result;
  const double length = distance(a, b);
  if (length < 1e-6 || width <= 0 || spacing <= 0)
    return result;
  const Point2 normal{-(b.y - a.y) / length, (b.x - a.x) / length};
  const uint32_t steps = uint32_t(std::max(2.0, std::ceil(length / spacing)));
  std::vector<Point2> crest(steps + 1);
  std::vector<double> offsets(steps + 1);
  const double search = std::min(width * .4, spacing * 3);
  for (uint32_t i = 1; i < steps; ++i)
  {
    const auto p = mix(a, b, double(i) / steps);
    double score = -1e30;
    for (int k = -8; k <= 8; ++k)
    {
      const double offset = k * search / 8;
      const Point2 q{p.x + normal.x * offset, p.y + normal.y * offset};
      if (!valid(q))
        continue;
      // A weak displacement penalty keeps adjacent spines from jumping to the
      // same distant summit while still following the erosion-shifted crest.
      const double value = height(q) - .015 * std::abs(offset);
      if (value > score)
      {
        score = value;
        offsets[i] = offset;
      }
    }
  }
  for (int pass = 0; pass < 2; ++pass)
  {
    auto previous = offsets;
    for (uint32_t i = 1; i < steps; ++i)
      offsets[i] = .5 * previous[i] + .25 * (previous[i - 1] + previous[i + 1]);
  }
  for (uint32_t i = 0; i <= steps; ++i)
  {
    const auto p = mix(a, b, double(i) / steps);
    crest[i] = {p.x + normal.x * offsets[i], p.y + normal.y * offsets[i]};
  }
  for (const double shoulder : {0.0, -.2, .2, -.55, .55})
  {
    std::vector<Point2> run;
    for (uint32_t i = 0; i <= steps; ++i)
    {
      const auto p = crest[i];
      const Point2 q{p.x + normal.x * width * shoulder, p.y + normal.y * width * shoulder};
      const Point2 left{p.x - normal.x * width * .5, p.y - normal.y * width * .5}, right{p.x + normal.x * width * .5, p.y + normal.y * width * .5};
      const bool ridge = valid(p) && valid(q) && valid(left) && valid(right) && height(p) > .5 * (height(left) + height(right)) + 8;
      if (!ridge)
      {
        emit(result, std::move(run), shoulder == 0 ? ChainKind::Ridge : ChainKind::Slope);
        run.clear();
      }
      else
        run.push_back(q);
    }
    emit(result, std::move(run), shoulder == 0 ? ChainKind::Ridge : ChainKind::Slope);
  }
  // Sparse cross-slope ribs subdivide the long strips between the rails.
  for (uint32_t i = 1; i < steps; i += 3)
  {
    const auto p = crest[i];
    const Point2 left{p.x - normal.x * width * .55, p.y - normal.y * width * .55}, right{p.x + normal.x * width * .55, p.y + normal.y * width * .55};
    if (valid(left) && valid(p) && valid(right) && height(p) > .5 * (height(left) + height(right)) + 8)
      emit(result, {left, mix(left, p, .6), p, mix(p, right, .4), right}, ChainKind::Slope);
  }
  return result;
}
std::vector<Chain> riverSurfaceGuides(const std::vector<CorridorGuide> &segments, double spacing)
{
  std::vector<Chain> out;
  using Key = std::pair<int64_t, int64_t>;
  const auto key = [](Point2 p)
  {
    return Key{int64_t(std::llround(p.x * 1000)), int64_t(std::llround(p.y * 1000))};
  };
  std::map<Key, std::vector<size_t>> incident;
  for (size_t i = 0; i < segments.size(); ++i)
  {
    incident[key(segments[i].a)].push_back(i);
    incident[key(segments[i].b)].push_back(i);
  }
  const auto sidePoint = [&](size_t id, bool end, double fraction)
  {
    const auto &s = segments[id];
    const auto p = end ? s.b : s.a;
    double tx = s.b.x - s.a.x, ty = s.b.y - s.a.y;
    double len = std::max(1e-9, std::hypot(tx, ty));
    tx /= len;
    ty /= len;
    const auto widthAt = [fraction](const CorridorGuide &reach, bool atEnd)
    {
      const double inner = atEnd && reach.innerWidthEnd >= 0 ? reach.innerWidthEnd : reach.innerWidth;
      const double outer = atEnd && reach.outerWidthEnd >= 0 ? reach.outerWidthEnd : reach.outerWidth;
      return std::abs(fraction) <= 1 ? inner * std::abs(fraction) : inner + (outer - inner) * (std::abs(fraction) - 1);
    };
    double width = widthAt(s, end);
    const auto &neighbors = incident.at(key(p));
    if (neighbors.size() == 2)
    {
      const auto other = neighbors[0] == id ? neighbors[1] : neighbors[0];
      const auto &t = segments[other];
      // Join only a continuous directed reach; confluences keep all arms.
      if ((end && key(t.a) == key(p)) || (!end && key(t.b) == key(p)))
      {
        const double l = std::max(1e-9, distance(t.a, t.b));
        const double dx = (t.b.x - t.a.x) / l, dy = (t.b.y - t.a.y) / l;
        const double sum = std::hypot(tx + dx, ty + dy);
        if (sum > .5)
        {
          const double cosine = (1 + tx * dx + ty * dy) / sum;
          tx = (tx + dx) / sum;
          ty = (ty + dy) / sum;
          const double otherWidth = widthAt(t, !end);
          width = .5 * (width + otherWidth) / std::max(.5, cosine);
        }
      }
    }
    const double sign = fraction < 0 ? -1 : 1;
    return Point2{p.x - ty * width * sign, p.y + tx * width * sign};
  };
  for (size_t id = 0; id < segments.size(); ++id)
  {
    const auto &s = segments[id];
    if (distance(s.a, s.b) < 1e-6)
      continue;
    for (double f : {-2.0, -1.5, -1.0, 0.0, 1.0, 1.5, 2.0})
      emit(out, {sidePoint(id, false, f), sidePoint(id, true, f)}, ChainKind::Stroke);
    // Cross sections at shared nodes and along long reaches keep bed/bank
    // samples paired. The PSLG handles the multi-arm confluence intersections.
    const uint32_t n = uint32_t(std::max(1.0, std::ceil(distance(s.a, s.b) / std::max(1.0, spacing))));
    for (uint32_t i = 0; i <= n; ++i)
    {
      std::vector<Point2> section;
      for (double f : {-2.0, -1.5, -1.0, 0.0, 1.0, 1.5, 2.0})
        section.push_back(mix(sidePoint(id, false, f), sidePoint(id, true, f), double(i) / n));
      emit(out, std::move(section), ChainKind::Stroke | ChainKind::Slope);
    }
  }
  return out;
}
std::vector<Chain> slopeSurfaceGuides(double extent, double spacing, uint64_t seed, const HeightFunction &height, const std::function<bool(Point2)> &valid)
{
  std::vector<Chain> result;
  if (extent <= 0 || spacing <= 0)
    return result;
  const double seedStep = std::max(spacing * 6, extent / 128), probe = spacing * .5;
  const auto side = uint32_t(std::ceil(extent / seedStep));
  // Steepest descent converges - that is what descent does - so two guides
  // started a seed apart end up running metres from each other while their own
  // vertices are still `spacing` apart. The triangulator has to honour both,
  // and what it can build between them is a ladder of slivers. Keep the guides
  // evenly spaced instead: a trace ends where an existing guide already
  // describes the ground, which is the standard evenly-spaced streamline rule.
  const double separation = spacing * 0.6, separationSquared = separation * separation;
  std::vector<std::vector<Point2>> occupancy(size_t(side) * side);
  const auto cellOf = [&](const Point2 &q)
  {
    const auto cx = uint32_t(std::clamp(q.x / seedStep, 0.0, double(side - 1)));
    const auto cy = uint32_t(std::clamp(q.y / seedStep, 0.0, double(side - 1)));
    return size_t(cy) * side + cx;
  };
  const auto crowded = [&](const Point2 &q)
  {
    const auto cx = int64_t(std::clamp(q.x / seedStep, 0.0, double(side - 1)));
    const auto cy = int64_t(std::clamp(q.y / seedStep, 0.0, double(side - 1)));
    for (int64_t oy = -1; oy <= 1; ++oy)
      for (int64_t ox = -1; ox <= 1; ++ox)
      {
        const int64_t nx = cx + ox, ny = cy + oy;
        if (nx < 0 || ny < 0 || nx >= int64_t(side) || ny >= int64_t(side)) continue;
        for (const Point2 &other : occupancy[size_t(ny) * side + size_t(nx)])
        {
          const double dx = q.x - other.x, dy = q.y - other.y;
          if (dx * dx + dy * dy < separationSquared) return true;
        }
      }
    return false;
  };
  for (uint32_t y = 0; y < side; ++y)
    for (uint32_t x = 0; x < side; ++x)
    {
      const uint64_t h = hash(seed + uint64_t(y) * side + x);
      Point2 p{(x + .15 + .7 * double(h & 65535) / 65535) * seedStep, (y + .15 + .7 * double((h >> 16) & 65535) / 65535) * seedStep};
      std::vector<Point2> line;
      for (int step = 0; step < 9; ++step)
      {
        if (p.x < probe || p.y < probe || p.x > extent - probe || p.y > extent - probe || !valid(p))
          break;
        const double l = height({p.x - probe, p.y}), r = height({p.x + probe, p.y}), b = height({p.x, p.y - probe}), t = height({p.x, p.y + probe}), z = height(p);
        const double gx = (r - l) / (2 * probe), gy = (t - b) / (2 * probe), slope = std::hypot(gx, gy);
        // Planar ground needs fewer triangles, not an extra regular lattice.
        if (slope < .12 || (step == 0 && std::max(std::abs(l + r - 2 * z), std::abs(t + b - 2 * z)) < 2))
          break;
        if (crowded(p))
          break;
        line.push_back(p);
        occupancy[cellOf(p)].push_back(p);
        const Point2 next{p.x - gx / slope * spacing, p.y - gy / slope * spacing};
        if (!valid(next) || height(next) >= z)
          break;
        p = next;
      }
      emit(result, std::move(line), ChainKind::Slope);
    }
  return result;
}
} // namespace procedural::geometry
