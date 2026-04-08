#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "rendering/assets/Image.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"

namespace virtualgeometry
{

class VirtualTextureSystem
{
public:
  static constexpr uint32_t MaxVirtualTextureSize = 16384u;
  static constexpr uint32_t MaxMipsPerTexture = 16u;
  static constexpr uint32_t DefaultPageSize = 128u;
  static constexpr uint32_t DefaultPhysicalPagesPerAxis = 32u;
  static constexpr uint32_t DefaultMaxUploadsPerFrame = 32u;
  static constexpr uint32_t InvalidPhysicalPage = UINT32_MAX;
  static constexpr uint32_t PageTableFlag_Resident = 1u << 0u;

  enum class TextureAddressMode : uint32_t
  {
    Repeat = 0u,
    ClampToEdge = 1u,
  };

  enum class TextureFilterMode : uint32_t
  {
    Nearest = 0u,
    Linear = 1u,
  };

  struct Settings
  {
    uint32_t pageSize = DefaultPageSize;
    uint32_t physicalPagesPerAxis = DefaultPhysicalPagesPerAxis;
    uint32_t maxUploadsPerFrame = DefaultMaxUploadsPerFrame;
  };

  struct VirtualTextureMipGPU
  {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t pageTableOffset = 0u;
    uint32_t pagesX = 0u;
    uint32_t pagesY = 0u;
    uint32_t _padding[3] = {};
  };

  struct TextureEntryGPU
  {
    VirtualTextureMipGPU mips[MaxMipsPerTexture];
    uint32_t mipCount = 0u;
    uint32_t pageSize = DefaultPageSize;
    uint32_t totalPageCount = 0u;
    uint32_t flags = 0u;
    uint32_t addressModeU = static_cast<uint32_t>(TextureAddressMode::Repeat);
    uint32_t addressModeV = static_cast<uint32_t>(TextureAddressMode::Repeat);
    uint32_t filterMode = static_cast<uint32_t>(TextureFilterMode::Nearest);
    float mipBias = 0.0f;
    uint32_t minMip = 0u;
    uint32_t maxMip = MaxMipsPerTexture - 1u;
    uint32_t _padding[2] = {};
  };

  struct MaterialEntryGPU
  {
    uint32_t textureIndices[MAX_TEXTURES_PER_MATERIAL] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t textureCount = 0u;
    uint32_t _padding[3] = {};
  };

  struct PageTableEntryGPU
  {
    uint32_t physicalPage = InvalidPhysicalPage;
    uint32_t flags = 0u;
    uint32_t _padding[2] = {};
  };

  struct AtlasInfoGPU
  {
    uint32_t pageSize = DefaultPageSize;
    uint32_t physicalPagesPerAxis = DefaultPhysicalPagesPerAxis;
    uint32_t atlasWidth = DefaultPageSize * DefaultPhysicalPagesPerAxis;
    uint32_t atlasHeight = DefaultPageSize * DefaultPhysicalPagesPerAxis;
    uint32_t totalPhysicalPages = DefaultPhysicalPagesPerAxis * DefaultPhysicalPagesPerAxis;
    uint32_t textureCount = 0u;
    uint32_t materialCount = 0u;
    uint32_t pageTableEntryCount = 0u;
  };

  struct TextureSource
  {
    std::string path;
    bool flipVertically = false;
  };

  struct TextureSampling
  {
    TextureAddressMode addressModeU = TextureAddressMode::Repeat;
    TextureAddressMode addressModeV = TextureAddressMode::Repeat;
    TextureFilterMode filterMode = TextureFilterMode::Nearest;
    float mipBias = 0.0f;
    uint32_t minMip = 0u;
    uint32_t maxMip = MaxMipsPerTexture - 1u;
  };

  struct TextureCreateInfo
  {
    TextureSource source;
    TextureSampling sampling;
  };

  struct PreparedMipLevel
  {
    uint32_t width = 0u;
    uint32_t height = 0u;
    std::vector<uint32_t> texels;
  };

  struct PreparedTexture
  {
    std::string sourcePath;
    TextureSampling sampling;
    std::vector<PreparedMipLevel> mips;
  };

  struct PreparedMaterial
  {
    std::array<PreparedTexture, MAX_TEXTURES_PER_MATERIAL> textures;
    uint32_t textureCount = 0u;
  };

  struct MaterialCreateInfo
  {
    std::array<TextureCreateInfo, MAX_TEXTURES_PER_MATERIAL> textures;
    uint32_t textureCount = 0u;
  };

  struct FeedbackRequestGPU
  {
    uint32_t textureIndex = 0u;
    uint32_t mipLevel = 0u;
    uint32_t pageX = 0u;
    uint32_t pageY = 0u;
  };

  struct FeedbackHeaderGPU
  {
    uint32_t requestCount = 0u;
    uint32_t overflowCount = 0u;
    uint32_t _padding[2] = {};
  };

  struct FeedbackStats
  {
    uint32_t requested = 0u;
    uint32_t deduplicated = 0u;
    uint32_t uploaded = 0u;
    uint32_t evicted = 0u;
    uint32_t overflowed = 0u;
  };

  static PreparedMaterial buildPreparedMaterial(const MaterialCreateInfo &info);

  void configure(const Settings &settings);
  const Settings &getSettings() const;

  bool registerMaterial(uint32_t materialIndex, const MaterialCreateInfo &info);
  bool registerPreparedMaterial(uint32_t materialIndex, const PreparedMaterial &material);
  bool hasMaterial(uint32_t materialIndex) const;
  void clear();

  bool processFeedbackRequests(const std::vector<FeedbackRequestGPU> &requests, uint32_t uploadBudget, FeedbackStats *stats = nullptr);

  const AtlasInfoGPU &getAtlasInfo() const;
  const std::vector<TextureEntryGPU> &getTextureEntries() const;
  const std::vector<MaterialEntryGPU> &getMaterialEntries() const;
  const std::vector<PageTableEntryGPU> &getPageTableEntries() const;
  const std::vector<uint32_t> &getPhysicalPagePixels() const;
  const std::vector<uint32_t> &getRegisteredMaterialIds() const;
  const std::vector<uint32_t> &getDirtyPhysicalPages() const;

  bool hasDirtyResidency() const;
  void clearDirtyTracking();

private:
  struct TextureMipCPU
  {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t pageTableOffset = 0u;
    uint32_t pagesX = 0u;
    uint32_t pagesY = 0u;
  };

  struct TextureAsset
  {
    std::string path;
    TextureSampling sampling;
    std::vector<PreparedMipLevel> mips;
    std::vector<TextureMipCPU> mipLayouts;
  };

  struct MaterialAsset
  {
    uint32_t materialIndex = 0u;
    std::vector<TextureAsset> textures;
  };

  struct VirtualPageId
  {
    uint32_t textureIndex = 0u;
    uint32_t mipLevel = 0u;
    uint32_t pageX = 0u;
    uint32_t pageY = 0u;
  };

  struct PhysicalPageState
  {
    uint32_t textureIndex = UINT32_MAX;
    uint32_t mipLevel = UINT32_MAX;
    uint32_t pageX = 0u;
    uint32_t pageY = 0u;
    uint64_t lastTouchedFrame = 0u;
    bool pinned = false;
    bool resident = false;
  };

  struct PagePriorityState
  {
    uint64_t lastRequestedFrame = 0u;
    uint32_t consecutiveFramesRequested = 0u;
  };

  static std::vector<PreparedMipLevel> buildMipChain(const rendering::assets::Image &image);

  void ensureBuilt() const;
  void rebuildVirtualTextureState();
  void installFallbackPages();

  uint32_t getPageTableIndex(uint32_t textureIndex, uint32_t mipLevel, uint32_t pageX, uint32_t pageY) const;
  void uploadVirtualPage(uint32_t pageTableIndex, FeedbackStats *stats);
  void writePhysicalPage(uint32_t physicalPageIndex, const VirtualPageId &pageId);
  uint32_t allocatePhysicalPage(const VirtualPageId &pageId, FeedbackStats *stats);
  uint32_t chooseEvictionCandidate() const;
  void touchPageTableEntry(uint32_t pageTableIndex, uint32_t physicalPageIndex);
  void markPhysicalPageDirty(uint32_t physicalPageIndex);

  Settings settings_;
  mutable bool dirty_ = false;
  mutable bool residencyDirty_ = false;
  mutable uint64_t frameCounter_ = 0u;

  mutable AtlasInfoGPU atlasInfo_;
  mutable std::vector<TextureEntryGPU> textureEntries_;
  mutable std::vector<MaterialEntryGPU> materialEntries_;
  mutable std::vector<const TextureAsset *> textureGpuAssets_;
  mutable std::vector<PageTableEntryGPU> pageTableEntries_;
  mutable std::vector<VirtualPageId> pageTableOwners_;
  mutable std::vector<uint32_t> physicalPagePixels_;
  mutable std::vector<PhysicalPageState> physicalPages_;
  mutable std::vector<uint32_t> freePhysicalPages_;
  mutable std::vector<uint32_t> dirtyPhysicalPages_;
  mutable std::vector<uint8_t> dirtyPhysicalPageFlags_;
  mutable std::vector<uint32_t> registeredMaterialIds_;
  std::vector<PagePriorityState> pagePriorityStates_;
  uint64_t requestFrameCounter_ = 0u;

  std::unordered_map<uint32_t, MaterialAsset> materials_;
};

} // namespace virtualgeometry
