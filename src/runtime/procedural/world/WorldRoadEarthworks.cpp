#include "WorldRoadEarthworks.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace procedural::world::rewrite {
namespace {
bool fail(std::string *e, const char *t)
{
  if (e)
  {
    *e = t;
  }
  return false;
}

double side(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

double length(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}

SurfacePlane plane(const std::array<Position, 3> &p)
{
  auto a = p[0], b = p[1], c = p[2];
  double d = side(a, b, c);
  SurfacePlane s;
  s.x = ((b.y - a.y) * (c.z - a.z) - (c.y - a.y) * (b.z - a.z)) / d;
  s.z = ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) / d;
  s.constant = a.y - s.x * a.x - s.z * a.z;
  return s;
}

Position mix(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
} // namespace

bool generateRoadEarthworks(
    const RoadNetworkProduct &network,
    const RoadField &terrain,
    const RoadWaterField &water,
    const std::vector<SurfaceContour> &seeds,
    const std::vector<BridgeSpan> &bridges,
    const RoadEarthworkSettings &s,
    RoadEarthworkProduct &out,
    std::string *error)
{
  if (!network.dependencyRevision || !terrain || !water || network.roads.empty() || network.roads.size() > 16 || bridges.size() > 16)
  {
    return fail(error, "invalid earthwork network or field");
  }
  for (double v : {s.maximumReachMetres, s.rootSpacingMetres, s.cutSlope, s.fillSlope, s.maximumAbutmentHeightMetres})
  {
    if (!std::isfinite(v) || v <= 0)
    {
      return fail(error, "invalid earthwork settings");
    }
  }
  if (s.maximumReachMetres > 100 || s.rootSpacingMetres < .1 || s.rootSpacingMetres > 1 || s.cutSlope > 4 || s.fillSlope > 4 || s.maximumAbutmentHeightMetres > 20)
  {
    return fail(error, "earthwork budget exceeded");
  }
  if (!std::isfinite(s.ditchDepthMetres) || !std::isfinite(s.ditchWidthMetres) || s.ditchDepthMetres < 0 || s.ditchDepthMetres > 1 || s.ditchWidthMetres <= 0 || s.ditchWidthMetres > 3)
  {
    return fail(error, "invalid road ditch settings");
  }
  RoadEarthworkProduct result;
  result.contours = seeds;
  uint64_t nextId = 1;
  for (auto &c : seeds)
  {
    nextId = std::max(nextId, c.id + 1);
  }
  std::vector<SurfaceContour> transitions, shoulders, treads, decks;
  auto triangle = [&](std::array<Position, 3> p, SurfaceRole role, uint32_t material, uint32_t layer, std::vector<SurfaceContour> &destination) {
    if (std::abs(side(p[0], p[1], p[2])) < 1e-9)
    {
      return;
    }
    SurfaceContour c;
    c.id = nextId++;
    c.role = role;
    c.material = material;
    c.layer = layer;
    c.terrainSurface = false;
    c.plane = plane(p);
    if (side(p[0], p[1], p[2]) < 0)
    {
      std::swap(p[1], p[2]);
    }
    c.loop.assign(p.begin(), p.end());
    destination.push_back(std::move(c));
  };
  auto protectedTriangle = [&](const std::array<Position, 3> &p) {
    int n = std::max(1, int(std::ceil(std::max({length(p[0], p[1]), length(p[1], p[2]), length(p[2], p[0])}))));
    for (int a = 0; a <= n; ++a)
    {
      for (int b = 0; b <= n - a; ++b)
      {
        double u = double(a) / n, v = double(b) / n;
        Position q{p[0].x * u + p[1].x * v + p[2].x * (1 - u - v), 0, p[0].z * u + p[1].z * v + p[2].z * (1 - u - v)};
        if (water(q.x, q.z).protectedChannel)
        {
          return true;
        }
      }
    }
    return false;
  };
  std::vector<std::vector<bool>> elevated(network.roads.size());
  for (size_t r = 0; r < network.roads.size(); ++r)
  {
    auto &road = network.roads[r].design;
    if (road.sections.size() < 2 || road.sections.size() > 256 || road.triangles.size() > 2048)
    {
      return fail(error, "invalid earthwork road section budget");
    }
    for (auto &section : road.sections)
    {
      if (!std::isfinite(section.distanceMetres))
      {
        return fail(error, "nonfinite road station");
      }
      for (auto p : section.points)
      {
        for (double v : {p.x, p.y, p.z})
        {
          if (!std::isfinite(v) || std::abs(v) > 1e6)
          {
            return fail(error, "nonfinite or unbounded road section");
          }
        }
      }
    }
    for (auto &t : road.triangles)
    {
      for (auto p : t.points)
      {
        for (double v : {p.x, p.y, p.z})
        {
          if (!std::isfinite(v) || std::abs(v) > 1e6)
          {
            return fail(error, "nonfinite road triangle");
          }
        }
      }
      for (int k = 0; k < 3; ++k)
      {
        if (length(t.points[k], t.points[(k + 1) % 3]) > 60)
        {
          return fail(error, "road panel exceeds sampling budget");
        }
      }
      if (std::abs(side(t.points[0], t.points[1], t.points[2])) < 1e-9)
      {
        return fail(error, "degenerate road triangle");
      }
    }
    elevated[r].resize(road.sections.size() - 1, false);
  }
  for (auto &span : bridges)
  {
    if (span.road >= network.roads.size())
    {
      return fail(error, "bridge road index invalid");
    }
    auto &road = network.roads[span.road].design;
    if (span.firstStation >= span.lastStation || span.lastStation >= road.sections.size() || !std::isfinite(span.deckThicknessMetres) || span.deckThicknessMetres <= 0 || span.deckThicknessMetres > 3 ||
        !std::isfinite(span.minimumWaterClearanceMetres) || span.minimumWaterClearanceMetres < 0 || !std::isfinite(span.maximumSpanMetres) || span.maximumSpanMetres <= 0 || span.maximumSpanMetres > 100)
    {
      return fail(error, "invalid bridge span");
    }
    if (road.sections[span.lastStation].distanceMetres - road.sections[span.firstStation].distanceMetres > span.maximumSpanMetres)
    {
      return fail(error, "bridge span exceeds structural preset");
    }
    for (uint32_t i = span.firstStation; i < span.lastStation; ++i)
    {
      if (elevated[span.road][i])
      {
        return fail(error, "overlapping bridge spans");
      }
      elevated[span.road][i] = true;
    }
    // Two dry end abutments: no piers or fill can occupy the channel.
    for (uint32_t index : {span.firstStation, span.lastStation})
    {
      auto &section = road.sections[index];
      auto a = section.points.front(), b = section.points.back();
      int count = int(std::ceil(length(a, b)));
      for (int k = 0; k <= count; ++k)
      {
        auto p = mix(a, b, double(k) / count);
        double h = terrain(p.x, p.z);
        if (!std::isfinite(h) || water(p.x, p.z).protectedChannel || p.y < h - s.map.seamToleranceMetres || p.y - h > s.maximumAbutmentHeightMetres)
        {
          return fail(error, "bridge requires supported dry abutments");
        }
      }
      // The connecting face reaches a shallow footing beneath natural terrain.
      for (size_t k = 1; k < section.points.size(); ++k)
      {
        auto pa = section.points[k - 1], pb = section.points[k];
        Position lowA{pa.x, terrain(pa.x, pa.z) - .5, pa.z}, lowB{pb.x, terrain(pb.x, pb.z) - .5, pb.z};
        result.structures.push_back({{pa, pb, lowB}});
        result.structures.push_back({{pa, lowB, lowA}});
      }
    }
    std::map<std::pair<uint32_t, uint32_t>, std::pair<int, std::array<Position, 2>>> perimeter;
    bool crossesWater = false;
    for (auto &t : road.triangles)
    {
      if (t.panel < span.firstStation || t.panel >= span.lastStation)
      {
        continue;
      }
      triangle(t.points, SurfaceRole::Bridge, 7, 2, decks);
      auto lower = t.points;
      for (auto &p : lower)
      {
        p.y -= span.deckThicknessMetres;
      }
      std::swap(lower[1], lower[2]);
      result.structures.push_back({lower});
      for (int i = 0; i < 3; ++i)
      {
        auto key = std::minmax(t.vertexIds[i], t.vertexIds[(i + 1) % 3]);
        auto &edge = perimeter[key];
        edge.first++;
        edge.second = {t.points[i], t.points[(i + 1) % 3]};
      }
      int samples = int(std::ceil(std::max({length(t.points[0], t.points[1]), length(t.points[1], t.points[2]), length(t.points[2], t.points[0])})));
      for (int a = 0; a <= samples; ++a)
      {
        for (int b = 0; b <= samples - a; ++b)
        {
          double u = double(a) / samples, v = double(b) / samples;
          Position p{
            t.points[0].x * u + t.points[1].x * v + t.points[2].x * (1 - u - v), t.points[0].y * u + t.points[1].y * v + t.points[2].y * (1 - u - v), t.points[0].z * u + t.points[1].z * v + t.points[2].z * (1 - u - v)};
          double groundHeight = terrain(p.x, p.z);
          if (!std::isfinite(groundHeight) || p.y - span.deckThicknessMetres <= groundHeight + s.map.seamToleranceMetres)
          {
            return fail(error, "bridge deck requires clearance above ground between its dry supports");
          }
          auto w = water(p.x, p.z);
          if (w.protectedChannel)
          {
            crossesWater = true;
            if (!std::isfinite(w.waterHeight) || p.y - span.deckThicknessMetres - w.waterHeight < span.minimumWaterClearanceMetres)
            {
              return fail(error, "bridge soffit lacks water clearance");
            }
          }
        }
      }
    }
    if (!crossesWater)
    {
      return fail(error, "declared bridge does not cross protected channel");
    }
    for (auto &[key, e] : perimeter)
    {
      if (e.first == 1)
      {
        // Abutments own the complete transverse end face, including the slab depth.
        if (key.first / 5 == key.second / 5 && (key.first / 5 == span.firstStation || key.first / 5 == span.lastStation))
        {
          continue;
        }
        auto a = e.second[0], b = e.second[1], c = b, d = a;
        c.y -= span.deckThicknessMetres;
        d.y -= span.deckThicknessMetres;
        result.structures.push_back({{a, c, b}});
        result.structures.push_back({{a, d, c}});
      }
    }
    ++result.bridges;
  }
  for (size_t r = 0; r < network.roads.size(); ++r)
  {
    auto &road = network.roads[r].design;
    for (auto &t : road.triangles)
    {
      if (t.panel >= elevated[r].size())
      {
        return fail(error, "invalid road panel index");
      }
      if (elevated[r][t.panel])
      {
        continue;
      }
      if (protectedTriangle(t.points))
      {
        return fail(error, "ground road intersects protected channel without bridge");
      }
      triangle(t.points, t.shoulder ? SurfaceRole::Shoulder : SurfaceRole::Road, t.shoulder ? 2 : 1, 0, t.shoulder ? shoulders : treads);
    }
    // Each contiguous ground interval gets its own closed excavation contour.
    for (size_t begin = 0; begin < elevated[r].size();)
    {
      if (elevated[r][begin])
      {
        ++begin;
        continue;
      }
      size_t end = begin + 1;
      while (end < elevated[r].size() && !elevated[r][end])
      {
        ++end;
      }
      std::vector<Position> ring;
      for (size_t i = begin; i <= end; ++i)
      {
        ring.push_back(road.sections[i].points[0]);
      }
      for (size_t k = 1; k < 5; ++k)
      {
        ring.push_back(road.sections[end].points[k]);
      }
      for (size_t i = end; i-- > begin;)
      {
        ring.push_back(road.sections[i].points[4]);
      }
      for (int k = 3; k > 0; --k)
      {
        ring.push_back(road.sections[begin].points[k]);
      }
      // Insert exact ground-contact points where a boundary changes cut/fill sign.
      std::vector<Position> split;
      for (size_t i = 0; i < ring.size(); ++i)
      {
        auto a = ring[i], b = ring[(i + 1) % ring.size()];
        split.push_back(a);
        double da = a.y - terrain(a.x, a.z), db = b.y - terrain(b.x, b.z);
        if (!std::isfinite(da) || !std::isfinite(db))
        {
          return fail(error, "unsupported road boundary terrain");
        }
        if (da * db < -1e-12)
        {
          double lo = 0, hi = 1;
          for (int pass = 0; pass < 48; ++pass)
          {
            double mid = (lo + hi) * .5;
            auto p = mix(a, b, mid);
            if ((p.y - terrain(p.x, p.z) > 0) == (da > 0))
            {
              lo = mid;
            }
            else
            {
              hi = mid;
            }
          }
          split.push_back(mix(a, b, (lo + hi) * .5));
        }
      }
      ring = std::move(split);
      std::vector<Position> outer(ring.size()), toe = ring;
      for (size_t i = 0; i < ring.size(); ++i)
      {
        auto p = ring[i], a = ring[(i + ring.size() - 1) % ring.size()], b = ring[(i + 1) % ring.size()];
        double al = length(a, p), bl = length(p, b);
        if (al < 1e-7 || bl < 1e-7)
        {
          return fail(error, "degenerate excavation boundary");
        }
        double nx = (p.z - a.z) / al + (b.z - p.z) / bl, nz = -(p.x - a.x) / al - (b.x - p.x) / bl, nl = std::hypot(nx, nz);
        if (nl < 1e-7)
        {
          return fail(error, "reversing excavation corner");
        }
        nx /= nl;
        nz /= nl;
        double difference = terrain(p.x, p.z) - p.y;
        if (std::abs(difference) < 1e-7)
        {
          outer[i] = p;
          continue;
        }
        double ditchWidth = 0;
        if (difference > 0 && s.ditchDepthMetres > 0)
        {
          ditchWidth = s.ditchWidthMetres;
          p.x += nx * ditchWidth;
          p.z += nz * ditchWidth;
          p.y -= s.ditchDepthMetres;
          toe[i] = p;
          difference = terrain(p.x, p.z) - p.y;
        }
        double slope = difference > 0 ? s.cutSlope : -s.fillSlope;
        auto residual = [&](double d) {
          return terrain(p.x + nx * d, p.z + nz * d) - (p.y + slope * d);
        };
        double previous = 0, root = -1;
        for (double d = s.rootSpacingMetres; d <= s.maximumReachMetres - ditchWidth + 1e-8; d += s.rootSpacingMetres)
        {
          double value = residual(d);
          if (!std::isfinite(value))
          {
            return fail(error, "unsupported excavation terrain");
          }
          if (value * difference <= 0)
          {
            double lo = previous, hi = d;
            for (int k = 0; k < 48; ++k)
            {
              double mid = (lo + hi) * .5;
              if (residual(mid) * difference > 0)
              {
                lo = mid;
              }
              else
              {
                hi = mid;
              }
            }
            root = (lo + hi) * .5;
            break;
          }
          previous = d;
        }
        if (root < 0)
        {
          return fail(error, "cut or fill cannot meet terrain within reserved reach");
        }
        outer[i] = {p.x + nx * root, 0, p.z + nz * root};
        outer[i].y = terrain(outer[i].x, outer[i].z);
        result.maximumReachMetres = std::max(result.maximumReachMetres, root + ditchWidth);
      }
      for (size_t i = 0; i < ring.size(); ++i)
      {
        size_t j = (i + 1) % ring.size();
        for (auto tri : std::vector<std::array<Position, 3>>{{ring[i], toe[i], toe[j]}, {ring[i], toe[j], ring[j]}})
        {
          if (std::abs(side(tri[0], tri[1], tri[2])) < 1e-9)
          {
            continue;
          }
          if (protectedTriangle(tri))
          {
            return fail(error, "ditch intersects protected channel");
          }
          triangle(tri, SurfaceRole::Ditch, 10, 0, transitions);
        }
        for (auto tri : std::vector<std::array<Position, 3>>{{toe[i], outer[i], outer[j]}, {toe[i], outer[j], toe[j]}})
        {
          if (std::abs(side(tri[0], tri[1], tri[2])) < 1e-9)
          {
            continue;
          }
          if (protectedTriangle(tri))
          {
            return fail(error, "cut or fill would alter protected channel");
          }
          Position center{(tri[0].x + tri[1].x + tri[2].x) / 3, (tri[0].y + tri[1].y + tri[2].y) / 3, (tri[0].z + tri[1].z + tri[2].z) / 3};
          bool cut = center.y < terrain(center.x, center.z);
          triangle(tri, cut ? SurfaceRole::Cut : SurfaceRole::Fill, cut ? 3 : 4, 0, transitions);
        }
      }
      begin = end;
    }
  }
  for (auto *list : {&transitions, &shoulders, &treads, &decks})
  {
    result.contours.insert(result.contours.end(), list->begin(), list->end());
  }
  if (!arrangeSurfaceContours(result.contours, s.map, terrain, network.dependencyRevision, result.map, error))
  {
    return false;
  }
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
