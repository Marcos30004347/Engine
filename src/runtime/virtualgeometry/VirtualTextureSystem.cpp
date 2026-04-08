#include "virtualgeometry/VirtualTextureSystem.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace virtualgeometry
{

namespace
{

static uint32_t packRGBA8(const uint8_t *rgba)
{
  return static_cast<uint32_t>(rgba[0]) | (static_cast<uint32_t>(rgba[1]) << 8u) | (static_cast<uint32_t>(rgba[2]) << 16u) | (static_cast<uint32_t>(rgba[3]) << 24u);
}

static uint32_t ceilDiv(uint32_t value, uint32_t divisor)
{
  return (value + divisor - 1u) / divisor;
}

static uint64_t makeRequestKey(const VirtualTextureSystem::FeedbackRequestGPU &request)
{
  return static_cast<uint64_t>(request.textureIndex) | (static_cast<uint64_t>(request.mipLevel) << 16u) | (static_cast<uint64_t>(request.pageX) << 24u) | (static_cast<uint64_t>(request.pageY) << 44u);
}

static uint32_t scoreHitBucket(uint32_t hits)
{
  uint32_t bucket = 0u;
  while (hits != 0u)
  {
    ++bucket;
    hits >>= 1u;
  }
  return bucket;
}

static bool isResidentPage(const VirtualTextureSystem::PageTableEntryGPU &entry)
{
  return (entry.flags & VirtualTextureSystem::PageTableFlag_Resident) != 0u && entry.physicalPage != VirtualTextureSystem::InvalidPhysicalPage;
}

static VirtualTextureSystem::TextureSampling normalizeSampling(VirtualTextureSystem::TextureSampling sampling, uint32_t mipCount)
{
  const uint32_t maxSupportedMip = VirtualTextureSystem::MaxMipsPerTexture - 1u;
  sampling.minMip = std::min(sampling.minMip, maxSupportedMip);
  sampling.maxMip = std::min(sampling.maxMip, maxSupportedMip);

  if (mipCount != 0u)
  {
    const uint32_t lastMip = mipCount - 1u;
    sampling.minMip = std::min(sampling.minMip, lastMip);
    sampling.maxMip = std::min(sampling.maxMip, lastMip);
  }

  if (sampling.minMip > sampling.maxMip)
    std::swap(sampling.minMip, sampling.maxMip);

  return sampling;
}

} // namespace

std::vector<VirtualTextureSystem::PreparedMipLevel> VirtualTextureSystem::buildMipChain(const rendering::assets::Image &image)
{
  if (!image.isValid())
    return {};

  std::vector<PreparedMipLevel> mips;
  PreparedMipLevel base;
  base.width = image.getWidth();
  base.height = image.getHeight();
  base.texels.resize(static_cast<size_t>(base.width) * static_cast<size_t>(base.height));

  const auto &pixels = image.getPixels();
  for (uint32_t y = 0u; y < base.height; ++y)
  {
    for (uint32_t x = 0u; x < base.width; ++x)
    {
      const size_t pixelIndex = (static_cast<size_t>(y) * base.width + x) * 4u;
      base.texels[static_cast<size_t>(y) * base.width + x] = packRGBA8(&pixels[pixelIndex]);
    }
  }
  mips.push_back(base);

  while (mips.back().width > 1u || mips.back().height > 1u)
  {
    const PreparedMipLevel &src = mips.back();
    PreparedMipLevel dst;
    dst.width = std::max(1u, src.width / 2u);
    dst.height = std::max(1u, src.height / 2u);
    dst.texels.resize(static_cast<size_t>(dst.width) * static_cast<size_t>(dst.height), 0u);

    for (uint32_t y = 0u; y < dst.height; ++y)
    {
      for (uint32_t x = 0u; x < dst.width; ++x)
      {
        uint32_t sum[4] = {};
        uint32_t taps = 0u;
        for (uint32_t oy = 0u; oy < 2u; ++oy)
        {
          for (uint32_t ox = 0u; ox < 2u; ++ox)
          {
            const uint32_t srcX = std::min(src.width - 1u, x * 2u + ox);
            const uint32_t srcY = std::min(src.height - 1u, y * 2u + oy);
            const uint32_t texel = src.texels[static_cast<size_t>(srcY) * src.width + srcX];
            sum[0] += texel & 0xFFu;
            sum[1] += (texel >> 8u) & 0xFFu;
            sum[2] += (texel >> 16u) & 0xFFu;
            sum[3] += (texel >> 24u) & 0xFFu;
            ++taps;
          }
        }

        dst.texels[static_cast<size_t>(y) * dst.width + x] = (sum[0] / taps) | ((sum[1] / taps) << 8u) | ((sum[2] / taps) << 16u) | ((sum[3] / taps) << 24u);
      }
    }

    mips.push_back(std::move(dst));
    if (mips.size() >= MaxMipsPerTexture)
      break;
  }

  return mips;
}

void VirtualTextureSystem::configure(const Settings &settings)
{
  if (settings.pageSize == 0u)
    throw std::runtime_error("VirtualTextureSystem page size must be greater than zero");
  if (settings.physicalPagesPerAxis == 0u)
    throw std::runtime_error("VirtualTextureSystem physical page grid must be greater than zero");
  if (settings.pageSize * settings.physicalPagesPerAxis > MaxVirtualTextureSize)
    throw std::runtime_error("VirtualTextureSystem physical atlas exceeds 16k limit");

  settings_ = settings;
  dirty_ = true;
}

const VirtualTextureSystem::Settings &VirtualTextureSystem::getSettings() const
{
  return settings_;
}

bool VirtualTextureSystem::registerMaterial(uint32_t materialIndex, const MaterialCreateInfo &info)
{
  return registerPreparedMaterial(materialIndex, buildPreparedMaterial(info));
}

VirtualTextureSystem::PreparedMaterial VirtualTextureSystem::buildPreparedMaterial(const MaterialCreateInfo &info)
{
  PreparedMaterial prepared;
  for (uint32_t textureIndex = 0u; textureIndex < info.textureCount && textureIndex < MAX_TEXTURES_PER_MATERIAL; ++textureIndex)
  {
    const TextureCreateInfo &createInfo = info.textures[textureIndex];
    if (createInfo.source.path.empty())
      continue;

    PreparedTexture texture;
    texture.sourcePath = createInfo.source.path;
    texture.mips = buildMipChain(rendering::assets::Image::load(createInfo.source.path, createInfo.source.flipVertically));
    if (texture.mips.empty())
      continue;
    if (texture.mips.front().width > MaxVirtualTextureSize || texture.mips.front().height > MaxVirtualTextureSize)
      throw std::runtime_error("VirtualTextureSystem only supports textures up to 16k x 16k");
    texture.sampling = normalizeSampling(createInfo.sampling, static_cast<uint32_t>(texture.mips.size()));

    prepared.textures[prepared.textureCount++] = std::move(texture);
  }

  return prepared;
}

bool VirtualTextureSystem::registerPreparedMaterial(uint32_t materialIndex, const PreparedMaterial &material)
{
  MaterialAsset asset;
  asset.materialIndex = materialIndex;
  asset.textures.reserve(material.textureCount);

  for (uint32_t textureIndex = 0u; textureIndex < material.textureCount && textureIndex < MAX_TEXTURES_PER_MATERIAL; ++textureIndex)
  {
    const PreparedTexture &preparedTexture = material.textures[textureIndex];
    if (preparedTexture.mips.empty())
      continue;

    TextureAsset textureAsset;
    textureAsset.path = preparedTexture.sourcePath;
    textureAsset.sampling = normalizeSampling(preparedTexture.sampling, static_cast<uint32_t>(preparedTexture.mips.size()));
    textureAsset.mips = preparedTexture.mips;
    asset.textures.push_back(std::move(textureAsset));
  }

  materials_[materialIndex] = std::move(asset);
  dirty_ = true;
  return true;
}

bool VirtualTextureSystem::hasMaterial(uint32_t materialIndex) const
{
  return materials_.find(materialIndex) != materials_.end();
}

void VirtualTextureSystem::clear()
{
  materials_.clear();
  dirty_ = true;
}

void VirtualTextureSystem::rebuildVirtualTextureState()
{
  atlasInfo_ = {};
  atlasInfo_.pageSize = settings_.pageSize;
  atlasInfo_.physicalPagesPerAxis = settings_.physicalPagesPerAxis;
  atlasInfo_.atlasWidth = settings_.pageSize * settings_.physicalPagesPerAxis;
  atlasInfo_.atlasHeight = settings_.pageSize * settings_.physicalPagesPerAxis;
  atlasInfo_.totalPhysicalPages = settings_.physicalPagesPerAxis * settings_.physicalPagesPerAxis;

  textureEntries_.clear();
  materialEntries_.clear();
  textureGpuAssets_.clear();
  pageTableEntries_.clear();
  pageTableOwners_.clear();
  physicalPagePixels_.clear();
  physicalPages_.clear();
  freePhysicalPages_.clear();
  dirtyPhysicalPages_.clear();
  dirtyPhysicalPageFlags_.clear();
  registeredMaterialIds_.clear();
  pagePriorityStates_.clear();
  residencyDirty_ = false;
  frameCounter_ = 0u;
  requestFrameCounter_ = 0u;

  if (materials_.empty())
  {
    dirty_ = false;
    return;
  }

  std::vector<uint32_t> materialIds;
  materialIds.reserve(materials_.size());
  for (const auto &entry : materials_)
    materialIds.push_back(entry.first);
  std::sort(materialIds.begin(), materialIds.end());
  registeredMaterialIds_ = materialIds;

  struct PendingTexture
  {
    uint32_t materialIndex = 0u;
    uint32_t localTextureIndex = 0u;
    TextureAsset *asset = nullptr;
  };

  std::vector<PendingTexture> pendingTextures;
  for (uint32_t materialId : materialIds)
  {
    MaterialAsset &material = materials_.at(materialId);
    for (uint32_t textureIndex = 0u; textureIndex < material.textures.size(); ++textureIndex)
      pendingTextures.push_back(PendingTexture{materialId, textureIndex, &material.textures[textureIndex]});
  }

  textureEntries_.resize(pendingTextures.size());
  textureGpuAssets_.resize(pendingTextures.size(), nullptr);
  const uint32_t maxMaterialId = materialIds.empty() ? 0u : materialIds.back();
  materialEntries_.assign(static_cast<size_t>(maxMaterialId) + 1u, MaterialEntryGPU{});

  uint32_t globalPageTableOffset = 0u;
  for (uint32_t textureGpuIndex = 0u; textureGpuIndex < pendingTextures.size(); ++textureGpuIndex)
  {
    PendingTexture &pending = pendingTextures[textureGpuIndex];
    textureGpuAssets_[textureGpuIndex] = pending.asset;
    TextureEntryGPU &textureEntry = textureEntries_[textureGpuIndex];
    textureEntry.mipCount = static_cast<uint32_t>(pending.asset->mips.size());
    textureEntry.pageSize = settings_.pageSize;
    const TextureSampling sampling = normalizeSampling(pending.asset->sampling, textureEntry.mipCount);
    textureEntry.addressModeU = static_cast<uint32_t>(sampling.addressModeU);
    textureEntry.addressModeV = static_cast<uint32_t>(sampling.addressModeV);
    textureEntry.filterMode = static_cast<uint32_t>(sampling.filterMode);
    textureEntry.mipBias = sampling.mipBias;
    textureEntry.minMip = sampling.minMip;
    textureEntry.maxMip = sampling.maxMip;

    MaterialEntryGPU &materialEntry = materialEntries_[pending.materialIndex];
    materialEntry.textureIndices[pending.localTextureIndex] = textureGpuIndex;
    materialEntry.textureCount = std::max(materialEntry.textureCount, pending.localTextureIndex + 1u);

    pending.asset->mipLayouts.resize(pending.asset->mips.size());
    for (uint32_t mipIndex = 0u; mipIndex < pending.asset->mips.size(); ++mipIndex)
    {
      const PreparedMipLevel &mip = pending.asset->mips[mipIndex];
      const uint32_t pagesX = std::max(1u, ceilDiv(mip.width, settings_.pageSize));
      const uint32_t pagesY = std::max(1u, ceilDiv(mip.height, settings_.pageSize));

      VirtualTextureMipGPU &gpuMip = textureEntry.mips[mipIndex];
      gpuMip.width = mip.width;
      gpuMip.height = mip.height;
      gpuMip.pageTableOffset = globalPageTableOffset;
      gpuMip.pagesX = pagesX;
      gpuMip.pagesY = pagesY;

      TextureMipCPU &cpuMip = pending.asset->mipLayouts[mipIndex];
      cpuMip.width = mip.width;
      cpuMip.height = mip.height;
      cpuMip.pageTableOffset = globalPageTableOffset;
      cpuMip.pagesX = pagesX;
      cpuMip.pagesY = pagesY;

      textureEntry.totalPageCount += pagesX * pagesY;
      globalPageTableOffset += pagesX * pagesY;
    }
  }

  atlasInfo_.textureCount = static_cast<uint32_t>(textureEntries_.size());
  atlasInfo_.materialCount = static_cast<uint32_t>(materialEntries_.size());
  atlasInfo_.pageTableEntryCount = globalPageTableOffset;

  pageTableEntries_.assign(globalPageTableOffset, PageTableEntryGPU{});
  pageTableOwners_.assign(globalPageTableOffset, VirtualPageId{});
  pagePriorityStates_.assign(globalPageTableOffset, PagePriorityState{});

  for (uint32_t textureGpuIndex = 0u; textureGpuIndex < pendingTextures.size(); ++textureGpuIndex)
  {
    const PendingTexture &pending = pendingTextures[textureGpuIndex];
    for (uint32_t mipIndex = 0u; mipIndex < pending.asset->mipLayouts.size(); ++mipIndex)
    {
      const TextureMipCPU &layout = pending.asset->mipLayouts[mipIndex];
      for (uint32_t pageY = 0u; pageY < layout.pagesY; ++pageY)
      {
        for (uint32_t pageX = 0u; pageX < layout.pagesX; ++pageX)
        {
          const uint32_t pageTableIndex = layout.pageTableOffset + pageY * layout.pagesX + pageX;
          pageTableOwners_[pageTableIndex] = VirtualPageId{textureGpuIndex, mipIndex, pageX, pageY};
        }
      }
    }
  }

  const size_t pixelsPerPhysicalPage = static_cast<size_t>(settings_.pageSize) * static_cast<size_t>(settings_.pageSize);
  physicalPagePixels_.assign(static_cast<size_t>(atlasInfo_.totalPhysicalPages) * pixelsPerPhysicalPage, 0u);
  physicalPages_.assign(atlasInfo_.totalPhysicalPages, PhysicalPageState{});
  dirtyPhysicalPageFlags_.assign(atlasInfo_.totalPhysicalPages, 0u);
  for (uint32_t physicalPage = 0u; physicalPage < atlasInfo_.totalPhysicalPages; ++physicalPage)
    freePhysicalPages_.push_back(physicalPage);

  installFallbackPages();

  dirty_ = false;
}

void VirtualTextureSystem::installFallbackPages()
{
  FeedbackStats stats;
  for (uint32_t textureIndex = 0u; textureIndex < textureEntries_.size(); ++textureIndex)
  {
    const TextureEntryGPU &entry = textureEntries_[textureIndex];
    if (entry.mipCount == 0u)
      continue;

    const uint32_t fallbackMip = entry.mipCount - 1u;
    const VirtualTextureMipGPU &fallbackLayout = entry.mips[fallbackMip];
    for (uint32_t pageY = 0u; pageY < fallbackLayout.pagesY; ++pageY)
    {
      for (uint32_t pageX = 0u; pageX < fallbackLayout.pagesX; ++pageX)
      {
        const uint32_t pageTableIndex = getPageTableIndex(textureIndex, fallbackMip, pageX, pageY);
        uploadVirtualPage(pageTableIndex, &stats);
        if (pageTableEntries_[pageTableIndex].physicalPage != InvalidPhysicalPage)
          physicalPages_[pageTableEntries_[pageTableIndex].physicalPage].pinned = true;
      }
    }
  }
}

uint32_t VirtualTextureSystem::getPageTableIndex(uint32_t textureIndex, uint32_t mipLevel, uint32_t pageX, uint32_t pageY) const
{
  if (textureIndex >= textureEntries_.size())
    return UINT32_MAX;
  const TextureEntryGPU &entry = textureEntries_[textureIndex];
  if (mipLevel >= entry.mipCount)
    return UINT32_MAX;

  const VirtualTextureMipGPU &mip = entry.mips[mipLevel];
  if (pageX >= mip.pagesX || pageY >= mip.pagesY)
    return UINT32_MAX;

  return mip.pageTableOffset + pageY * mip.pagesX + pageX;
}

void VirtualTextureSystem::touchPageTableEntry(uint32_t pageTableIndex, uint32_t physicalPageIndex)
{
  PageTableEntryGPU &entry = pageTableEntries_[pageTableIndex];
  const bool changed = entry.physicalPage != physicalPageIndex || entry.flags != PageTableFlag_Resident;
  entry.physicalPage = physicalPageIndex;
  entry.flags = PageTableFlag_Resident;

  PhysicalPageState &physicalPage = physicalPages_[physicalPageIndex];
  physicalPage.lastTouchedFrame = ++frameCounter_;
  residencyDirty_ = residencyDirty_ || changed;
}

void VirtualTextureSystem::markPhysicalPageDirty(uint32_t physicalPageIndex)
{
  if (physicalPageIndex >= dirtyPhysicalPageFlags_.size())
    return;
  if (dirtyPhysicalPageFlags_[physicalPageIndex] != 0u)
    return;

  dirtyPhysicalPageFlags_[physicalPageIndex] = 1u;
  dirtyPhysicalPages_.push_back(physicalPageIndex);
}

uint32_t VirtualTextureSystem::chooseEvictionCandidate() const
{
  uint64_t oldestFrame = std::numeric_limits<uint64_t>::max();
  uint32_t candidate = InvalidPhysicalPage;

  for (uint32_t physicalPageIndex = 0u; physicalPageIndex < physicalPages_.size(); ++physicalPageIndex)
  {
    const PhysicalPageState &page = physicalPages_[physicalPageIndex];
    if (!page.resident || page.pinned)
      continue;
    if (page.lastTouchedFrame < oldestFrame)
    {
      oldestFrame = page.lastTouchedFrame;
      candidate = physicalPageIndex;
    }
  }

  return candidate;
}

uint32_t VirtualTextureSystem::allocatePhysicalPage(const VirtualPageId &pageId, FeedbackStats *stats)
{
  if (!freePhysicalPages_.empty())
  {
    const uint32_t physicalPage = freePhysicalPages_.back();
    freePhysicalPages_.pop_back();
    return physicalPage;
  }

  const uint32_t evictedPhysicalPage = chooseEvictionCandidate();
  if (evictedPhysicalPage == InvalidPhysicalPage)
    return InvalidPhysicalPage;

  const PhysicalPageState &oldState = physicalPages_[evictedPhysicalPage];
  const uint32_t oldPageTableIndex = getPageTableIndex(oldState.textureIndex, oldState.mipLevel, oldState.pageX, oldState.pageY);
  if (oldPageTableIndex != UINT32_MAX)
  {
    pageTableEntries_[oldPageTableIndex] = PageTableEntryGPU{};
    residencyDirty_ = true;
  }

  if (stats != nullptr)
    stats->evicted += 1u;

  return evictedPhysicalPage;
}

void VirtualTextureSystem::writePhysicalPage(uint32_t physicalPageIndex, const VirtualPageId &pageId)
{
  if (pageId.textureIndex >= textureGpuAssets_.size())
    return;
  const TextureAsset *asset = textureGpuAssets_[pageId.textureIndex];
  if (asset == nullptr || pageId.mipLevel >= asset->mips.size())
    return;

  const PreparedMipLevel &mip = asset->mips[pageId.mipLevel];
  const size_t pixelsPerPhysicalPage = static_cast<size_t>(settings_.pageSize) * static_cast<size_t>(settings_.pageSize);
  uint32_t *dst = physicalPagePixels_.data() + static_cast<size_t>(physicalPageIndex) * pixelsPerPhysicalPage;

  const uint32_t srcBaseX = pageId.pageX * settings_.pageSize;
  const uint32_t srcBaseY = pageId.pageY * settings_.pageSize;
  for (uint32_t localY = 0u; localY < settings_.pageSize; ++localY)
  {
    const uint32_t srcY = std::min(mip.height - 1u, srcBaseY + localY);
    for (uint32_t localX = 0u; localX < settings_.pageSize; ++localX)
    {
      const uint32_t srcX = std::min(mip.width - 1u, srcBaseX + localX);
      dst[static_cast<size_t>(localY) * settings_.pageSize + localX] = mip.texels[static_cast<size_t>(srcY) * mip.width + srcX];
    }
  }

  markPhysicalPageDirty(physicalPageIndex);
}

void VirtualTextureSystem::uploadVirtualPage(uint32_t pageTableIndex, FeedbackStats *stats)
{
  if (pageTableIndex == UINT32_MAX || pageTableIndex >= pageTableEntries_.size())
    return;

  const VirtualPageId &pageId = pageTableOwners_[pageTableIndex];
  PageTableEntryGPU &entry = pageTableEntries_[pageTableIndex];
  if ((entry.flags & PageTableFlag_Resident) != 0u && entry.physicalPage != InvalidPhysicalPage)
  {
    touchPageTableEntry(pageTableIndex, entry.physicalPage);
    return;
  }

  const uint32_t physicalPageIndex = allocatePhysicalPage(pageId, stats);
  if (physicalPageIndex == InvalidPhysicalPage)
    return;

  writePhysicalPage(physicalPageIndex, pageId);

  PhysicalPageState &physicalPage = physicalPages_[physicalPageIndex];
  physicalPage.textureIndex = pageId.textureIndex;
  physicalPage.mipLevel = pageId.mipLevel;
  physicalPage.pageX = pageId.pageX;
  physicalPage.pageY = pageId.pageY;
  physicalPage.pinned = (pageId.mipLevel + 1u) == textureEntries_[pageId.textureIndex].mipCount;
  physicalPage.resident = true;

  touchPageTableEntry(pageTableIndex, physicalPageIndex);

  if (stats != nullptr)
    stats->uploaded += 1u;
}

bool VirtualTextureSystem::processFeedbackRequests(const std::vector<FeedbackRequestGPU> &requests, uint32_t uploadBudget, FeedbackStats *stats)
{
  ensureBuilt();
  ++requestFrameCounter_;
  FeedbackStats localStats{};
  localStats.requested = static_cast<uint32_t>(requests.size());

  if (requests.empty() || textureEntries_.empty())
  {
    if (stats != nullptr)
      *stats = localStats;
    return false;
  }

  struct RankedRequest
  {
    FeedbackRequestGPU request;
    uint32_t hits = 0u;
  };

  struct AggregatedPageRequest
  {
    uint32_t pageTableIndex = UINT32_MAX;
    uint32_t hits = 0u;
    uint32_t nearestResidentMip = MaxMipsPerTexture;
  };

  struct ScoredPageRequest
  {
    uint32_t pageTableIndex = UINT32_MAX;
    uint32_t hits = 0u;
    uint64_t score = 0u;
  };

  std::unordered_map<uint64_t, RankedRequest> rankedRequests;
  rankedRequests.reserve(requests.size());

  for (const FeedbackRequestGPU &request : requests)
  {
    const uint64_t key = makeRequestKey(request);
    auto &ranked = rankedRequests[key];
    ranked.request = request;
    ranked.hits += 1u;
  }

  std::vector<RankedRequest> deduplicated;
  deduplicated.reserve(rankedRequests.size());
  for (const auto &[key, ranked] : rankedRequests)
  {
    (void)key;
    deduplicated.push_back(ranked);
  }

  std::sort(
      deduplicated.begin(),
      deduplicated.end(),
      [](const RankedRequest &a, const RankedRequest &b)
      {
        if (a.hits != b.hits)
          return a.hits > b.hits;
        if (a.request.mipLevel != b.request.mipLevel)
          return a.request.mipLevel < b.request.mipLevel;
        if (a.request.textureIndex != b.request.textureIndex)
          return a.request.textureIndex < b.request.textureIndex;
        if (a.request.pageY != b.request.pageY)
          return a.request.pageY < b.request.pageY;
        return a.request.pageX < b.request.pageX;
      });

  localStats.deduplicated = static_cast<uint32_t>(deduplicated.size());
  const uint32_t effectiveBudget = std::max(1u, uploadBudget == 0u ? settings_.maxUploadsPerFrame : uploadBudget);
  std::unordered_map<uint32_t, AggregatedPageRequest> prioritizedMissingPages;
  prioritizedMissingPages.reserve(deduplicated.size() * 2u);

  for (const RankedRequest &ranked : deduplicated)
  {
    const FeedbackRequestGPU &request = ranked.request;
    if (request.textureIndex >= textureEntries_.size())
      continue;

    const TextureEntryGPU &textureEntry = textureEntries_[request.textureIndex];
    if (request.mipLevel >= textureEntry.mipCount)
      continue;

    const VirtualTextureMipGPU &requestedMipInfo = textureEntry.mips[request.mipLevel];
    if (request.pageX >= requestedMipInfo.pagesX || request.pageY >= requestedMipInfo.pagesY)
      continue;

    uint32_t pageX = request.pageX;
    uint32_t pageY = request.pageY;
    uint32_t nearestResidentMip = textureEntry.mipCount;
    uint32_t residentPageTableIndex = UINT32_MAX;
    std::vector<uint32_t> missingChain;
    missingChain.reserve(textureEntry.mipCount - request.mipLevel);

    for (uint32_t mipLevel = request.mipLevel; mipLevel < textureEntry.mipCount; ++mipLevel)
    {
      const VirtualTextureMipGPU &mipInfo = textureEntry.mips[mipLevel];
      pageX = std::min(pageX, mipInfo.pagesX - 1u);
      pageY = std::min(pageY, mipInfo.pagesY - 1u);

      const uint32_t pageTableIndex = getPageTableIndex(request.textureIndex, mipLevel, pageX, pageY);
      if (pageTableIndex == UINT32_MAX || pageTableIndex >= pageTableEntries_.size())
        break;

      const PageTableEntryGPU &entry = pageTableEntries_[pageTableIndex];
      if (isResidentPage(entry))
      {
        nearestResidentMip = mipLevel;
        residentPageTableIndex = pageTableIndex;
        break;
      }

      missingChain.push_back(pageTableIndex);

      if (mipLevel + 1u < textureEntry.mipCount)
      {
        const VirtualTextureMipGPU &nextMipInfo = textureEntry.mips[mipLevel + 1u];
        pageX = std::min(pageX >> 1u, nextMipInfo.pagesX - 1u);
        pageY = std::min(pageY >> 1u, nextMipInfo.pagesY - 1u);
      }
    }

    if (residentPageTableIndex != UINT32_MAX)
    {
      const PageTableEntryGPU &residentEntry = pageTableEntries_[residentPageTableIndex];
      touchPageTableEntry(residentPageTableIndex, residentEntry.physicalPage);
    }

    for (uint32_t pageTableIndex : missingChain)
    {
      AggregatedPageRequest &aggregated = prioritizedMissingPages[pageTableIndex];
      aggregated.pageTableIndex = pageTableIndex;
      aggregated.hits += ranked.hits;
      aggregated.nearestResidentMip = std::min(aggregated.nearestResidentMip, nearestResidentMip);
    }
  }

  std::vector<ScoredPageRequest> scoredRequests;
  scoredRequests.reserve(prioritizedMissingPages.size());
  for (auto &[pageTableIndex, aggregated] : prioritizedMissingPages)
  {
    if (pageTableIndex >= pageTableOwners_.size() || isResidentPage(pageTableEntries_[pageTableIndex]))
      continue;

    const VirtualPageId &pageId = pageTableOwners_[pageTableIndex];
    const TextureEntryGPU &textureEntry = textureEntries_[pageId.textureIndex];
    PagePriorityState &priorityState = pagePriorityStates_[pageTableIndex];
    if (priorityState.lastRequestedFrame + 1u == requestFrameCounter_)
      priorityState.consecutiveFramesRequested = std::min(priorityState.consecutiveFramesRequested + 1u, 255u);
    else
      priorityState.consecutiveFramesRequested = 1u;
    priorityState.lastRequestedFrame = requestFrameCounter_;

    const uint32_t gapToResident = aggregated.nearestResidentMip > pageId.mipLevel ? (aggregated.nearestResidentMip - pageId.mipLevel) : 0u;
    const uint32_t bridgeBoost = 64u - std::min(gapToResident, 63u);
    const uint32_t parentMip = pageId.mipLevel + 1u;
    bool immediateParentResident = parentMip >= textureEntry.mipCount;
    if (!immediateParentResident)
    {
      const VirtualTextureMipGPU &parentMipInfo = textureEntry.mips[parentMip];
      const uint32_t parentPageX = std::min(pageId.pageX >> 1u, parentMipInfo.pagesX - 1u);
      const uint32_t parentPageY = std::min(pageId.pageY >> 1u, parentMipInfo.pagesY - 1u);
      const uint32_t parentPageTableIndex = getPageTableIndex(pageId.textureIndex, parentMip, parentPageX, parentPageY);
      immediateParentResident = parentPageTableIndex != UINT32_MAX && isResidentPage(pageTableEntries_[parentPageTableIndex]);
    }

    const uint32_t detailLevel = textureEntry.mipCount > pageId.mipLevel ? (textureEntry.mipCount - 1u - pageId.mipLevel) : 0u;
    const uint32_t hitBucket = scoreHitBucket(aggregated.hits);

    uint64_t score = 0u;
    score += static_cast<uint64_t>(hitBucket) * 500000ull;
    score += static_cast<uint64_t>(priorityState.consecutiveFramesRequested) * 50000ull;
    score += static_cast<uint64_t>(bridgeBoost) * 25000ull;
    if (immediateParentResident)
      score += static_cast<uint64_t>(detailLevel + 1u) * 2000000ull;
    else
      score += static_cast<uint64_t>(pageId.mipLevel + 1u) * 250000ull;

    scoredRequests.push_back(ScoredPageRequest{
        .pageTableIndex = pageTableIndex,
        .hits = aggregated.hits,
        .score = score,
    });
  }

  std::sort(
      scoredRequests.begin(),
      scoredRequests.end(),
      [this](const ScoredPageRequest &a, const ScoredPageRequest &b)
      {
        if (a.score != b.score)
          return a.score > b.score;
        if (a.hits != b.hits)
          return a.hits > b.hits;
        const VirtualPageId &pageA = pageTableOwners_[a.pageTableIndex];
        const VirtualPageId &pageB = pageTableOwners_[b.pageTableIndex];
        if (pageA.mipLevel != pageB.mipLevel)
          return pageA.mipLevel > pageB.mipLevel;
        return a.pageTableIndex < b.pageTableIndex;
      });

  for (const ScoredPageRequest &scored : scoredRequests)
  {
    if (localStats.uploaded >= effectiveBudget)
      break;
    uploadVirtualPage(scored.pageTableIndex, &localStats);
  }

  const bool changed = localStats.uploaded != 0u || localStats.evicted != 0u;
  if (stats != nullptr)
    *stats = localStats;
  return changed;
}

void VirtualTextureSystem::ensureBuilt() const
{
  if (dirty_)
    const_cast<VirtualTextureSystem *>(this)->rebuildVirtualTextureState();
}

const VirtualTextureSystem::AtlasInfoGPU &VirtualTextureSystem::getAtlasInfo() const
{
  ensureBuilt();
  return atlasInfo_;
}

const std::vector<VirtualTextureSystem::TextureEntryGPU> &VirtualTextureSystem::getTextureEntries() const
{
  ensureBuilt();
  return textureEntries_;
}

const std::vector<VirtualTextureSystem::MaterialEntryGPU> &VirtualTextureSystem::getMaterialEntries() const
{
  ensureBuilt();
  return materialEntries_;
}

const std::vector<VirtualTextureSystem::PageTableEntryGPU> &VirtualTextureSystem::getPageTableEntries() const
{
  ensureBuilt();
  return pageTableEntries_;
}

const std::vector<uint32_t> &VirtualTextureSystem::getPhysicalPagePixels() const
{
  ensureBuilt();
  return physicalPagePixels_;
}

const std::vector<uint32_t> &VirtualTextureSystem::getRegisteredMaterialIds() const
{
  ensureBuilt();
  return registeredMaterialIds_;
}

const std::vector<uint32_t> &VirtualTextureSystem::getDirtyPhysicalPages() const
{
  ensureBuilt();
  return dirtyPhysicalPages_;
}

bool VirtualTextureSystem::hasDirtyResidency() const
{
  ensureBuilt();
  return residencyDirty_ || !dirtyPhysicalPages_.empty();
}

void VirtualTextureSystem::clearDirtyTracking()
{
  ensureBuilt();
  residencyDirty_ = false;
  dirtyPhysicalPages_.clear();
  std::fill(dirtyPhysicalPageFlags_.begin(), dirtyPhysicalPageFlags_.end(), 0u);
}

} // namespace virtualgeometry
