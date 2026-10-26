#include "virtualgeometry/VirtualImageFile.hpp"

#include <cstdio>

namespace virtualgeometry
{

namespace
{

static void writeU32(std::FILE *file, uint32_t value)
{
  std::fwrite(&value, sizeof(value), 1u, file);
}

static bool readU32(std::FILE *file, uint32_t &value)
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

bool VirtualImageFile::create(const std::string &path, const VirtualTextureSystem::TextureCreateInfo &info)
{
  return saveFromCreateInfo(path, info);
}

bool VirtualImageFile::load(const std::string &path, VirtualTextureSystem::PreparedImage &image)
{
  image = {};

  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  uint32_t magic = 0u;
  uint32_t version = 0u;
  uint32_t mipCount = 0u;
  const bool headerOk = readU32(file, magic) && readU32(file, version) && readString(file, image.sourcePath) && readU32(file, mipCount);
  if (!headerOk || magic != IMAGE_MAGIC || version != IMAGE_VERSION)
  {
    std::fclose(file);
    return false;
  }

  image.mips.resize(mipCount);
  for (uint32_t mipIndex = 0u; mipIndex < mipCount; ++mipIndex)
  {
    auto &mip = image.mips[mipIndex];
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

  std::fclose(file);
  return true;
}

bool VirtualImageFile::save(const std::string &path, const VirtualTextureSystem::PreparedImage &image)
{
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    return false;

  writeU32(file, IMAGE_MAGIC);
  writeU32(file, IMAGE_VERSION);
  writeString(file, image.sourcePath);
  writeU32(file, static_cast<uint32_t>(image.mips.size()));
  for (const auto &mip : image.mips)
  {
    writeU32(file, mip.width);
    writeU32(file, mip.height);
    writeU32(file, static_cast<uint32_t>(mip.texels.size()));
    if (!mip.texels.empty())
      std::fwrite(mip.texels.data(), sizeof(uint32_t), mip.texels.size(), file);
  }

  std::fclose(file);
  return true;
}

bool VirtualImageFile::saveFromCreateInfo(const std::string &path, const VirtualTextureSystem::TextureCreateInfo &info)
{
  return save(path, VirtualTextureSystem::buildPreparedImage(info.source));
}

} // namespace virtualgeometry
