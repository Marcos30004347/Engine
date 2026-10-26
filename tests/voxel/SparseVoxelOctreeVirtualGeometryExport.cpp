// Export one small voxel octree chunk as an encoded virtual geometry asset.
//
// The point is to see the thing. VirtualGeometryRenderer registers only the
// virtual geometry passes - page decode, streaming page selection, culling,
// depth pre-pass, hardware draw, material - and it can now open an already
// encoded `.virtualgeometry` file directly. So:
//
//   SparseVoxelOctreeVirtualGeometryExport --out voxel-chunk.virtualgeometry
//   VirtualGeometryRenderer --asset voxel-chunk.virtualgeometry
//
// and the window shows the voxel surface drawn as triangles by the virtual
// geometry raster, with the material pass writing albedo. Nothing from the
// sparse voxel renderer participates.
//
// Run with no arguments it writes next to the executable's assets directory and
// verifies the file round-trips, which is what the ctest entry checks.

#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "os/File.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;
namespace vg = virtualgeometry;
namespace fs = std::filesystem;

namespace
{

int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

// The same deterministic terrain the page streaming tests use: a flat apron, two
// terrace steps, an isolated pillar and a pit. Small enough to reason about,
// varied enough that the LOD chain has something to lose.
uint32_t columnHeight(const uint32_t x, const uint32_t z, const uint32_t side)
{
  uint32_t height = 4u;
  if (x >= side / 2u)
    height += 6u;
  if (z >= side * 3u / 4u)
    height += 3u;
  if (x == side / 4u && z == side / 4u)
    height += 9u;
  if (x > side / 8u && x < side / 8u + 3u && z > side / 2u && z < side / 2u + 3u)
    height = height > 3u ? height - 3u : 1u;
  return height;
}

// Distinct albedo per material band so the window shows the terraces rather than
// one flat grey. The virtual geometry vertex format carries no per-face colour,
// so the palette lands in the asset's material table instead.
uint32_t bandAlbedo(const uint32_t band)
{
  static constexpr std::array<uint32_t, 4> kColours{
      0xff5f9e4eu, // grass
      0xff7a6a52u, // dirt
      0xff9a9a9au, // rock
      0xffd8d2c0u, // sand
  };
  return kColours[band % kColours.size()];
}

bool buildChunkOctree(
    const uint32_t side,
    svr::SparseVoxelBrickHierarchy &hierarchy,
    uint64_t &occupiedCells,
    std::string *error)
{
  std::vector<svr::SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<svr::SparseVoxelMaterialGPU> materials;
  std::vector<uint32_t> leaves;
  occupiedCells = 0u;

  constexpr uint32_t kShellDepth = 3u;
  for (uint32_t x = 0u; x < side; ++x)
    for (uint32_t z = 0u; z < side; ++z)
    {
      const uint32_t top = columnHeight(x, z, side);
      uint32_t lowestNeighbour = top;
      const std::array<std::array<int32_t, 2>, 4> neighbours{{
          {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
      }};
      for (const auto &offset : neighbours)
      {
        const int32_t nx = static_cast<int32_t>(x) + offset[0];
        const int32_t nz = static_cast<int32_t>(z) + offset[1];
        if (nx < 0 || nz < 0 || nx >= static_cast<int32_t>(side) ||
            nz >= static_cast<int32_t>(side))
          continue;
        lowestNeighbour = std::min(
            lowestNeighbour,
            columnHeight(static_cast<uint32_t>(nx), static_cast<uint32_t>(nz), side));
      }
      const uint32_t floor = std::min(
          top > kShellDepth ? top - kShellDepth : 0u,
          lowestNeighbour > 0u ? lowestNeighbour - 1u : 0u);

      for (uint32_t y = floor; y < top; ++y)
      {
        svr::SparseVoxelCenterLengthGPU cell{};
        cell.center = {
            static_cast<float>(x) + 0.5f,
            static_cast<float>(y) + 0.5f,
            static_cast<float>(z) + 0.5f};
        cell.length = 1.0f;
        svr::SparseVoxelMaterialGPU material{};
        const uint32_t band = y + 1u == top ? (x >= side / 2u ? 0u : 3u)
                                            : (y + 2u >= top ? 1u : 2u);
        material.albedoRGBA8 = bandAlbedo(band);
        material.roughnessMetallicUNorm8 = 0x000000c0u;
        material.materialId = band;
        leaves.push_back(static_cast<uint32_t>(centersLengths.size()));
        centersLengths.push_back(cell);
        materials.push_back(material);
        ++occupiedCells;
      }
    }

  svr::SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  settings.maximumBricks = 1u << 16u;
  return svr::buildSparseVoxelBrickHierarchy(
      centersLengths, materials, leaves, settings, hierarchy, error);
}

} // namespace

int main(int argc, char **argv)
{
  uint32_t side = 64u;
  uint32_t maxGroupsPerPage = 8u;
  uint32_t maxRootPageGroups = 1u;
  fs::path outputPath;

  for (int index = 1; index < argc; ++index)
  {
    const std::string argument = argv[index];
    if (argument == "--out" && index + 1 < argc)
      outputPath = argv[++index];
    else if (argument == "--side" && index + 1 < argc)
      side = static_cast<uint32_t>(std::stoul(argv[++index]));
    else if (argument == "--groups-per-page" && index + 1 < argc)
      maxGroupsPerPage = static_cast<uint32_t>(std::stoul(argv[++index]));
    else if (argument == "--root-page-groups" && index + 1 < argc)
      maxRootPageGroups = static_cast<uint32_t>(std::stoul(argv[++index]));
    else if (argument == "--help" || argument == "-h")
    {
      std::cout
          << "Usage: " << argv[0]
          << " [--out <file.virtualgeometry>] [--side N] [--groups-per-page N]"
             " [--root-page-groups N]\n"
             "  Then view it with:\n"
             "    VirtualGeometryRenderer --asset <file.virtualgeometry>\n";
      return 0;
    }
    else
    {
      std::cerr << "Unknown option: " << argument << '\n';
      return 2;
    }
  }

  if (outputPath.empty())
  {
    const fs::path assetRoot =
        fs::path(os::io::getExecutableDirectory()) / "assets" / "generated";
    std::error_code directoryError;
    fs::create_directories(assetRoot, directoryError);
    outputPath = assetRoot / "voxel-octree-chunk.virtualgeometry";
  }

  std::cout << "Sparse voxel octree virtual geometry export\n";

  svr::SparseVoxelBrickHierarchy hierarchy{};
  uint64_t occupiedCells = 0u;
  std::string error;
  check(buildChunkOctree(side, hierarchy, occupiedCells, &error),
        "voxel chunk octree builds: " + error);
  if (hierarchy.empty())
    return 1;

  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  // Centre the chunk on the origin so the renderer's default framing sees it.
  octreeSettings.originOffset = {
      static_cast<double>(side) * 0.5, 0.0, static_cast<double>(side) * 0.5};

  vg::VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = maxGroupsPerPage;
  buildSettings.maxRootPageGroups = maxRootPageGroups;

  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  check(
      svr::buildSparseVoxelOctreeVirtualGeometry(
          hierarchy,
          0u,
          static_cast<uint32_t>(hierarchy.nodes.size()),
          octreeSettings,
          buildSettings,
          build,
          &stats,
          &error),
      "octree virtual geometry builds: " + error);
  if (build.clusters.empty())
    return 1;

  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;
  quantization.padMeshlets = true;
  vg::VirtualGeometryEncodedData encoded =
      vg::VirtualGeometryCompressor::encode(build, quantization);
  check(!encoded.pages.empty(), "the encoded asset has pages");
  check(!encoded.hierarchy.empty(), "the encoded asset has hierarchy nodes");
  check(!encoded.shapes.empty(), "the encoded asset has a shape");

  {
    vg::VirtualGeometryFile writer(outputPath.string(), /*write=*/true);
    check(writer.isOpen(), "the output file opens for writing: " + outputPath.string());
    if (!writer.isOpen())
      return 1;
    check(
        writer.write(encoded, build.pages, vg::MESHLET_LZ4),
        "the encoded asset writes to disk");
  }

  // Round-trip: what the renderer will read has to be what was built.
  vg::VirtualGeometryEncodedData reloaded{};
  {
    vg::VirtualGeometryFile reader(outputPath.string(), /*write=*/false);
    check(reader.isOpen(), "the written file opens for reading");
    check(reader.readAll(reloaded), "the written file reads back");
  }
  check(reloaded.pages.size() == encoded.pages.size(), "page count survives the round trip");
  check(
      reloaded.hierarchy.size() == encoded.hierarchy.size(),
      "hierarchy node count survives the round trip");

  uint32_t writtenMeshlets = 0u;
  uint32_t reloadedMeshlets = 0u;
  uint32_t emptyMeshlets = 0u;
  for (const auto &page : encoded.pages)
    writtenMeshlets += static_cast<uint32_t>(page.meshlets.size());
  for (const auto &page : reloaded.pages)
    for (const auto &meshlet : page.meshlets)
    {
      ++reloadedMeshlets;
      if (meshlet.vertex_count == 0u || meshlet.triangle_count == 0u)
        ++emptyMeshlets;
    }
  check(reloadedMeshlets == writtenMeshlets, "meshlet count survives the round trip");
  check(emptyMeshlets == 0u, "every reloaded meshlet has vertices and triangles");

  std::cout << std::fixed << std::setprecision(3)
            << "  chunk side=" << side << " cells=" << occupiedCells
            << " octreeLevels=" << stats.levelCount
            << " bricks=" << stats.nodeCount << '\n'
            << "  faces=" << stats.exposedFaceCount << " quads=" << stats.quadCount
            << " clusters=" << stats.clusterCount << " triangles=" << stats.triangleCount
            << " groups=" << stats.groupCount << '\n'
            << "  pages=" << encoded.pages.size()
            << " page0 clusters=" << build.pages[0].clusterCount
            << " meshlets=" << writtenMeshlets
            << " build_ms=" << stats.meshingMilliseconds + stats.hierarchyMilliseconds
            << '\n'
            << "  wrote " << outputPath.string() << '\n'
            << "  view it with: VirtualGeometryRenderer --asset "
            << outputPath.string() << '\n';

  if (failures != 0)
  {
    std::cout << failures << " sparse voxel octree export checks failed\n";
    return 1;
  }
  std::cout << "all sparse voxel octree export checks passed\n";
  return 0;
}
