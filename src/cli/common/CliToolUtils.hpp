#pragma once

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace cli::common
{

inline std::vector<std::string> collectArgs(int argc, char **argv)
{
  std::vector<std::string> args;
  args.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0u);
  for (int index = 1; index < argc; ++index)
    args.emplace_back(argv[index]);
  return args;
}

inline bool isHelpFlag(const std::string &arg)
{
  return arg == "-h" || arg == "--help";
}

inline std::string toLower(std::string value)
{
  for (char &c : value)
  {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return value;
}

inline std::string requireOptionValue(const std::vector<std::string> &args, size_t &index, const char *option)
{
  if (index + 1u >= args.size())
    throw std::runtime_error(std::string("Missing value for ") + option);
  return args[++index];
}

template <typename T> inline T parseNumber(const std::string &value, const char *option)
{
  static_assert(std::is_arithmetic<T>::value, "parseNumber only supports arithmetic types");

  std::istringstream stream(value);
  T parsed{};
  stream >> parsed;
  stream >> std::ws;
  if (!stream || !stream.eof())
    throw std::runtime_error(std::string("Invalid value for ") + option + ": " + value);
  return parsed;
}

inline bool parseBool(const std::string &value, const char *option)
{
  const std::string normalized = toLower(value);
  if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on")
    return true;
  if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off")
    return false;
  throw std::runtime_error(std::string("Invalid value for ") + option + ": " + value + " (expected true/false)");
}

inline void ensureParentDirectory(const std::filesystem::path &path)
{
  const std::filesystem::path parent = path.parent_path();
  if (parent.empty())
    return;

  std::error_code error;
  std::filesystem::create_directories(parent, error);
  if (error)
    throw std::runtime_error("Failed to create directory " + parent.string() + ": " + error.message());
}

inline void requireFileExists(const std::filesystem::path &path, const char *option)
{
  if (!std::filesystem::exists(path))
    throw std::runtime_error(std::string(option) + " does not exist: " + path.string());
}

inline uint8_t parseUint8(const std::string &value, const char *option)
{
  const uint32_t parsed = parseNumber<uint32_t>(value, option);
  if (parsed > 255u)
    throw std::runtime_error(std::string("Value for ") + option + " must be in [0, 255]");
  return static_cast<uint8_t>(parsed);
}

} // namespace cli::common
