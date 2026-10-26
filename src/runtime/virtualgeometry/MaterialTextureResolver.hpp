#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "virtualgeometry/Material.hpp"
#include "virtualgeometry/VirtualTextureSystem.hpp"

namespace virtualgeometry
{

inline bool materialTextureContainsInsensitive(const std::string &text, const char *pattern)
{
  if (pattern == nullptr || *pattern == '\0')
    return false;

  std::string lowerText;
  lowerText.resize(text.size());
  std::transform(text.begin(), text.end(), lowerText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::string lowerPattern(pattern);
  std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lowerText.find(lowerPattern) != std::string::npos;
}

inline bool materialTextureMatchesAnyPattern(const Material::TextureSlot &texture, const std::initializer_list<const char *> &patterns)
{
  for (const char *pattern : patterns)
  {
    if (materialTextureContainsInsensitive(texture.name, pattern) || materialTextureContainsInsensitive(texture.imageFilePath, pattern))
      return true;
  }
  return false;
}

inline uint32_t findNamedMaterialTextureSlot(const Material &material, const std::initializer_list<const char *> &patterns)
{
  for (uint32_t textureIndex = 0u; textureIndex < material.getTextureCount(); ++textureIndex)
  {
    const auto &texture = material.getTexture(textureIndex);
    if (materialTextureMatchesAnyPattern(texture, patterns))
      return textureIndex;
  }
  return Material::InvalidTextureId;
}

inline uint32_t resolveMaterialTextureId(const Material *material, const VirtualTextureSystem::MaterialEntryGPU &materialEntry, bool wantNormal)
{
  uint32_t slot = Material::InvalidTextureId;
  if (material != nullptr)
  {
    slot = wantNormal
               ? findNamedMaterialTextureSlot(*material, {"normal", "normalmap", "normal_map", "bump"})
               : findNamedMaterialTextureSlot(*material, {"basecolor", "base_color", "albedo", "diffuse", "color"});
  }

  if (slot == Material::InvalidTextureId)
  {
    if (!wantNormal && materialEntry.textureCount > 0u)
    {
      const bool firstTextureLooksLikeNormal =
          material != nullptr && material->getTextureCount() > 0u &&
          materialTextureMatchesAnyPattern(material->getTexture(0u), {"normal", "normalmap", "normal_map", "bump"});
      if (!firstTextureLooksLikeNormal)
        slot = 0u;
    }
    else if (wantNormal && materialEntry.textureCount > 1u)
    {
      const bool secondTextureLooksLikeNormal =
          material == nullptr ||
          (material->getTextureCount() > 1u && materialTextureMatchesAnyPattern(material->getTexture(1u), {"normal", "normalmap", "normal_map", "bump"}));
      if (secondTextureLooksLikeNormal)
        slot = 1u;
    }
  }

  if (slot == Material::InvalidTextureId || slot >= MAX_TEXTURES_PER_MATERIAL)
    return Material::InvalidTextureId;
  return materialEntry.textureIndices[slot];
}

inline uint32_t resolveBaseColorMaterialTextureId(const Material *material, const VirtualTextureSystem::MaterialEntryGPU &materialEntry)
{
  return resolveMaterialTextureId(material, materialEntry, false);
}

inline uint32_t resolveNormalMaterialTextureId(const Material *material, const VirtualTextureSystem::MaterialEntryGPU &materialEntry)
{
  return resolveMaterialTextureId(material, materialEntry, true);
}

} // namespace virtualgeometry
