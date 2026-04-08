#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli/common/CliToolUtils.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"
#include "virtualgeometry/VirtualTextureSystem.hpp"

namespace
{

namespace fs = std::filesystem;
using virtualgeometry::MAX_TEXTURES_PER_MATERIAL;
using virtualgeometry::VirtualMaterialFile;
using virtualgeometry::VirtualTextureSystem;

struct Options
{
  std::string outputPath;
  std::vector<std::string> textures;
  bool flipVertically = false;
  VirtualTextureSystem::TextureSampling sampling{};
  bool helpRequested = false;
};

void printHelp(std::ostream &stream, const char *programName)
{
  stream
      << "Usage: " << programName << " --output <material.vmat> --texture <image> [--texture <image> ...] [options]\n"
      << "\n"
      << "Create a virtual-texture-backed material file from source images such as PNG and JPEG.\n"
      << "Each --texture fills the next texture slot in the generated .vmat file.\n"
      << "\n"
      << "Options:\n"
      << "  -h, --help                      Show this help message\n"
      << "  --output <path>                 Output .vmat file\n"
      << "  --texture <path>                Source image path, repeat up to " << MAX_TEXTURES_PER_MATERIAL << " times\n"
      << "  --flip-vertical <true|false>    Flip every input image on load (default: false)\n"
      << "  --address-mode-u <repeat|clamp> Address mode for U (default: repeat)\n"
      << "  --address-mode-v <repeat|clamp> Address mode for V (default: repeat)\n"
      << "  --filter <nearest|linear>       Texture filter mode (default: nearest)\n"
      << "  --mip-bias <float>              Bias applied to mip selection (default: 0)\n"
      << "  --min-mip <index>               Minimum streamed mip level (default: 0)\n"
      << "  --max-mip <index>               Maximum streamed mip level (default: 15)\n"
      << "\n"
      << "Example:\n"
      << "  " << programName << " --output build/materials/stone.vmat \\\n"
      << "    --texture albedo.png --texture normal.png --filter linear\n";
}

VirtualTextureSystem::TextureAddressMode parseAddressMode(const std::string &value, const char *option)
{
  const std::string normalized = cli::common::toLower(value);
  if (normalized == "repeat")
    return VirtualTextureSystem::TextureAddressMode::Repeat;
  if (normalized == "clamp" || normalized == "clamptoedge" || normalized == "clamp_to_edge")
    return VirtualTextureSystem::TextureAddressMode::ClampToEdge;
  throw std::runtime_error(std::string("Invalid value for ") + option + ": " + value + " (expected repeat|clamp)");
}

VirtualTextureSystem::TextureFilterMode parseFilterMode(const std::string &value)
{
  const std::string normalized = cli::common::toLower(value);
  if (normalized == "nearest")
    return VirtualTextureSystem::TextureFilterMode::Nearest;
  if (normalized == "linear")
    return VirtualTextureSystem::TextureFilterMode::Linear;
  throw std::runtime_error("Invalid value for --filter: " + value + " (expected nearest|linear)");
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
    else if (arg == "--output")
    {
      options.outputPath = cli::common::requireOptionValue(args, index, "--output");
    }
    else if (arg == "--texture")
    {
      options.textures.push_back(cli::common::requireOptionValue(args, index, "--texture"));
    }
    else if (arg == "--flip-vertical")
    {
      options.flipVertically =
          cli::common::parseBool(cli::common::requireOptionValue(args, index, "--flip-vertical"), "--flip-vertical");
    }
    else if (arg == "--address-mode-u")
    {
      options.sampling.addressModeU =
          parseAddressMode(cli::common::requireOptionValue(args, index, "--address-mode-u"), "--address-mode-u");
    }
    else if (arg == "--address-mode-v")
    {
      options.sampling.addressModeV =
          parseAddressMode(cli::common::requireOptionValue(args, index, "--address-mode-v"), "--address-mode-v");
    }
    else if (arg == "--filter")
    {
      options.sampling.filterMode = parseFilterMode(cli::common::requireOptionValue(args, index, "--filter"));
    }
    else if (arg == "--mip-bias")
    {
      options.sampling.mipBias =
          cli::common::parseNumber<float>(cli::common::requireOptionValue(args, index, "--mip-bias"), "--mip-bias");
    }
    else if (arg == "--min-mip")
    {
      options.sampling.minMip =
          cli::common::parseNumber<uint32_t>(cli::common::requireOptionValue(args, index, "--min-mip"), "--min-mip");
    }
    else if (arg == "--max-mip")
    {
      options.sampling.maxMip =
          cli::common::parseNumber<uint32_t>(cli::common::requireOptionValue(args, index, "--max-mip"), "--max-mip");
    }
    else
    {
      throw std::runtime_error("Unknown argument: " + arg);
    }
  }

  if (!options.helpRequested)
  {
    if (options.outputPath.empty())
      throw std::runtime_error("Missing required argument --output");
    if (options.textures.empty())
      throw std::runtime_error("At least one --texture argument is required");
    if (options.textures.size() > MAX_TEXTURES_PER_MATERIAL)
      throw std::runtime_error("Too many --texture arguments, maximum is " + std::to_string(MAX_TEXTURES_PER_MATERIAL));
    if (options.sampling.minMip > options.sampling.maxMip)
      throw std::runtime_error("--min-mip must be less than or equal to --max-mip");
  }

  return options;
}

int run(const Options &options)
{
  const fs::path outputPath = fs::path(options.outputPath).lexically_normal();
  cli::common::ensureParentDirectory(outputPath);

  VirtualTextureSystem::MaterialCreateInfo createInfo{};
  createInfo.textureCount = static_cast<uint32_t>(options.textures.size());
  for (size_t textureIndex = 0u; textureIndex < options.textures.size(); ++textureIndex)
  {
    const fs::path inputPath = fs::path(options.textures[textureIndex]).lexically_normal();
    cli::common::requireFileExists(inputPath, "--texture");

    auto &texture = createInfo.textures[textureIndex];
    texture.source.path = inputPath.string();
    texture.source.flipVertically = options.flipVertically;
    texture.sampling = options.sampling;
  }

  if (!VirtualMaterialFile::saveFromCreateInfo(outputPath.string(), createInfo))
    throw std::runtime_error("Failed to write virtual texture material file: " + outputPath.string());

  std::cout
      << "Wrote " << outputPath.string()
      << " with " << options.textures.size() << " texture slot(s)\n";
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
