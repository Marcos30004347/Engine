#include "voxel/svraster/rendering/SparseVoxelPlanetBackgroundPass.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

using voxel::svraster::rendering::SparseVoxelPlanetBackgroundGrid;
using voxel::svraster::rendering::sparseVoxelPlanetBackgroundGridIndexCount;
using voxel::svraster::rendering::sparseVoxelPlanetBackgroundGridIndices;
using voxel::svraster::rendering::sparseVoxelPlanetBackgroundGridStride;
using voxel::svraster::rendering::sparseVoxelPlanetBackgroundVisibleRingCount;

void check(const bool condition, const std::string &message)
{
  if (condition) return;
  std::cerr << "SparseVoxelPlanetBackgroundTests: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

} // namespace

int main()
{
  const SparseVoxelPlanetBackgroundGrid small{4u, 8u};
  check(sparseVoxelPlanetBackgroundGridStride(small, 1u) == 1u,
      "full-quality stride remains one");
  check(sparseVoxelPlanetBackgroundGridStride(small, 3u) == 2u,
      "unsupported stride rounds down to a divisible power of two");
  check(sparseVoxelPlanetBackgroundGridStride(small, 8u) == 4u,
      "stride cannot skip the entire radial grid");

  const auto full = sparseVoxelPlanetBackgroundGridIndices(small, 1u);
  const auto half = sparseVoxelPlanetBackgroundGridIndices(small, 2u);
  check(full.size() == 8u * 3u + 3u * 8u * 6u,
      "full grid retains every source triangle");
  check(half.size() == 4u * 3u + 4u * 6u,
      "half-axis grid submits one quarter of the triangles");
  check(*std::max_element(half.begin(), half.end()) < 33u,
      "decimated indices stay inside the shared vertex buffer");
  check(sparseVoxelPlanetBackgroundGridIndexCount(
            small, 2u, 2u) == 12u,
      "one visible submitted ring draws only its centre fan");
  check(sparseVoxelPlanetBackgroundGridIndexCount(
            small, 2u, 4u) == half.size(),
      "the full visible prefix covers the decimated index buffer");

  const SparseVoxelPlanetBackgroundGrid production{512u, 1024u};
  const uint32_t flatVisible =
      sparseVoxelPlanetBackgroundVisibleRingCount(
          production, 2u, 320'000.0, 320'002.0,
          0.0, 0.0, 512'000.0, 0.0, true);
  check(flatVisible > 0u && flatVisible < production.ringCount,
      "a surface camera rejects rings hidden beyond the flat horizon");
  check(flatVisible % 2u == 0u,
      "the visible boundary lands on a submitted ring");
  const uint32_t reliefVisible =
      sparseVoxelPlanetBackgroundVisibleRingCount(
          production, 2u, 320'000.0, 320'002.0,
          -1'000.0, 4'000.0, 512'000.0, 0.0, true);
  check(reliefVisible >= flatVisible,
      "terrain relief conservatively expands the visible horizon");
  const uint32_t offsetVisible =
      sparseVoxelPlanetBackgroundVisibleRingCount(
          production, 2u, 320'000.0, 320'002.0,
          0.0, 0.0, 512'000.0, 20'000.0, true);
  check(offsetVisible >= flatVisible,
      "movement away from the mesh anchor expands the retained prefix");
  check(sparseVoxelPlanetBackgroundVisibleRingCount(
            production, 2u, 320'000.0, 320'002.0,
            0.0, 0.0, 512'000.0, 0.0, false) == production.ringCount,
      "the diagnostic switch restores the complete cap");
  check(sparseVoxelPlanetBackgroundVisibleRingCount(
            production, 2u, 320'000.0, 319'999.0,
            0.0, 0.0, 512'000.0, 0.0, true) == production.ringCount,
      "a camera inside the conservative inner sphere cannot horizon-cull");

  std::cout << "Sparse voxel planet background tests passed\n";
  return EXIT_SUCCESS;
}
