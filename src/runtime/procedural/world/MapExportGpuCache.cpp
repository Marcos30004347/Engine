#include "procedural/world/MapExportGpuCache.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace procedural::world {

static_assert(static_cast<uint32_t>(MapExportFormat::R8Uint) == 0u);
static_assert(static_cast<uint32_t>(MapExportFormat::R16Float) == 1u);
static_assert(static_cast<uint32_t>(MapExportFormat::R16Unorm) == 2u);
static_assert(static_cast<uint32_t>(MapExportFormat::R32Uint) == 3u);
static_assert(static_cast<uint32_t>(MapExportFormat::RG16Float) == 4u);
static_assert(static_cast<uint32_t>(MapExportFormat::RGBA8Uint) == 5u);
static_assert(static_cast<uint32_t>(MapExportFormat::RGBA8Unorm) == 6u);
static_assert(static_cast<uint32_t>(MapExportFilter::Nearest) == 0u);
static_assert(static_cast<uint32_t>(MapExportFilter::Linear) == 1u);
static_assert(static_cast<uint32_t>(MapExportFilter::SparseMixture) == 2u);

namespace {

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
  {
    *error = message;
  }
  return false;
}

uint32_t pageTableBase(const uint32_t level)
{
  return ((uint32_t{1u} << (level * 2u)) - 1u) / 3u;
}

struct KeyLess
{
  bool operator()(const MapExportStreamKey &left, const MapExportStreamKey &right) const
  {
    return std::tie(left.field, left.address.level, left.address.x, left.address.y) < std::tie(right.field, right.address.level, right.address.x, right.address.y);
  }
};

} // namespace

std::vector<MapExportGpuPageTableDirtyRange> mapExportGpuPageTableDirtyRanges(const std::vector<uint32_t> &previous, const std::vector<uint32_t> &next)
{
  std::vector<MapExportGpuPageTableDirtyRange> ranges;
  if (previous.size() != next.size())
  {
    if (!next.empty())
    {
      ranges.push_back({0u, static_cast<uint32_t>(std::min<size_t>(next.size(), std::numeric_limits<uint32_t>::max()))});
    }
    return ranges;
  }

  for (size_t index = 0u; index < next.size();)
  {
    if (previous[index] == next[index])
    {
      ++index;
      continue;
    }
    const size_t first = index;
    do
    {
      ++index;
    } while (index < next.size() && previous[index] != next[index]);
    ranges.push_back({
      static_cast<uint32_t>(first),
      static_cast<uint32_t>(index - first),
    });
  }
  return ranges;
}

bool MapExportGpuPageLayout::initialize(const MapExportBundle &manifest, const uint32_t slotCapacity, std::string *error)
{
  clear();
  if (slotCapacity == 0u || manifest.pageSizePixels == 0u || manifest.maximumPageLevel > 8u)
  {
    return fail(error, "GPU page-cache layout configuration is invalid");
  }
  const uint32_t tableEntries = pageTableBase(static_cast<uint32_t>(manifest.maximumPageLevel) + 1u);
  if (tableEntries == 0u)
  {
    return fail(error, "GPU page-cache layout has no logical entries");
  }

  const uint32_t storedSide = manifest.pageSizePixels + manifest.gutterPixels * 2u;
  size_t slotBytes = 0u;
  for (const MapExportField &field : manifest.fields)
  {
    if (!field.available)
    {
      continue;
    }
    const uint32_t texelBytes = mapExportTexelBytes(field.format);
    const size_t pageBytes = static_cast<size_t>(storedSide) * storedSide * texelBytes;
    if (texelBytes == 0u || pageBytes == 0u)
    {
      return fail(error, "GPU page-cache field format is invalid");
    }
    const uint32_t tableOffset = static_cast<uint32_t>(fields_.size()) * tableEntries;
    fields_.push_back({field.name, field.format, field.filter, texelBytes, tableOffset});
    MapExportGpuFieldDescriptor descriptor{};
    descriptor.format = static_cast<uint32_t>(field.format);
    descriptor.filter = static_cast<uint32_t>(field.filter);
    descriptor.texelBytes = texelBytes;
    descriptor.pageTableOffset = tableOffset;
    for (size_t component = 0u; component < 4u; ++component)
    {
      descriptor.scale[component] = static_cast<float>(field.scale[component]);
      descriptor.offset[component] = static_cast<float>(field.offset[component]);
      if (!std::isfinite(descriptor.scale[component]) || !std::isfinite(descriptor.offset[component]))
      {
        clear();
        return fail(error, "GPU page-cache field transform is not representable");
      }
    }
    descriptors_.push_back(descriptor);
    slotBytes = std::max(slotBytes, pageBytes);
  }
  if (fields_.empty() || slotBytes == 0u || slotBytes > std::numeric_limits<size_t>::max() / slotCapacity)
  {
    clear();
    return fail(error, "GPU page-cache physical allocation is invalid");
  }
  manifest_ = &manifest;
  slotCapacity_ = slotCapacity;
  entriesPerField_ = tableEntries;
  // Storage-buffer writes and u32 byte-address decoding require word alignment.
  physicalSlotBytes_ = (slotBytes + 3u) & ~size_t{3u};
  if (physicalSlotBytes_ > std::numeric_limits<uint32_t>::max())
  {
    clear();
    return fail(error, "GPU page-cache slot stride exceeds the shader contract");
  }
  header_ = {
    manifest.pageSizePixels,
    manifest.gutterPixels,
    storedSide,
    manifest.maximumPageLevel,
    static_cast<uint32_t>(physicalSlotBytes_),
    entriesPerField_,
    static_cast<uint32_t>(fields_.size()),
    manifest.resolution,
  };
  return true;
}

void MapExportGpuPageLayout::clear()
{
  manifest_ = nullptr;
  fields_.clear();
  header_ = {};
  descriptors_.clear();
  slotCapacity_ = 0u;
  entriesPerField_ = 0u;
  physicalSlotBytes_ = 0u;
}

std::optional<uint32_t> MapExportGpuPageLayout::fieldIndex(const std::string &name) const
{
  const auto field = std::find_if(fields_.begin(), fields_.end(), [&](const MapExportGpuField &candidate) {
    return candidate.name == name;
  });
  if (field == fields_.end())
  {
    return std::nullopt;
  }
  return static_cast<uint32_t>(std::distance(fields_.begin(), field));
}

std::optional<uint32_t> MapExportGpuPageLayout::pageTableIndex(const uint32_t fieldIndex, const MapExportPageAddress &address) const
{
  if (fieldIndex >= fields_.size() || manifest_ == nullptr || !validMapExportPageAddress(address, manifest_->maximumPageLevel))
  {
    return std::nullopt;
  }
  const uint64_t index = static_cast<uint64_t>(fields_[fieldIndex].pageTableOffset) + pageTableBase(address.level) + mapExportPageMorton(address);
  if (index >= static_cast<uint64_t>(entriesPerField_) * fields_.size())
  {
    return std::nullopt;
  }
  return static_cast<uint32_t>(index);
}

bool MapExportGpuPageLayout::encode(const MapExportPagePublication &publication, MapExportGpuPublication &encoded, std::string *error) const
{
  encoded = {};
  if (manifest_ == nullptr || fields_.empty())
  {
    return fail(error, "GPU page-cache layout is not initialized");
  }
  encoded.revision = publication.revision;
  encoded.pageTable.assign(static_cast<size_t>(entriesPerField_) * fields_.size(), 0u);

  std::set<uint32_t> usedSlots;
  std::set<MapExportStreamKey, KeyLess> mappedKeys;
  for (const MapExportPhysicalMapping &mapping : publication.pageTable)
  {
    const std::optional<uint32_t> field = fieldIndex(mapping.key.field);
    if (!field.has_value() || mapping.slot >= slotCapacity_ || !validMapExportPageAddress(mapping.key.address, manifest_->maximumPageLevel) || !usedSlots.insert(mapping.slot).second ||
        !mappedKeys.insert(mapping.key).second)
    {
      return fail(error, "GPU publication contains an invalid mapping");
    }
    const MapExportField *source = manifest_->field(mapping.key.field);
    if (source == nullptr || source->page(mapping.key.address) == nullptr)
    {
      return fail(error, "GPU publication maps a page absent from the manifest");
    }
    const std::optional<uint32_t> logical = pageTableIndex(*field, mapping.key.address);
    if (!logical.has_value() || encoded.pageTable[*logical] != 0u)
    {
      return fail(error, "GPU publication aliases a logical address");
    }
    encoded.pageTable[*logical] = mapping.slot + 1u;
  }

  // A sparse sampling group may be absent at an address, but never partial.
  for (const MapExportPhysicalMapping &mapping : publication.pageTable)
  {
    const MapExportField *field = manifest_->field(mapping.key.field);
    if (field == nullptr || field->samplingGroup.empty())
    {
      continue;
    }
    for (const MapExportField &member : manifest_->fields)
    {
      if (member.available && member.samplingGroup == field->samplingGroup && mappedKeys.find({member.name, mapping.key.address}) == mappedKeys.end())
      {
        return fail(error, "GPU publication splits a semantic sampling group");
      }
    }
  }

  encoded.physicalWrites.reserve(publication.uploads.size());
  for (const MapExportPhysicalUpload &upload : publication.uploads)
  {
    const MapExportField *field = manifest_->field(upload.key.field);
    const MapExportPage *page = field == nullptr ? nullptr : field->page(upload.key.address);
    const size_t expectedBytes = field == nullptr || page == nullptr ? 0u : static_cast<size_t>(page->width) * page->height * mapExportTexelBytes(field->format);
    const auto mapped = std::find_if(publication.pageTable.begin(), publication.pageTable.end(), [&](const MapExportPhysicalMapping &entry) {
      return entry.key == upload.key && entry.slot == upload.slot;
    });
    if (upload.slot >= slotCapacity_ || expectedBytes == 0u || upload.data.size() != expectedBytes || upload.data.size() > physicalSlotBytes_ || mapped == publication.pageTable.end())
    {
      return fail(error, "GPU publication contains an invalid physical upload");
    }
    encoded.physicalWrites.push_back({upload.key, upload.slot, static_cast<size_t>(upload.slot) * physicalSlotBytes_, upload.data.size()});
  }
  return true;
}

MapExportGpuPageCache::MapExportGpuPageCache(::rendering::RHI &rhi) : rhi_(rhi)
{
}

MapExportGpuPageCache::~MapExportGpuPageCache()
{
  clear();
}

bool MapExportGpuPageCache::initialize(const MapExportBundle &manifest, const uint32_t slotCapacity, std::string *error)
{
  clear();
  if (!layout_.initialize(manifest, slotCapacity, error))
  {
    return false;
  }
  try
  {
    physicalBuffer_ = rhi_.createBuffer({
      .name = "mapExport.physicalPages",
      .size = layout_.physicalBufferBytes(),
      .usage = static_cast<::rendering::BufferUsage>(::rendering::BufferUsage_Storage | ::rendering::BufferUsage_Push),
    });
    pageTableBuffer_ = rhi_.createBuffer({
      .name = "mapExport.pageTable",
      .size = static_cast<size_t>(layout_.entriesPerField()) * layout_.fields().size() * sizeof(uint32_t),
      .usage = static_cast<::rendering::BufferUsage>(::rendering::BufferUsage_Storage | ::rendering::BufferUsage_Push),
    });
    metadataBuffer_ = rhi_.createBuffer({
      .name = "mapExport.metadata",
      .size = sizeof(MapExportGpuHeader) + layout_.descriptors().size() * sizeof(MapExportGpuFieldDescriptor),
      .usage = static_cast<::rendering::BufferUsage>(::rendering::BufferUsage_Storage | ::rendering::BufferUsage_Push),
    });
    pageTableShadow_.assign(static_cast<size_t>(layout_.entriesPerField()) * layout_.fields().size(), 0u);
    rhi_.bufferWrite(pageTableBuffer_, 0u, pageTableShadow_.size() * sizeof(uint32_t), pageTableShadow_.data());
    rhi_.bufferWrite(metadataBuffer_, 0u, sizeof(MapExportGpuHeader), const_cast<MapExportGpuHeader *>(&layout_.header()));
    rhi_.bufferWrite(metadataBuffer_, sizeof(MapExportGpuHeader), layout_.descriptors().size() * sizeof(MapExportGpuFieldDescriptor), const_cast<MapExportGpuFieldDescriptor *>(layout_.descriptors().data()));
    return true;
  }
  catch (const std::exception &exception)
  {
    clear();
    return fail(error, exception.what());
  }
}

void MapExportGpuPageCache::clear()
{
  if (metadataBuffer_ != ::rendering::BufferId::Invalid)
  {
    rhi_.deleteBuffer(metadataBuffer_);
    metadataBuffer_ = ::rendering::BufferId::Invalid;
  }
  if (pageTableBuffer_ != ::rendering::BufferId::Invalid)
  {
    rhi_.deleteBuffer(pageTableBuffer_);
    pageTableBuffer_ = ::rendering::BufferId::Invalid;
  }
  if (physicalBuffer_ != ::rendering::BufferId::Invalid)
  {
    rhi_.deleteBuffer(physicalBuffer_);
    physicalBuffer_ = ::rendering::BufferId::Invalid;
  }
  layout_.clear();
  pageTableShadow_.clear();
  publishedRevision_ = 0u;
}

bool MapExportGpuPageCache::apply(const MapExportPagePublication &publication, std::string *error)
{
  if (physicalBuffer_ == ::rendering::BufferId::Invalid || pageTableBuffer_ == ::rendering::BufferId::Invalid || metadataBuffer_ == ::rendering::BufferId::Invalid)
  {
    return fail(error, "GPU page cache is not initialized");
  }
  if (publication.revision < publishedRevision_)
  {
    return fail(error, "GPU page cache rejected a stale publication");
  }
  MapExportGpuPublication encoded;
  if (!layout_.encode(publication, encoded, error))
  {
    return false;
  }
  try
  {
    for (size_t index = 0u; index < encoded.physicalWrites.size(); ++index)
    {
      const MapExportGpuPhysicalWrite &write = encoded.physicalWrites[index];
      rhi_.bufferWrite(physicalBuffer_, write.byteOffset, write.byteSize, const_cast<uint8_t *>(publication.uploads[index].data.data()));
    }
    // Commit point: shaders cannot resolve new slots until these final table
    // writes. Keep them sparse so tiny streaming changes do not stage/upload
    // the entire logical atlas page table.
    if (pageTableShadow_.size() != encoded.pageTable.size())
    {
      pageTableShadow_.assign(encoded.pageTable.size(), 0u);
    }
    const auto dirtyRanges = mapExportGpuPageTableDirtyRanges(pageTableShadow_, encoded.pageTable);
    for (const auto range : dirtyRanges)
    {
      if (!range.valid())
      {
        continue;
      }
      rhi_.bufferWrite(pageTableBuffer_, static_cast<size_t>(range.first) * sizeof(uint32_t), static_cast<size_t>(range.count) * sizeof(uint32_t), encoded.pageTable.data() + range.first);
    }
    pageTableShadow_ = std::move(encoded.pageTable);
    publishedRevision_ = publication.revision;
    return true;
  }
  catch (const std::exception &exception)
  {
    return fail(error, exception.what());
  }
}

} // namespace procedural::world
