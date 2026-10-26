#include "virtualgeometry/Material.hpp"

#include <algorithm>
#include <stdexcept>

namespace virtualgeometry
{

namespace
{

static const std::string kDefaultMaterialShaderPath = "assets/shaders/spirv/material-default";

} // namespace

Material::Material(const Definition &definition) : shaderPath_(definition.shaderPath.empty() ? defaultShaderPath() : definition.shaderPath), textures_(definition.textures)
{
  setTextureCount(definition.textureCount);
  setBaseColorFactor(
      definition.baseColorFactor[0],
      definition.baseColorFactor[1],
      definition.baseColorFactor[2],
      definition.baseColorFactor[3]);
  setRoughnessFactor(definition.roughnessFactor);
  setMetallicFactor(definition.metallicFactor);
  setEmissiveFactor(definition.emissiveFactor[0], definition.emissiveFactor[1], definition.emissiveFactor[2]);
  setEmissiveStrength(definition.emissiveStrength);
  setOpacity(definition.opacity);
}

const std::string &Material::defaultShaderPath()
{
  return kDefaultMaterialShaderPath;
}

const std::string &Material::getShaderPath() const
{
  return shaderPath_;
}

void Material::setShaderPath(const std::string &shaderPath)
{
  shaderPath_ = shaderPath.empty() ? defaultShaderPath() : shaderPath;
}

uint32_t Material::getTextureCount() const
{
  return textureCount_;
}

void Material::setTextureCount(uint32_t textureCount)
{
  textureCount_ = std::min(textureCount, MAX_TEXTURES_PER_MATERIAL);
}

const Material::TextureSlot &Material::getTexture(uint32_t textureIndex) const
{
  if (textureIndex >= MAX_TEXTURES_PER_MATERIAL)
    throw std::out_of_range("Material texture index out of range");
  return textures_[textureIndex];
}

Material::TextureSlot &Material::getTexture(uint32_t textureIndex)
{
  if (textureIndex >= MAX_TEXTURES_PER_MATERIAL)
    throw std::out_of_range("Material texture index out of range");
  return textures_[textureIndex];
}

const std::array<Material::TextureSlot, MAX_TEXTURES_PER_MATERIAL> &Material::getTextures() const
{
  return textures_;
}

const float *Material::getBaseColorFactor() const
{
  return baseColorFactor_;
}

void Material::setBaseColorFactor(float r, float g, float b, float a)
{
  baseColorFactor_[0] = r;
  baseColorFactor_[1] = g;
  baseColorFactor_[2] = b;
  baseColorFactor_[3] = a;
}

float Material::getRoughnessFactor() const
{
  return roughnessFactor_;
}

void Material::setRoughnessFactor(float roughness)
{
  roughnessFactor_ = std::clamp(roughness, 0.0f, 1.0f);
}

float Material::getMetallicFactor() const
{
  return metallicFactor_;
}

void Material::setMetallicFactor(float metallic)
{
  metallicFactor_ = std::clamp(metallic, 0.0f, 1.0f);
}

const float *Material::getEmissiveFactor() const
{
  return emissiveFactor_;
}

void Material::setEmissiveFactor(float r, float g, float b)
{
  emissiveFactor_[0] = std::max(0.0f, r);
  emissiveFactor_[1] = std::max(0.0f, g);
  emissiveFactor_[2] = std::max(0.0f, b);
}

float Material::getEmissiveStrength() const
{
  return emissiveStrength_;
}

void Material::setEmissiveStrength(float strength)
{
  emissiveStrength_ = std::max(0.0f, strength);
}

float Material::getOpacity() const
{
  return opacity_;
}

void Material::setOpacity(float opacity)
{
  opacity_ = std::clamp(opacity, 0.0f, 1.0f);
}

uint32_t Material::findTextureSlotByName(const std::string &name) const
{
  for (uint32_t textureIndex = 0u; textureIndex < textureCount_; ++textureIndex)
  {
    if (textures_[textureIndex].name == name)
      return textureIndex;
  }
  return InvalidTextureId;
}

uint32_t Material::findTextureIdByName(const std::string &name) const
{
  const uint32_t textureSlot = findTextureSlotByName(name);
  if (textureSlot == InvalidTextureId)
    return InvalidTextureId;
  return textures_[textureSlot].textureId;
}

void Material::setTextureId(uint32_t textureIndex, uint32_t textureId)
{
  if (textureIndex >= MAX_TEXTURES_PER_MATERIAL)
    return;
  textures_[textureIndex].textureId = textureId;
}

Material::Definition Material::toDefinition() const
{
  Material::Definition definition;
  definition.shaderPath = shaderPath_;
  definition.textureCount = textureCount_;
  definition.textures = textures_;
  std::copy(std::begin(baseColorFactor_), std::end(baseColorFactor_), std::begin(definition.baseColorFactor));
  definition.roughnessFactor = roughnessFactor_;
  definition.metallicFactor = metallicFactor_;
  std::copy(std::begin(emissiveFactor_), std::end(emissiveFactor_), std::begin(definition.emissiveFactor));
  definition.emissiveStrength = emissiveStrength_;
  definition.opacity = opacity_;
  return definition;
}

} // namespace virtualgeometry
