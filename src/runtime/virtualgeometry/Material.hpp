#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "virtualgeometry/VirtualGeometryData.hpp"
#include "virtualgeometry/VirtualTextureSystem.hpp"

namespace virtualgeometry
{

class Material
{
public:
  static constexpr uint32_t InvalidTextureId = UINT32_MAX;

  struct TextureSlot
  {
    std::string name;
    std::string imageFilePath;
    VirtualTextureSystem::TextureSampling sampling{};
    uint32_t textureId = InvalidTextureId;
  };

  struct Definition
  {
    std::string shaderPath = defaultShaderPath();
    std::array<TextureSlot, MAX_TEXTURES_PER_MATERIAL> textures;
    uint32_t textureCount = 0u;
    float baseColorFactor[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float roughnessFactor = 1.0f;
    float metallicFactor = 0.0f;
    float emissiveFactor[3]{0.0f, 0.0f, 0.0f};
    float emissiveStrength = 0.0f;
    float opacity = 1.0f;
  };

  Material() = default;
  explicit Material(const Definition &definition);

  static const std::string &defaultShaderPath();

  const std::string &getShaderPath() const;
  void setShaderPath(const std::string &shaderPath);

  uint32_t getTextureCount() const;
  void setTextureCount(uint32_t textureCount);

  const TextureSlot &getTexture(uint32_t textureIndex) const;
  TextureSlot &getTexture(uint32_t textureIndex);

  const std::array<TextureSlot, MAX_TEXTURES_PER_MATERIAL> &getTextures() const;

  const float *getBaseColorFactor() const;
  void setBaseColorFactor(float r, float g, float b, float a = 1.0f);
  float getRoughnessFactor() const;
  void setRoughnessFactor(float roughness);
  float getMetallicFactor() const;
  void setMetallicFactor(float metallic);
  const float *getEmissiveFactor() const;
  void setEmissiveFactor(float r, float g, float b);
  float getEmissiveStrength() const;
  void setEmissiveStrength(float strength);
  float getOpacity() const;
  void setOpacity(float opacity);

  uint32_t findTextureSlotByName(const std::string &name) const;
  uint32_t findTextureIdByName(const std::string &name) const;
  void setTextureId(uint32_t textureIndex, uint32_t textureId);

  Definition toDefinition() const;

private:
  std::string shaderPath_ = defaultShaderPath();
  std::array<TextureSlot, MAX_TEXTURES_PER_MATERIAL> textures_{};
  uint32_t textureCount_ = 0u;
  float baseColorFactor_[4]{1.0f, 1.0f, 1.0f, 1.0f};
  float roughnessFactor_ = 1.0f;
  float metallicFactor_ = 0.0f;
  float emissiveFactor_[3]{0.0f, 0.0f, 0.0f};
  float emissiveStrength_ = 0.0f;
  float opacity_ = 1.0f;
};

} // namespace virtualgeometry
