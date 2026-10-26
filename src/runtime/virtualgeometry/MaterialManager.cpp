#include "virtualgeometry/MaterialManager.hpp"

#include <algorithm>
#include <filesystem>

#include "virtualgeometry/VirtualImageFile.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"
#include "virtualgeometry/VirtualTextureSystem.hpp"

namespace virtualgeometry
{

namespace fs = std::filesystem;

MaterialManager::MaterialManager(VirtualTextureSystem *virtualTextureSystem) : virtualTextureSystem_(virtualTextureSystem)
{
}

void MaterialManager::attachVirtualTextureSystem(VirtualTextureSystem &virtualTextureSystem)
{
  virtualTextureSystem_ = &virtualTextureSystem;
}

bool MaterialManager::registerMaterial(uint32_t materialIndex, const Material &material, const std::string &filePath, uint32_t referenceIncrement)
{
  Record record;
  record.material = material;
  record.filePath = filePath;
  record.referenceCount = referenceIncrement;

  if (virtualTextureSystem_ != nullptr)
  {
    if (!virtualTextureSystem_->registerPreparedMaterial(materialIndex, buildPreparedMaterial(material, filePath)))
      return false;
  }

  materials_[materialIndex] = std::move(record);
  ++revision_;
  return true;
}

bool MaterialManager::loadMaterialFile(uint32_t materialIndex, const std::string &materialPath, uint32_t referenceIncrement)
{
  Material material;
  if (!VirtualMaterialFile::load(materialPath, material))
    return false;
  return registerMaterial(materialIndex, material, materialPath, referenceIncrement);
}

bool MaterialManager::ensureMaterial(uint32_t materialIndex, uint32_t referenceIncrement)
{
  auto it = materials_.find(materialIndex);
  if (it != materials_.end())
  {
    it->second.referenceCount += referenceIncrement;
    return true;
  }

  Material material;
  return registerMaterial(materialIndex, material, std::string(), referenceIncrement);
}

bool MaterialManager::unloadMaterial(uint32_t materialIndex, uint32_t referenceDecrement)
{
  auto it = materials_.find(materialIndex);
  if (it == materials_.end())
    return false;

  if (referenceDecrement < it->second.referenceCount)
  {
    it->second.referenceCount -= referenceDecrement;
    return true;
  }

  if (virtualTextureSystem_ != nullptr)
    virtualTextureSystem_->unregisterMaterial(materialIndex);

  materials_.erase(it);
  ++revision_;
  return true;
}

void MaterialManager::syncTextureIds()
{
  if (virtualTextureSystem_ == nullptr || materials_.empty())
    return;

  const auto &materialEntries = virtualTextureSystem_->getMaterialEntries();
  for (auto &[materialIndex, record] : materials_)
  {
    for (uint32_t textureIndex = 0u; textureIndex < record.material.getTextureCount(); ++textureIndex)
    {
      record.material.setTextureId(textureIndex, Material::InvalidTextureId);
    }

    if (materialIndex >= materialEntries.size())
      continue;

    const auto &materialEntry = materialEntries[materialIndex];
    for (uint32_t textureIndex = 0u; textureIndex < record.material.getTextureCount(); ++textureIndex)
    {
      record.material.setTextureId(textureIndex, materialEntry.textureIndices[textureIndex]);
    }
  }
}

bool MaterialManager::hasMaterial(uint32_t materialIndex) const
{
  return materials_.find(materialIndex) != materials_.end();
}

const Material *MaterialManager::getMaterial(uint32_t materialIndex) const
{
  auto it = materials_.find(materialIndex);
  if (it == materials_.end())
    return nullptr;
  return &it->second.material;
}

Material *MaterialManager::getMaterial(uint32_t materialIndex)
{
  auto it = materials_.find(materialIndex);
  if (it == materials_.end())
    return nullptr;
  return &it->second.material;
}

const MaterialManager::Record *MaterialManager::getRecord(uint32_t materialIndex) const
{
  auto it = materials_.find(materialIndex);
  if (it == materials_.end())
    return nullptr;
  return &it->second;
}

std::vector<uint32_t> MaterialManager::getRegisteredMaterialIds() const
{
  std::vector<uint32_t> ids;
  ids.reserve(materials_.size());
  for (const auto &entry : materials_)
    ids.push_back(entry.first);
  std::sort(ids.begin(), ids.end());
  return ids;
}

uint64_t MaterialManager::getRevision() const
{
  return revision_;
}

VirtualTextureSystem::PreparedMaterial MaterialManager::buildPreparedMaterial(const Material &material, const std::string &materialFilePath)
{
  VirtualTextureSystem::PreparedMaterial preparedMaterial{};
  const fs::path basePath = materialFilePath.empty() ? fs::path() : fs::path(materialFilePath).lexically_normal().parent_path();

  for (uint32_t textureIndex = 0u; textureIndex < material.getTextureCount() && textureIndex < MAX_TEXTURES_PER_MATERIAL; ++textureIndex)
  {
    const auto &texture = material.getTexture(textureIndex);
    if (texture.imageFilePath.empty())
      continue;

    fs::path imagePath = fs::path(texture.imageFilePath);
    if (imagePath.is_relative() && !basePath.empty())
      imagePath = basePath / imagePath;
    imagePath = imagePath.lexically_normal();

    VirtualTextureSystem::PreparedTexture preparedTexture;
    preparedTexture.imageFilePath = texture.imageFilePath;
    preparedTexture.sampling = texture.sampling;
    if (!VirtualImageFile::load(imagePath.string(), preparedTexture.image))
      continue;

    preparedTexture.sourcePath = preparedTexture.image.sourcePath;
    preparedTexture.mips = preparedTexture.image.mips;
    preparedMaterial.textures[textureIndex] = std::move(preparedTexture);
    preparedMaterial.textureCount = std::max(preparedMaterial.textureCount, textureIndex + 1u);
  }

  return preparedMaterial;
}

} // namespace virtualgeometry
