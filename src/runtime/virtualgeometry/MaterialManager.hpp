#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "virtualgeometry/Material.hpp"

namespace virtualgeometry
{

class VirtualTextureSystem;

class MaterialManager
{
public:
  struct Record
  {
    Material material;
    std::string filePath;
    uint32_t referenceCount = 0u;
  };

  explicit MaterialManager(VirtualTextureSystem *virtualTextureSystem = nullptr);

  void attachVirtualTextureSystem(VirtualTextureSystem &virtualTextureSystem);

  bool registerMaterial(uint32_t materialIndex, const Material &material, const std::string &filePath = std::string(), uint32_t referenceIncrement = 1u);
  bool loadMaterialFile(uint32_t materialIndex, const std::string &materialPath, uint32_t referenceIncrement = 1u);
  bool ensureMaterial(uint32_t materialIndex, uint32_t referenceIncrement = 0u);
  bool unloadMaterial(uint32_t materialIndex, uint32_t referenceDecrement = 1u);
  void syncTextureIds();

  bool hasMaterial(uint32_t materialIndex) const;
  const Material *getMaterial(uint32_t materialIndex) const;
  Material *getMaterial(uint32_t materialIndex);
  const Record *getRecord(uint32_t materialIndex) const;

  std::vector<uint32_t> getRegisteredMaterialIds() const;
  uint64_t getRevision() const;

private:
  static VirtualTextureSystem::PreparedMaterial buildPreparedMaterial(const Material &material, const std::string &materialFilePath);

  VirtualTextureSystem *virtualTextureSystem_ = nullptr;
  std::unordered_map<uint32_t, Record> materials_;
  uint64_t revision_ = 0u;
};

} // namespace virtualgeometry
