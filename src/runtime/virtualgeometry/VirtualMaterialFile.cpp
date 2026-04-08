#include "virtualgeometry/VirtualMaterialFile.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace virtualgeometry
{

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

} // namespace

bool VirtualMaterialFile::create(const std::string &path, const VirtualTextureSystem::MaterialCreateInfo &info)
{
  return saveFromCreateInfo(path, info);
}

bool VirtualMaterialFile::load(const std::string &path, VirtualTextureSystem::PreparedMaterial &material)
{
  material = {};

  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  uint32_t magic = 0u;
  uint32_t version = 0u;
  uint32_t textureCount = 0u;
  const bool headerOk = readU32(file, magic) && readU32(file, version) && readU32(file, textureCount);
  if (!headerOk || magic != MATERIAL_MAGIC || (version != 1u && version != MATERIAL_VERSION) || textureCount > MAX_TEXTURES_PER_MATERIAL)
  {
    std::fclose(file);
    return false;
  }

  material.textureCount = textureCount;
  for (uint32_t textureIndex = 0u; textureIndex < textureCount; ++textureIndex)
  {
    auto &texture = material.textures[textureIndex];
    if (!readString(file, texture.sourcePath))
    {
      std::fclose(file);
      return false;
    }

    texture.sampling = {};
    if (version >= 2u)
    {
      uint32_t addressModeU = 0u;
      uint32_t addressModeV = 0u;
      uint32_t filterMode = 0u;
      if (!readU32(file, addressModeU) ||
          !readU32(file, addressModeV) ||
          !readU32(file, filterMode) ||
          !readF32(file, texture.sampling.mipBias) ||
          !readU32(file, texture.sampling.minMip) ||
          !readU32(file, texture.sampling.maxMip))
      {
        std::fclose(file);
        return false;
      }
      texture.sampling.addressModeU = static_cast<VirtualTextureSystem::TextureAddressMode>(addressModeU);
      texture.sampling.addressModeV = static_cast<VirtualTextureSystem::TextureAddressMode>(addressModeV);
      texture.sampling.filterMode = static_cast<VirtualTextureSystem::TextureFilterMode>(filterMode);
    }

    uint32_t mipCount = 0u;
    if (!readU32(file, mipCount))
    {
      std::fclose(file);
      return false;
    }

    texture.mips.resize(mipCount);
    for (uint32_t mipIndex = 0u; mipIndex < mipCount; ++mipIndex)
    {
      auto &mip = texture.mips[mipIndex];
      uint32_t texelCount = 0u;
      if (!readU32(file, mip.width) || !readU32(file, mip.height) || !readU32(file, texelCount))
      {
        std::fclose(file);
        return false;
      }

      mip.texels.resize(texelCount);
      if (texelCount != 0u && std::fread(mip.texels.data(), sizeof(uint32_t), texelCount, file) != texelCount)
      {
        std::fclose(file);
        return false;
      }
    }
  }

  std::fclose(file);
  return true;
}

bool VirtualMaterialFile::save(const std::string &path, const VirtualTextureSystem::PreparedMaterial &material)
{
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    return false;

  writeU32(file, MATERIAL_MAGIC);
  writeU32(file, MATERIAL_VERSION);
  writeU32(file, material.textureCount);

  for (uint32_t textureIndex = 0u; textureIndex < material.textureCount; ++textureIndex)
  {
    const auto &texture = material.textures[textureIndex];
    writeString(file, texture.sourcePath);
    writeU32(file, static_cast<uint32_t>(texture.sampling.addressModeU));
    writeU32(file, static_cast<uint32_t>(texture.sampling.addressModeV));
    writeU32(file, static_cast<uint32_t>(texture.sampling.filterMode));
    writeF32(file, texture.sampling.mipBias);
    writeU32(file, texture.sampling.minMip);
    writeU32(file, texture.sampling.maxMip);
    writeU32(file, static_cast<uint32_t>(texture.mips.size()));
    for (const auto &mip : texture.mips)
    {
      writeU32(file, mip.width);
      writeU32(file, mip.height);
      writeU32(file, static_cast<uint32_t>(mip.texels.size()));
      if (!mip.texels.empty())
        std::fwrite(mip.texels.data(), sizeof(uint32_t), mip.texels.size(), file);
    }
  }

  std::fclose(file);
  return true;
}

bool VirtualMaterialFile::saveFromCreateInfo(const std::string &path, const VirtualTextureSystem::MaterialCreateInfo &info)
{
  return save(path, VirtualTextureSystem::buildPreparedMaterial(info));
}

bool VirtualMaterialFile::updateTextureFile(
    const std::string &path,
    uint32_t textureSlot,
    const std::string &imagePath,
    bool flipVertically)
{
  VirtualTextureSystem::PreparedMaterial material;
  if (!load(path, material))
    return false;
  if (!updateTexture(material, textureSlot, imagePath, flipVertically))
    return false;
  return save(path, material);
}

bool VirtualMaterialFile::updateTexture(
    VirtualTextureSystem::PreparedMaterial &material,
    uint32_t textureSlot,
    const std::string &imagePath,
    bool flipVertically)
{
  if (textureSlot >= MAX_TEXTURES_PER_MATERIAL)
    return false;

  VirtualTextureSystem::MaterialCreateInfo info{};
  info.textureCount = 1u;
  info.textures[0].source.path = imagePath;
  info.textures[0].source.flipVertically = flipVertically;
  if (textureSlot < material.textureCount)
    info.textures[0].sampling = material.textures[textureSlot].sampling;
  const VirtualTextureSystem::PreparedMaterial prepared = VirtualTextureSystem::buildPreparedMaterial(info);
  if (prepared.textureCount == 0u)
    return false;

  material.textures[textureSlot] = prepared.textures[0];
  material.textureCount = std::max(material.textureCount, textureSlot + 1u);
  return true;
}

bool VirtualMaterialFile::updateTextureSamplingFile(
    const std::string &path,
    uint32_t textureSlot,
    const VirtualTextureSystem::TextureSampling &sampling)
{
  VirtualTextureSystem::PreparedMaterial material;
  if (!load(path, material))
    return false;
  if (!updateTextureSampling(material, textureSlot, sampling))
    return false;
  return save(path, material);
}

bool VirtualMaterialFile::updateTextureSampling(
    VirtualTextureSystem::PreparedMaterial &material,
    uint32_t textureSlot,
    const VirtualTextureSystem::TextureSampling &sampling)
{
  if (textureSlot >= material.textureCount)
    return false;
  material.textures[textureSlot].sampling = sampling;
  return true;
}

} // namespace virtualgeometry
