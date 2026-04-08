#include <cassert>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "virtualgeometry/VirtualMaterialFile.hpp"
#include "virtualgeometry/VirtualTextureSystem.hpp"

using namespace virtualgeometry;

namespace
{

namespace fs = std::filesystem;

fs::path sourceRoot()
{
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

fs::path sampleTexturePath()
{
  return sourceRoot() / "assets/meshes/gltf/CesiumMan_img0.jpg";
}

bool floatEqual(float a, float b, float epsilon = 1e-6f)
{
  return std::fabs(a - b) <= epsilon;
}

VirtualTextureSystem::MaterialCreateInfo buildCreateInfo()
{
  VirtualTextureSystem::MaterialCreateInfo info{};
  info.textureCount = 1u;
  info.textures[0].source.path = sampleTexturePath().string();
  info.textures[0].sampling.addressModeU = VirtualTextureSystem::TextureAddressMode::ClampToEdge;
  info.textures[0].sampling.addressModeV = VirtualTextureSystem::TextureAddressMode::Repeat;
  info.textures[0].sampling.filterMode = VirtualTextureSystem::TextureFilterMode::Linear;
  info.textures[0].sampling.mipBias = 1.5f;
  info.textures[0].sampling.minMip = 1u;
  info.textures[0].sampling.maxMip = 8u;
  return info;
}

VirtualTextureSystem::PreparedMaterial buildPreparedMaterial()
{
  return VirtualTextureSystem::buildPreparedMaterial(buildCreateInfo());
}

VirtualTextureSystem::PreparedMaterial buildSyntheticPreparedMaterial()
{
  VirtualTextureSystem::PreparedMaterial material{};
  material.textureCount = 1u;

  auto &texture = material.textures[0];
  texture.sourcePath = "synthetic_priority_texture";
  texture.sampling.filterMode = VirtualTextureSystem::TextureFilterMode::Nearest;
  texture.sampling.minMip = 0u;
  texture.sampling.maxMip = 2u;

  const std::array<uint32_t, 3u> mipSizes = {8u, 4u, 2u};
  for (uint32_t mipLevel = 0u; mipLevel < mipSizes.size(); ++mipLevel)
  {
    VirtualTextureSystem::PreparedMipLevel mip{};
    mip.width = mipSizes[mipLevel];
    mip.height = mipSizes[mipLevel];
    mip.texels.resize(static_cast<size_t>(mip.width) * static_cast<size_t>(mip.height), 0xFF000000u | (mipLevel * 0x00010101u));
    texture.mips.push_back(std::move(mip));
  }

  return material;
}

void testPreparedMaterialBuild()
{
  const auto info = buildCreateInfo();
  const auto prepared = buildPreparedMaterial();

  assert(prepared.textureCount == 1u);
  const auto &texture = prepared.textures[0];
  assert(texture.sourcePath == info.textures[0].source.path);
  assert(texture.mips.size() > 1u);
  assert(texture.sampling.addressModeU == info.textures[0].sampling.addressModeU);
  assert(texture.sampling.addressModeV == info.textures[0].sampling.addressModeV);
  assert(texture.sampling.filterMode == info.textures[0].sampling.filterMode);
  assert(floatEqual(texture.sampling.mipBias, info.textures[0].sampling.mipBias));
  assert(texture.sampling.minMip == info.textures[0].sampling.minMip);
  assert(texture.sampling.maxMip == info.textures[0].sampling.maxMip);
}

void testMaterialFileRoundTrip()
{
  const auto prepared = buildPreparedMaterial();
  const fs::path tempPath = fs::temp_directory_path() / "engine_virtual_texture_system_tests.vmat";
  std::error_code error;
  fs::remove(tempPath, error);

  assert(VirtualMaterialFile::save(tempPath.string(), prepared));

  VirtualTextureSystem::PreparedMaterial loaded;
  assert(VirtualMaterialFile::load(tempPath.string(), loaded));
  assert(loaded.textureCount == prepared.textureCount);
  assert(loaded.textures[0].sourcePath == prepared.textures[0].sourcePath);
  assert(loaded.textures[0].mips.size() == prepared.textures[0].mips.size());
  assert(loaded.textures[0].mips[0].width == prepared.textures[0].mips[0].width);
  assert(loaded.textures[0].mips[0].height == prepared.textures[0].mips[0].height);
  assert(loaded.textures[0].sampling.addressModeU == prepared.textures[0].sampling.addressModeU);
  assert(loaded.textures[0].sampling.addressModeV == prepared.textures[0].sampling.addressModeV);
  assert(loaded.textures[0].sampling.filterMode == prepared.textures[0].sampling.filterMode);
  assert(floatEqual(loaded.textures[0].sampling.mipBias, prepared.textures[0].sampling.mipBias));

  VirtualTextureSystem::TextureSampling updatedSampling = loaded.textures[0].sampling;
  updatedSampling.mipBias = -0.5f;
  updatedSampling.minMip = 0u;
  updatedSampling.maxMip = 3u;
  assert(VirtualMaterialFile::updateTextureSamplingFile(tempPath.string(), 0u, updatedSampling));

  VirtualTextureSystem::PreparedMaterial updated;
  assert(VirtualMaterialFile::load(tempPath.string(), updated));
  assert(floatEqual(updated.textures[0].sampling.mipBias, updatedSampling.mipBias));
  assert(updated.textures[0].sampling.minMip == updatedSampling.minMip);
  assert(updated.textures[0].sampling.maxMip == updatedSampling.maxMip);

  fs::remove(tempPath, error);
}

void testVirtualTextureRegistration()
{
  VirtualTextureSystem virtualTextures;
  VirtualTextureSystem::Settings settings{};
  settings.pageSize = 64u;
  settings.physicalPagesPerAxis = 8u;
  settings.maxUploadsPerFrame = 8u;
  virtualTextures.configure(settings);

  const auto prepared = buildPreparedMaterial();
  assert(virtualTextures.registerPreparedMaterial(3u, prepared));
  assert(virtualTextures.hasMaterial(3u));

  const auto &materialEntries = virtualTextures.getMaterialEntries();
  assert(materialEntries.size() > 3u);
  assert(materialEntries[3].textureCount == 1u);
  assert(materialEntries[3].textureIndices[0] == 0u);

  const auto &textureEntries = virtualTextures.getTextureEntries();
  assert(textureEntries.size() == 1u);
  const auto &entry = textureEntries[0];
  assert(entry.mipCount == prepared.textures[0].mips.size());
  assert(entry.addressModeU == static_cast<uint32_t>(VirtualTextureSystem::TextureAddressMode::ClampToEdge));
  assert(entry.addressModeV == static_cast<uint32_t>(VirtualTextureSystem::TextureAddressMode::Repeat));
  assert(entry.filterMode == static_cast<uint32_t>(VirtualTextureSystem::TextureFilterMode::Linear));
  assert(floatEqual(entry.mipBias, prepared.textures[0].sampling.mipBias));

  const auto &pageTableEntries = virtualTextures.getPageTableEntries();
  const auto &fallbackLayout = entry.mips[entry.mipCount - 1u];
  for (uint32_t pageY = 0u; pageY < fallbackLayout.pagesY; ++pageY)
  {
    for (uint32_t pageX = 0u; pageX < fallbackLayout.pagesX; ++pageX)
    {
      const uint32_t pageTableIndex = fallbackLayout.pageTableOffset + pageY * fallbackLayout.pagesX + pageX;
      assert(pageTableEntries[pageTableIndex].physicalPage != VirtualTextureSystem::InvalidPhysicalPage);
      assert((pageTableEntries[pageTableIndex].flags & VirtualTextureSystem::PageTableFlag_Resident) != 0u);
    }
  }

  if (entry.mipCount > 1u)
  {
    VirtualTextureSystem::FeedbackStats stats{};
    const bool changed = virtualTextures.processFeedbackRequests(
        std::vector<VirtualTextureSystem::FeedbackRequestGPU>{{0u, 0u, 0u, 0u}},
        1u,
        &stats);
    assert(changed);
    const auto &updatedPageTableEntries = virtualTextures.getPageTableEntries();
    bool installedRequestedAncestryPage = false;
    for (uint32_t mipLevel = 0u; mipLevel + 1u < entry.mipCount; ++mipLevel)
    {
      const uint32_t pageTableIndex = entry.mips[mipLevel].pageTableOffset;
      if (updatedPageTableEntries[pageTableIndex].physicalPage == VirtualTextureSystem::InvalidPhysicalPage)
        continue;
      if ((updatedPageTableEntries[pageTableIndex].flags & VirtualTextureSystem::PageTableFlag_Resident) == 0u)
        continue;
      installedRequestedAncestryPage = true;
      break;
    }
    assert(installedRequestedAncestryPage);
    assert(stats.uploaded >= 1u);
  }
}

void testFeedbackPrioritizesHighDemandPages()
{
  VirtualTextureSystem virtualTextures;
  VirtualTextureSystem::Settings settings{};
  settings.pageSize = 2u;
  settings.physicalPagesPerAxis = 8u;
  settings.maxUploadsPerFrame = 1u;
  virtualTextures.configure(settings);

  assert(virtualTextures.registerPreparedMaterial(0u, buildSyntheticPreparedMaterial()));

  const auto &textureEntry = virtualTextures.getTextureEntries()[0];
  assert(textureEntry.mipCount == 3u);

  const uint32_t finePageIndex = textureEntry.mips[0].pageTableOffset + 0u;
  const uint32_t broadPageIndex = textureEntry.mips[1].pageTableOffset + 0u;

  VirtualTextureSystem::FeedbackStats stats{};
  const bool changed = virtualTextures.processFeedbackRequests(
      {
          {0u, 0u, 0u, 0u},
          {0u, 1u, 0u, 0u},
          {0u, 1u, 0u, 0u},
          {0u, 1u, 0u, 0u},
          {0u, 1u, 0u, 0u},
      },
      1u,
      &stats);

  assert(changed);
  assert(stats.uploaded == 1u);

  const auto &pageTableEntries = virtualTextures.getPageTableEntries();
  assert(pageTableEntries[broadPageIndex].physicalPage != VirtualTextureSystem::InvalidPhysicalPage);
  assert((pageTableEntries[broadPageIndex].flags & VirtualTextureSystem::PageTableFlag_Resident) != 0u);
  assert(pageTableEntries[finePageIndex].physicalPage == VirtualTextureSystem::InvalidPhysicalPage);
}

void testFeedbackInstallsParentsBeforeFineChildren()
{
  VirtualTextureSystem virtualTextures;
  VirtualTextureSystem::Settings settings{};
  settings.pageSize = 2u;
  settings.physicalPagesPerAxis = 8u;
  settings.maxUploadsPerFrame = 1u;
  virtualTextures.configure(settings);

  assert(virtualTextures.registerPreparedMaterial(0u, buildSyntheticPreparedMaterial()));

  const auto &textureEntry = virtualTextures.getTextureEntries()[0];
  assert(textureEntry.mipCount == 3u);

  const uint32_t finePageIndex = textureEntry.mips[0].pageTableOffset + 0u;
  const uint32_t parentPageIndex = textureEntry.mips[1].pageTableOffset + 0u;

  VirtualTextureSystem::FeedbackStats stats{};
  const bool changed = virtualTextures.processFeedbackRequests(
      {
          {0u, 0u, 0u, 0u},
      },
      1u,
      &stats);

  assert(changed);
  assert(stats.uploaded == 1u);

  const auto &pageTableEntries = virtualTextures.getPageTableEntries();
  assert(pageTableEntries[parentPageIndex].physicalPage != VirtualTextureSystem::InvalidPhysicalPage);
  assert((pageTableEntries[parentPageIndex].flags & VirtualTextureSystem::PageTableFlag_Resident) != 0u);
  assert(pageTableEntries[finePageIndex].physicalPage == VirtualTextureSystem::InvalidPhysicalPage);
}

void testFeedbackPrioritizesVisibleRefinementOverNewBroadPages()
{
  VirtualTextureSystem virtualTextures;
  VirtualTextureSystem::Settings settings{};
  settings.pageSize = 2u;
  settings.physicalPagesPerAxis = 8u;
  settings.maxUploadsPerFrame = 1u;
  virtualTextures.configure(settings);

  assert(virtualTextures.registerPreparedMaterial(0u, buildSyntheticPreparedMaterial()));

  const auto &textureEntry = virtualTextures.getTextureEntries()[0];
  assert(textureEntry.mipCount == 3u);

  const uint32_t finePageIndex = textureEntry.mips[0].pageTableOffset + 0u;
  const uint32_t initialParentPageIndex = textureEntry.mips[1].pageTableOffset + 0u;
  const uint32_t competingBroadPageIndex = textureEntry.mips[1].pageTableOffset + 1u;

  VirtualTextureSystem::FeedbackStats stats{};
  assert(virtualTextures.processFeedbackRequests({{0u, 0u, 0u, 0u}}, 1u, &stats));
  assert(stats.uploaded == 1u);

  const auto &afterParentInstall = virtualTextures.getPageTableEntries();
  assert(afterParentInstall[initialParentPageIndex].physicalPage != VirtualTextureSystem::InvalidPhysicalPage);
  assert(afterParentInstall[finePageIndex].physicalPage == VirtualTextureSystem::InvalidPhysicalPage);
  assert(afterParentInstall[competingBroadPageIndex].physicalPage == VirtualTextureSystem::InvalidPhysicalPage);

  stats = {};
  assert(virtualTextures.processFeedbackRequests(
      {
          {0u, 0u, 0u, 0u},
          {0u, 1u, 1u, 0u},
          {0u, 1u, 1u, 0u},
          {0u, 1u, 1u, 0u},
          {0u, 1u, 1u, 0u},
          {0u, 1u, 1u, 0u},
          {0u, 1u, 1u, 0u},
          {0u, 1u, 1u, 0u},
          {0u, 1u, 1u, 0u},
      },
      1u,
      &stats));
  assert(stats.uploaded == 1u);

  const auto &pageTableEntries = virtualTextures.getPageTableEntries();
  assert(pageTableEntries[finePageIndex].physicalPage != VirtualTextureSystem::InvalidPhysicalPage);
  assert((pageTableEntries[finePageIndex].flags & VirtualTextureSystem::PageTableFlag_Resident) != 0u);
  assert(pageTableEntries[competingBroadPageIndex].physicalPage == VirtualTextureSystem::InvalidPhysicalPage);
}

} // namespace

int main()
{
  testPreparedMaterialBuild();
  testMaterialFileRoundTrip();
  testVirtualTextureRegistration();
  testFeedbackPrioritizesHighDemandPages();
  testFeedbackInstallsParentsBeforeFineChildren();
  testFeedbackPrioritizesVisibleRefinementOverNewBroadPages();
  std::cout << "VirtualTextureSystemTests passed" << std::endl;
  return 0;
}
