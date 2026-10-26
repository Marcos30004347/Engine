#include "WorldFarmFields.hpp"
#include "WorldRuralPatches.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <set>
namespace procedural::world::rewrite
{
namespace
{
using Poly = std::vector<Position>;
double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
double dist(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}
Position mix(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}
double area(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  return a * .5;
}
Poly clip(const Poly &p, double nx, double nz, double c)
{
  Poly r;
  if (p.empty())
    return r;
  for (size_t i = 0; i < p.size(); ++i)
  {
    auto a = p[i], b = p[(i + 1) % p.size()];
    double da = nx * a.x + nz * a.z - c, db = nx * b.x + nz * b.z - c;
    if (da <= 1e-8)
      r.push_back(a);
    if ((da < -1e-8 && db > 1e-8) || (db < -1e-8 && da > 1e-8))
      r.push_back(mix(a, b, da / (da - db)));
  }
  Poly q;
  for (auto v : r)
    if (q.empty() || dist(q.back(), v) > 1e-7)
      q.push_back(v);
  if (q.size() > 1 && dist(q.front(), q.back()) < 1e-7)
    q.pop_back();
  if (q.size() < 3 || area(q) < 1e-7)
    q.clear();
  return q;
}
bool inside(const Poly &p, Position q)
{
  for (size_t i = 0; i < p.size(); ++i)
    if (cross(p[i], p[(i + 1) % p.size()], q) < -1e-6)
      return false;
  return !p.empty();
}
Poly intersection(Poly p, const Poly &q)
{
  for (size_t i = 0; i < q.size(); ++i)
  {
    auto a = q[i], b = q[(i + 1) % q.size()];
    p = clip(p, b.z - a.z, a.x - b.x, (b.z - a.z) * a.x + (a.x - b.x) * a.z);
  }
  return p;
}
// Exact interval coverage over convex pieces catches exclusions narrower than
// the centerline sampling spacing.
bool coveredSegment(Position a, Position b, const std::vector<Poly> &pieces)
{
  std::vector<std::pair<double, double>> intervals;
  for (const auto &p : pieces)
  {
    double lo = 0, hi = 1;
    for (size_t k = 0; k < p.size(); ++k)
    {
      double da = cross(p[k], p[(k + 1) % p.size()], a), db = cross(p[k], p[(k + 1) % p.size()], b);
      if (da < -1e-8 && db < -1e-8)
      {
        hi = -1;
        break;
      }
      if (da < -1e-8)
        lo = std::max(lo, da / (da - db));
      if (db < -1e-8)
        hi = std::min(hi, da / (da - db));
    }
    if (hi >= lo)
      intervals.emplace_back(lo, hi);
  }
  std::sort(intervals.begin(), intervals.end());
  double end = 0;
  for (auto [lo, hi] : intervals)
  {
    if (lo > end + 1e-8)
      break;
    end = std::max(end, hi);
  }
  return end >= 1 - 1e-8;
}
std::vector<Poly> subtract(Poly p, const Poly &q)
{
  if (intersection(p, q).empty())
    return {p};
  std::vector<Poly> r;
  for (size_t i = 0; i < q.size() && !p.empty(); ++i)
  {
    auto a = q[i], b = q[(i + 1) % q.size()];
    double nx = b.z - a.z, nz = a.x - b.x, c = nx * a.x + nz * a.z;
    auto out = clip(p, -nx, -nz, -c);
    if (!out.empty())
      r.push_back(out);
    p = clip(p, nx, nz, c);
  }
  return r;
}
Poly hull(Poly p)
{
  std::sort(
      p.begin(),
      p.end(),
      [](auto a, auto b)
      {
        return a.x == b.x ? a.z < b.z : a.x < b.x;
      });
  Poly h;
  for (auto v : p)
  {
    while (h.size() > 1 && cross(h[h.size() - 2], h.back(), v) <= 1e-8)
      h.pop_back();
    h.push_back(v);
  }
  size_t k = h.size();
  for (auto it = p.rbegin() + 1; it != p.rend(); ++it)
  {
    while (h.size() > k && cross(h[h.size() - 2], h.back(), *it) <= 1e-8)
      h.pop_back();
    h.push_back(*it);
  }
  if (h.size() > 1)
    h.pop_back();
  return h;
}
void merge(std::vector<Poly> &p)
{
  bool changed = true;
  while (changed)
  {
    changed = false;
    for (size_t i = 0; i < p.size() && !changed; ++i)
      for (size_t j = i + 1; j < p.size(); ++j)
      {
        Poly all = p[i];
        all.insert(all.end(), p[j].begin(), p[j].end());
        auto h = hull(all);
        if (std::abs(area(h) - area(p[i]) - area(p[j])) < 1e-5)
        {
          p[i] = h;
          p.erase(p.begin() + j);
          changed = true;
          break;
        }
      }
  }
}
Poly inset(Poly p, double d)
{
  auto original = p;
  for (size_t i = 0; i < original.size(); ++i)
  {
    auto a = original[i], b = original[(i + 1) % original.size()];
    double nx = b.z - a.z, nz = a.x - b.x, l = dist(a, b);
    if (l < 1e-8)
      return {};
    p = clip(p, nx, nz, nx * a.x + nz * a.z - d * l);
  }
  return p;
}
Poly band(Position a, Position b, double width)
{
  double l = dist(a, b);
  if (l < 1e-8)
    return {};
  double x = -(b.z - a.z) * width / (2 * l), z = (b.x - a.x) * width / (2 * l);
  return {{a.x - x, 0, a.z - z}, {b.x - x, 0, b.z - z}, {b.x + x, 0, b.z + z}, {a.x + x, 0, a.z + z}};
}
Poly square(Position p, double width)
{
  double h = width * .5;
  return {{p.x - h, 0, p.z - h}, {p.x + h, 0, p.z - h}, {p.x + h, 0, p.z + h}, {p.x - h, 0, p.z + h}};
}
double coverage(const Poly &p, const std::vector<Poly> &parts)
{
  double a = 0;
  for (auto &q : parts)
    a += area(intersection(p, q));
  return a;
}
FarmField prepare(const Poly &parent, const FarmFieldSettings &s)
{
  FarmField f;
  auto p = inset(parent, s.headland);
  // An inscribed 16-gon of this circumradius contains the required width disk.
  double radius = s.minimumWidth / (2 * std::cos(3.141592653589793 / 16));
  auto core = inset(p, radius);
  if (core.empty())
    return f;
  Poly expanded;
  for (auto v : core)
    for (int k = 0; k < 16; ++k)
    {
      double t = k * 6.283185307179586 / 16;
      expanded.push_back({v.x + radius * std::cos(t), 0, v.z + radius * std::sin(t)});
    }
  p = intersection(hull(expanded), p);
  if (p.empty())
    return f;
  f.area = area(p);
  f.minimumWidth = 1e30;
  f.minimumAngleDegrees = 180;
  double maxLength = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    auto a = p[i], b = p[(i + 1) % p.size()], c = p[(i + p.size() - 1) % p.size()];
    double len = dist(a, b), other = dist(a, c);
    if (len < 1e-7 || other < 1e-7)
      return {};
    double lo = 1e30, hi = -1e30;
    for (auto q : p)
    {
      double t = ((b.z - a.z) * q.x - (b.x - a.x) * q.z) / len;
      lo = std::min(lo, t);
      hi = std::max(hi, t);
      maxLength = std::max(maxLength, dist(a, q));
    }
    f.minimumWidth = std::min(f.minimumWidth, hi - lo);
    double cosine = ((b.x - a.x) * (c.x - a.x) + (b.z - a.z) * (c.z - a.z)) / (len * other);
    f.minimumAngleDegrees = std::min(f.minimumAngleDegrees, std::acos(std::clamp(cosine, -1., 1.)) * 180 / 3.141592653589793);
  }
  f.aspect = maxLength / f.minimumWidth;
  if (f.area < s.minimumArea || f.minimumWidth + 1e-6 < s.minimumWidth || f.minimumAngleDegrees < 30 || f.aspect > s.maximumAspect || inset(p, s.minimumWidth * .499).empty())
    return {};
  f.boundary = std::move(p);
  return f;
}
bool decompose(const FarmFieldInput &in, std::vector<Poly> &pieces, std::string *error)
{
  BoundaryRegistry reg;
  PatchDomain d;
  d.id = in.patch;
  std::vector<BoundaryInput> edges;
  uint64_t vid = 1, eid = 1;
  auto add = [&](const Poly &p, std::vector<BoundaryUse> &uses)
  {
    auto first = vid;
    for (size_t i = 0; i < p.size(); ++i)
    {
      edges.push_back({eid, {vid++, p[i]}, {i + 1 == p.size() ? first : vid, p[(i + 1) % p.size()]}, {}});
      uses.push_back({eid++, i + 1 == p.size()});
    }
  };
  add(in.outer, d.outer);
  for (auto &h : in.holes)
  {
    d.holes.emplace_back();
    add(h, d.holes.back());
  }
  PatchMesh m;
  if (!reg.commit(edges, error) || !compilePatchMesh(
                                       d,
                                       reg,
                                       {},
                                       [](double, double)
                                       {
                                         return 0.;
                                       },
                                       m,
                                       error))
    return false;
  for (auto t : m.triangles)
  {
    Poly p;
    for (auto i : t)
      p.push_back(m.vertices[i].position);
    if (area(p) < 0)
      std::reverse(p.begin(), p.end());
    pieces.push_back(p);
  }
  return true;
}
} // namespace
FarmFieldInput farmFieldInput(const AgriculturalPatch &p, uint64_t revision)
{
  FarmFieldInput in;
  in.patch = p.id;
  in.source = p.source;
  in.revision = revision;
  in.outer = p.boundary;
  in.access = p.access;
  return in;
}
FarmFieldSettings farmFieldPreset(FieldRecipe recipe)
{
  FarmFieldSettings s;
  s.recipe = recipe;
  if (recipe == FieldRecipe::Strips)
  {
    s.minimumWidth = 4;
    s.maximumAspect = 32;
    s.targetArea = 1800;
    s.maximumArea = 3000;
  }
  return s;
}
bool generateFarmFields(const FarmFieldInput &input, const FarmFieldSettings &provided, FarmFieldProduct &out, std::string *error)
{
  auto s = provided;
  auto fail = [&](const std::string &e)
  {
    if (error)
      *error = e;
    return false;
  };
  if (!input.patch || !input.source || !input.revision || input.outer.size() < 3 || input.outer.size() > 256 || input.holes.size() > 16 || input.access.size() < 2 || input.access.size() > 256)
    return fail("invalid field identity/domain/access budget");
  for (double v : {s.targetArea, s.maximumArea, s.minimumArea, s.minimumWidth, s.maximumAspect, s.headland, s.trackWidth, s.entranceWidth})
    if (!std::isfinite(v) || v <= 0)
      return fail("invalid field dimensions");
  if (!std::isfinite(s.grainRadians) || s.minimumArea > s.targetArea || s.targetArea > s.maximumArea || s.maximumDepth > 16 || s.maximumAttempts > 4096 || s.entranceWidth < 1 || s.headland < s.entranceWidth * 1.5 ||
      s.trackWidth < s.entranceWidth * 1.5 || s.minimumWidth > 1000)
    return fail("unsupported field dimensions/budgets");
  for (auto &h : input.holes)
    if (h.size() < 3 || h.size() > 256)
      return fail("invalid field hole");
  for (auto p : input.access)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      return fail("nonfinite field access");
  if (s.recipe != FieldRecipe::Enclosed && s.recipe != FieldRecipe::Strips)
    return fail("unknown field recipe");
  if (s.headland > 100 || s.trackWidth > 100 || s.entranceWidth > 20 || s.maximumArea > 1e9)
    return fail("field dimensions exceed numeric budget");
  for (auto p : input.outer)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::abs(p.x) > 1e6 || std::abs(p.z) > 1e6)
      return fail("invalid field coordinate");
  for (auto &h : input.holes)
    for (auto p : h)
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::abs(p.x) > 1e6 || std::abs(p.z) > 1e6)
        return fail("invalid field hole coordinate");
  if (s.deriveGrainFromAccess)
    for (size_t i = input.access.size() - 1; i > 0; --i)
      if (dist(input.access[i], input.access[i - 1]) > 1e-6)
      {
        s.grainRadians = std::atan2(input.access[i].z - input.access[i - 1].z, input.access[i].x - input.access[i - 1].x);
        break;
      }
  auto in = input;
  for (auto &p : in.outer)
    p.y = 0;
  for (auto &h : in.holes)
    for (auto &p : h)
      p.y = 0;
  std::vector<Poly> base;
  if (!decompose(in, base, error))
    return false;
  // Remove triangulation seams before road subtraction; otherwise those seams
  // become artificial diagonal field boundaries around access tracks.
  merge(base);
  FarmFieldProduct r;
  r.revision = in.revision;
  r.grainRadians = s.grainRadians;
  for (auto &p : base)
    r.patchArea += area(p);
  if (in.sharedTracks.size() > 512)
    return fail("shared field track budget exceeded");
  std::vector<Poly> tracks = in.sharedTracks;
  for (const auto &p : tracks)
  {
    if (p.size() < 3 || p.size() > 256 || area(p) <= 0)
      return fail("invalid shared field track");
    for (size_t i = 0; i < p.size(); ++i)
    {
      if (!std::isfinite(p[i].x) || !std::isfinite(p[i].y) || !std::isfinite(p[i].z) || std::abs(p[i].x) > 1e6 || std::abs(p[i].z) > 1e6)
        return fail("invalid shared track coordinate");
      for (auto q : p)
        if (cross(p[i], p[(i + 1) % p.size()], q) < -1e-7)
          return fail("shared track must be convex CCW");
    }
  }
  for (size_t i = 1; i < in.access.size(); ++i)
  {
    auto p = band(in.access[i - 1], in.access[i], s.trackWidth);
    if (!p.empty())
      tracks.push_back(p);
  }
  for (auto p : in.access)
    tracks.push_back(square(p, s.trackWidth));
  std::vector<Poly> usable = base;
  for (auto &track : tracks)
  {
    std::vector<Poly> next;
    for (auto &p : usable)
    {
      auto parts = subtract(p, track);
      next.insert(next.end(), parts.begin(), parts.end());
    }
    usable = std::move(next);
    if (usable.size() > 512)
      return fail("field Boolean piece budget exceeded");
  }
  double usableArea = 0;
  for (auto &p : usable)
    usableArea += area(p);
  r.trackArea = r.patchArea - usableArea;
  merge(usable);
  std::vector<FarmField> candidates;
  std::function<void(Poly, uint32_t)> split = [&](Poly p, uint32_t depth)
  {
    auto terminal = prepare(p, s);
    if (!terminal.boundary.empty() && terminal.area <= s.targetArea)
    {
      candidates.push_back(std::move(terminal));
      return;
    }
    bool divided = false;
    if (depth < s.maximumDepth)
      for (int axis = 0; axis < (s.recipe == FieldRecipe::Strips ? 1 : 2) && !divided; ++axis)
      {
        double angle = s.grainRadians + (s.recipe == FieldRecipe::Strips ? 1 : axis) * 1.5707963267948966, nx = std::cos(angle), nz = std::sin(angle), lo = 1e30, hi = -1e30;
        for (auto v : p)
        {
          double t = nx * v.x + nz * v.z;
          lo = std::min(lo, t);
          hi = std::max(hi, t);
        }
        for (double f : {.5, .43, .57})
        {
          if (r.splitAttempts >= s.maximumAttempts)
            break;
          ++r.splitAttempts;
          double c = lo + (hi - lo) * f;
          auto a = clip(p, nx, nz, c), b = clip(p, -nx, -nz, -c);
          auto fa = prepare(a, s), fb = prepare(b, s);
          if (fa.boundary.empty() || fb.boundary.empty())
            continue;
          divided = true;
          split(a, depth + 1);
          split(b, depth + 1);
          break;
        }
      }
    if (!divided)
    {
      if (!terminal.boundary.empty() && terminal.area <= s.maximumArea)
        candidates.push_back(std::move(terminal));
      else
        r.rejections.push_back("unusable width/tip/area or exhausted split budget: retained as margin");
    }
  };
  for (auto &p : usable)
  {
    split(p, 0);
    if (candidates.size() > 128)
      return fail("field count budget exceeded");
  }
  // Conservative full-width margin reachability. Adjacent free squares overlap;
  // the entire swept path between their centers is therefore inside safe land.
  double minX = 1e30, minZ = 1e30, maxX = -1e30, maxZ = -1e30;
  for (auto p : in.outer)
  {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minZ = std::min(minZ, p.z);
    maxZ = std::max(maxZ, p.z);
  }
  double step = s.entranceWidth * .5, nx = std::ceil((maxX - minX) / step), nz = std::ceil((maxZ - minZ) / step);
  if (nx < 1 || nz < 1 || nx * nz > 65536)
    return fail("field access grid budget exceeded");
  uint32_t cols = nx, rows = nz;
  size_t count = cols * rows;
  std::vector<uint8_t> free(count);
  std::vector<uint32_t> parent(count, UINT32_MAX);
  uint32_t root = UINT32_MAX;
  double rootScore = 1e30;
  auto point = [&](uint32_t i)
  {
    return Position{minX + (i % cols + .5) * step, 0, minZ + (i / cols + .5) * step};
  };
  auto clear = [&](const Poly &p)
  {
    if (p.empty() || std::abs(coverage(p, base) - area(p)) > 1e-5)
      return false;
    for (auto &f : candidates)
      if (area(intersection(p, f.boundary)) > 1e-6)
        return false;
    return true;
  };
  for (uint32_t i = 0; i < count; ++i)
  {
    auto p = point(i);
    auto box = square(p, s.entranceWidth);
    if (!clear(box))
      continue;
    free[i] = 1;
    bool onTrack = false;
    for (auto &t : tracks)
      if (inside(t, p))
        onTrack = true;
    if (onTrack)
    {
      double score = dist(p, in.access.front());
      if (score < rootScore)
      {
        rootScore = score;
        root = i;
      }
    }
  }
  std::queue<uint32_t> queue;
  if (root != UINT32_MAX)
  {
    parent[root] = root;
    queue.push(root);
  }
  while (!queue.empty())
  {
    auto i = queue.front();
    queue.pop();
    std::vector<uint32_t> ns;
    if (i % cols)
      ns.push_back(i - 1);
    if (i % cols + 1 < cols)
      ns.push_back(i + 1);
    if (i >= cols)
      ns.push_back(i - cols);
    if (i + cols < count)
      ns.push_back(i + cols);
    for (auto j : ns)
      if (free[j] && parent[j] == UINT32_MAX)
      {
        parent[j] = i;
        queue.push(j);
      }
  }
  for (auto &f : candidates)
  {
    uint32_t chosen = UINT32_MAX;
    Position entrance;
    double best = 1e30;
    for (uint32_t i = 0; i < count; ++i)
      if (parent[i] != UINT32_MAX)
      {
        auto p = point(i);
        for (size_t j = 0; j < f.boundary.size(); ++j)
        {
          auto a = f.boundary[j], b = f.boundary[(j + 1) % f.boundary.size()];
          double l = dist(a, b);
          if (l < s.entranceWidth)
            continue;
          double t = ((p.x - a.x) * (b.x - a.x) + (p.z - a.z) * (b.z - a.z)) / (l * l);
          if (t < s.entranceWidth / (2 * l) || t > 1 - s.entranceWidth / (2 * l))
            continue;
          auto q = mix(a, b, t);
          double d = dist(p, q);
          if (d > 2 * s.entranceWidth || d < 1e-7 || d >= best)
            continue;
          auto connector = band(p, q, s.entranceWidth);
          if (!clear(connector))
            continue;
          chosen = i;
          entrance = q;
          best = d;
        }
      }
    if (chosen == UINT32_MAX)
    {
      r.rejections.push_back("no full-width entrance connected to upstream track: retained as margin");
      continue;
    }
    uint32_t i = chosen;
    while (i != root)
    {
      f.access.push_back(point(i));
      i = parent[i];
    }
    f.access.push_back(point(root));
    std::reverse(f.access.begin(), f.access.end());
    f.access.push_back(entrance);
    Poly simple;
    for (auto p : f.access)
    {
      while (simple.size() > 1 && std::abs(cross(simple[simple.size() - 2], simple.back(), p)) < 1e-8)
        simple.pop_back();
      simple.push_back(p);
    }
    f.access = std::move(simple);
    f.id = r.fields.size() + 1;
    f.patch = in.patch;
    f.source = in.source;
    r.cropArea += f.area;
    r.fields.push_back(f);
  }
  r.marginArea = usableArea - r.cropArea;
  if (r.marginArea < -1e-5)
    return fail("negative field area ledger");
  // Retain disjoint accounting pieces, but do not promote their internal edges
  // into visual boundaries. Domain/crop/track masks label the final arrangement.
  std::vector<Poly> margins = usable;
  for (auto &f : r.fields)
  {
    std::vector<Poly> next;
    for (auto &p : margins)
    {
      auto parts = subtract(p, f.boundary);
      next.insert(next.end(), parts.begin(), parts.end());
    }
    margins = std::move(next);
    if (margins.size() > 2048)
      return fail("field margin contour budget exceeded");
  }
  std::vector<Poly> trackPieces = base;
  for (auto &p : usable)
  {
    std::vector<Poly> next;
    for (auto &q : trackPieces)
    {
      auto parts = subtract(q, p);
      next.insert(next.end(), parts.begin(), parts.end());
    }
    trackPieces = std::move(next);
    if (trackPieces.size() > 2048)
      return fail("field track contour budget exceeded");
  }
  uint64_t id = 1;
  auto emit = [&](const Poly &p, uint32_t material, SurfaceRole role)
  {
    SurfaceContour c;
    c.id = id++;
    c.material = material;
    c.role = role;
    c.loop = p;
    r.contours.push_back(std::move(c));
  };
  r.margins = margins;
  r.tracks = trackPieces;
  for (auto &p : base)
    emit(p, 11, SurfaceRole::Ground);
  for (auto &p : trackPieces)
    emit(p, 1, SurfaceRole::Road);
  for (auto &f : r.fields)
    emit(f.boundary, 20 + f.id, SurfaceRole::Ground);
  if (r.contours.size() > 2048)
    return fail("field contour budget exceeded");
  if (error)
    error->clear();
  out = std::move(r);
  return true;
}
} // namespace procedural::world::rewrite
