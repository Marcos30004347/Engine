#pragma once

#include <cstdint>
#include <string>

#include "virtualgeometry/VirtualTextureSystem.hpp"

namespace virtualgeometry
{

class VirtualImageFile
{
public:
  static constexpr uint32_t IMAGE_MAGIC = 0x56494D47u; // VIMG
  static constexpr uint32_t IMAGE_VERSION = 1u;

  static bool create(const std::string &path, const VirtualTextureSystem::TextureCreateInfo &info);
  static bool load(const std::string &path, VirtualTextureSystem::PreparedImage &image);
  static bool save(const std::string &path, const VirtualTextureSystem::PreparedImage &image);
  static bool saveFromCreateInfo(const std::string &path, const VirtualTextureSystem::TextureCreateInfo &info);
};

} // namespace virtualgeometry
