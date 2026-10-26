#include "WorldSplineLandforms.hpp"
#include "procedural/geometry/TerrainLandforms.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
namespace procedural::world::rewrite
{
namespace
{
Position add(Position a, Position b)
{
  return {a.x + b.x, 0, a.z + b.z};
}
Position mul(Position a, double s)
{
  return {a.x * s, 0, a.z * s};
}
Position sub(Position a, Position b)
{
  return add(a, mul(b, -1));
}
double dot(Position a, Position b)
{
  return a.x * b.x + a.z * b.z;
}
double len(Position a)
{
  return std::hypot(a.x, a.z);
}
double cross(Position a, Position b)
{
  return a.x * b.z - a.z * b.x;
}
double smooth(double t)
{
  t = std::clamp(t, 0., 1.);
  return t * t * (3 - 2 * t);
}
double distance(Position p, Position a, Position b, double *parameter = nullptr)
{
  auto d = sub(b, a);
  double t = std::clamp(dot(sub(p, a), d) / dot(d, d), 0., 1.);
  if (parameter)
    *parameter = t;
  return len(sub(p, add(a, mul(d, t))));
}
bool contact(Position a, Position b, Position c, Position d)
{
  if (std::max(a.x, b.x) + 1e-8 < std::min(c.x, d.x) || std::max(c.x, d.x) + 1e-8 < std::min(a.x, b.x) || std::max(a.z, b.z) + 1e-8 < std::min(c.z, d.z) || std::max(c.z, d.z) + 1e-8 < std::min(a.z, b.z))
    return false;
  double p = cross(sub(b, a), sub(c, a)), q = cross(sub(b, a), sub(d, a)), r = cross(sub(d, c), sub(a, c)), s = cross(sub(d, c), sub(b, c));
  return ((p <= 0 && q >= 0) || (p >= 0 && q <= 0)) && ((r <= 0 && s >= 0) || (r >= 0 && s <= 0));
}
bool inside(Position p, const std::vector<Position> &v)
{
  bool yes = false;
  for (size_t i = 1; i < v.size(); ++i)
  {
    auto a = v[i - 1], b = v[i];
    if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x)
      yes = !yes;
  }
  return yes;
}
bool finite(double v)
{
  return std::isfinite(v) && std::abs(v) <= 1e9;
}
double noise(double x, double z, const SplineFeature &f)
{
  x /= f.noiseWavelengthMetres;
  z /= f.noiseWavelengthMetres;
  auto ix = int64_t(std::floor(x)), iz = int64_t(std::floor(z));
  auto value = [&](int64_t a, int64_t b)
  {
    return double(featureSeed(f.seed, 1, f.id, uint64_t(a), uint64_t(b)) >> 11) / 9007199254740992. * 2 - 1;
  };
  auto mix = [](double a, double b, double t)
  {
    return a + (b - a) * t;
  };
  return mix(mix(value(ix, iz), value(ix + 1, iz), smooth(x - ix)), mix(value(ix, iz + 1), value(ix + 1, iz + 1), smooth(x - ix)), smooth(z - iz));
}
} // namespace
bool WorldSplineLandforms::build(std::vector<SplineFeature> input, const SplineSampling &settings, std::string *error)
{
  auto fail = [&](const char *s)
  {
    if (error)
      *error = s;
    return false;
  };
  if (!finite(settings.toleranceMetres) || settings.toleranceMetres < 1e-5 || !finite(settings.maxSegmentMetres) || settings.maxSegmentMetres < settings.toleranceMetres || settings.maximumSegments < 4 ||
      settings.maximumSegments > 8192 || input.size() > 128)
    return fail("Invalid spline sampling budget");
  WorldSplineLandforms result;
  std::map<uint64_t, SplineFeature> pending;
  for (auto f : input)
  {
    if (f.version != 1 || f.id == 0 || pending.count(f.id) || f.parentId == f.id || f.controls.size() < (f.kind == SplineKind::Plateau ? 3 : 2) || f.controls.size() > 256 ||
        (f.kind != SplineKind::Plateau && f.kind != SplineKind::OpenCliff))
      return fail("Invalid version, identity or controls");
    for (double v :
         {f.heightMetres,
          f.slopeX,
          f.slopeZ,
          f.faceWidthMetres,
          f.weatheredWidthMetres,
          f.apronWidthMetres,
          f.freeFaceFraction,
          f.weatheredFraction,
          f.highSideWidthMetres,
          f.endTaperMetres,
          f.roughnessMetres,
          f.noiseWavelengthMetres})
      if (!finite(v))
        return fail("Nonfinite profile");
    if (f.faceWidthMetres < 0 || f.weatheredWidthMetres <= 0 || f.apronWidthMetres <= 0 || f.freeFaceFraction <= 0 || f.weatheredFraction < 0 || f.freeFaceFraction + f.weatheredFraction >= 1 ||
        f.highSideWidthMetres <= 0 || f.endTaperMetres <= 0 || (f.highSide != 1 && f.highSide != -1) || f.roughnessMetres < 0 || f.noiseWavelengthMetres < .01 || (f.parentId && f.heightMetres < 0))
      return fail("Invalid profile ranges");
    for (size_t i = 0; i < f.controls.size(); ++i)
    {
      auto p = f.controls[i];
      if (!finite(p.x) || !finite(p.y) || !finite(p.z) || p.y != 0)
        return fail("Controls must be finite map positions with y=0");
      if (i && len(sub(p, f.controls[i - 1])) < 1e-6)
        return fail("Duplicate controls");
    }
    pending.emplace(f.id, f);
  }
  while (!pending.empty())
  {
    bool progress = false;
    for (auto it = pending.begin(); it != pending.end();)
    {
      auto f = it->second;
      size_t parent = SIZE_MAX;
      if (f.parentId)
      {
        for (size_t i = 0; i < result.definitions_.size(); ++i)
          if (result.definitions_[i].id == f.parentId)
            parent = i;
        if (parent == SIZE_MAX)
        {
          ++it;
          continue;
        }
        if (f.kind != SplineKind::Plateau || result.definitions_[parent].kind != SplineKind::Plateau)
          return fail("Only closed plateaus may nest");
      }
      Form form;
      form.parent = parent;
      auto &v = f.controls;
      bool closed = f.kind == SplineKind::Plateau;
      if (closed && len(sub(v.front(), v.back())) < 1e-6)
        return fail("Closed controls must not repeat endpoint");
      auto get = [&](int i)
      {
        if (closed)
          return v[(i + int(v.size())) % int(v.size())];
        if (i < 0)
          return sub(mul(v.front(), 2), v[1]);
        if (i >= int(v.size()))
          return sub(mul(v.back(), 2), v[v.size() - 2]);
        return v[i];
      };
      form.points.push_back(v.front());
      std::function<bool(Position, Position, Position, Position, int)> subdivide;
      subdivide = [&](Position a, Position b, Position c, Position d, int depth)
      {
        double length = len(sub(d, a));
        // Bezier convex hull bounds geometric chord error; endpoint tangent turn
        // and segment length bound station interpolation even on straight curves.
        bool flat = length > 1e-8 && distance(b, a, d) <= settings.toleranceMetres && distance(c, a, d) <= settings.toleranceMetres;
        auto u = sub(b, a), w = sub(d, c);
        bool tangent = dot(u, w) >= .996 * len(u) * len(w);
        if (flat && tangent && length <= settings.maxSegmentMetres)
        {
          if (form.points.size() > settings.maximumSegments)
            return false;
          form.points.push_back(d);
          return true;
        }
        if (depth == 24)
          return false;
        auto ab = mul(add(a, b), .5), bc = mul(add(b, c), .5), cd = mul(add(c, d), .5), abc = mul(add(ab, bc), .5), bcd = mul(add(bc, cd), .5), mid = mul(add(abc, bcd), .5);
        return subdivide(a, ab, abc, mid, depth + 1) && subdivide(mid, bcd, cd, d, depth + 1);
      };
      for (int i = 0; i < int(v.size()) - (closed ? 0 : 1); ++i)
      {
        auto a = get(i), d = get(i + 1);
        auto before = get(i - 1), after = get(i + 2);
        double t0 = std::sqrt(len(sub(a, before))), t1 = std::sqrt(len(sub(d, a))), t2 = std::sqrt(len(sub(after, d)));
        auto m1 = mul(add(sub(mul(sub(a, before), 1 / t0), mul(sub(d, before), 1 / (t0 + t1))), mul(sub(d, a), 1 / t1)), t1);
        auto m2 = mul(add(sub(mul(sub(d, a), 1 / t1), mul(sub(after, a), 1 / (t1 + t2))), mul(sub(after, d), 1 / t2)), t1);
        auto b = add(a, mul(m1, 1. / 3)), c = sub(d, mul(m2, 1. / 3));
        if (!subdivide(a, b, c, d, 0))
          return fail("Spline sampling budget or cusp failure");
      }
      for (size_t i = 1; i < form.points.size(); ++i)
        for (size_t j = i + 2; j < form.points.size(); ++j)
        {
          if (closed && i == 1 && j == form.points.size() - 1)
            continue;
          if (contact(form.points[i - 1], form.points[i], form.points[j - 1], form.points[j]))
            return fail("Self-crossing spline");
        }
      double area = 0;
      for (size_t i = 1; i < form.points.size(); ++i)
        area += cross(form.points[i - 1], form.points[i]);
      if (closed && std::abs(area) < 1e-6)
        return fail("Collapsed closed spline");
      if (closed && area < 0)
        std::reverse(form.points.begin(), form.points.end());
      form.along.push_back(0);
      for (size_t i = 1; i < form.points.size(); ++i)
        form.along.push_back(form.along.back() + len(sub(form.points[i], form.points[i - 1])));
      form.minX = form.maxX = form.points[0].x;
      form.minZ = form.maxZ = form.points[0].z;
      for (auto p : form.points)
      {
        form.minX = std::min(form.minX, p.x);
        form.maxX = std::max(form.maxX, p.x);
        form.minZ = std::min(form.minZ, p.z);
        form.maxZ = std::max(form.maxZ, p.z);
      }
      double reach = f.faceWidthMetres + f.weatheredWidthMetres + f.apronWidthMetres;
      if (!closed)
        reach = std::max(reach, f.highSideWidthMetres);
      form.minX -= reach;
      form.maxX += reach;
      form.minZ -= reach;
      form.maxZ += reach;
      if (parent != SIZE_MAX)
      {
        // Distance is 1-Lipschitz: vertex clearance plus half segment length
        // certifies the whole child boundary and its outward support corridor.
        for (size_t k = 1; k < form.points.size(); ++k)
        {
          auto p = form.points[k];
          auto s = result.closest(parent, p.x, p.z, false);
          if (s.signedDistanceMetres >= -(reach + .5 * len(sub(p, form.points[k - 1])) + settings.toleranceMetres))
            return fail("Child support is not wholly inside parent cap");
        }
      }
      for (size_t k = 0; k < result.forms_.size(); ++k)
        if (result.forms_[k].parent == parent)
        {
          auto b = result.forms_[k];
          if (form.minX <= b.maxX && form.maxX >= b.minX && form.minZ <= b.maxZ && form.maxZ >= b.minZ)
            return fail("Ambiguous sibling/root support boxes overlap");
        }
      for (size_t k = 1; k < form.points.size(); ++k)
        form.segments.push_back(k);
      std::function<size_t(size_t, size_t)> indexSegments = [&](size_t begin, size_t end)
      {
        Form::Node node;
        node.begin = begin;
        node.end = end;
        auto first = form.points[form.segments[begin]];
        node.minX = node.maxX = first.x;
        node.minZ = node.maxZ = first.z;
        for (size_t j = begin; j < end; ++j)
          for (size_t k : {form.segments[j] - 1, form.segments[j]})
          {
            auto p = form.points[k];
            node.minX = std::min(node.minX, p.x);
            node.maxX = std::max(node.maxX, p.x);
            node.minZ = std::min(node.minZ, p.z);
            node.maxZ = std::max(node.maxZ, p.z);
          }
        size_t n = form.tree.size();
        form.tree.push_back(node);
        if (end - begin > 8)
        {
          bool x = node.maxX - node.minX >= node.maxZ - node.minZ;
          size_t middle = (begin + end) / 2;
          std::stable_sort(
              form.segments.begin() + begin,
              form.segments.begin() + end,
              [&](size_t a, size_t b)
              {
                auto pa = add(form.points[a - 1], form.points[a]), pb = add(form.points[b - 1], form.points[b]);
                double av = x ? pa.x : pa.z, bv = x ? pb.x : pb.z;
                return av != bv ? av < bv : a < b;
              });
          auto left = indexSegments(begin, middle), right = indexSegments(middle, end);
          form.tree[n].left = left;
          form.tree[n].right = right;
        }
        return n;
      };
      indexSegments(0, form.segments.size());
      result.forms_.push_back(std::move(form));
      result.definitions_.push_back(f);
      it = pending.erase(it);
      progress = true;
    }
    if (!progress)
      return fail("Cyclic or missing parent");
  }
  *this = std::move(result);
  return true;
}
SplineStation WorldSplineLandforms::closest(size_t i, double x, double z, bool indexed) const
{
  auto &f = forms_[i];
  auto &spec = definitions_[i];
  Position p{x, 0, z};
  SplineStation out;
  out.distanceMetres = std::numeric_limits<double>::infinity();
  size_t best = SIZE_MAX;
  auto evaluate = [&](size_t k)
  {
    auto a = f.points[k - 1], b = f.points[k];
    double t, d = distance(p, a, b, &t);
    if (d < out.distanceMetres || (d == out.distanceMetres && k < best))
    {
      best = k;
      out.distanceMetres = d;
      out.position = add(a, mul(sub(b, a), t));
      out.tangent = mul(sub(b, a), 1 / len(sub(b, a)));
      out.alongMetres = f.along[k - 1] + t * (f.along[k] - f.along[k - 1]);
    }
  };
  if (indexed)
  {
    std::function<void(size_t)> visit = [&](size_t index)
    {
      auto &node = f.tree[index];
      double dx = std::max({node.minX - x, x - node.maxX, 0.}), dz = std::max({node.minZ - z, z - node.maxZ, 0.});
      if (std::hypot(dx, dz) > out.distanceMetres + 1e-9)
        return;
      if (node.left == SIZE_MAX)
      {
        for (size_t j = node.begin; j < node.end; ++j)
          evaluate(f.segments[j]);
      }
      else
      {
        visit(node.left);
        visit(node.right);
      }
    };
    visit(0);
  }
  else
    for (size_t k = 1; k < f.points.size(); ++k)
      evaluate(k);
  bool high = spec.kind == SplineKind::Plateau ? inside(p, f.points) : cross(out.tangent, sub(p, out.position)) * spec.highSide >= 0;
  out.signedDistanceMetres = (high ? -1 : 1) * out.distanceMetres;
  return out;
}
bool WorldSplineLandforms::station(uint64_t id, double x, double z, SplineStation &out, bool indexed) const
{
  if (!finite(x) || !finite(z))
    return false;
  for (size_t i = 0; i < definitions_.size(); ++i)
    if (definitions_[i].id == id)
    {
      out = closest(i, x, z, indexed);
      return true;
    }
  return false;
}
double WorldSplineLandforms::apply(size_t i, double x, double z, double base, SplineSurfaceSample &out, bool indexed) const
{
  const auto &f = definitions_[i];
  const auto &form = forms_[i];
  auto s = closest(i, x, z, indexed);
  double d = s.signedDistanceMetres;
  double extent = f.faceWidthMetres + f.weatheredWidthMetres + f.apronWidthMetres;
  if (d >= extent)
    return base;
  double mask = 1;
  if (f.kind == SplineKind::OpenCliff)
  {
    mask = smooth(std::min(s.alongMetres, form.along.back() - s.alongMetres) / f.endTaperMetres);
    if (d < 0)
      mask *= 1 - smooth((-d - f.highSideWidthMetres * .5) / (f.highSideWidthMetres * .5));
  }
  if (mask == 0)
    return base;
  double plane = f.heightMetres + f.slopeX * (x - f.controls[0].x) + f.slopeZ * (z - f.controls[0].z) + (f.parentId ? base : 0);
  double coefficient = 1;
  SplineRegion region = SplineRegion::Cap;
  if (d > 0)
  {
    if (d < f.faceWidthMetres)
    {
      coefficient = 1 - f.freeFaceFraction * smooth(d / f.faceWidthMetres);
      region = d < f.faceWidthMetres * .15 ? SplineRegion::Brink : SplineRegion::FreeFace;
    }
    else if (d < f.faceWidthMetres + f.weatheredWidthMetres)
    {
      coefficient = 1 - f.freeFaceFraction - f.weatheredFraction * smooth((d - f.faceWidthMetres) / f.weatheredWidthMetres);
      region = SplineRegion::WeatheredFace;
    }
    else
    {
      coefficient = (1 - f.freeFaceFraction - f.weatheredFraction) * (1 - smooth((d - f.faceWidthMetres - f.weatheredWidthMetres) / f.apronWidthMetres));
      region = SplineRegion::Apron;
    }
  }
  double rough = f.roughnessMetres * noise(x, z, f) * coefficient;
  double h = base + mask * ((plane - base) * coefficient + rough);
  out = {h, mask * coefficient, f.id, region};
  return h;
}
bool WorldSplineLandforms::sample(double x, double z, const BaseSurface &base, SplineSurfaceSample &out, bool indexed) const
{
  if (!finite(x) || !finite(z) || !base)
    return false;
  double h = base(x, z);
  if (!finite(h))
    return false;
  SplineSurfaceSample result{h};
  std::vector<double> heights(forms_.size(), h);
  std::vector<bool> active(forms_.size(), false);
  for (size_t i = 0; i < forms_.size(); ++i)
  {
    const auto &f = forms_[i];
    if (f.parent != SIZE_MAX && !active[f.parent])
      continue;
    if (x < f.minX || x > f.maxX || z < f.minZ || z > f.maxZ)
      continue;
    double ground = f.parent == SIZE_MAX ? h : heights[f.parent];
    SplineSurfaceSample tmp{ground};
    heights[i] = apply(i, x, z, ground, tmp, indexed);
    if (tmp.featureId)
    {
      active[i] = true;
      result = tmp;
    }
  }
  if (!finite(result.height))
    return false;
  out = result;
  return true;
}
bool WorldSplineLandforms::verticalFaces(const BaseSurface &base, SplineFaceMesh &out, std::string *error) const
{
  SplineFaceMesh result;
  auto fail = [&](const char *s)
  {
    if (error)
      *error = s;
    return false;
  };
  if (!base)
    return fail("Missing base surface");
  for (size_t i = 0; i < forms_.size(); ++i)
  {
    auto &f = definitions_[i];
    auto &form = forms_[i];
    if (f.faceWidthMetres != 0)
      continue;
    for (size_t k = 1; k < form.points.size(); ++k)
    {
      auto a = form.points[k - 1], b = form.points[k];
      Position tangent = mul(sub(b, a), 1 / len(sub(b, a)));
      int side = f.kind == SplineKind::Plateau ? 1 : f.highSide;
      Position normal{tangent.z * side, 0, -tangent.x * side};
      auto ends = [&](Position p, double along)
      {
        double g = base(p.x, p.z);
        if (form.parent != SIZE_MAX)
        {
          std::vector<size_t> ancestors;
          for (size_t j = form.parent; j != SIZE_MAX; j = forms_[j].parent)
            ancestors.push_back(j);
          for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
          {
            SplineSurfaceSample s;
            g = apply(*it, p.x, p.z, g, s, true);
          }
        }
        double mask = f.kind == SplineKind::Plateau ? 1 : smooth(std::min(along, form.along.back() - along) / f.endTaperMetres);
        double plane = f.heightMetres + f.slopeX * (p.x - f.controls[0].x) + f.slopeZ * (p.z - f.controls[0].z) + (f.parentId ? g : 0);
        double rough = f.roughnessMetres * noise(p.x, p.z, f);
        double top = g + mask * (plane - g + rough), low = g + mask * ((plane - g + rough) * (1 - f.freeFaceFraction));
        return std::array<Position, 2>{Position{p.x, top, p.z}, Position{p.x, low, p.z}};
      };
      auto pa = ends(a, form.along[k - 1]), pb = ends(b, form.along[k]);
      for (auto p : {pa[0], pa[1], pb[0], pb[1]})
        if (!finite(p.y))
          return fail("Nonfinite face height");
      if (pa[0].y < pa[1].y || pb[0].y < pb[1].y)
        return fail("Vertical cap below supporting terrain");
      uint32_t n = uint32_t(result.vertices.size());
      result.vertices.insert(result.vertices.end(), {pa[0], pa[1], pb[0], pb[1]});
      auto emit = [&](std::array<uint32_t, 3> tri)
      {
        if (side > 0)
          std::swap(tri[1], tri[2]);
        result.triangles.push_back(tri);
        result.expectedNormals.push_back(normal);
      };
      if (pa[0].y - pa[1].y > 1e-8)
        emit({n, n + 1, n + 2});
      if (pb[0].y - pb[1].y > 1e-8)
        emit({n + 2, n + 1, n + 3});
    }
  }
  out = std::move(result);
  return true;
}
bool migrateClosedLandforms(const std::vector<procedural::geometry::RockFormation> &old, std::vector<SplineFeature> &out, std::string *error)
{
  std::vector<SplineFeature> migrated;
  for (size_t i = 0; i < old.size(); ++i)
  {
    auto &r = old[i];
    if (r.rampWidthMetres != 0 || (r.parent != UINT32_MAX && r.parent >= i))
    {
      if (error)
        *error = "Migration requires valid earlier parents and no access ramps";
      return false;
    }
    SplineFeature f;
    f.id = i + 1;
    f.parentId = r.parent == UINT32_MAX ? 0 : r.parent + 1;
    f.seed = r.seed;
    f.heightMetres = r.topMetres - (f.parentId ? old[r.parent].topMetres : 0);
    f.faceWidthMetres = r.faceWidthMetres;
    f.weatheredWidthMetres = std::max(.01, r.talusWidthMetres * .5);
    f.apronWidthMetres = std::max(.01, r.talusWidthMetres * .5);
    f.freeFaceFraction = r.freeFaceFraction;
    f.weatheredFraction = (1 - r.freeFaceFraction) * .75;
    f.roughnessMetres = r.capReliefMetres;
    f.noiseWavelengthMetres = r.gullySpacingMetres;
    for (auto p : r.controls)
      f.controls.push_back({p.x, 0, p.y});
    migrated.push_back(f);
  }
  WorldSplineLandforms validator;
  if (!validator.build(migrated, {}, error))
    return false;
  out = std::move(migrated);
  return true;
}
} // namespace procedural::world::rewrite
