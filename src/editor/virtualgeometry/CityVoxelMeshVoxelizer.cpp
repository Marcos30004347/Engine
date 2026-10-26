#include "editor/virtualgeometry/CityVoxelMeshVoxelizer.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace virtualgeometry
{
namespace
{
using V = std::array<double, 3>;
using I = std::array<int32_t, 3>;
using Cell = voxel::svraster::CityVoxelCell;

V sub(V a, V b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(V a, V b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

bool fail(std::string *error, const char *reason)
{
  if (error)
    *error = reason;
  return false;
}

bool axisOverlap(V axis, V a, V b, V c)
{
  if (dot(axis, axis) < 1e-24)
    return true;
  const double radius = .5 * (std::abs(axis[0]) + std::abs(axis[1]) + std::abs(axis[2]));
  return std::min({dot(axis, a), dot(axis, b), dot(axis, c)}) <= radius && std::max({dot(axis, a), dot(axis, b), dot(axis, c)}) >= -radius;
}

// Triangle against the unit cell centred at `center` (Akenine-Moller).
bool triangleCell(V a, V b, V c, V center)
{
  a = sub(a, center);
  b = sub(b, center);
  c = sub(c, center);
  const V edges[] = {sub(b, a), sub(c, b), sub(a, c)};
  for (V e : edges)
    if (!axisOverlap({0, -e[2], e[1]}, a, b, c) || !axisOverlap({e[2], 0, -e[0]}, a, b, c) || !axisOverlap({-e[1], e[0], 0}, a, b, c))
      return false;
  for (unsigned d = 0; d < 3; ++d)
    if (std::min({a[d], b[d], c[d]}) > .5 || std::max({a[d], b[d], c[d]}) < -.5)
      return false;
  const V n = cross(sub(b, a), sub(c, a));
  const double radius = .5 * (std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]));
  return std::abs(dot(n, a)) <= radius;
}

struct CellKeyHash
{
  size_t operator()(const I &p) const noexcept
  {
    uint64_t h = uint32_t(p[0]) * 0x9E3779B97F4A7C15ull;
    h ^= uint32_t(p[1]) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= uint32_t(p[2]) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    return size_t(h);
  }
};

} // namespace

bool voxelizeCityVoxelMesh(const std::vector<CityVoxelMeshTriangle> &triangles, bool fillEnclosed, uint64_t maximumCells,
                           std::vector<Cell> &cells, CityVoxelMeshVoxelizeStats *stats, std::string *error)
{
  cells.clear();
  if (stats)
    *stats = {};
  std::unordered_map<I, Cell, CellKeyHash> surface;
  for (const CityVoxelMeshTriangle &triangle : triangles)
  {
    const V a = triangle.corners[0], b = triangle.corners[1], c = triangle.corners[2];
    I low{}, high{};
    for (unsigned d = 0; d < 3; ++d)
    {
      const double lo = std::floor(std::min({a[d], b[d], c[d]}) - 1);
      const double hi = std::ceil(std::max({a[d], b[d], c[d]}) + 1);
      if (!std::isfinite(lo) || !std::isfinite(hi) || lo < -(1 << 28) || hi > (1 << 28))
        return fail(error, "mesh triangle outside the lattice");
      low[d] = int32_t(lo);
      high[d] = int32_t(hi);
    }
    const V n = cross(sub(b, a), sub(c, a));
    const double length = std::sqrt(dot(n, n));
    if (length < 1e-12)
      continue; // No area.
    // Walk the projection on the dominant axis: over one column's square the
    // plane spans at most two cells, so the candidates grow with the area.
    unsigned d = 0;
    for (unsigned k = 1; k < 3; ++k)
      if (std::abs(n[k]) > std::abs(n[d]))
        d = k;
    const unsigned u = (d + 1) % 3, w = (d + 2) % 3;
    if (uint64_t(int64_t(high[u]) - low[u] + 1) * uint64_t(int64_t(high[w]) - low[w] + 1) > 64'000'000ull)
      return fail(error, "mesh triangle too large to voxelise");
    const auto depth = [&](double pu, double pw) { return a[d] - (n[u] * (pu - a[u]) + n[w] * (pw - a[w])) / n[d]; };
    for (int32_t cu = low[u]; cu <= high[u]; ++cu)
      for (int32_t cw = low[w]; cw <= high[w]; ++cw)
      {
        double lo = 1e300, hi = -1e300;
        for (int du = 0; du < 2; ++du)
          for (int dw = 0; dw < 2; ++dw)
          {
            const double z = depth(double(cu + du), double(cw + dw));
            lo = std::min(lo, z);
            hi = std::max(hi, z);
          }
        const int64_t k0 = std::max<int64_t>(low[d], int64_t(std::floor(lo)) - 1);
        const int64_t k1 = std::min<int64_t>(high[d], int64_t(std::floor(hi)) + 1);
        for (int64_t k = k0; k <= k1; ++k)
        {
          I at{};
          at[d] = int32_t(k);
          at[u] = cu;
          at[w] = cw;
          if (triangleCell(a, b, c, {double(at[0]) + .5, double(at[1]) + .5, double(at[2]) + .5}))
            surface[at] = Cell{at, triangle.rgba, triangle.materialProfile};
        }
      }
    if (surface.size() > maximumCells)
      return fail(error, "mesh voxelises to more cells than the budget");
  }
  if (surface.empty())
    return true;

  cells.reserve(surface.size());
  for (const auto &[position, cell] : surface)
    cells.push_back(cell);
  if (stats)
    stats->surfaceCells = cells.size();

  if (fillEnclosed)
  {
    I lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
    for (const Cell &cell : cells)
      for (int a = 0; a < 3; ++a)
      {
        lo[a] = std::min(lo[a], cell.position[a] - 1);
        hi[a] = std::max(hi[a], cell.position[a] + 1);
      }
    const uint64_t sx = uint64_t(hi[0] - lo[0] + 1), sy = uint64_t(hi[1] - lo[1] + 1), sz = uint64_t(hi[2] - lo[2] + 1);
    constexpr uint64_t kMaximumFloodCells = 48'000'000ull;
    if (sx * sy * sz <= kMaximumFloodCells)
    {
      // 0 empty, 1 surface, 2 reached from outside. The box has a one-cell
      // empty margin, so its corner is outside.
      std::vector<uint8_t> state(sx * sy * sz, 0u);
      const auto at = [&](int64_t x, int64_t y, int64_t z) { return (uint64_t(y) * sz + uint64_t(z)) * sx + uint64_t(x); };
      for (const Cell &cell : cells)
        state[at(cell.position[0] - lo[0], cell.position[1] - lo[1], cell.position[2] - lo[2])] = 1u;
      std::vector<uint64_t> stack{0u};
      state[0] = 2u;
      while (!stack.empty())
      {
        const uint64_t index = stack.back();
        stack.pop_back();
        const int64_t x = int64_t(index % sx), z = int64_t((index / sx) % sz), y = int64_t(index / (sx * sz));
        const int64_t next[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z}, {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
        for (const auto &p : next)
        {
          if (p[0] < 0 || p[1] < 0 || p[2] < 0 || p[0] >= int64_t(sx) || p[1] >= int64_t(sy) || p[2] >= int64_t(sz))
            continue;
          const uint64_t j = at(p[0], p[1], p[2]);
          if (state[j] == 0u)
          {
            state[j] = 2u;
            stack.push_back(j);
          }
        }
      }
      // Each enclosed cell takes the colour of the surface cell above it in
      // its column, which is what an opened wall would show.
      std::vector<Cell> filled;
      for (int64_t x = 0; x < int64_t(sx); ++x)
        for (int64_t z = 0; z < int64_t(sz); ++z)
        {
          const Cell *above = nullptr;
          for (int64_t y = int64_t(sy) - 1; y >= 0; --y)
          {
            const uint8_t s = state[at(x, y, z)];
            if (s == 1u)
            {
              const auto found = surface.find(I{int32_t(x + lo[0]), int32_t(y + lo[1]), int32_t(z + lo[2])});
              above = found == surface.end() ? above : &found->second;
            }
            else if (s == 0u && above)
              filled.push_back(Cell{I{int32_t(x + lo[0]), int32_t(y + lo[1]), int32_t(z + lo[2])}, above->rgba, above->materialProfile});
          }
        }
      if (cells.size() + filled.size() > maximumCells)
        return fail(error, "filled mesh exceeds the cell budget");
      if (stats)
      {
        stats->filledCells = filled.size();
        stats->filled = true;
      }
      cells.insert(cells.end(), filled.begin(), filled.end());
    }
  }
  std::sort(cells.begin(), cells.end(), [](const Cell &a, const Cell &b) { return a.position < b.position; });
  return true;
}

double cityVoxelMeshPlanYaw(const std::vector<std::array<std::array<double, 3>, 3>> &worldTriangles)
{
  // Candidate directions: the longest horizontal edges, folded to [0, pi/2).
  std::vector<std::pair<double, double>> edges; // (length, angle)
  for (const auto &t : worldTriangles)
    for (int e = 0; e < 3; ++e)
    {
      const auto &p = t[e], &q = t[(e + 1) % 3];
      const double dx = q[0] - p[0], dz = q[2] - p[2];
      const double length = std::hypot(dx, dz);
      if (length < 0.05 || std::abs(q[1] - p[1]) > 0.25 * length)
        continue;
      double angle = std::atan2(dz, dx);
      angle = std::fmod(angle + 4.0 * M_PI, 0.5 * M_PI);
      edges.emplace_back(length, angle);
    }
  std::sort(edges.begin(), edges.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  std::vector<double> candidates{0.0};
  for (size_t i = 0; i < edges.size() && candidates.size() < 9; ++i)
  {
    bool known = false;
    for (double c : candidates)
      known = known || std::abs(c - edges[i].second) < 1e-3;
    if (!known)
      candidates.push_back(edges[i].second);
  }
  double best = 0.0, bestArea = INFINITY;
  for (const double yaw : candidates)
  {
    const double ax = std::cos(yaw), az = std::sin(yaw);
    double lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
    for (const auto &t : worldTriangles)
      for (const auto &p : t)
      {
        const double u = p[0] * ax + p[2] * az, v = -p[0] * az + p[2] * ax;
        lo[0] = std::min(lo[0], u);
        hi[0] = std::max(hi[0], u);
        lo[1] = std::min(lo[1], v);
        hi[1] = std::max(hi[1], v);
      }
    const double area = (hi[0] - lo[0]) * (hi[1] - lo[1]);
    if (area < bestArea - 1e-9)
    {
      bestArea = area;
      best = yaw;
    }
  }
  return best;
}

} // namespace virtualgeometry
