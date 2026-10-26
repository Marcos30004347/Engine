#include "WorldRoadNetwork.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
namespace procedural::world::rewrite
{
namespace
{
bool fail(std::string *e, const char *message)
{
  if (e)
    *e = message;
  return false;
}
double length(const Position &a, const Position &b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}
bool finite(const Position &p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool valid(const RoadSettings &s)
{
  for (double v :
       {s.spacingMetres,
        s.lateralSpacingMetres,
        s.corridorHalfWidthMetres,
        s.halfWidthMetres,
        s.shoulderMetres,
        s.crossSlope,
        s.crownSlope,
        s.maximumGrade,
        s.maximumGradeChangePerMetre,
        s.maximumEarthworkMetres,
        s.minimumBendRadiusMetres,
        s.earthworkWeight,
        s.resistanceWeight,
        s.turningWeight})
    if (!std::isfinite(v))
      return false;
  return s.spacingMetres >= 1 && s.spacingMetres <= 20 && s.lateralSpacingMetres >= .5 && s.corridorHalfWidthMetres >= 0 && s.corridorHalfWidthMetres <= 1000 && s.halfWidthMetres >= 1 && s.halfWidthMetres <= 20 &&
         s.shoulderMetres > 0 && s.shoulderMetres <= 10 && std::abs(s.crossSlope) + s.crownSlope <= .1 && s.crownSlope >= 0 && s.maximumGrade > 0 && s.maximumGrade <= .4 && s.maximumGradeChangePerMetre > 0 &&
         s.maximumGradeChangePerMetre <= 1 && s.maximumEarthworkMetres >= 0 && s.maximumEarthworkMetres <= 100 && s.minimumBendRadiusMetres >= s.halfWidthMetres + s.shoulderMetres && s.earthworkWeight >= 0 &&
         s.resistanceWeight >= 0 && s.turningWeight >= 0 && s.smoothingPasses <= 8;
}
double orient(const Position &a, const Position &b, const Position &c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
bool bary(const std::array<Position, 3> &p, double x, double z, double &y)
{
  Position q{x, 0, z};
  double d = orient(p[0], p[1], p[2]);
  if (std::abs(d) < 1e-10)
    return false;
  double a = orient(p[1], p[2], q) / d, b = orient(p[2], p[0], q) / d, c = 1 - a - b;
  if (std::min({a, b, c}) < -1e-9)
    return false;
  y = a * p[0].y + b * p[1].y + c * p[2].y;
  return true;
}
} // namespace
bool selectRoadLinks(const std::vector<RoadAnchor> &input, std::vector<RoadLink> &out, std::string *error)
{
  if (input.empty() || input.size() > 512)
    return fail(error, "invalid road anchor budget");
  auto a = input;
  std::sort(
      a.begin(),
      a.end(),
      [](auto &x, auto &y)
      {
        return x.id < y.id;
      });
  std::map<uint64_t, size_t> ids;
  std::vector<size_t> towns;
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (!a[i].id || !finite(a[i].position) || !ids.emplace(a[i].id, i).second)
      return fail(error, "invalid road anchor");
    if (a[i].town)
      towns.push_back(i);
  }
  if (towns.empty())
    return fail(error, "road network needs a town");
  std::vector<RoadLink> links;
  for (auto &v : a)
    if (!v.town)
    {
      auto t = ids.find(v.parentTown);
      if (t == ids.end() || !a[t->second].town)
        return fail(error, "missing village parent");
      links.push_back({v.parentTown, v.id});
    }
  std::vector<std::tuple<double, size_t, size_t>> edges;
  for (size_t i = 0; i < towns.size(); ++i)
    for (size_t j = i + 1; j < towns.size(); ++j)
      edges.emplace_back(length(a[towns[i]].position, a[towns[j]].position), towns[i], towns[j]);
  std::sort(edges.begin(), edges.end());
  std::vector<size_t> root(a.size());
  std::iota(root.begin(), root.end(), 0);
  auto find = [&](size_t i)
  {
    while (root[i] != i)
      i = root[i];
    return i;
  };
  for (auto [d, i, j] : edges)
    if (find(i) != find(j))
    {
      root[find(j)] = find(i);
      links.push_back({a[i].id, a[j].id});
    }
  std::sort(
      links.begin(),
      links.end(),
      [](auto &a, auto &b)
      {
        return std::tie(a.from, a.to) < std::tie(b.from, b.to);
      });
  out = std::move(links);
  return true;
}
bool RoadProduct::height(double x, double z, double &y) const
{
  if (!std::isfinite(x) || !std::isfinite(z))
    return false;
  for (auto &t : triangles)
    if (bary(t.points, x, z, y))
      return true;
  return false;
}
bool designRoad(const std::vector<Position> &path, const std::vector<RoadHeightLock> &locks, const RoadField &terrain, const RoadField &resistance, const RoadSettings &s, RoadProduct &out, std::string *error)
{
  if (!valid(s) || !terrain || !resistance || path.size() < 2 || path.size() > 2048 || locks.size() > path.size())
    return fail(error, "invalid road design input");
  const size_t n = path.size();
  std::vector<double> dist(n), target(n), h(n), fixed(n, std::numeric_limits<double>::quiet_NaN());
  Position chord{path.back().x - path.front().x, 0, path.back().z - path.front().z};
  double chordLength = length(path.front(), path.back());
  if (chordLength < 1)
    return fail(error, "road endpoints too close");
  for (size_t i = 0; i < n; ++i)
  {
    if (!finite(path[i]))
      return fail(error, "nonfinite road station");
    if (i)
    {
      double d = length(path[i - 1], path[i]);
      if (d < .1 || d > 40)
        return fail(error, "road station spacing outside budget");
      dist[i] = dist[i - 1] + d;
      if ((path[i].x - path[i - 1].x) * chord.x + (path[i].z - path[i - 1].z) * chord.z <= 0)
        return fail(error, "road path must progress forward");
    }
    double lateral = std::abs((path[i].x - path[0].x) * chord.z - (path[i].z - path[0].z) * chord.x) / chordLength;
    if (lateral > s.corridorHalfWidthMetres + 1e-8)
      return fail(error, "road outside search corridor");
    target[i] = h[i] = terrain(path[i].x, path[i].z);
    if (!std::isfinite(h[i]))
      return fail(error, "road touches forbidden terrain");
  }
  fixed[0] = path[0].y;
  fixed[n - 1] = path[n - 1].y;
  for (auto &l : locks)
  {
    if (l.station >= n || !std::isfinite(l.heightMetres))
      return fail(error, "invalid road height lock");
    if (std::isfinite(fixed[l.station]) && std::abs(fixed[l.station] - l.heightMetres) > 1e-9)
      return fail(error, "conflicting road height lock");
    fixed[l.station] = l.heightMetres;
  }
  // Cyclic projections onto convex grade, grade-change, earthwork and lock sets.
  // This finds a feasible profile near natural ground, not a global optimal earthwork solution.
  auto project = [&](const std::vector<std::pair<size_t, double>> &terms, double limit)
  {
    double value = 0, norm = 0;
    for (auto [i, c] : terms)
    {
      value += c * h[i];
      if (!std::isfinite(fixed[i]))
        norm += c * c;
    }
    if (value > limit && norm > 0)
      for (auto [i, c] : terms)
        if (!std::isfinite(fixed[i]))
          h[i] -= (value - limit) * c / norm;
  };
  for (int pass = 0; pass < 4000; ++pass)
  {
    double delta = 0;
    auto old = h;
    for (size_t i = 0; i < n; ++i)
      h[i] = std::isfinite(fixed[i]) ? fixed[i] : std::clamp(h[i], target[i] - s.maximumEarthworkMetres, target[i] + s.maximumEarthworkMetres);
    for (size_t i = 1; i < n; ++i)
    {
      double d = dist[i] - dist[i - 1];
      project({{i, 1}, {i - 1, -1}}, s.maximumGrade * d);
      project({{i, -1}, {i - 1, 1}}, s.maximumGrade * d);
    }
    for (size_t i = 1; i + 1 < n; ++i)
    {
      double a = dist[i] - dist[i - 1], b = dist[i + 1] - dist[i], limit = s.maximumGradeChangePerMetre * (a + b) * .5;
      project({{i - 1, 1 / a}, {i, -1 / a - 1 / b}, {i + 1, 1 / b}}, limit);
      project({{i - 1, -1 / a}, {i, 1 / a + 1 / b}, {i + 1, -1 / b}}, limit);
    }
    for (size_t i = 0; i < n; ++i)
      delta = std::max(delta, std::abs(old[i] - h[i]));
    if (delta < 1e-10)
      break;
  }
  RoadProduct product;
  product.sections.resize(n);
  double previousGrade = 0;
  for (size_t i = 0; i < n; ++i)
  {
    if (std::abs(h[i] - target[i]) > s.maximumEarthworkMetres + 1e-6 || (std::isfinite(fixed[i]) && std::abs(h[i] - fixed[i]) > 1e-8))
      return fail(error, "infeasible earthwork or height lock");
    if (i)
    {
      double g = (h[i] - h[i - 1]) / (dist[i] - dist[i - 1]);
      product.maximumGrade = std::max(product.maximumGrade, std::abs(g));
      if (i > 1)
        product.maximumGradeChange = std::max(product.maximumGradeChange, std::abs(g - previousGrade) / ((dist[i] - dist[i - 2]) * .5));
      previousGrade = g;
    }
    Position tangent;
    if (i == 0)
      tangent = {path[1].x - path[0].x, 0, path[1].z - path[0].z};
    else if (i + 1 == n)
      tangent = {path[i].x - path[i - 1].x, 0, path[i].z - path[i - 1].z};
    else
    {
      double a = length(path[i - 1], path[i]), b = length(path[i], path[i + 1]);
      double ux = (path[i].x - path[i - 1].x) / a, uz = (path[i].z - path[i - 1].z) / a, vx = (path[i + 1].x - path[i].x) / b, vz = (path[i + 1].z - path[i].z) / b;
      double angle = std::acos(std::clamp(ux * vx + uz * vz, -1., 1.));
      if (angle / ((a + b) * .5) > 1 / s.minimumBendRadiusMetres + 1e-8)
        return fail(error, "road bend radius too small");
      tangent = {ux + vx, 0, uz + vz};
    }
    double len = std::hypot(tangent.x, tangent.z);
    tangent.x /= len;
    tangent.z /= len;
    auto &section = product.sections[i];
    section.center = {path[i].x, h[i], path[i].z};
    section.naturalHeightMetres = target[i];
    section.distanceMetres = dist[i];
    std::array<double, 5> offsets = {-s.halfWidthMetres - s.shoulderMetres, -s.halfWidthMetres, 0, s.halfWidthMetres, s.halfWidthMetres + s.shoulderMetres};
    for (size_t k = 0; k < 5; ++k)
    {
      double l = offsets[k];
      section.points[k] = {path[i].x - tangent.z * l, h[i] + s.crossSlope * l - s.crownSlope * std::abs(l), path[i].z + tangent.x * l};
    }
  }
  if (product.maximumGrade > s.maximumGrade + 1e-6 || product.maximumGradeChange > s.maximumGradeChangePerMetre + 1e-6)
    return fail(error, "infeasible longitudinal road profile");
  // Each strip is an explicit constrained planar patch. Crown/shoulder edges survive CDT.
  for (size_t i = 1; i < n; ++i)
    for (size_t k = 0; k < 4; ++k)
    {
      std::array<Position, 4> q = {product.sections[i - 1].points[k], product.sections[i].points[k], product.sections[i].points[k + 1], product.sections[i - 1].points[k + 1]};
      for (size_t j = 0; j < 4; ++j)
        if (orient(q[j], q[(j + 1) % 4], q[(j + 2) % 4]) <= 1e-8)
          return fail(error, "folded road panel");
      BoundaryRegistry registry;
      std::vector<BoundaryInput> edges;
      PatchDomain domain;
      domain.id = 1;
      for (size_t j = 0; j < 4; ++j)
      {
        edges.push_back({j + 1, {j + 1, q[j]}, {(j + 1) % 4 + 1, q[(j + 1) % 4]}, {}});
        domain.outer.push_back({j + 1, j == 3});
      }
      PatchMesh mesh;
      if (!registry.commit(edges, error) || !compilePatchMesh(
                                                domain,
                                                registry,
                                                {},
                                                [](double, double)
                                                {
                                                  return 0.;
                                                },
                                                mesh,
                                                error))
        return false;
      for (auto &t : mesh.triangles)
      {
        RoadTriangle tri;
        tri.panel = uint32_t(i - 1);
        tri.shoulder = k == 0 || k == 3;
        for (int v = 0; v < 3; ++v)
          tri.points[v] = mesh.vertices[t[v]].position;
        // Export upward winding in x/y/z coordinates.
        if (orient(tri.points[0], tri.points[1], tri.points[2]) > 0)
          std::swap(tri.points[1], tri.points[2]);
        for (size_t v = 0; v < 3; ++v)
          for (size_t corner = 0; corner < 4; ++corner)
            if (length(tri.points[v], q[corner]) < 1e-9)
              tri.vertexIds[v] = uint32_t((corner == 0 || corner == 3 ? i - 1 : i) * 5 + k + (corner >= 2 ? 1 : 0));
        // Check the actual triangle gradient along its panel, including curved strip effects.
        const auto &a = tri.points[0], &b = tri.points[1], &c = tri.points[2];
        double determinant = orient(a, b, c);
        double gx = ((b.y - a.y) * (c.z - a.z) - (c.y - a.y) * (b.z - a.z)) / determinant;
        double gz = ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) / determinant;
        double dx = path[i].x - path[i - 1].x, dz = path[i].z - path[i - 1].z, dl = std::hypot(dx, dz);
        if (std::abs((gx * dx + gz * dz) / dl) > s.maximumGrade + 1e-6)
          return fail(error, "road triangle exceeds longitudinal grade");
        product.triangles.push_back(tri);
        // Sample every actual panel at <=1 metre spacing, not only its centerline.
        double longest = std::max({length(tri.points[0], tri.points[1]), length(tri.points[1], tri.points[2]), length(tri.points[2], tri.points[0])});
        int steps = int(std::ceil(longest));
        for (int a = 0; a <= steps; ++a)
          for (int b = 0; b <= steps - a; ++b)
          {
            double u = double(a) / steps, v = double(b) / steps, w = 1 - u - v;
            Position p{u * tri.points[0].x + v * tri.points[1].x + w * tri.points[2].x, u * tri.points[0].y + v * tri.points[1].y + w * tri.points[2].y, u * tri.points[0].z + v * tri.points[1].z + w * tri.points[2].z};
            double ground = terrain(p.x, p.z), r = resistance(p.x, p.z);
            if (!std::isfinite(ground) || !std::isfinite(r) || r < 0)
              return fail(error, "road width intersects forbidden terrain");
            double work = std::abs(p.y - ground);
            product.maximumEarthwork = std::max(product.maximumEarthwork, work);
            if (work > s.maximumEarthworkMetres + 1e-6)
              return fail(error, "road width exceeds earthwork limit");
          }
      }
    }
  out = std::move(product);
  return true;
}
bool routeRoad(const Position &start, const Position &end, const RoadField &terrain, const RoadField &resistance, const RoadSettings &s, RoadProduct &out, std::string *error)
{
  if (!valid(s) || !finite(start) || !finite(end) || !terrain || !resistance)
    return fail(error, "invalid road routing input");
  double chord = length(start, end);
  if (chord < s.spacingMetres * 2 || chord > 4000)
    return fail(error, "road length outside search budget");
  int layers = int(std::ceil(chord / s.spacingMetres)), radius = int(std::floor(s.corridorHalfWidthMetres / s.lateralSpacingMetres)), width = radius * 2 + 1;
  if (uint64_t(layers + 1) * width * 5 > 2000000)
    return fail(error, "road search state budget exceeded");
  double dx = (end.x - start.x) / chord, dz = (end.z - start.z) / chord, ds = chord / layers;
  auto point = [&](int i, int j)
  {
    double l = (j - radius) * s.lateralSpacingMetres;
    return Position{start.x + dx * ds * i - dz * l, 0, start.z + dz * ds * i + dx * l};
  };
  auto idx = [&](int i, int j, int heading)
  {
    return (i * width + j) * 5 + heading;
  };
  double inf = std::numeric_limits<double>::infinity();
  std::vector<double> cost((layers + 1) * width * 5, inf);
  std::vector<int> parent(cost.size(), -1);
  std::vector<double> ground((layers + 1) * width, inf);
  for (int i = 0; i <= layers; ++i)
    for (int j = 0; j < width; ++j)
    {
      auto p = point(i, j);
      double g = terrain(p.x, p.z), r = resistance(p.x, p.z);
      if (std::isfinite(g) && std::isfinite(r) && r >= 0)
      {
        ground[i * width + j] = g;
      }
    }
  cost[idx(0, radius, 2)] = 0;
  uint64_t fieldSamples = 0;
  uint64_t visited = 0;
  for (int i = 1; i <= layers; ++i)
    for (int j = 0; j < width; ++j)
    {
      if ((i == 1 || i >= layers - 1) && j != radius)
        continue;
      for (int heading = 0; heading < 5; ++heading)
      {
        int step = heading - 2, prev = j - step;
        if (prev < 0 || prev >= width)
          continue;
        auto a = point(i - 1, prev), b = point(i, j);
        double len = length(a, b), ga = ground[(i - 1) * width + prev], gb = ground[i * width + j];
        if (!std::isfinite(ga) || !std::isfinite(gb))
          continue;
        double slope = std::abs(gb - ga) / len;
        if (std::abs(gb - ga) > s.maximumGrade * len + 2 * s.maximumEarthworkMetres)
          continue;
        bool clear = true;
        double resistanceCost = 0, earthworkCost = 0;
        int along = int(std::ceil(len));
        int across = int(std::ceil(2 * (s.halfWidthMetres + s.shoulderMetres)));
        for (int t = 0; t <= along && clear; ++t)
          for (int k = 0; k <= across; ++k)
          {
            double u = double(t) / along, l = (double(k) / across * 2 - 1) * (s.halfWidthMetres + s.shoulderMetres), x = a.x + (b.x - a.x) * u - (b.z - a.z) / len * l, z = a.z + (b.z - a.z) * u + (b.x - a.x) / len * l;
            if (++fieldSamples > 32000000)
              return fail(error, "road field-sampling budget exceeded");
            double g = terrain(x, z), r = resistance(x, z);
            if (!std::isfinite(g) || !std::isfinite(r) || r < 0)
            {
              clear = false;
              break;
            }
            resistanceCost += r / ((along + 1) * (across + 1));
            double work = ga + (gb - ga) * u + s.crossSlope * l - s.crownSlope * std::abs(l) - g;
            earthworkCost += work * work / ((along + 1) * (across + 1));
          }
        if (!clear)
          continue;
        for (int old = 0; old < 5; ++old)
        {
          int previous = idx(i - 1, prev, old);
          if (!std::isfinite(cost[previous]))
            continue;
          ++visited;
          double turn = std::atan2(step * s.lateralSpacingMetres, ds) - std::atan2((old - 2) * s.lateralSpacingMetres, ds);
          if (std::abs(turn) / ((len + std::hypot(ds, (old - 2) * s.lateralSpacingMetres)) * .5) > 1 / s.minimumBendRadiusMetres)
            continue;
          double c = cost[previous] + len * (1 + s.earthworkWeight * (slope * slope + earthworkCost) + s.resistanceWeight * resistanceCost) + s.turningWeight * turn * turn;
          if (c < cost[idx(i, j, heading)])
          {
            cost[idx(i, j, heading)] = c;
            parent[idx(i, j, heading)] = previous;
          }
        }
      }
    }
  int best = idx(layers, radius, 2);
  if (!std::isfinite(cost[best]))
    return fail(error, "no road route through corridor");
  std::vector<Position> path;
  for (int at = best; at >= 0; at = parent[at])
  {
    int cell = at / 5;
    path.push_back(point(cell / width, cell % width));
  }
  std::reverse(path.begin(), path.end());
  // Fixed-end Laplacian relaxation removes lattice kinks. Every output panel is revalidated.
  for (uint32_t pass = 0; pass < s.smoothingPasses; ++pass)
  {
    auto copy = path;
    for (size_t i = 1; i + 1 < path.size(); ++i)
    {
      path[i].x = .25 * copy[i - 1].x + .5 * copy[i].x + .25 * copy[i + 1].x;
      path[i].z = .25 * copy[i - 1].z + .5 * copy[i].z + .25 * copy[i + 1].z;
    }
  }
  std::vector<Position> adaptive;
  for (size_t i = 1; i < path.size(); ++i)
  {
    const auto &a = path[i - 1], &b = path[i];
    double middle = terrain((a.x + b.x) * .5, (a.z + b.z) * .5);
    double deviation = std::abs(middle - (terrain(a.x, a.z) + terrain(b.x, b.z)) * .5);
    double turn = 0;
    if (i + 1 < path.size())
    {
      double u = length(a, b), v = length(b, path[i + 1]);
      turn = std::acos(std::clamp(((b.x - a.x) * (path[i + 1].x - b.x) + (b.z - a.z) * (path[i + 1].z - b.z)) / (u * v), -1., 1.));
    }
    int parts = (deviation > .05 || turn > .08) ? 2 : 1;
    for (int j = 0; j < parts; ++j)
    {
      double t = double(j) / parts;
      adaptive.push_back({a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t});
    }
  }
  adaptive.push_back(path.back());
  path = std::move(adaptive);
  path.front() = start;
  path.back() = end;
  RoadProduct product;
  if (!designRoad(path, {}, terrain, resistance, s, product, error))
    return false;
  product.routeCost = cost[best];
  product.statesVisited = visited;
  out = std::move(product);
  return true;
}
bool buildRoadNetwork(const std::vector<RoadAnchor> &anchors, const RoadField &terrain, const RoadField &resistance, const RoadSettings &settings, uint64_t revision, RoadNetworkProduct &output, std::string *error)
{
  if (!valid(settings) || !terrain || !resistance || !revision || anchors.size() > 64)
    return fail(error, "invalid road network revision or budget");
  std::vector<RoadLink> links;
  if (!selectRoadLinks(anchors, links, error))
    return false;
  RoadNetworkProduct product;
  product.dependencyRevision = revision;
  for (auto link : links)
  {
    auto find = [&](uint64_t id)
    {
      return std::find_if(
          anchors.begin(),
          anchors.end(),
          [&](const auto &a)
          {
            return a.id == id;
          });
    };
    RoutedRoad road;
    road.link = link;
    if (!routeRoad(find(link.from)->position, find(link.to)->position, terrain, resistance, settings, road.design, error))
      return false;
    product.roads.push_back(std::move(road));
  }
  output = std::move(product);
  return true;
}
} // namespace procedural::world::rewrite
