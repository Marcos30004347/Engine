#include "WorldRiverRouting.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace procedural::world::rewrite {
namespace {
double dist(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}

double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

double pointSegment(Position p, Position a, Position b)
{
  double dx = b.x - a.x, dz = b.z - a.z, t = std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / (dx * dx + dz * dz), 0., 1.);
  return std::hypot(p.x - a.x - t * dx, p.z - a.z - t * dz);
}

double segmentDistance(Position a, Position b, Position c, Position d)
{
  if (((cross(a, b, c) > 0 && cross(a, b, d) < 0) || (cross(a, b, c) < 0 && cross(a, b, d) > 0)) && ((cross(c, d, a) > 0 && cross(c, d, b) < 0) || (cross(c, d, a) < 0 && cross(c, d, b) > 0)))
  {
    return 0;
  }
  return std::min({pointSegment(a, c, d), pointSegment(b, c, d), pointSegment(c, a, b), pointSegment(d, a, b)});
}
} // namespace

bool refineRiverPaths(
    const RiverGrowthProduct &source,
    const std::function<double(double, double)> &terrain,
    const std::function<double(double, double)> &resistance,
    const RiverRoutingSettings &s,
    RiverGrowthProduct &out,
    std::vector<RiverRouteMetrics> &metrics,
    std::string *error)
{
  auto fail = [&](const char *message) {
    if (error)
    {
      *error = message;
    }
    return false;
  };
  if (!terrain || !resistance || source.nodes.empty() || source.nodes.size() > 4096 || s.smoothingPasses > 256)
  {
    return fail("Invalid routing inputs or budget");
  }
  for (double v :
       {s.longitudinalSpacingMetres,
        s.lateralSpacingMetres,
        s.corridorHalfWidthMetres,
        s.maximumIncisionMetres,
        s.maximumWaterAboveGroundMetres,
        s.incisionWeight,
        s.resistanceWeight,
        s.adverseGradeWeight,
        s.turningWeight,
        s.minimumBendRadiusMetres,
        s.minimumSeparationMetres,
        s.junctionRadiusMetres,
        s.meanderAmplitudeMetres,
        s.meanderWavelengthMetres,
        s.meanderWeight,
        s.maximumMeanderSlope})
  {
    if (!std::isfinite(v) || v < 0 || v > 1e6)
    {
      return fail("Invalid routing settings");
    }
  }
  if (s.longitudinalSpacingMetres < .1 || s.longitudinalSpacingMetres > 32 || s.lateralSpacingMetres < .1 || s.lateralSpacingMetres > s.longitudinalSpacingMetres || s.corridorHalfWidthMetres > 512 ||
      s.corridorHalfWidthMetres < s.lateralSpacingMetres || s.minimumBendRadiusMetres <= 0 || s.meanderWavelengthMetres < 4 * s.longitudinalSpacingMetres)
  {
    return fail("Unsupported routing resolution");
  }
  RiverGrowthProduct result = source;
  result.reachPaths.clear();
  result.reachPaths.resize(source.nodes.size());
  std::vector<RiverRouteMetrics> stats;
  size_t totalPoints = 0;
  uint64_t totalStates = 0;
  for (size_t id = 0; id < source.nodes.size(); ++id)
  {
    auto n = source.nodes[id];
    for (double v : {n.position.x, n.position.y, n.position.z})
    {
      if (!std::isfinite(v) || std::abs(v) >= 1e12)
      {
        return fail("Invalid routing node");
      }
    }
    if (n.receiver == UINT32_MAX)
    {
      continue;
    }
    if (n.receiver >= id)
    {
      return fail("Routing requires fixed downstream-first receivers");
    }
    auto a = n.position, b = source.nodes[n.receiver].position;
    double L = dist(a, b);
    if (L < 4 * s.longitudinalSpacingMetres || L > 100000 || a.y < b.y)
    {
      return fail("Uphill outlet or unsupported reach length");
    }
    double ux = (b.x - a.x) / L, uz = (b.z - a.z) / L;
    int layers = int(std::ceil(L / s.longitudinalSpacingMetres)), half = int(std::floor(s.corridorHalfWidthMetres / s.lateralSpacingMetres)), width = 2 * half + 1;
    constexpr int headings = 5; // lateral steps -2..2, stable ascending tie order.
    uint64_t count = uint64_t(layers + 1) * width * headings;
    totalStates += count;
    if (count > 2000000 || totalStates > 16000000)
    {
      return fail("Routing state budget exceeded");
    }
    double ds = L / layers;
    auto point = [&](int i, double lateral) {
      double t = double(i) / layers;
      return Position{a.x + ux * t * L - uz * lateral, a.y + (b.y - a.y) * t, a.z + uz * t * L + ux * lateral};
    };
    auto index = [&](int i, int j, int h) {
      return (size_t(i) * width + j) * headings + h;
    };
    auto allowed = [&](Position p) {
      double ground = terrain(p.x, p.z), r = resistance(p.x, p.z);
      return std::isfinite(ground) && std::isfinite(r) && r >= 0 && ground - p.y <= s.maximumIncisionMetres && p.y - ground <= s.maximumWaterAboveGroundMetres;
    };
    std::vector<double> costs(count, std::numeric_limits<double>::infinity());
    std::vector<int8_t> parents(count, -1);
    costs[index(0, half, 2)] = 0;
    uint64_t visited = 0;
    double phase = double((s.seed * 6364136223846793005ULL + id * 1442695040888963407ULL) >> 40) / 16777216. * 6.283185307179586;
    for (int i = 1; i <= layers; ++i)
    {
      for (int j = 0; j < width; ++j)
      {
        if ((i <= 2 || i >= layers - 2) && j != half)
        {
          continue;
        }
        double offset = (j - half) * s.lateralSpacingMetres;
        auto q = point(i, offset);
        if (!allowed(q))
        {
          continue;
        }
        double envelope = std::pow(std::sin(3.141592653589793 * double(i) / layers), 2);
        double amplitude = (a.y - b.y) / L <= s.maximumMeanderSlope ? std::min(s.meanderAmplitudeMetres, s.corridorHalfWidthMetres * .7) : 0;
        double guide = amplitude * envelope * (.8 * std::sin(6.283185307179586 * i * ds / s.meanderWavelengthMetres + phase) + .2 * std::sin(3.88322207745 * i * ds / s.meanderWavelengthMetres + phase * .7));
        for (int h = 0; h < headings; ++h)
        {
          int prev = j - (h - 2);
          if (prev < 0 || prev >= width)
          {
            continue;
          }
          auto p = point(i - 1, (prev - half) * s.lateralSpacingMetres);
          double distance = dist(p, q), edge = 0;
          bool valid = true;
          int probes = std::max(2, int(std::ceil(distance / std::min(1., ds / 4))));
          for (int k = 0; k <= probes; ++k)
          {
            double t = double(k) / probes;
            Position v{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t};
            if (!allowed(v))
            {
              valid = false;
              break;
            }
            double incision = std::max(0., terrain(v.x, v.z) - v.y);
            edge += (1 + s.incisionWeight * incision * incision + s.resistanceWeight * resistance(v.x, v.z) + s.meanderWeight * (offset - guide) * (offset - guide)) / (probes + 1);
          }
          if (!valid)
          {
            continue;
          }
          edge *= distance;
          edge += s.adverseGradeWeight * std::max(0., terrain(q.x, q.z) - terrain(p.x, p.z));
          for (int ph = 0; ph < headings; ++ph)
          {
            double angle = std::atan2((h - 2) * s.lateralSpacingMetres, ds) - std::atan2((ph - 2) * s.lateralSpacingMetres, ds);
            double c = costs[index(i - 1, prev, ph)] + edge + s.turningWeight * angle * angle;
            if (c < costs[index(i, j, h)])
            {
              costs[index(i, j, h)] = c;
              parents[index(i, j, h)] = int8_t(ph);
            }
          }
          ++visited;
        }
      }
    }
    int h = 2, j = half;
    if (!std::isfinite(costs[index(layers, j, h)]))
    {
      return fail("No feasible valley route; structural outlet solution required");
    }
    std::vector<double> lateral(layers + 1);
    for (int i = layers; i > 0; --i)
    {
      lateral[i] = (j - half) * s.lateralSpacingMetres;
      int ph = parents[index(i, j, h)];
      j -= h - 2;
      h = ph;
    }
    // Bounded averaging has no overshoot. Frozen two-station endpoint tangents.
    for (uint32_t pass = 0; pass < s.smoothingPasses; ++pass)
    {
      auto next = lateral;
      for (int i = 3; i < layers - 2; ++i)
      {
        next[i] = .25 * lateral[i - 1] + .5 * lateral[i] + .25 * lateral[i + 1];
      }
      lateral = std::move(next);
    }
    auto &path = result.reachPaths[id];
    std::vector<double> arclength(layers + 1);
    for (int i = 0; i <= layers; ++i)
    {
      path.push_back(point(i, lateral[i]));
      if (i)
      {
        arclength[i] = arclength[i - 1] + dist(path[i - 1], path[i]);
      }
    }
    for (int i = 0; i <= layers; ++i)
    {
      path[i].y = a.y + (b.y - a.y) * arclength[i] / arclength.back();
    }
    path.front() = a;
    path.back() = b;
    RiverRouteMetrics m;
    m.upstreamNode = uint32_t(id);
    m.chordMetres = L;
    m.lengthMetres = arclength.back();
    m.sinuosity = m.lengthMetres / L;
    m.statesVisited = visited;
    for (int i = 0; i <= layers; ++i)
    {
      m.maximumOffset = std::max(m.maximumOffset, std::abs(lateral[i]));
      if (i > 0 && i < layers)
      {
        double ab = dist(path[i - 1], path[i]), bc = dist(path[i], path[i + 1]), ac = dist(path[i - 1], path[i + 1]);
        double curvature = 2 * std::abs(cross(path[i - 1], path[i], path[i + 1])) / (ab * bc * ac);
        m.maximumCurvature = std::max(m.maximumCurvature, curvature);
      }
      if (!i)
      {
        continue;
      }
      int probes = std::max(2, int(std::ceil(dist(path[i - 1], path[i]) / std::min(1., ds / 4))));
      for (int k = 0; k <= probes; ++k)
      {
        double t = double(k) / probes;
        auto p = path[i - 1], q = path[i];
        Position v{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t};
        if (!allowed(v))
        {
          return fail("Smoothed route violates terrain/resistance corridor; no shortcut accepted");
        }
        m.maximumIncision = std::max(m.maximumIncision, terrain(v.x, v.z) - v.y);
      }
    }
    if (m.maximumCurvature > 1 / s.minimumBendRadiusMetres + 1e-10)
    {
      return fail("Smoothed route exceeds bank curvature bound");
    }
    totalPoints += path.size();
    if (totalPoints > 131072)
    {
      return fail("River path point budget exceeded");
    }
    stats.push_back(m);
  }
  // Reject any unintended connection, including remote crossings of related reaches.
  uint64_t comparisons = 0;
  for (size_t i = 0; i < result.nodes.size(); ++i)
  {
    for (size_t j = i + 1; j < result.nodes.size(); ++j)
    {
      auto &p = result.reachPaths[i];
      auto &q = result.reachPaths[j];
      if (p.empty() || q.empty())
      {
        continue;
      }
      uint32_t shared = UINT32_MAX;
      for (auto u : {uint32_t(i), result.nodes[i].receiver})
      {
        for (auto v : {uint32_t(j), result.nodes[j].receiver})
        {
          if (u == v)
          {
            shared = u;
          }
        }
      }
      for (size_t x = 1; x < p.size(); ++x)
      {
        for (size_t y = 1; y < q.size(); ++y)
        {
          if (++comparisons > 8000000)
          {
            return fail("River separation validation budget exceeded");
          }
          if (shared != UINT32_MAX)
          {
            auto node = result.nodes[shared].position;
            if (std::max(dist(p[x - 1], node), dist(p[x], node)) <= s.junctionRadiusMetres && std::max(dist(q[y - 1], node), dist(q[y], node)) <= s.junctionRadiusMetres)
            {
              continue;
            }
          }
          if (segmentDistance(p[x - 1], p[x], q[y - 1], q[y]) <= s.minimumSeparationMetres)
          {
            return fail("Refined rivers cross or violate separation outside a junction");
          }
        }
      }
    }
  }
  out = std::move(result);
  metrics = std::move(stats);
  return true;
}
} // namespace procedural::world::rewrite
