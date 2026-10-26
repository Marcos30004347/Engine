#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "cli/common/CliToolUtils.hpp"
#include "editor/virtualgeometry/FallbackMeshBuilder.hpp"
#include "editor/virtualgeometry/SparseSDFBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "softwarebvh/SoftwareRayTraceSceneFile.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"

namespace
{

namespace fs = std::filesystem;
using virtualgeometry::MeshletCompression;
using virtualgeometry::MESHLET_LZ4;
using virtualgeometry::MESHLET_MINIZ;
using virtualgeometry::MESHLET_RAW;
using virtualgeometry::QuantizationConfig;
using virtualgeometry::FallbackMeshBuilder;
using virtualgeometry::FallbackMeshData;
using virtualgeometry::SparseSDFBuildSettings;
using virtualgeometry::SparseSDFBuilder;
using virtualgeometry::VirtualGeometryBuildData;
using virtualgeometry::VirtualGeometryBuildSettings;
using virtualgeometry::VirtualGeometryCompressor;
using virtualgeometry::VirtualGeometryEncoder;
using virtualgeometry::VirtualGeometryFile;
using virtualgeometry::VirtualGeometrySourceAsset;

struct Options
{
  std::string inputPath;
  std::string outputPath;
  std::string vsdfOutputPath;
  std::string materialsDir;
  MeshletCompression compression = MESHLET_LZ4;
  QuantizationConfig quantization{};
  VirtualGeometryBuildSettings buildSettings{};
  SparseSDFBuildSettings sdfSettings{};
  bool emitVSDF = false;
  bool helpRequested = false;
};

void printHelp(std::ostream &stream, const char *programName)
{
  stream
      << "Usage: " << programName << " --input <mesh.obj> --output <mesh.virtualgeometry> [options]\n"
      << "\n"
      << "Create a virtual geometry file from an OBJ mesh.\n"
      << "Also generates sibling <name>.softwareraytrace software ray tracing data.\n"
      << "\n"
      << "Options:\n"
      << "  -h, --help                      Show this help message\n"
      << "  --input <path>                  Source OBJ file\n"
      << "  --output <path>                 Output .virtualgeometry file\n"
      << "  --materials-dir <dir>           Optional directory for generated .vmat and .vimg files\n"
      << "  --compression <raw|miniz|lz4>   Meshlet page compression (default: lz4)\n"
      << "  --quantization-factor <0-255>   Position quantization factor (default: 4)\n"
      << "  --unit-scale <float>            Unit scale stored in the file (default: 100)\n"
      << "  --pad-meshlets <true|false>     Pad meshlets to full triangle capacity (default: true)\n"
      << "  --pad-pages <true|false>        Pad pages to the max page size (default: false)\n"
      << "  --max-groups-per-page <count>   Max cluster groups per page (default: 64)\n"
      << "  --max-root-page-groups <count>  Root-page group limit, 0 reuses max-groups-per-page\n"
      << "  --emit-vsdf <true|false>        Generate sibling sparse instance SDF file (default: false)\n"
      << "  --vsdf-output <path>            Output .vsdf file, default is sibling of output\n"
      << "  --sdf-error <float>             VSDF target mesh-to-voxel volume ratio (default: 0.05)\n"
      << "  --sdf-max-level <count>         VSDF max adaptive level (default: 1)\n"
      << "  --sdf-guard-levels <count>      Extra occupied leaf refinement levels for crack padding (default: 0)\n"
      << "  --sdf-workers <count>           Worker threads for VSDF brick emission (default: 64)\n"
      << "  --sdf-max-refined-cells <count> Max child cells refined by one brick (default: 16)\n"
      << "  --sdf-close-voxels <count>      Voxel radius for closing open surface gaps in volume pass (default: 1)\n"
      << "\n"
      << "Example:\n"
      << "  " << programName << " --input assets/mesh.obj --output build/mesh.virtualgeometry \\\n"
      << "    --materials-dir build/materials --compression lz4\n";
}

MeshletCompression parseCompression(const std::string &value)
{
  const std::string normalized = cli::common::toLower(value);
  if (normalized == "raw")
    return MESHLET_RAW;
  if (normalized == "miniz")
    return MESHLET_MINIZ;
  if (normalized == "lz4")
    return MESHLET_LZ4;
  throw std::runtime_error("Invalid value for --compression: " + value + " (expected raw|miniz|lz4)");
}

fs::path buildSoftwareRaytraceScenePath(const fs::path &virtualGeometryPath)
{
  return virtualGeometryPath.parent_path() /
         (virtualGeometryPath.stem().string() + ".softwareraytrace");
}

fs::path buildVSDFPath(const fs::path &virtualGeometryPath)
{
  return virtualGeometryPath.parent_path() /
         (virtualGeometryPath.stem().string() + ".vsdf");
}

void writeFallbackMeshCollection(
    const fs::path &virtualGeometryPath,
    const std::vector<FallbackMeshData> &proxyMeshes)
{
  std::vector<softwarebvh::SoftwareRayTraceSceneInstanceData> sceneEntries;
  sceneEntries.reserve(proxyMeshes.size());
  std::unordered_map<uint32_t, uint32_t> shapeIndexByLod;
  for (const FallbackMeshData &proxyMesh : proxyMeshes)
  {
    const uint32_t shapeIndex = shapeIndexByLod[proxyMesh.lodLevel]++;
    sceneEntries.push_back(softwarebvh::SoftwareRayTraceSceneFile::buildInstance(
        proxyMesh,
        virtualgeometry::INVALID_INSTANCE_ID,
        virtualgeometry::INVALID_SCENE_INSTANCE_ID,
        shapeIndex,
        virtualGeometryPath.string()));
  }

  const fs::path scenePath = buildSoftwareRaytraceScenePath(virtualGeometryPath);
  if (!softwarebvh::SoftwareRayTraceSceneFile::save(scenePath.string(), sceneEntries))
    throw std::runtime_error("Failed to write software raytrace scene file: " + scenePath.string());
}

Options parseArgs(int argc, char **argv)
{
  Options options;
  const std::vector<std::string> args = cli::common::collectArgs(argc, argv);
  for (size_t index = 0u; index < args.size(); ++index)
  {
    const std::string &arg = args[index];
    if (cli::common::isHelpFlag(arg))
    {
      options.helpRequested = true;
    }
    else if (arg == "--input")
    {
      options.inputPath = cli::common::requireOptionValue(args, index, "--input");
    }
    else if (arg == "--output")
    {
      options.outputPath = cli::common::requireOptionValue(args, index, "--output");
    }
    else if (arg == "--materials-dir")
    {
      options.materialsDir = cli::common::requireOptionValue(args, index, "--materials-dir");
    }
    else if (arg == "--compression")
    {
      options.compression = parseCompression(cli::common::requireOptionValue(args, index, "--compression"));
    }
    else if (arg == "--quantization-factor")
    {
      options.quantization.quantization_factor =
          cli::common::parseUint8(cli::common::requireOptionValue(args, index, "--quantization-factor"), "--quantization-factor");
    }
    else if (arg == "--unit-scale")
    {
      options.quantization.unit_scale =
          cli::common::parseNumber<float>(cli::common::requireOptionValue(args, index, "--unit-scale"), "--unit-scale");
    }
    else if (arg == "--pad-meshlets")
    {
      options.quantization.padMeshlets =
          cli::common::parseBool(cli::common::requireOptionValue(args, index, "--pad-meshlets"), "--pad-meshlets");
    }
    else if (arg == "--pad-pages")
    {
      options.buildSettings.padPagesToMaxSize =
          cli::common::parseBool(cli::common::requireOptionValue(args, index, "--pad-pages"), "--pad-pages");
    }
    else if (arg == "--max-groups-per-page")
    {
      options.buildSettings.maxGroupsPerPage =
          cli::common::parseNumber<uint32_t>(cli::common::requireOptionValue(args, index, "--max-groups-per-page"), "--max-groups-per-page");
    }
    else if (arg == "--max-root-page-groups")
    {
      options.buildSettings.maxRootPageGroups =
          cli::common::parseNumber<uint32_t>(cli::common::requireOptionValue(args, index, "--max-root-page-groups"), "--max-root-page-groups");
    }
    else if (arg == "--emit-vsdf")
    {
      options.emitVSDF = cli::common::parseBool(cli::common::requireOptionValue(args, index, "--emit-vsdf"), "--emit-vsdf");
    }
    else if (arg == "--vsdf-output")
    {
      options.vsdfOutputPath = cli::common::requireOptionValue(args, index, "--vsdf-output");
      options.emitVSDF = true;
    }
    else if (arg == "--sdf-error")
    {
      options.sdfSettings.requestedError =
          cli::common::parseNumber<float>(cli::common::requireOptionValue(args, index, "--sdf-error"), "--sdf-error");
      options.emitVSDF = true;
    }
    else if (arg == "--sdf-max-level")
    {
      const int parsedMaxLevel =
          cli::common::parseNumber<int>(cli::common::requireOptionValue(args, index, "--sdf-max-level"), "--sdf-max-level");
      if (parsedMaxLevel < 0)
        throw std::runtime_error("--sdf-max-level must be greater than or equal to zero");
      options.sdfSettings.maxLevel = static_cast<uint32_t>(parsedMaxLevel);
      options.emitVSDF = true;
    }
    else if (arg == "--sdf-guard-levels")
    {
      const int parsedGuardLevels =
          cli::common::parseNumber<int>(cli::common::requireOptionValue(args, index, "--sdf-guard-levels"), "--sdf-guard-levels");
      if (parsedGuardLevels < 0)
        throw std::runtime_error("--sdf-guard-levels must be greater than or equal to zero");
      options.sdfSettings.guardLeafLevels = static_cast<uint32_t>(parsedGuardLevels);
      options.emitVSDF = true;
    }
    else if (arg == "--sdf-workers")
    {
      const int parsedWorkers =
          cli::common::parseNumber<int>(cli::common::requireOptionValue(args, index, "--sdf-workers"), "--sdf-workers");
      if (parsedWorkers <= 0)
        throw std::runtime_error("--sdf-workers must be greater than zero");
      options.sdfSettings.sdfWorkerCount = static_cast<uint32_t>(parsedWorkers);
      options.emitVSDF = true;
    }
    else if (arg == "--sdf-max-refined-cells")
    {
      const int parsedMaxRefinedCells =
          cli::common::parseNumber<int>(cli::common::requireOptionValue(args, index, "--sdf-max-refined-cells"), "--sdf-max-refined-cells");
      if (parsedMaxRefinedCells <= 0)
        throw std::runtime_error("--sdf-max-refined-cells must be greater than zero");
      options.sdfSettings.maxRefinedCellsPerBrick = static_cast<uint32_t>(parsedMaxRefinedCells);
      options.emitVSDF = true;
    }
    else if (arg == "--sdf-close-voxels")
    {
      const int parsedCloseVoxels =
          cli::common::parseNumber<int>(cli::common::requireOptionValue(args, index, "--sdf-close-voxels"), "--sdf-close-voxels");
      if (parsedCloseVoxels < 0)
        throw std::runtime_error("--sdf-close-voxels must be greater than or equal to zero");
      options.sdfSettings.surfaceCloseVoxels = static_cast<uint32_t>(parsedCloseVoxels);
      options.emitVSDF = true;
    }
    else
    {
      throw std::runtime_error("Unknown argument: " + arg);
    }
  }

  if (!options.helpRequested)
  {
    if (options.inputPath.empty())
      throw std::runtime_error("Missing required argument --input");
    if (options.outputPath.empty())
      throw std::runtime_error("Missing required argument --output");
    if (options.buildSettings.maxGroupsPerPage == 0u)
      throw std::runtime_error("--max-groups-per-page must be greater than zero");
    if (options.quantization.unit_scale <= 0.0f)
      throw std::runtime_error("--unit-scale must be greater than zero");
    if (options.emitVSDF && options.sdfSettings.requestedError <= 0.0f)
      throw std::runtime_error("--sdf-error must be greater than zero");
  }

  return options;
}

int run(const Options &options)
{
  const fs::path inputPath = fs::path(options.inputPath).lexically_normal();
  const fs::path outputPath = fs::path(options.outputPath).lexically_normal();

  cli::common::requireFileExists(inputPath, "--input");
  cli::common::ensureParentDirectory(outputPath);

  const VirtualGeometrySourceAsset asset = VirtualGeometryEncoder::loadAssetFromOBJFile(inputPath.string(), options.materialsDir);
  const VirtualGeometryBuildData build = virtualgeometry::VirtualGeometryBuilder::build(asset, options.buildSettings);
  const auto encoded = VirtualGeometryCompressor::encode(build, options.quantization);
  const std::vector<FallbackMeshData> fallbackMeshes = FallbackMeshBuilder::buildSDFProxyPerShape(asset);

  VirtualGeometryFile writer(outputPath.string(), true);
  if (!writer.isOpen())
    throw std::runtime_error("Failed to open output file for writing: " + outputPath.string());
  if (!writer.write(encoded, build.pages, options.compression))
    throw std::runtime_error("Failed to write virtual geometry file: " + outputPath.string());

  writeFallbackMeshCollection(outputPath, fallbackMeshes);

  if (options.emitVSDF)
  {
    const fs::path vsdfPath = options.vsdfOutputPath.empty() ? buildVSDFPath(outputPath) : fs::path(options.vsdfOutputPath).lexically_normal();
    cli::common::ensureParentDirectory(vsdfPath);
    std::string vsdfError;
    if (!SparseSDFBuilder::writeVSDF(vsdfPath.string(), asset, options.sdfSettings, &vsdfError))
      throw std::runtime_error("Failed to write VSDF file: " + vsdfPath.string() + " (" + vsdfError + ")");
    std::cout << "Wrote " << vsdfPath.string() << " sparse instance SDF sidecar\n";
  }

  std::cout
      << "Wrote " << outputPath.string()
      << " with " << encoded.shapes.size() << " shape(s), "
      << build.pages.size() << " page(s), and "
      << encoded.materialFiles.size() << " material file reference(s)"
      << "; generated " << fallbackMeshes.size() << " fallback mesh entry(s)\n";
  return 0;
}

} // namespace

int main(int argc, char **argv)
{
  try
  {
    if (argc <= 1)
    {
      printHelp(std::cerr, argv[0]);
      return 1;
    }

    const Options options = parseArgs(argc, argv);
    if (options.helpRequested)
    {
      printHelp(std::cout, argv[0]);
      return 0;
    }

    return run(options);
  }
  catch (const std::exception &exception)
  {
    std::cerr << "Error: " << exception.what() << '\n';
    std::cerr << "Use -h or --help for usage.\n";
    return 1;
  }
}
