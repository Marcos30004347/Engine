#pragma once

#include <cstdint>
#include <string>

#include "virtualgeometry/Material.hpp"
#include "virtualgeometry/VirtualTextureSystem.hpp"

namespace virtualgeometry
{

class VirtualMaterialFile
{
public:
  static constexpr uint32_t MATERIAL_MAGIC = 0x564D4154u; // VMAT
  static constexpr uint32_t MATERIAL_VERSION = 2u;

  static bool create(const std::string &path, const VirtualTextureSystem::MaterialCreateInfo &info);
  static bool load(const std::string &path, Material &material);
  static bool load(const std::string &path, VirtualTextureSystem::PreparedMaterial &material);
  static bool loadDefinition(const std::string &path, Material::Definition &material);
  static bool loadDefinition(const std::string &path, VirtualTextureSystem::MaterialDefinition &material);
  static bool save(const std::string &path, const Material &material);
  static bool save(const std::string &path, const VirtualTextureSystem::PreparedMaterial &material);
  static bool save(const std::string &path, const Material::Definition &material);
  static bool save(const std::string &path, const VirtualTextureSystem::MaterialDefinition &material);
  static bool saveFromCreateInfo(const std::string &path, const VirtualTextureSystem::MaterialCreateInfo &info);
  static bool updateTextureFile(
      const std::string &path,
      uint32_t textureSlot,
      const std::string &imagePath,
      bool flipVertically = false);
  static bool updateTexture(
      VirtualTextureSystem::MaterialDefinition &material,
      uint32_t textureSlot,
      const std::string &imageFilePath);
  static bool updateTextureSamplingFile(
      const std::string &path,
      uint32_t textureSlot,
      const VirtualTextureSystem::TextureSampling &sampling);
  static bool updateTextureSampling(
      VirtualTextureSystem::MaterialDefinition &material,
      uint32_t textureSlot,
      const VirtualTextureSystem::TextureSampling &sampling);
};

} // namespace virtualgeometry
