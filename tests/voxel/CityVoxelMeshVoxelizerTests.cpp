#undef NDEBUG
#include "editor/virtualgeometry/CityVoxelMeshVoxelizer.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>

using namespace virtualgeometry;
using Triangle = std::array<std::array<double, 3>, 3>;

namespace
{
// A closed box [lo, hi] (lattice units), outward-facing.
std::vector<CityVoxelMeshTriangle> box(std::array<double, 3> lo, std::array<double, 3> hi, uint32_t rgba)
{
  std::vector<CityVoxelMeshTriangle> out;
  const auto p = [&](int i) { return std::array<double, 3>{i & 1 ? hi[0] : lo[0], i & 2 ? hi[1] : lo[1], i & 4 ? hi[2] : lo[2]}; };
  const int faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  for (const auto &f : faces)
  {
    out.push_back({{p(f[0]), p(f[1]), p(f[2])}, rgba, 1u});
    out.push_back({{p(f[0]), p(f[2]), p(f[3])}, rgba, 1u});
  }
  return out;
}
} // namespace

int main()
{
  // A 3 m x 1 m x 2 m wall block at 10 cm: the closed box fills solid, so
  // every cell of the box is occupied once.
  {
    std::vector<voxel::svraster::CityVoxelCell> cells;
    CityVoxelMeshVoxelizeStats stats;
    std::string error;
    assert(voxelizeCityVoxelMesh(box({0.25, 0.25, 0.25}, {30.25, 20.25, 10.25}, 0xff0000ffu), true, 1u << 20, cells, &stats, &error));
    assert(stats.filled);
    std::set<std::array<int32_t, 3>> unique;
    for (const auto &cell : cells)
      unique.insert(cell.position);
    assert(unique.size() == cells.size());
    // Cells touched by [0.25, 30.25]: 0..30 on x, 0..20 on y, 0..10 on z.
    assert(cells.size() == size_t(31 * 21 * 11));
    assert(stats.filledCells == size_t(29 * 19 * 9));
    for (size_t i = 1; i < cells.size(); ++i)
      assert(cells[i - 1].position < cells[i].position);
  }
  // Without the fill the box is a shell.
  {
    std::vector<voxel::svraster::CityVoxelCell> cells;
    CityVoxelMeshVoxelizeStats stats;
    assert(voxelizeCityVoxelMesh(box({0.25, 0.25, 0.25}, {30.25, 20.25, 10.25}, 0xff0000ffu), false, 1u << 20, cells, &stats));
    assert(!stats.filled && stats.filledCells == 0u);
    assert(cells.size() == size_t(31 * 21 * 11 - 29 * 19 * 9));
  }
  // An open quad fills nothing.
  {
    std::vector<CityVoxelMeshTriangle> quad{{{{{0.5, 0.5, 0.5}, {10.5, 0.5, 0.5}, {10.5, 0.5, 10.5}}}, 0xff00ff00u, 1u},
                                            {{{{0.5, 0.5, 0.5}, {10.5, 0.5, 10.5}, {0.5, 0.5, 10.5}}}, 0xff00ff00u, 1u}};
    std::vector<voxel::svraster::CityVoxelCell> cells;
    CityVoxelMeshVoxelizeStats stats;
    assert(voxelizeCityVoxelMesh(quad, true, 1u << 20, cells, &stats));
    assert(stats.filledCells == 0u && cells.size() == 11u * 11u);
  }
  // The plan yaw of a wall at 30 degrees is 30 degrees: its box in that
  // frame is the wall's own footprint.
  {
    const double yaw = 30.0 * M_PI / 180.0, c = std::cos(yaw), s = std::sin(yaw);
    std::vector<Triangle> wall;
    for (const auto &t : box({0, 0, 0}, {40, 8, 2}, 0u))
    {
      Triangle w;
      for (int k = 0; k < 3; ++k)
        w[k] = {t.corners[k][0] * c - t.corners[k][2] * s, t.corners[k][1], t.corners[k][0] * s + t.corners[k][2] * c};
      wall.push_back(w);
    }
    assert(std::abs(cityVoxelMeshPlanYaw(wall) - yaw) < 1e-6);
  }
  std::printf("CityVoxelMeshVoxelizerTests passed\n");
  return 0;
}
