#include "atmosphere/AtmosphereReference.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{

uint32_t parsePositiveU32(const char *value, const char *option)
{
  const unsigned long parsed = std::stoul(value);
  if (parsed == 0ul || parsed > 1'000'000ul)
    throw std::runtime_error(std::string(option) + " must be in [1, 1000000]");
  return static_cast<uint32_t>(parsed);
}

void printUsage()
{
  std::cout
      << "Usage: AtmosphereReference [options]\n"
      << "  --output DIR     Output directory (default: atmosphere-reference)\n"
      << "  --width N        LUT width (default: 256)\n"
      << "  --height N       LUT height (default: 64)\n"
      << "  --steps N        Midpoint integration steps (default: 256)\n"
      << "  --help           Show this help\n";
}

} // namespace

int main(int argc, char **argv)
{
  try
  {
    std::filesystem::path output = "atmosphere-reference";
    atmosphere::AtmosphereReferenceExportSettings settings{};
    for (int index = 1; index < argc; ++index)
    {
      const std::string argument = argv[index];
      if (argument == "--help")
      {
        printUsage();
        return EXIT_SUCCESS;
      }
      if (index + 1 >= argc)
        throw std::runtime_error("Missing value after " + argument);
      if (argument == "--output")
        output = argv[++index];
      else if (argument == "--width")
        settings.width = parsePositiveU32(argv[++index], "--width");
      else if (argument == "--height")
        settings.height = parsePositiveU32(argv[++index], "--height");
      else if (argument == "--steps")
        settings.integrationSteps = parsePositiveU32(argv[++index], "--steps");
      else
        throw std::runtime_error("Unknown option: " + argument);
    }

    std::string error;
    if (!atmosphere::writeTransmittanceReference(
            output, atmosphere::earthAtmosphereProfile(), settings, &error))
    {
      std::cerr << "Atmosphere reference export failed: " << error << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "Wrote " << (output / "transmittance.csv") << '\n'
              << "Wrote " << (output / "transmittance-probes.csv") << '\n'
              << "Wrote " << (output / "transmittance.ppm") << '\n'
              << "Wrote " << (output / "reference.txt") << '\n';
    return EXIT_SUCCESS;
  }
  catch (const std::exception &exception)
  {
    std::cerr << "AtmosphereReference: " << exception.what() << '\n';
    printUsage();
    return EXIT_FAILURE;
  }
}
