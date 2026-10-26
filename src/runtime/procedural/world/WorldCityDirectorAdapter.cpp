#include "WorldCityDirectorAdapter.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
namespace procedural::world::rewrite
{
namespace
{
using Poly = std::vector<Position>;
double cross(Position a, Position b)
{
  return a.x * b.z - a.z * b.x;
}
Position delta(Position a, Position b)
{
  return {a.x - b.x, 0, a.z - b.z};
}
double length(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}
Position mix(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}
bool inside(const Poly &p, Position q)
{
  for (size_t i = 0; i < p.size(); ++i)
    if (cross(delta(p[(i + 1) % p.size()], p[i]), delta(q, p[i])) < -1e-7)
      return false;
  return !p.empty();
}
} // namespace
bool prepareCityDirector(const CityLayout &city, const SettlementDirectorInput &policy, SettlementDirectorInput &out, std::string *error)
{
  auto fail = [&](const char *m)
  {
    if (error)
      *error = m;
    return false;
  };
  if (!city.revision || !policy.settlement || city.objects.size() > 4096 || city.streets.size() > 2048 || city.walls.size() > 2048 || city.towers.size() > 512)
    return fail("invalid city director adapter budget");
  struct Segment
  {
    uint64_t street;
    Position a, b;
    std::vector<double> cuts;
  };
  std::vector<Segment> segments;
  std::vector<Poly> barriers;
  for (auto &w : city.walls)
  {
    double l = length(w.a, w.b);
    if (!std::isfinite(l) || l < 1e-6 || !std::isfinite(w.halfWidth) || w.halfWidth <= 0)
      return fail("invalid city wall");
    double x = -(w.b.z - w.a.z) / l * (w.halfWidth + .35), z = (w.b.x - w.a.x) / l * (w.halfWidth + .35);
    barriers.push_back({{w.a.x - x, 0, w.a.z - z}, {w.b.x - x, 0, w.b.z - z}, {w.b.x + x, 0, w.b.z + z}, {w.a.x + x, 0, w.a.z + z}});
  }
  for (auto &t : city.towers)
  {
    double x0 = INFINITY, z0 = INFINITY, x1 = -INFINITY, z1 = -INFINITY;
    for (auto p : t.footprint)
    {
      x0 = std::min(x0, p.x);
      z0 = std::min(z0, p.z);
      x1 = std::max(x1, p.x);
      z1 = std::max(z1, p.z);
    }
    if (!std::isfinite(x0))
      return fail("invalid city tower");
    barriers.push_back({{x0 - .35, 0, z0 - .35}, {x1 + .35, 0, z0 - .35}, {x1 + .35, 0, z1 + .35}, {x0 - .35, 0, z1 + .35}});
  }
  for (auto &s : city.streets)
  {
    for (auto p : s.path)
      for (double v : {p.x, p.y, p.z})
        if (!std::isfinite(v) || std::abs(v) > 1e6)
          return fail("invalid city street coordinate");
    if (s.path.size() < 2)
      return fail("city street lacks accepted path");
    for (size_t i = 1; i < s.path.size(); ++i)
      if (length(s.path[i - 1], s.path[i]) > 1e-6)
        segments.push_back({s.id, s.path[i - 1], s.path[i], {0, 1}});
  }
  if (segments.size() > 4096 || segments.empty())
    return fail("city walking segment budget exceeded");
  auto intersection = [&](Segment &a, Position c, Position d, Segment *other)
  {
    auto u = delta(a.b, a.a), v = delta(d, c), w = delta(c, a.a);
    double denom = cross(u, v);
    if (std::abs(denom) < 1e-9)
    {
      for (auto p : {c, d})
      {
        double l2 = u.x * u.x + u.z * u.z, t = ((p.x - a.a.x) * u.x + (p.z - a.a.z) * u.z) / l2;
        if (t > 0 && t < 1 && std::abs(cross(u, delta(p, a.a))) < 1e-5)
          a.cuts.push_back(t);
      }
      return;
    }
    double t = cross(w, v) / denom, q = cross(w, u) / denom;
    if (t >= -1e-7 && t <= 1 + 1e-7 && q >= -1e-7 && q <= 1 + 1e-7)
    {
      a.cuts.push_back(std::clamp(t, 0., 1.));
      if (other)
        other->cuts.push_back(std::clamp(q, 0., 1.));
    }
  };
  for (size_t i = 0; i < segments.size(); ++i)
  {
    for (size_t j = 0; j < i; ++j)
    {
      intersection(segments[i], segments[j].a, segments[j].b, &segments[j]);
      intersection(segments[j], segments[i].a, segments[i].b, &segments[i]);
    }
    for (auto &p : barriers)
      for (size_t k = 0; k < p.size(); ++k)
        intersection(segments[i], p[k], p[(k + 1) % p.size()], nullptr);
  }
  struct Attachment
  {
    const CityObject *object;
    size_t segment;
    double t;
  };
  std::vector<Attachment> attachments;
  for (auto &o : city.objects)
  {
    for (auto p : {o.entrance, o.access})
      for (double v : {p.x, p.y, p.z})
        if (!std::isfinite(v) || std::abs(v) > 1e6)
          return fail("invalid city attachment coordinate");
    if ((o.kind != CityObjectKind::House && o.kind != CityObjectKind::Workshop) || o.parts.size() != 1 || !o.courtyard.empty())
      continue;
    double best = 1e30, t = 0;
    size_t selected = segments.size();
    for (size_t i = 0; i < segments.size(); ++i)
    {
      auto &s = segments[i];
      if (s.street != o.street)
        continue;
      auto d = delta(s.b, s.a);
      double u = std::clamp(((o.access.x - s.a.x) * d.x + (o.access.z - s.a.z) * d.z) / (d.x * d.x + d.z * d.z), 0., 1.);
      double distance = length(o.access, mix(s.a, s.b, u));
      if (distance < best)
      {
        best = distance;
        t = u;
        selected = i;
      }
    }
    if (selected == segments.size() || best > 1e-4)
      return fail("building attachment is not on its accepted street");
    segments[selected].cuts.push_back(t);
    attachments.push_back({&o, selected, t});
  }
  auto result = policy;
  result.sites.clear();
  result.nodes.clear();
  result.edges.clear();
  std::map<std::pair<int64_t, int64_t>, uint64_t> ids;
  std::set<uint64_t> usedIds;
  auto node = [&](Position p)
  {
    auto key = std::make_pair(int64_t(std::llround(p.x * 1e5)), int64_t(std::llround(p.z * 1e5)));
    auto it = ids.find(key);
    if (it != ids.end())
      return it->second;
    uint64_t id = featureSeed(policy.settlement, 1, uint64_t(key.first), uint64_t(key.second));
    if (!id || !usedIds.insert(id).second)
      return uint64_t(0);
    ids[key] = id;
    result.nodes.push_back({id, p});
    return id;
  };
  std::set<std::pair<uint64_t, uint64_t>> published;
  std::set<uint64_t> connected;
  auto edge = [&](Position a, Position b)
  {
    if (length(a, b) < 1e-5)
      return true;
    auto x = node(a), y = node(b);
    if (!x || !y)
      return false;
    if (x == y)
      return true;
    auto key = std::minmax(x, y);
    if (published.insert(key).second)
      result.edges.push_back({x, y, length(a, b)});
    connected.insert(x);
    connected.insert(y);
    return true;
  };
  for (auto &s : segments)
  {
    std::sort(s.cuts.begin(), s.cuts.end());
    s.cuts.erase(
        std::unique(
            s.cuts.begin(),
            s.cuts.end(),
            [](double a, double b)
            {
              return std::abs(a - b) < 1e-9;
            }),
        s.cuts.end());
    for (size_t k = 1; k < s.cuts.size(); ++k)
    {
      auto a = mix(s.a, s.b, s.cuts[k - 1]), b = mix(s.a, s.b, s.cuts[k]);
      bool clear = true;
      for (auto &p : barriers)
        if (inside(p, mix(a, b, .5)))
          clear = false;
      if (clear && !edge(a, b))
        return fail("walking node identity collision");
    }
  }
  for (auto a : attachments)
  {
    auto &o = *a.object;
    auto at = mix(segments[a.segment].a, segments[a.segment].b, a.t);
    auto streetNode = node(at);
    if (!connected.count(streetNode))
      continue;
    bool clear = true;
    Segment approach{0, o.entrance, o.access, {0, 1}};
    for (auto &p : barriers)
    {
      if (inside(p, o.entrance) || inside(p, o.access))
        clear = false;
      if (length(o.entrance, o.access) > 1e-6)
        for (size_t k = 0; k < p.size(); ++k)
          intersection(approach, p[k], p[(k + 1) % p.size()], nullptr);
    }
    std::sort(approach.cuts.begin(), approach.cuts.end());
    for (size_t k = 1; k < approach.cuts.size(); ++k)
      for (auto &p : barriers)
        if (inside(p, mix(approach.a, approach.b, (approach.cuts[k - 1] + approach.cuts[k]) * .5)))
          clear = false;
    if (!clear)
      continue;
    if (!edge(o.entrance, o.access) || !edge(o.access, at))
      return fail("entrance node identity collision");
    auto supplied = std::find_if(
        policy.sites.begin(),
        policy.sites.end(),
        [&](auto &s)
        {
          return s.id == o.id;
        });
    if (supplied == policy.sites.end())
      continue;
    SettlementSite site = *supplied;
    site.id = o.id;
    site.node = node(o.entrance);
    site.shell.id = o.id;
    site.shell.shell = o.parts.front().footprint;
    site.shell.entrance = o.entrance;
    site.shell.foundation = o.foundation;
    site.shell.availableHeight = o.parts.front().height;
    site.shell.floorRise = std::min(2.8, site.shell.availableHeight);
    site.allowedKinds &= (1u << unsigned(ProgrammeKind::Home)) | (1u << unsigned(ProgrammeKind::Shop)) | (1u << unsigned(ProgrammeKind::Collective)) | (1u << unsigned(ProgrammeKind::Warehouse));
    result.sites.push_back(std::move(site));
  }
  if (result.nodes.size() > 8192 || result.edges.size() > 32768)
    return fail("city walking network exceeds director budget");
  std::sort(
      result.nodes.begin(),
      result.nodes.end(),
      [](auto &a, auto &b)
      {
        return a.id < b.id;
      });
  std::sort(
      result.sites.begin(),
      result.sites.end(),
      [](auto &a, auto &b)
      {
        return a.id < b.id;
      });
  std::sort(
      result.edges.begin(),
      result.edges.end(),
      [](auto &a, auto &b)
      {
        return std::tie(a.a, a.b) < std::tie(b.a, b.b);
      });
  if (error)
    error->clear();
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
