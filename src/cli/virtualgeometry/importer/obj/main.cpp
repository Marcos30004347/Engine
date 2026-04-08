#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli/common/CliToolUtils.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"

namespace
{

namespace fs = std::filesystem;
using virtualgeometry::MeshletCompression;
using virtualgeometry::MESHLET_LZ4;
using virtualgeometry::MESHLET_MINIZ;
using virtualgeometry::MESHLET_RAW;
using virtualgeometry::QuantizationConfig;
using virtualgeometry::VirtualGeometryBuildData;
using virtualgeometry::VirtualGeometryBuildSettings;
using virtualgeometry::VirtualGeometryCompressor;
using virtualgeometry::VirtualGeometryEncoder;
using virtualgeometry::VirtualGeometryFile;

struct Options
{
  std::string inputPath;
  std::string outputPath;
  std::string materialsDir;
  MeshletCompression compression = MESHLET_LZ4;
  QuantizationConfig quantization{};
  VirtualGeometryBuildSettings buildSettings{};
  bool helpRequested = false;
};

void printHelp(std::ostream &stream, const char *programName)
{
  stream
      << "Usage: " << programName << " --input <mesh.obj> --output <mesh.virtualgeometry> [options]\n"
      << "\n"
      << "Create a virtual geometry file from an OBJ mesh.\n"
      << "\n"
      << "Options:\n"
      << "  -h, --help                      Show this help message\n"
      << "  --input <path>                  Source OBJ file\n"
      << "  --output <path>                 Output .virtualgeometry file\n"
      << "  --materials-dir <dir>           Optional directory for generated .vmat files\n"
      << "  --compression <raw|miniz|lz4>   Meshlet page compression (default: lz4)\n"
      << "  --quantization-factor <0-255>   Position quantization factor (default: 4)\n"
      << "  --unit-scale <float>            Unit scale stored in the file (default: 100)\n"
      << "  --pad-meshlets <true|false>     Pad meshlets to full triangle capacity (default: true)\n"
      << "  --pad-pages <true|false>        Pad pages to the max page size (default: false)\n"
      << "  --max-groups-per-page <count>   Max cluster groups per page (default: 64)\n"
      << "  --max-root-page-groups <count>  Root-page group limit, 0 reuses max-groups-per-page\n"
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
  }

  return options;
}

int run(const Options &options)
{
  const fs::path inputPath = fs::path(options.inputPath).lexically_normal();
  const fs::path outputPath = fs::path(options.outputPath).lexically_normal();

  cli::common::requireFileExists(inputPath, "--input");
  cli::common::ensureParentDirectory(outputPath);

  const VirtualGeometryBuildData build =
      VirtualGeometryEncoder::buildFromOBJFile(inputPath.string(), options.buildSettings, options.materialsDir);
  const auto encoded = VirtualGeometryCompressor::encode(build, options.quantization);

  VirtualGeometryFile writer(outputPath.string(), true);
  if (!writer.isOpen())
    throw std::runtime_error("Failed to open output file for writing: " + outputPath.string());
  if (!writer.write(encoded, build.pages, options.compression))
    throw std::runtime_error("Failed to write virtual geometry file: " + outputPath.string());

  std::cout
      << "Wrote " << outputPath.string()
      << " with " << encoded.shapes.size() << " shape(s), "
      << build.pages.size() << " page(s), and "
      << encoded.materialFiles.size() << " material file reference(s)\n";
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
