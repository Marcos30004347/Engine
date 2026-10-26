#include "WorldPhysicalTerrain.hpp"
#include "WorldPatchMesh.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace procedural::world::rewrite {
namespace {
double dot(Position a, Position b)
{
  return a.x * b.x + a.z * b.z;
}

Position sub(Position a, Position b)
{
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

double length(Position a)
{
  return std::hypot(a.x, a.z);
}

double smooth(double t)
{
  t = std::clamp(t, 0., 1.);
  return t * t * (3 - 2 * t);
}

bool finite(double x)
{
  return std::isfinite(x) && std::abs(x) < 1e9;
}

double nearest(double x, double z, Position a, Position b, double &t)
{
  auto d = sub(b, a);
  t = std::clamp(((x - a.x) * d.x + (z - a.z) * d.z) / dot(d, d), 0., 1.);
  return std::hypot(x - a.x - t * d.x, z - a.z - t * d.z);
}

double segmentDistance(Position a, Position b, Position c, Position d)
{
  auto u = sub(b, a), v = sub(d, c), w = sub(c, a);
  double den = u.x * v.z - u.z * v.x;
  if (std::abs(den) > 1e-15)
  {
    double t = (w.x * v.z - w.z * v.x) / den, s = (w.x * u.z - w.z * u.x) / den;
    if (t >= 0 && t <= 1 && s >= 0 && s <= 1)
    {
      return 0;
    }
  }
  double t;
  return std::min({nearest(a.x, a.z, c, d, t), nearest(b.x, b.z, c, d, t), nearest(c.x, c.z, a, b, t), nearest(d.x, d.z, a, b, t)});
}

bool triangleHeight(const std::array<Position, 3> &v, double x, double z, double &h)
{
  auto a = v[0], u = sub(v[1], a), w = sub(v[2], a);
  double det = u.x * w.z - u.z * w.x;
  double p = ((x - a.x) * w.z - (z - a.z) * w.x) / det, q = (u.x * (z - a.z) - u.z * (x - a.x)) / det;
  if (p < -1e-9 || q < -1e-9 || p + q > 1 + 1e-9)
  {
    return false;
  }
  h = a.y + p * u.y + q * w.y;
  return true;
}
} // namespace

bool constrainRampMesh(const std::vector<RampPanel> &panels, const SplineFaceMesh &source, SplineFaceMesh &out, std::vector<uint8_t> &roles, std::string *error)
{
  auto fail = [&](const char *s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (source.triangles.size() != source.expectedNormals.size() || source.triangles.size() > 100000 || panels.size() > 1024)
  {
    return fail("Invalid ramp mesh budget");
  }
  for (auto p : source.vertices)
  {
    if (!finite(p.x) || !finite(p.y) || !finite(p.z))
    {
      return fail("Nonfinite ramp mesh source");
    }
  }

  struct Polygon
  {
    std::vector<Position> points;
    uint8_t role;
    Position normal;
  };

  std::vector<Polygon> polygons;
  auto signedSide = [](Position p, Position a, Position b) {
    return (b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x);
  };
  for (size_t i = 0; i < source.triangles.size(); ++i)
  {
    std::vector<Position> original;
    for (auto id : source.triangles[i])
    {
      if (id >= source.vertices.size())
      {
        return fail("Invalid source mesh index");
      }
      original.push_back(source.vertices[id]);
    }
    if (source.expectedNormals[i].y == 0)
    {
      polygons.push_back({original, 0, source.expectedNormals[i]});
      continue;
    }
    std::vector<std::vector<Position>> pending{original};
    for (auto panel : panels)
    {
      std::vector<std::vector<Position>> next;
      double orientation = signedSide(panel.vertices[2], panel.vertices[0], panel.vertices[1]) > 0 ? 1 : -1;
      for (auto poly : pending)
      {
        double minX = 1e30, maxX = -1e30, minZ = 1e30, maxZ = -1e30;
        for (auto p : poly)
        {
          minX = std::min(minX, p.x);
          maxX = std::max(maxX, p.x);
          minZ = std::min(minZ, p.z);
          maxZ = std::max(maxZ, p.z);
        }
        double rminX = 1e30, rmaxX = -1e30, rminZ = 1e30, rmaxZ = -1e30;
        for (auto p : panel.vertices)
        {
          rminX = std::min(rminX, p.x);
          rmaxX = std::max(rmaxX, p.x);
          rminZ = std::min(rminZ, p.z);
          rmaxZ = std::max(rmaxZ, p.z);
        }
        if (maxX < rminX || minX > rmaxX || maxZ < rminZ || minZ > rmaxZ)
        {
          next.push_back(poly);
          continue;
        }
        auto inside = poly;
        for (int edge = 0; edge < 3 && inside.size() >= 3; ++edge)
        {
          std::vector<Position> yes, no;
          auto a = panel.vertices[edge], b = panel.vertices[(edge + 1) % 3];
          for (size_t k = 0; k < inside.size(); ++k)
          {
            auto p = inside[k], q = inside[(k + 1) % inside.size()];
            double dp = orientation * signedSide(p, a, b), dq = orientation * signedSide(q, a, b);
            if (dp >= 0)
            {
              yes.push_back(p);
            }
            if (dp <= 0)
            {
              no.push_back(p);
            }
            if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0))
            {
              double t = dp / (dp - dq);
              Position cut{p.x + t * (q.x - p.x), p.y + t * (q.y - p.y), p.z + t * (q.z - p.z)};
              yes.push_back(cut);
              no.push_back(cut);
            }
          }
          if (no.size() >= 3)
          {
            next.push_back(no);
          }
          inside = std::move(yes);
        }
        if (inside.size() >= 3)
        {
          polygons.push_back({inside, 2, {0, 1, 0}});
        }
      }
      pending = std::move(next);
    }
    for (auto p : pending)
    {
      polygons.push_back({p, 0, {0, 1, 0}});
    }
  }
  SplineFaceMesh result;
  std::vector<uint8_t> material;
  std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> vertices;
  auto publish = [&](Position p) {
    auto key = std::make_tuple(int64_t(std::llround(p.x * 1e9)), int64_t(std::llround(p.y * 1e9)), int64_t(std::llround(p.z * 1e9)));
    auto it = vertices.find(key);
    if (it != vertices.end())
    {
      return it->second;
    }
    uint32_t id = result.vertices.size();
    vertices[key] = id;
    result.vertices.push_back(p);
    return id;
  };
  for (auto poly : polygons)
  {
    if (poly.normal.y == 0)
    {
      std::array<uint32_t, 3> t;
      for (int j = 0; j < 3; ++j)
      {
        t[j] = publish(poly.points[j]);
      }
      result.triangles.push_back(t);
      result.expectedNormals.push_back(poly.normal);
      material.push_back(0);
      continue;
    }
    std::vector<Position> points;
    for (auto p : poly.points)
    {
      for (auto panel : panels)
      {
        double h;
        if (triangleHeight(panel.vertices, p.x, p.z, h))
        {
          p.y = h;
          break;
        }
      }
      if (points.empty() || std::hypot(p.x - points.back().x, p.z - points.back().z) > 1e-9)
      {
        points.push_back(p);
      }
    }
    if (points.size() > 1 && std::hypot(points.front().x - points.back().x, points.front().z - points.back().z) < 1e-9)
    {
      points.pop_back();
    }
    if (points.size() < 3)
    {
      continue;
    }
    double area = 0;
    for (size_t k = 1; k + 1 < points.size(); ++k)
    {
      area += signedSide(points[k + 1], points[0], points[k]);
    }
    if (std::abs(area) < 1e-10)
    {
      continue;
    }
    if (points.size() == 3)
    {
      std::array<uint32_t, 3> t = {publish(points[0]), publish(points[1]), publish(points[2])};
      if (area > 0)
      {
        std::swap(t[1], t[2]);
      }
      result.triangles.push_back(t);
      result.expectedNormals.push_back({0, 1, 0});
      material.push_back(poly.role);
      continue;
    }
    std::vector<BoundaryInput> edges;
    PatchDomain domain;
    domain.id = 1;
    for (size_t k = 0; k < points.size(); ++k)
    {
      auto a = publish(points[k]), b = publish(points[(k + 1) % points.size()]);
      auto pa = result.vertices[a], pb = result.vertices[b];
      pa.x -= points[0].x;
      pa.z -= points[0].z;
      pb.x -= points[0].x;
      pb.z -= points[0].z;
      edges.push_back({k + 1, {uint64_t(a) + 1, pa}, {uint64_t(b) + 1, pb}, {}});
      domain.outer.push_back({k + 1, a > b});
    }
    BoundaryRegistry registry;
    PatchMesh mesh;
    if (!registry.commit(edges, error) || !compilePatchMesh(
                                              domain,
                                              registry,
                                              {},
                                              [](double, double) {
                                                return 0.;
                                              },
                                              mesh,
                                              error))
    {
      return false;
    }
    for (auto t : mesh.triangles)
    {
      result.triangles.push_back({uint32_t(mesh.vertices[t[0]].index - 1), uint32_t(mesh.vertices[t[1]].index - 1), uint32_t(mesh.vertices[t[2]].index - 1)});
      result.expectedNormals.push_back({0, 1, 0});
      material.push_back(poly.role);
    }
  }
  out = std::move(result);
  roles = std::move(material);
  return true;
}

bool WorldPhysicalTerrain::build(const WorldSplineLandforms &landforms, BaseSurface base, std::vector<AccessRamp> ramps, const RiverGrowthProduct &rivers, std::vector<RiverReservation> reservations, std::string *error)
{
  auto fail = [&](const char *s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (!base || ramps.size() > 128 || rivers.nodes.size() > 4096)
  {
    return fail("Invalid physical terrain budget or base");
  }
  WorldPhysicalTerrain result;
  result.landforms_ = landforms;
  result.base_ = base;
  result.rivers_ = rivers;
  if (!resolveRiverPaths(rivers, result.paths_))
  {
    return fail("Invalid refined river paths");
  }
  std::sort(ramps.begin(), ramps.end(), [](auto a, auto b) {
    return a.id < b.id;
  });
  std::set<uint64_t> ids;
  for (auto f : landforms.features())
  {
    ids.insert(f.id);
  }
  auto pre = [&](double x, double z) {
    SplineSurfaceSample s;
    if (!landforms.sample(x, z, base, s))
    {
      return std::numeric_limits<double>::quiet_NaN();
    }
    return s.height;
  };
  for (const auto &r : ramps)
  {
    if (!r.id || (!result.ramps_.empty() && result.ramps_.back().id == r.id) || r.stations.size() < 2 || r.stations.size() > 256 || r.fromFeature == r.toFeature || (r.fromFeature && !ids.count(r.fromFeature)) ||
        (r.toFeature && !ids.count(r.toFeature)))
    {
      return fail("Invalid ramp identity or endpoint features");
    }
    for (double v : {r.halfWidthMetres, r.shoulderMetres, r.maximumGrade, r.maximumCrossSlope})
    {
      if (!finite(v))
      {
        return fail("Nonfinite ramp settings");
      }
    }
    if (r.halfWidthMetres <= 0 || r.shoulderMetres <= 0 || r.maximumGrade <= 0 || r.maximumGrade > 1 || r.maximumCrossSlope < 0 || r.maximumCrossSlope > .2)
    {
      return fail("Invalid ramp grade or clearance");
    }
    for (auto p : r.stations)
    {
      if (!finite(p.x) || !finite(p.y) || !finite(p.z))
      {
        return fail("Nonfinite ramp station");
      }
    }
    std::vector<Position> directions, normals, left, right;
    for (size_t k = 1; k < r.stations.size(); ++k)
    {
      auto d = sub(r.stations[k], r.stations[k - 1]);
      double l = length(d);
      if (l < 2 * (r.halfWidthMetres + r.shoulderMetres))
      {
        return fail("Ramp segment too short for corridor clearance");
      }
      if (std::abs(d.y) / l > r.maximumGrade + 1e-10)
      {
        return fail("Ramp horizontal length cannot meet grade");
      }
      directions.push_back({d.x / l, 0, d.z / l});
      normals.push_back({-d.z / l, 0, d.x / l});
    }
    for (size_t k = 0; k < r.stations.size(); ++k)
    {
      Position m = k == 0 ? normals.front() : normals[k - 1];
      if (k && k < normals.size())
      {
        double den = 1 + dot(normals[k - 1], normals[k]);
        if (den < .5)
        {
          return fail("Ramp turn needs an explicit landing");
        }
        m = {(normals[k - 1].x + normals[k].x) / den, 0, (normals[k - 1].z + normals[k].z) / den};
      }
      auto p = r.stations[k];
      left.push_back({p.x + m.x * r.halfWidthMetres, p.y, p.z + m.z * r.halfWidthMetres});
      right.push_back({p.x - m.x * r.halfWidthMetres, p.y, p.z - m.z * r.halfWidthMetres});
    }
    for (size_t k = 1; k < r.stations.size(); ++k)
    {
      for (auto v : std::vector<std::array<Position, 3>>{{left[k - 1], left[k], right[k - 1]}, {right[k - 1], left[k], right[k]}})
      {
        auto u = sub(v[1], v[0]), w = sub(v[2], v[0]);
        double det = u.x * w.z - u.z * w.x;
        if (std::abs(det) < 1e-8)
        {
          return fail("Collapsed ramp panel");
        }
        double gx = (u.y * w.z - u.z * w.y) / det, gz = (u.x * w.y - u.y * w.x) / det;
        double grade = gx * directions[k - 1].x + gz * directions[k - 1].z, cross = gx * normals[k - 1].x + gz * normals[k - 1].z;
        if (std::abs(grade) > r.maximumGrade + 1e-9 || std::abs(cross) > r.maximumCrossSlope + 1e-9)
        {
          return fail("Whole ramp panel exceeds longitudinal or cross grade");
        }
        if (det > 0)
        {
          std::swap(v[1], v[2]);
        }
        result.panels_.push_back({r.id, v, grade, cross});
      }
    }
    for (size_t k = 1; k < r.stations.size(); ++k)
    {
      auto a = r.stations[k - 1], b = r.stations[k];
      double len = length(sub(b, a));
      if (len > 100000)
      {
        return fail("Ramp sampling budget exceeded");
      }
      int count = int(std::ceil(len / 2));
      for (int j = 0; j <= count; ++j)
      {
        for (int side = -2; side <= 2; ++side)
        {
          double t = double(j) / count, lateral = side * (r.halfWidthMetres + r.shoulderMetres) / 2;
          double x = a.x + t * (b.x - a.x) + normals[k - 1].x * lateral, z = a.z + t * (b.z - a.z) + normals[k - 1].z * lateral;
          if (!finite(pre(x, z)))
          {
            return fail("Ramp corridor crosses unsupported terrain");
          }
        }
      }
    }
    // End sections, including shoulders, must join existing ground/cap. This
    // prevents a valid centreline profile from ending in a lateral cliff step.
    for (size_t k : {size_t(0), r.stations.size() - 1})
    {
      auto p = r.stations[k], n = k == 0 ? normals.front() : normals.back();
      for (int j = -4; j <= 4; ++j)
      {
        double lateral = j * (r.halfWidthMetres + r.shoulderMetres) / 4;
        double h = pre(p.x + n.x * lateral, p.z + n.z * lateral);
        if (!finite(h) || std::abs(h - p.y) > .02)
        {
          return fail("Ramp tread/shoulder endpoints do not meet terrain");
        }
      }
      SplineSurfaceSample s;
      landforms.sample(p.x, p.z, base, s);
      uint64_t expected = k == 0 ? r.fromFeature : r.toFeature;
      if (expected ? (s.featureId != expected || s.region != SplineRegion::Cap) : s.influence > 0)
      {
        return fail("Ramp endpoint feature does not match resolved terrain");
      }
    }
    // Conservative corridor spacing protects shoulders and avoids self-crossings.
    for (size_t k = 1; k < r.stations.size(); ++k)
    {
      for (size_t j = k + 2; j < r.stations.size(); ++j)
      {
        double t;
        auto a = r.stations[k - 1], b = r.stations[k], c = r.stations[j - 1], d = r.stations[j];
        double clearance = 2 * (r.halfWidthMetres + r.shoulderMetres);
        if (segmentDistance(a, b, c, d) < clearance)
        {
          return fail("Nonadjacent ramp corridors overlap");
        }
      }
    }
    for (auto &other : result.ramps_)
    {
      for (auto p : r.stations)
      {
        for (size_t k = 1; k < other.stations.size(); ++k)
        {
          double t;
          if (nearest(p.x, p.z, other.stations[k - 1], other.stations[k], t) < r.halfWidthMetres + r.shoulderMetres + other.halfWidthMetres + other.shoulderMetres)
          {
            return fail("Ramp corridors require a designed junction");
          }
        }
      }
    }
    result.ramps_.push_back(r);
  }
  std::map<uint32_t, RiverReservation> reserved;
  for (auto r : reservations)
  {
    if (r.upstreamNode >= rivers.nodes.size() || rivers.nodes[r.upstreamNode].receiver == UINT32_MAX || reserved.count(r.upstreamNode))
    {
      return fail("Unknown or duplicate river reservation");
    }
    for (double v : {r.halfWidthMetres, r.bankTransitionMetres, r.depthMetres, r.maximumCutMetres})
    {
      if (!finite(v) || v <= 0)
      {
        return fail("Invalid river reservation dimensions");
      }
    }
    reserved.emplace(r.upstreamNode, r);
  }
  for (size_t i = 0; i < rivers.nodes.size(); ++i)
  {
    auto n = rivers.nodes[i];
    if (!finite(n.position.x) || !finite(n.position.y) || !finite(n.position.z))
    {
      return fail("Invalid fixed river node");
    }
    if (n.receiver == UINT32_MAX)
    {
      continue;
    }
    if (n.receiver >= i)
    {
      return fail("Fixed receiver graph must remain downstream-first");
    }
    const auto &path = result.paths_[i];
    for (size_t segment = 1; segment < path.points.size(); ++segment)
    {
      n.position = path.points[segment - 1];
      auto to = path.points[segment];
      double l = length(sub(to, n.position));
      if (l < 1e-6 || l > 100000 || n.position.y < to.y)
      {
        return fail("Blocked/uphill river outlet or invalid reach");
      }
      auto it = reserved.find(uint32_t(i));
      if (it != reserved.end())
      {
        for (auto panel : result.panels_)
        {
          double h;
          bool intersects = triangleHeight(panel.vertices, n.position.x, n.position.z, h) || triangleHeight(panel.vertices, to.x, to.z, h);
          double clearance = it->second.halfWidthMetres + it->second.bankTransitionMetres;
          for (auto ramp : ramps)
          {
            if (ramp.id == panel.rampId)
            {
              clearance += ramp.shoulderMetres;
            }
          }
          for (int side = 0; side < 3; ++side)
          {
            intersects |= segmentDistance(n.position, to, panel.vertices[side], panel.vertices[(side + 1) % 3]) < clearance;
          }
          if (intersects)
          {
            return fail("Ramp intersects protected river corridor");
          }
        }
      }
      int count = int(std::ceil(l / 2));
      for (int k = 0; k <= count; ++k)
      {
        double t = double(k) / count;
        auto p = Position{n.position.x + (to.x - n.position.x) * t, n.position.y + (to.y - n.position.y) * t, n.position.z + (to.z - n.position.z) * t};
        SplineSurfaceSample original;
        if (!landforms.sample(p.x, p.z, base, original))
        {
          return fail("Unsupported river corridor");
        }
        if (original.influence > 0 && original.height > p.y && it == reserved.end())
        {
          return fail("Landform obstructs fixed river without a canyon reservation");
        }
        if (it != reserved.end())
        {
          auto r = it->second;
          if (original.height - (p.y - r.depthMetres) > r.maximumCutMetres)
          {
            return fail("Reserved canyon exceeds supported cut depth");
          }
          for (auto ramp : ramps)
          {
            for (size_t j = 1; j < ramp.stations.size(); ++j)
            {
              double u;
              if (nearest(p.x, p.z, ramp.stations[j - 1], ramp.stations[j], u) < r.halfWidthMetres + r.bankTransitionMetres + ramp.halfWidthMetres + ramp.shoulderMetres)
              {
                return fail("Ramp intersects river reservation; bridge/ford required");
              }
            }
          }
        }
      }
    }
  }
  for (auto [id, r] : reserved)
  {
    result.reservations_.push_back(r);
  }
  result.reachable_.insert(0);
  bool changed = true;
  while (changed)
  {
    changed = false;
    for (auto r : ramps)
    {
      if (result.reachable_.count(r.fromFeature) && !result.reachable_.count(r.toFeature))
      {
        result.reachable_.insert(r.toFeature);
        changed = true;
      }
      else if (result.reachable_.count(r.toFeature) && !result.reachable_.count(r.fromFeature))
      {
        result.reachable_.insert(r.fromFeature);
        changed = true;
      }
    }
  }
  *this = std::move(result);
  return true;
}

double WorldPhysicalTerrain::modify(double x, double z, double h, PhysicalSample &out) const
{
  for (auto r : ramps_)
  {
    bool tread = false;
    for (auto p : panels_)
    {
      if (p.rampId == r.id)
      {
        double design;
        if (triangleHeight(p.vertices, x, z, design))
        {
          h = design;
          out.rampId = r.id;
          tread = true;
          break;
        }
      }
    }
    if (tread)
    {
      continue;
    }
    double best = 1e100, design = 0;
    for (auto panel : panels_)
    {
      if (panel.rampId == r.id)
      {
        for (int edge = 0; edge < 3; ++edge)
        {
          auto a = panel.vertices[edge], b = panel.vertices[(edge + 1) % 3];
          double t, d = nearest(x, z, a, b, t);
          if (d < best)
          {
            best = d;
            design = a.y + t * (b.y - a.y);
          }
        }
      }
    }
    if (best < r.shoulderMetres)
    {
      double w = 1 - smooth(best / r.shoulderMetres);
      h += (design - h) * w;
      out.rampId = r.id;
    }
  }
  for (auto r : reservations_)
  {
    const auto &path = paths_[r.upstreamNode];
    double station = 0, d = path.project(x, z, station);
    if (d >= r.halfWidthMetres + r.bankTransitionMetres)
    {
      continue;
    }
    double bed = path.at(station).y - r.depthMetres;
    double w = 1 - smooth((d - r.halfWidthMetres) / r.bankTransitionMetres);
    h -= std::max(0., h - bed) * w;
    out.protectedRiver = true;
  }
  out.height = h;
  return h;
}

bool WorldPhysicalTerrain::sample(double x, double z, PhysicalSample &out) const
{
  SplineSurfaceSample s;
  if (!landforms_.sample(x, z, base_, s))
  {
    return false;
  }
  PhysicalSample result;
  result.region = s.region;
  modify(x, z, s.height, result);
  if (!finite(result.height))
  {
    return false;
  }
  out = result;
  return true;
}

bool WorldPhysicalTerrain::verticalFaces(SplineFaceMesh &out, std::string *error) const
{
  SplineFaceMesh original;
  if (!landforms_.verticalFaces(base_, original, error))
  {
    return false;
  }
  SplineFaceMesh result;
  // Split the original strips at <=1 m stations before cutting both limits.
  // Samples retain two heights at one map location, never a skinny heightfield.
  for (size_t i = 0; i + 3 < original.vertices.size(); i += 4)
  {
    auto a = original.vertices[i], al = original.vertices[i + 1], b = original.vertices[i + 2], bl = original.vertices[i + 3];
    int count = std::max(1, int(std::ceil(length(sub(b, a)))));
    Position normal{b.z - a.z, 0, a.x - b.x};
    double l = length(normal);
    normal.x /= l;
    normal.z /= l;
    // Original winding chooses the exposed side, including right-high strokes.
    for (size_t j = 0; j < original.triangles.size(); ++j)
    {
      if (original.triangles[j][0] >= i && original.triangles[j][0] < i + 4)
      {
        normal = original.expectedNormals[j];
        break;
      }
    }
    for (int k = 0; k < count; ++k)
    {
      std::array<Position, 4> v;
      for (int end = 0; end < 2; ++end)
      {
        double t = double(k + end) / count;
        Position top{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)}, low{top.x, al.y + t * (bl.y - al.y), top.z};
        PhysicalSample sample;
        top.y = modify(top.x, top.z, top.y, sample);
        low.y = modify(low.x, low.z, low.y, sample);
        if (top.y < low.y)
        {
          top.y = low.y;
        }
        v[end * 2] = top;
        v[end * 2 + 1] = low;
      }
      uint32_t offset = result.vertices.size();
      result.vertices.insert(result.vertices.end(), v.begin(), v.end());
      for (auto t : std::vector<std::array<uint32_t, 3>>{{0, 1, 2}, {2, 1, 3}})
      {
        auto u = sub(v[t[1]], v[t[0]]), w = sub(v[t[2]], v[t[0]]);
        double nx = u.y * w.z - u.z * w.y, nz = u.x * w.y - u.y * w.x;
        if (std::hypot(nx, nz) < 1e-8)
        {
          continue;
        }
        if (nx * normal.x + nz * normal.z < 0)
        {
          std::swap(t[1], t[2]);
        }
        result.triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
        result.expectedNormals.push_back(normal);
      }
    }
  }
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
