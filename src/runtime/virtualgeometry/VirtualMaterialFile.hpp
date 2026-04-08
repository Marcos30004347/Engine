#pragma once

#include <cstdint>
#include <string>

#include "virtualgeometry/VirtualTextureSystem.hpp"

namespace virtualgeometry
{

class VirtualMaterialFile
{
public:
  static constexpr uint32_t MATERIAL_MAGIC = 0x564D4154u; // VMAT
  static constexpr uint32_t MATERIAL_VERSION = 2u;

  static bool create(const std::string &path, const VirtualTextureSystem::MaterialCreateInfo &info);
  static bool load(const std::string &path, VirtualTextureSystem::PreparedMaterial &material);
  static bool save(const std::string &path, const VirtualTextureSystem::PreparedMaterial &material);
  static bool saveFromCreateInfo(const std::string &path, const VirtualTextureSystem::MaterialCreateInfo &info);
  static bool updateTextureFile(
      const std::string &path,
      uint32_t textureSlot,
      const std::string &imagePath,
      bool flipVertically = false);
  static bool updateTexture(
      VirtualTextureSystem::PreparedMaterial &material,
      uint32_t textureSlot,
      const std::string &imagePath,
      bool flipVertically = false);
  static bool updateTextureSamplingFile(
      const std::string &path,
      uint32_t textureSlot,
      const VirtualTextureSystem::TextureSampling &sampling);
  static bool updateTextureSampling(
      VirtualTextureSystem::PreparedMaterial &material,
      uint32_t textureSlot,
      const VirtualTextureSystem::TextureSampling &sampling);
};

} // namespace virtualgeometry
