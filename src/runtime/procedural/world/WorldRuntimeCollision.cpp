#include "WorldRuntimeCollision.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
namespace procedural::world::rewrite
{
namespace
{
Position sub(Position a, Position b)
{
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Position cross(Position a, Position b)
{
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double dot(Position a, Position b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
double axis(Position p, int a)
{
  return a == 0 ? p.x : a == 1 ? p.y : p.z;
}
bool valid(Position p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) <= 1e12;
}
bool triangle(const std::array<Position, 3> &p, Position from, Position direction, double &t, Position &normal)
{
  auto e1 = sub(p[1], p[0]), e2 = sub(p[2], p[0]), h = cross(direction, e2);
  const double determinant = dot(e1, h);
  // Reject parallel/coplanar rays, with a relative roundoff threshold.
  const double scale = std::sqrt(dot(e1, e1) * dot(h, h));
  if (!std::isfinite(scale) || std::abs(determinant) <= scale * 1e-14)
    return false;
  auto s = sub(from, p[0]);
  const double u = dot(s, h) / determinant;
  auto q = cross(s, e1);
  const double v = dot(direction, q) / determinant;
  t = dot(e2, q) / determinant;
  if (u < -1e-12 || v < -1e-12 || u + v > 1 + 1e-12 || t < 0 || t > 1)
    return false;
  normal = cross(e1, e2);
  const double length = std::sqrt(dot(normal, normal));
  if (!(length > 0) || !std::isfinite(length))
    return false;
  normal = {normal.x / length, normal.y / length, normal.z / length};
  return true;
}
bool bounds(Position lo, Position hi, Position from, Position direction, double limit)
{
  double near = 0, far = limit;
  for (int a = 0; a < 3; ++a)
  {
    const double origin = axis(from, a), delta = axis(direction, a);
    // Match the triangle edge tolerance so the accelerator cannot reject a hit
    // accepted by the primitive test at a shared edge.
    const double pad = 1e-12 * std::max(1., axis(hi, a) - axis(lo, a));
    const double lower = std::nextafter(axis(lo, a) - pad, -INFINITY), upper = std::nextafter(axis(hi, a) + pad, INFINITY);
    if (delta == 0)
    {
      if (origin < lower || origin > upper)
        return false;
      continue;
    }
    double t0 = (lower - origin) / delta, t1 = (upper - origin) / delta;
    if (t0 > t1)
      std::swap(t0, t1);
    near = std::max(near, t0);
    far = std::min(far, t1);
    if (near > far)
      return false;
  }
  return true;
}
} // namespace
uint32_t WorldRuntimeCollision::split(uint32_t begin, uint32_t end)
{
  const auto index = uint32_t(nodes_.size());
  Node node;
  node.lo = {INFINITY, INFINITY, INFINITY};
  node.hi = {-INFINITY, -INFINITY, -INFINITY};
  node.begin = begin;
  node.count = end - begin;
  for (uint32_t i = begin; i < end; ++i)
    for (auto p : primitives_[order_[i]].points)
    {
      node.lo = {std::min(node.lo.x, p.x), std::min(node.lo.y, p.y), std::min(node.lo.z, p.z)};
      node.hi = {std::max(node.hi.x, p.x), std::max(node.hi.y, p.y), std::max(node.hi.z, p.z)};
    }
  nodes_.push_back(node);
  if (end - begin > 8)
  {
    const auto extent = sub(node.hi, node.lo);
    int a = extent.y > extent.x ? 1 : 0;
    if (extent.z > axis(extent, a))
      a = 2;
    const auto middle = begin + (end - begin) / 2;
    auto center = [&](uint32_t id)
    {
      const auto &p = primitives_[id].points;
      return (axis(p[0], a) + axis(p[1], a) + axis(p[2], a)) / 3;
    };
    std::nth_element(
        order_.begin() + begin,
        order_.begin() + middle,
        order_.begin() + end,
        [&](auto x, auto y)
        {
          const double cx = center(x), cy = center(y);
          return cx == cy ? x < y : cx < cy;
        });
    const auto left = split(begin, middle), right = split(middle, end);
    nodes_[index].left = left;
    nodes_[index].right = right;
    nodes_[index].count = 0;
  }
  return index;
}
bool WorldRuntimeCollision::build(const RuntimeSnapshot &snapshot, uint32_t maximumTriangles, std::string *error)
{
  auto fail = [&](const char *message)
  {
    if (error)
      *error = message;
    return false;
  };
  if (!snapshot.assets || !validateRuntimeAssets(*snapshot.assets, error))
    return false;
  const auto &assets = *snapshot.assets;
  const auto &mesh = assets.levels[0].world.mesh;
  const size_t count = mesh.triangles.size() + assets.structures.triangles.size();
  if (!count || count > maximumTriangles || count > 1000000)
    return fail("collision triangle budget exceeded");
  WorldRuntimeCollision result;
  result.revision_ = assets.levels[0].world.map.revision;
  result.primitives_.reserve(count);
  for (const auto &t : mesh.triangles)
    result.primitives_.push_back({{mesh.vertices[t.vertices[0]], mesh.vertices[t.vertices[1]], mesh.vertices[t.vertices[2]]}, t.face, t.material, t.role == SurfaceRole::Water ? CollisionWater : CollisionSurface});
  for (size_t i = 0; i < assets.structures.triangles.size(); ++i)
  {
    const auto t = assets.structures.triangles[i];
    const auto &v = assets.structures.vertices;
    result.primitives_.push_back({{v[t[0]], v[t[1]], v[t[2]]}, 0, assets.structuralMaterials[i], CollisionStructure});
  }
  for (const auto &p : result.primitives_)
    for (auto v : p.points)
      if (!valid(v))
        return fail("collision coordinates exceed supported range");
  result.order_.resize(count);
  std::iota(result.order_.begin(), result.order_.end(), 0);
  result.nodes_.reserve(count * 2);
  result.split(0, uint32_t(count));
  *this = std::move(result);
  return true;
}
bool WorldRuntimeCollision::segment(Position from, Position to, uint32_t mask, RuntimeCollisionHit &out, bool indexed) const
{
  if (nodes_.empty() || !valid(from) || !valid(to) || !mask || (mask & ~CollisionAll))
    return false;
  auto direction = sub(to, from);
  const double length = std::sqrt(dot(direction, direction));
  if (!(length > 0))
    return false;
  bool found = false;
  RuntimeCollisionHit best;
  best.fraction = 1;
  best.primitive = UINT32_MAX;
  auto test = [&](uint32_t id)
  {
    const auto &p = primitives_[id];
    if (!(mask & p.category))
      return;
    double t;
    Position normal;
    if (!triangle(p.points, from, direction, t, normal) || (found && (t > best.fraction || (t == best.fraction && id >= best.primitive))))
      return;
    found = true;
    best = {{from.x + direction.x * t, from.y + direction.y * t, from.z + direction.z * t}, normal, t, t * length, revision_, p.face, id, p.material, p.category};
  };
  if (!indexed)
    for (uint32_t i = 0; i < primitives_.size(); ++i)
      test(i);
  else
  {
    std::vector<uint32_t> stack{0};
    while (!stack.empty())
    {
      const auto id = stack.back();
      stack.pop_back();
      const auto &n = nodes_[id];
      if (!bounds(n.lo, n.hi, from, direction, best.fraction))
        continue;
      if (n.count)
        for (uint32_t i = n.begin; i < n.begin + n.count; ++i)
          test(order_[i]);
      else
      {
        stack.push_back(n.right);
        stack.push_back(n.left);
      }
    }
  }
  if (found)
    out = best;
  return found;
}
} // namespace procedural::world::rewrite
