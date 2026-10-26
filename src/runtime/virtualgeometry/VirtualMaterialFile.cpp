#include "virtualgeometry/VirtualMaterialFile.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "virtualgeometry/VirtualImageFile.hpp"

namespace virtualgeometry
{

namespace fs = std::filesystem;

namespace
{

static void writeU32(std::FILE *file, uint32_t value)
{
  std::fwrite(&value, sizeof(value), 1u, file);
}

static void writeF32(std::FILE *file, float value)
{
  std::fwrite(&value, sizeof(value), 1u, file);
}

static bool readU32(std::FILE *file, uint32_t &value)
{
  return std::fread(&value, sizeof(value), 1u, file) == 1u;
}

static bool readF32(std::FILE *file, float &value)
{
  return std::fread(&value, sizeof(value), 1u, file) == 1u;
}

static void writeString(std::FILE *file, const std::string &value)
{
  writeU32(file, static_cast<uint32_t>(value.size()));
  if (!value.empty())
    std::fwrite(value.data(), 1u, value.size(), file);
}

static bool readString(std::FILE *file, std::string &value)
{
  uint32_t length = 0u;
  if (!readU32(file, length))
    return false;
  value.resize(length);
  return length == 0u || std::fread(value.data(), 1u, length, file) == length;
}

static bool readSampling(std::FILE *file, VirtualTextureSystem::TextureSampling &sampling)
{
  uint32_t addressModeU = 0u;
  uint32_t addressModeV = 0u;
  uint32_t filterMode = 0u;
  if (!readU32(file, addressModeU) ||
      !readU32(file, addressModeV) ||
      !readU32(file, filterMode) ||
      !readF32(file, sampling.mipBias) ||
      !readU32(file, sampling.minMip) ||
      !readU32(file, sampling.maxMip))
  {
    return false;
  }

  sampling.addressModeU = static_cast<VirtualTextureSystem::TextureAddressMode>(addressModeU);
  sampling.addressModeV = static_cast<VirtualTextureSystem::TextureAddressMode>(addressModeV);
  sampling.filterMode = static_cast<VirtualTextureSystem::TextureFilterMode>(filterMode);
  return true;
}

static void writeSampling(std::FILE *file, const VirtualTextureSystem::TextureSampling &sampling)
{
  writeU32(file, static_cast<uint32_t>(sampling.addressModeU));
  writeU32(file, static_cast<uint32_t>(sampling.addressModeV));
  writeU32(file, static_cast<uint32_t>(sampling.filterMode));
  writeF32(file, sampling.mipBias);
  writeU32(file, sampling.minMip);
  writeU32(file, sampling.maxMip);
}

static fs::path makeVirtualImagePath(const fs::path &materialPath, uint32_t textureSlot)
{
  const std::string stem = materialPath.stem().string();
  return materialPath.parent_path() / (stem + "_tex" + std::to_string(textureSlot) + ".vimg");
}

static std::string makeStoredPath(const fs::path &materialPath, const fs::path &assetPath)
{
  const fs::path normalizedMaterialPath = materialPath.lexically_normal();
  const fs::path normalizedAssetPath = assetPath.lexically_normal();
  const fs::path relativeAssetPath = normalizedAssetPath.lexically_relative(normalizedMaterialPath.parent_path());
  if (!relativeAssetPath.empty())
    return relativeAssetPath.generic_string();
  return normalizedAssetPath.string();
}

static VirtualTextureSystem::PreparedImage makePreparedImage(const VirtualTextureSystem::PreparedTexture &texture)
{
  if (!texture.image.mips.empty())
    return texture.image;

  VirtualTextureSystem::PreparedImage image;
  image.sourcePath = texture.sourcePath;
  image.mips = texture.mips;
  return image;
}

static Material::Definition makeMaterialDefinition(const VirtualTextureSystem::MaterialDefinition &definition)
{
  Material::Definition materialDefinition;
  materialDefinition.textureCount = definition.textureCount;
  for (uint32_t textureIndex = 0u; textureIndex < definition.textureCount && textureIndex < MAX_TEXTURES_PER_MATERIAL; ++textureIndex)
  {
    materialDefinition.textures[textureIndex].name = "texture" + std::to_string(textureIndex);
    materialDefinition.textures[textureIndex].imageFilePath = definition.textures[textureIndex].imageFilePath;
    materialDefinition.textures[textureIndex].sampling = definition.textures[textureIndex].sampling;
  }
  return materialDefinition;
}

static VirtualTextureSystem::MaterialDefinition makeVirtualTextureDefinition(const Material::Definition &definition)
{
  VirtualTextureSystem::MaterialDefinition vtDefinition;
  vtDefinition.textureCount = definition.textureCount;
  for (uint32_t textureIndex = 0u; textureIndex < definition.textureCount && textureIndex < MAX_TEXTURES_PER_MATERIAL; ++textureIndex)
  {
    vtDefinition.textures[textureIndex].imageFilePath = definition.textures[textureIndex].imageFilePath;
    vtDefinition.textures[textureIndex].sampling = definition.textures[textureIndex].sampling;
  }
  return vtDefinition;
}

} // namespace

bool VirtualMaterialFile::create(const std::string &path, const VirtualTextureSystem::MaterialCreateInfo &info)
{
  return saveFromCreateInfo(path, info);
}

bool VirtualMaterialFile::load(const std::string &path, Material &material)
{
  Material::Definition definition;
  if (!loadDefinition(path, definition))
    return false;

  material = Material(definition);
  return true;
}

bool VirtualMaterialFile::load(const std::string &path, VirtualTextureSystem::PreparedMaterial &material)
{
  Material::Definition definition;
  if (!loadDefinition(path, definition))
    return false;

  material = {};
  const fs::path materialPath = fs::path(path).lexically_normal();
  material.textureCount = definition.textureCount;
  for (uint32_t textureIndex = 0u; textureIndex < definition.textureCount; ++textureIndex)
  {
    const auto &definitionTexture = definition.textures[textureIndex];
    auto &preparedTexture = material.textures[textureIndex];
    preparedTexture.imageFilePath = definitionTexture.imageFilePath;
    preparedTexture.sampling = definitionTexture.sampling;

    if (definitionTexture.imageFilePath.empty())
      continue;

    fs::path imagePath = definitionTexture.imageFilePath;
    if (imagePath.is_relative())
      imagePath = materialPath.parent_path() / imagePath;
    imagePath = imagePath.lexically_normal();

    if (!VirtualImageFile::load(imagePath.string(), preparedTexture.image))
      return false;
    preparedTexture.sourcePath = preparedTexture.image.sourcePath;
    preparedTexture.mips = preparedTexture.image.mips;
  }

  return true;
}

bool VirtualMaterialFile::loadDefinition(const std::string &path, Material::Definition &material)
{
  material = {};

  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  uint32_t magic = 0u;
  uint32_t version = 0u;
  uint32_t textureCount = 0u;
  const bool headerOk = readU32(file, magic) && readU32(file, version) && readU32(file, textureCount);
  if (!headerOk || magic != MATERIAL_MAGIC || version == 0u || version > MATERIAL_VERSION || textureCount > MAX_TEXTURES_PER_MATERIAL)
  {
    std::fclose(file);
    return false;
  }

  material.textureCount = textureCount;
  if (!readString(file, material.shaderPath))
  {
    std::fclose(file);
    return false;
  }
  if (material.shaderPath.empty())
    material.shaderPath = Material::defaultShaderPath();

  if (version >= 2u)
  {
    if (!readF32(file, material.baseColorFactor[0]) ||
        !readF32(file, material.baseColorFactor[1]) ||
        !readF32(file, material.baseColorFactor[2]) ||
        !readF32(file, material.baseColorFactor[3]) ||
        !readF32(file, material.roughnessFactor) ||
        !readF32(file, material.metallicFactor) ||
        !readF32(file, material.emissiveFactor[0]) ||
        !readF32(file, material.emissiveFactor[1]) ||
        !readF32(file, material.emissiveFactor[2]) ||
        !readF32(file, material.emissiveStrength) ||
        !readF32(file, material.opacity))
    {
      std::fclose(file);
      return false;
    }
  }

  for (uint32_t textureIndex = 0u; textureIndex < textureCount; ++textureIndex)
  {
    auto &texture = material.textures[textureIndex];
    if (!readString(file, texture.name) || !readString(file, texture.imageFilePath) || !readSampling(file, texture.sampling))
    {
      std::fclose(file);
      return false;
    }
    if (texture.name.empty())
      texture.name = "texture" + std::to_string(textureIndex);
  }

  std::fclose(file);
  return true;
}

bool VirtualMaterialFile::loadDefinition(const std::string &path, VirtualTextureSystem::MaterialDefinition &material)
{
  Material::Definition definition;
  if (!loadDefinition(path, definition))
    return false;
  material = makeVirtualTextureDefinition(definition);
  return true;
}

bool VirtualMaterialFile::save(const std::string &path, const Material &material)
{
  return save(path, material.toDefinition());
}

bool VirtualMaterialFile::save(const std::string &path, const Material::Definition &material)
{
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    return false;

  writeU32(file, MATERIAL_MAGIC);
  writeU32(file, MATERIAL_VERSION);
  writeU32(file, material.textureCount);
  writeString(file, material.shaderPath.empty() ? Material::defaultShaderPath() : material.shaderPath);
  writeF32(file, material.baseColorFactor[0]);
  writeF32(file, material.baseColorFactor[1]);
  writeF32(file, material.baseColorFactor[2]);
  writeF32(file, material.baseColorFactor[3]);
  writeF32(file, material.roughnessFactor);
  writeF32(file, material.metallicFactor);
  writeF32(file, material.emissiveFactor[0]);
  writeF32(file, material.emissiveFactor[1]);
  writeF32(file, material.emissiveFactor[2]);
  writeF32(file, material.emissiveStrength);
  writeF32(file, material.opacity);

  for (uint32_t textureIndex = 0u; textureIndex < material.textureCount; ++textureIndex)
  {
    const auto &texture = material.textures[textureIndex];
    writeString(file, texture.name.empty() ? ("texture" + std::to_string(textureIndex)) : texture.name);
    writeString(file, texture.imageFilePath);
    writeSampling(file, texture.sampling);
  }

  std::fclose(file);
  return true;
}

bool VirtualMaterialFile::save(const std::string &path, const VirtualTextureSystem::MaterialDefinition &material)
{
  return save(path, makeMaterialDefinition(material));
}

bool VirtualMaterialFile::save(const std::string &path, const VirtualTextureSystem::PreparedMaterial &material)
{
  const fs::path materialPath = fs::path(path).lexically_normal();
  Material::Definition materialDefinition{};

  for (uint32_t textureIndex = 0u; textureIndex < material.textureCount && textureIndex < MAX_TEXTURES_PER_MATERIAL; ++textureIndex)
  {
    const auto &preparedTexture = material.textures[textureIndex];
    const VirtualTextureSystem::PreparedImage image = makePreparedImage(preparedTexture);
    if (image.mips.empty())
      continue;

    const fs::path imagePath = makeVirtualImagePath(materialPath, textureIndex);
    if (!VirtualImageFile::save(imagePath.string(), image))
      return false;

    auto &materialTexture = materialDefinition.textures[textureIndex];
    materialDefinition.textureCount = std::max(materialDefinition.textureCount, textureIndex + 1u);
    materialTexture.name = "texture" + std::to_string(textureIndex);
    materialTexture.imageFilePath = makeStoredPath(materialPath, imagePath);
    materialTexture.sampling = preparedTexture.sampling;
  }

  return save(path, materialDefinition);
}

bool VirtualMaterialFile::saveFromCreateInfo(const std::string &path, const VirtualTextureSystem::MaterialCreateInfo &info)
{
  const fs::path materialPath = fs::path(path).lexically_normal();
  Material::Definition materialDefinition{};
  materialDefinition.roughnessFactor = 0.5f;
  materialDefinition.metallicFactor = 0.0f;

  for (uint32_t textureIndex = 0u; textureIndex < info.textureCount && textureIndex < MAX_TEXTURES_PER_MATERIAL; ++textureIndex)
  {
    const auto &createInfo = info.textures[textureIndex];
    if (createInfo.source.path.empty())
      continue;

    const fs::path imagePath = makeVirtualImagePath(materialPath, textureIndex);
    if (!VirtualImageFile::saveFromCreateInfo(imagePath.string(), createInfo))
      return false;

    auto &materialTexture = materialDefinition.textures[textureIndex];
    materialDefinition.textureCount = std::max(materialDefinition.textureCount, textureIndex + 1u);
    materialTexture.name = createInfo.name.empty() ? ("texture" + std::to_string(textureIndex)) : createInfo.name;
    materialTexture.imageFilePath = makeStoredPath(materialPath, imagePath);
    materialTexture.sampling = createInfo.sampling;
  }

  return save(path, materialDefinition);
}

bool VirtualMaterialFile::updateTextureFile(
    const std::string &path,
    uint32_t textureSlot,
    const std::string &imagePath,
    bool flipVertically)
{
  if (textureSlot >= MAX_TEXTURES_PER_MATERIAL)
    return false;

  VirtualTextureSystem::MaterialDefinition material;
  if (!loadDefinition(path, material))
    return false;

  VirtualTextureSystem::TextureCreateInfo createInfo{};
  createInfo.source.path = imagePath;
  createInfo.source.flipVertically = flipVertically;
  if (textureSlot < material.textureCount)
    createInfo.sampling = material.textures[textureSlot].sampling;

  const fs::path materialPath = fs::path(path).lexically_normal();
  fs::path virtualImagePath = textureSlot < material.textureCount && !material.textures[textureSlot].imageFilePath.empty()
                                  ? fs::path(material.textures[textureSlot].imageFilePath)
                                  : makeVirtualImagePath(materialPath, textureSlot);
  if (virtualImagePath.is_relative())
    virtualImagePath = materialPath.parent_path() / virtualImagePath;
  virtualImagePath = virtualImagePath.lexically_normal();

  if (!VirtualImageFile::saveFromCreateInfo(virtualImagePath.string(), createInfo))
    return false;
  if (!updateTexture(material, textureSlot, makeStoredPath(materialPath, virtualImagePath)))
    return false;
  return save(path, material);
}

bool VirtualMaterialFile::updateTexture(
    VirtualTextureSystem::MaterialDefinition &material,
    uint32_t textureSlot,
    const std::string &imageFilePath)
{
  if (textureSlot >= MAX_TEXTURES_PER_MATERIAL)
    return false;

  material.textures[textureSlot].imageFilePath = imageFilePath;
  material.textureCount = std::max(material.textureCount, textureSlot + 1u);
  return true;
}

bool VirtualMaterialFile::updateTextureSamplingFile(
    const std::string &path,
    uint32_t textureSlot,
    const VirtualTextureSystem::TextureSampling &sampling)
{
  VirtualTextureSystem::MaterialDefinition material;
  if (!loadDefinition(path, material))
    return false;
  if (!updateTextureSampling(material, textureSlot, sampling))
    return false;
  return save(path, material);
}

bool VirtualMaterialFile::updateTextureSampling(
    VirtualTextureSystem::MaterialDefinition &material,
    uint32_t textureSlot,
    const VirtualTextureSystem::TextureSampling &sampling)
{
  if (textureSlot >= material.textureCount)
    return false;
  material.textures[textureSlot].sampling = sampling;
  return true;
}

} // namespace virtualgeometry
