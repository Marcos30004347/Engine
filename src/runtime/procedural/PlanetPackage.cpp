#include "procedural/PlanetPackage.hpp"

#include "lz4.h"
#include "lz4hc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <tuple>

namespace procedural
{
namespace
{

constexpr uint32_t kHeaderSize = 128u;
constexpr uint32_t kChannelRecordSize = 64u;
constexpr uint32_t kBlockRecordSize = 80u;
constexpr uint64_t kPayloadAlignment = 16u;
constexpr uint32_t kBlockHasValueRange = 1u << 0u;
constexpr std::array<uint8_t, 8> kMagic{{'P', 'L', 'N', 'T', 'P', 'K', 'G', 0u}};
constexpr double kPi = 3.14159265358979323846264338327950288;

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool checkedAdd(const uint64_t a, const uint64_t b, uint64_t &result)
{
  if (a > std::numeric_limits<uint64_t>::max() - b)
    return false;
  result = a + b;
  return true;
}

bool checkedMultiply(const uint64_t a, const uint64_t b, uint64_t &result)
{
  if (a != 0u && b > std::numeric_limits<uint64_t>::max() / a)
    return false;
  result = a * b;
  return true;
}

bool alignUp(const uint64_t value, const uint64_t alignment, uint64_t &result)
{
  const uint64_t remainder = value % alignment;
  if (remainder == 0u)
  {
    result = value;
    return true;
  }
  return checkedAdd(value, alignment - remainder, result);
}

void writeU16(std::vector<uint8_t> &bytes, const size_t offset, const uint16_t value)
{
  bytes[offset + 0u] = static_cast<uint8_t>(value & 0xffu);
  bytes[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

void writeU32(std::vector<uint8_t> &bytes, const size_t offset, const uint32_t value)
{
  for (uint32_t byte = 0u; byte < 4u; ++byte)
    bytes[offset + byte] = static_cast<uint8_t>((value >> (byte * 8u)) & 0xffu);
}

void writeU64(std::vector<uint8_t> &bytes, const size_t offset, const uint64_t value)
{
  for (uint32_t byte = 0u; byte < 8u; ++byte)
    bytes[offset + byte] = static_cast<uint8_t>((value >> (byte * 8u)) & 0xffu);
}

void writeF64(std::vector<uint8_t> &bytes, const size_t offset, const double value)
{
  uint64_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  writeU64(bytes, offset, bits);
}

uint16_t readU16(const uint8_t *bytes)
{
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8u;
}

uint32_t readU32(const uint8_t *bytes)
{
  uint32_t value = 0u;
  for (uint32_t byte = 0u; byte < 4u; ++byte)
    value |= static_cast<uint32_t>(bytes[byte]) << (byte * 8u);
  return value;
}

uint64_t readU64(const uint8_t *bytes)
{
  uint64_t value = 0u;
  for (uint32_t byte = 0u; byte < 8u; ++byte)
    value |= static_cast<uint64_t>(bytes[byte]) << (byte * 8u);
  return value;
}

double readF64(const uint8_t *bytes)
{
  const uint64_t bits = readU64(bytes);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool validDomain(const PlanetPackageDomain domain)
{
  return domain == PlanetPackageDomain::Raster || domain == PlanetPackageDomain::Graph;
}

bool validCompression(const PlanetPackageCompression compression)
{
  return compression == PlanetPackageCompression::None || compression == PlanetPackageCompression::LZ4;
}

bool validFormat(const PlanetPackageFormat format)
{
  return format >= PlanetPackageFormat::Unknown && format <= PlanetPackageFormat::RG8UNorm;
}

bool validMipPolicy(const PlanetPackageMipPolicy policy)
{
  return policy >= PlanetPackageMipPolicy::None && policy <= PlanetPackageMipPolicy::NormalizeAverage;
}

bool validBlockKey(const PlanetPackageBlockKey &key, const uint8_t maximumLevel)
{
  if (!validDomain(key.domain) || key.channelId == 0u)
    return false;
  if (key.domain == PlanetPackageDomain::Graph)
    return key.tile.face == kPlanetPackageGraphFace && key.tile.level == 0u && key.tile.x == 0u && key.tile.y == 0u;
  return validPlanetTileAddress(key.tile) && key.tile.level <= maximumLevel;
}

auto channelOrder(const PlanetPackageChannel &channel)
{
  return std::tie(channel.domain, channel.id);
}

bool writeBytes(std::ofstream &file, const uint8_t *data, const size_t size)
{
  if (size == 0u)
    return true;
  file.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
  return static_cast<bool>(file);
}

bool readBytesAt(std::ifstream &file, const uint64_t offset, uint8_t *data, const size_t size)
{
  if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) || size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
    return false;
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file)
    return false;
  if (size == 0u)
    return true;
  file.read(reinterpret_cast<char *>(data), static_cast<std::streamsize>(size));
  return static_cast<bool>(file);
}

bool channelExists(const std::vector<PlanetPackageChannel> &channels, const PlanetPackageDomain domain, const uint32_t id)
{
  const auto found = std::lower_bound(
      channels.begin(),
      channels.end(),
      std::make_pair(domain, id),
      [](const PlanetPackageChannel &channel, const auto &key)
      {
        return std::make_pair(channel.domain, channel.id) < key;
      });
  return found != channels.end() && found->domain == domain && found->id == id;
}

std::vector<uint8_t> serializeChannel(const PlanetPackageChannel &channel)
{
  std::vector<uint8_t> bytes(kChannelRecordSize, 0u);
  writeU32(bytes, 0u, channel.id);
  writeU32(bytes, 4u, static_cast<uint32_t>(channel.domain));
  writeU32(bytes, 8u, static_cast<uint32_t>(channel.format));
  writeU32(bytes, 12u, static_cast<uint32_t>(channel.mipPolicy));
  writeU32(bytes, 16u, channel.flags);
  writeF64(bytes, 24u, channel.scale);
  writeF64(bytes, 32u, channel.bias);
  std::copy(channel.name.begin(), channel.name.end(), bytes.begin() + 40u);
  return bytes;
}

std::vector<uint8_t> serializeBlock(const PlanetPackageBlockRecord &block)
{
  std::vector<uint8_t> bytes(kBlockRecordSize, 0u);
  writeU32(bytes, 0u, static_cast<uint32_t>(block.key.domain));
  writeU32(bytes, 4u, block.key.channelId);
  bytes[8u] = block.key.tile.face;
  bytes[9u] = block.key.tile.level;
  writeU16(bytes, 10u, block.hasValueRange ? kBlockHasValueRange : 0u);
  writeU32(bytes, 12u, block.key.tile.x);
  writeU32(bytes, 16u, block.key.tile.y);
  writeU32(bytes, 20u, static_cast<uint32_t>(block.compression));
  writeU64(bytes, 24u, block.fileOffset);
  writeU64(bytes, 32u, block.storedSize);
  writeU64(bytes, 40u, block.uncompressedSize);
  writeU64(bytes, 48u, block.checksum);
  writeF64(bytes, 56u, block.minimumValue);
  writeF64(bytes, 64u, block.maximumValue);
  return bytes;
}

std::vector<uint8_t> serializeHeader(
    const PlanetPackageMetadata &metadata,
    const uint64_t fileSize,
    const uint64_t channelDirectoryOffset,
    const uint32_t channelCount,
    const uint64_t blockDirectoryOffset,
    const uint32_t blockCount,
    const uint64_t payloadOffset)
{
  std::vector<uint8_t> bytes(kHeaderSize, 0u);
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
  writeU32(bytes, 8u, kPlanetPackageVersion);
  writeU32(bytes, 12u, kPlanetPackageEndianMarker);
  writeU32(bytes, 16u, kHeaderSize);
  writeU32(bytes, 20u, kChannelRecordSize);
  writeU32(bytes, 24u, kBlockRecordSize);
  writeU64(bytes, 32u, fileSize);
  writeU64(bytes, 40u, channelDirectoryOffset);
  writeU32(bytes, 48u, channelCount);
  writeU32(bytes, 52u, blockCount);
  writeU64(bytes, 56u, blockDirectoryOffset);
  writeU64(bytes, 64u, payloadOffset);
  writeU64(bytes, 72u, metadata.contentHash);
  writeU64(bytes, 80u, metadata.buildHash);
  writeF64(bytes, 88u, metadata.planetRadiusMetres);
  writeF64(bytes, 96u, metadata.heightScaleMetres);
  writeF64(bytes, 104u, metadata.heightBiasMetres);
  writeF64(bytes, 112u, metadata.nominalRenderPatchEdgeMetres);
  writeU32(bytes, 120u, metadata.tileSizePixels);
  writeU16(bytes, 124u, metadata.gutterPixels);
  bytes[126u] = metadata.maximumTileLevel;
  bytes[127u] = kPlanetPackageCubeFaceCount;
  return bytes;
}

} // namespace

bool operator==(const PlanetTileAddress &a, const PlanetTileAddress &b)
{
  return a.face == b.face && a.level == b.level && a.x == b.x && a.y == b.y;
}

bool validPlanetTileAddress(const PlanetTileAddress &address)
{
  if (address.face >= kPlanetPackageCubeFaceCount || address.level > 31u)
    return false;
  const uint32_t dimension = uint32_t{1u} << address.level;
  return address.x < dimension && address.y < dimension;
}

PlanetTileAddress planetTileChild(const PlanetTileAddress &parent, const uint32_t child)
{
  if (!validPlanetTileAddress(parent) || parent.level >= 31u || child >= 4u)
    return {kPlanetPackageGraphFace, 0xffu, 0u, 0u};
  return {
    parent.face,
    static_cast<uint8_t>(parent.level + 1u),
    parent.x * 2u + (child & 1u),
    parent.y * 2u + ((child >> 1u) & 1u),
  };
}

PlanetTileAddress planetTileParent(const PlanetTileAddress &child)
{
  if (!validPlanetTileAddress(child) || child.level == 0u)
    return child;
  return {child.face, static_cast<uint8_t>(child.level - 1u), child.x >> 1u, child.y >> 1u};
}

uint64_t planetTileMorton(const PlanetTileAddress &address)
{
  if (!validPlanetTileAddress(address))
    return std::numeric_limits<uint64_t>::max();
  uint64_t morton = 0u;
  for (uint32_t bit = 0u; bit < address.level; ++bit)
  {
    morton |= static_cast<uint64_t>((address.x >> bit) & 1u) << (bit * 2u);
    morton |= static_cast<uint64_t>((address.y >> bit) & 1u) << (bit * 2u + 1u);
  }
  return morton;
}

double planetTileNominalEdgeMetres(const double planetRadiusMetres, const uint8_t level)
{
  if (!(planetRadiusMetres > 0.0) || !std::isfinite(planetRadiusMetres) || level > 31u)
    return 0.0;
  return (planetRadiusMetres * kPi * 0.5) / static_cast<double>(uint64_t{1u} << level);
}

double planetRadiusKilometres(const PlanetPackageMetadata &metadata)
{
  return metadata.planetRadiusMetres / 1000.0;
}

double planetDiameterKilometres(const PlanetPackageMetadata &metadata)
{
  return planetRadiusKilometres(metadata) * 2.0;
}

double planetCircumferenceKilometres(const PlanetPackageMetadata &metadata)
{
  return planetRadiusKilometres(metadata) * 2.0 * kPi;
}

double planetSurfaceAreaSquareKilometres(
    const PlanetPackageMetadata &metadata)
{
  const double radius = planetRadiusKilometres(metadata);
  return 4.0 * kPi * radius * radius;
}

uint8_t planetTileLevelForTargetEdge(const double planetRadiusMetres, const double targetEdgeMetres)
{
  if (!(planetRadiusMetres > 0.0) || !(targetEdgeMetres > 0.0) || !std::isfinite(planetRadiusMetres) || !std::isfinite(targetEdgeMetres))
    return 0u;
  uint8_t level = 0u;
  while (level < 31u && planetTileNominalEdgeMetres(planetRadiusMetres, level) > targetEdgeMetres)
    ++level;
  return level;
}

bool operator==(const PlanetPackageBlockKey &a, const PlanetPackageBlockKey &b)
{
  return a.domain == b.domain && a.channelId == b.channelId && a.tile == b.tile;
}

bool operator<(const PlanetPackageBlockKey &a, const PlanetPackageBlockKey &b)
{
  return std::tie(a.domain, a.tile.face, a.tile.level, a.tile.y, a.tile.x, a.channelId) < std::tie(b.domain, b.tile.face, b.tile.level, b.tile.y, b.tile.x, b.channelId);
}

PlanetPackageBlockKey planetRasterBlockKey(const uint32_t channelId, const PlanetTileAddress &tile)
{
  return {PlanetPackageDomain::Raster, channelId, tile};
}

PlanetPackageBlockKey planetGraphBlockKey(const uint32_t channelId)
{
  return {PlanetPackageDomain::Graph, channelId, {kPlanetPackageGraphFace, 0u, 0u, 0u}};
}

uint64_t planetPackageChecksum(const uint8_t *data, const size_t size)
{
  uint64_t hash = 1469598103934665603ull;
  for (size_t byte = 0u; byte < size; ++byte)
    hash = (hash ^ data[byte]) * 1099511628211ull;
  return hash;
}

bool writePlanetPackage(
    const std::filesystem::path &path,
    const PlanetPackageMetadata &metadata,
    const std::vector<PlanetPackageChannel> &inputChannels,
    const std::vector<PlanetPackageBlockInput> &inputBlocks,
    const PlanetPackageWriteOptions &options,
    PlanetPackageStatistics *statistics,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (path.empty())
    return fail(error, "Planet package path is empty");
  if (!(metadata.planetRadiusMetres > 0.0) || !std::isfinite(metadata.planetRadiusMetres) || !(metadata.heightScaleMetres > 0.0) || !std::isfinite(metadata.heightScaleMetres) ||
      !std::isfinite(metadata.heightBiasMetres) || !(metadata.nominalRenderPatchEdgeMetres > 0.0) || !std::isfinite(metadata.nominalRenderPatchEdgeMetres) || metadata.tileSizePixels == 0u ||
      metadata.maximumTileLevel > 31u)
    return fail(error, "Planet package metadata is invalid");
  if (!validCompression(options.compression))
    return fail(error, "Planet package compression is invalid");
  if (inputChannels.empty())
    return fail(error, "Planet package has no channel definitions");
  if (inputChannels.size() > std::numeric_limits<uint32_t>::max() || inputBlocks.size() > std::numeric_limits<uint32_t>::max())
    return fail(error, "Planet package directory exceeds the format limits");

  std::vector<PlanetPackageChannel> channels = inputChannels;
  std::sort(
      channels.begin(),
      channels.end(),
      [](const auto &a, const auto &b)
      {
        return channelOrder(a) < channelOrder(b);
      });
  for (size_t index = 0u; index < channels.size(); ++index)
  {
    const PlanetPackageChannel &channel = channels[index];
    if (!validDomain(channel.domain) || channel.id == 0u || !validFormat(channel.format) || !validMipPolicy(channel.mipPolicy) || !std::isfinite(channel.scale) || !std::isfinite(channel.bias) || channel.name.empty() ||
        channel.name.size() > 23u)
      return fail(error, "Planet package contains an invalid channel definition");
    if (index != 0u && channelOrder(channels[index - 1u]) == channelOrder(channel))
      return fail(error, "Planet package contains duplicate channel definitions");
  }

  std::vector<const PlanetPackageBlockInput *> blocks;
  blocks.reserve(inputBlocks.size());
  for (const PlanetPackageBlockInput &block : inputBlocks)
  {
    if (!validBlockKey(block.key, metadata.maximumTileLevel) || !channelExists(channels, block.key.domain, block.key.channelId) ||
        (block.hasValueRange && (!std::isfinite(block.minimumValue) || !std::isfinite(block.maximumValue) || block.minimumValue > block.maximumValue)))
      return fail(error, "Planet package contains an invalid block");
    if (block.payload.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
      return fail(error, "Planet package block exceeds the LZ4 block size limit");
    blocks.push_back(&block);
  }
  std::sort(
      blocks.begin(),
      blocks.end(),
      [](const auto *a, const auto *b)
      {
        return a->key < b->key;
      });
  for (size_t index = 1u; index < blocks.size(); ++index)
    if (blocks[index - 1u]->key == blocks[index]->key)
      return fail(error, "Planet package contains duplicate block keys");

  uint64_t channelDirectoryBytes = 0u;
  uint64_t blockDirectoryBytes = 0u;
  uint64_t blockDirectoryOffset = 0u;
  uint64_t payloadOffsetUnaligned = 0u;
  uint64_t payloadOffset = 0u;
  if (!checkedMultiply(channels.size(), kChannelRecordSize, channelDirectoryBytes) || !checkedMultiply(blocks.size(), kBlockRecordSize, blockDirectoryBytes) ||
      !checkedAdd(kHeaderSize, channelDirectoryBytes, blockDirectoryOffset) || !checkedAdd(blockDirectoryOffset, blockDirectoryBytes, payloadOffsetUnaligned) ||
      !alignUp(payloadOffsetUnaligned, kPayloadAlignment, payloadOffset))
    return fail(error, "Planet package directory size overflow");
  if (payloadOffset > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    return fail(error, "Planet package directory cannot be represented in memory");

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file)
    return fail(error, "Could not create planet package");
  std::vector<uint8_t> directoryPlaceholder(static_cast<size_t>(payloadOffset), 0u);
  if (!writeBytes(file, directoryPlaceholder.data(), directoryPlaceholder.size()))
    return fail(error, "Could not reserve the planet package directory");

  PlanetPackageStatistics result{};
  result.channelCount = static_cast<uint32_t>(channels.size());
  result.blockCount = static_cast<uint32_t>(blocks.size());
  std::vector<PlanetPackageBlockRecord> records;
  records.reserve(blocks.size());
  uint64_t fileOffset = payloadOffset;
  for (const PlanetPackageBlockInput *input : blocks)
  {
    uint64_t alignedOffset = 0u;
    if (!alignUp(fileOffset, kPayloadAlignment, alignedOffset))
      return fail(error, "Planet package payload offset overflow");
    const size_t padding = static_cast<size_t>(alignedOffset - fileOffset);
    if (padding != 0u)
    {
      const std::array<uint8_t, kPayloadAlignment> zeros{};
      if (!writeBytes(file, zeros.data(), padding))
        return fail(error, "Could not align a planet package block");
    }
    fileOffset = alignedOffset;

    const std::vector<uint8_t> *stored = &input->payload;
    std::vector<uint8_t> compressed;
    PlanetPackageCompression compression = PlanetPackageCompression::None;
    if (options.compression == PlanetPackageCompression::LZ4 && !input->payload.empty())
    {
      const int inputSize = static_cast<int>(input->payload.size());
      compressed.resize(static_cast<size_t>(LZ4_compressBound(inputSize)));
      const int compressedSize =
          options.highCompression
              ? LZ4_compress_HC(reinterpret_cast<const char *>(input->payload.data()), reinterpret_cast<char *>(compressed.data()), inputSize, static_cast<int>(compressed.size()), LZ4HC_CLEVEL_DEFAULT)
              : LZ4_compress_default(reinterpret_cast<const char *>(input->payload.data()), reinterpret_cast<char *>(compressed.data()), inputSize, static_cast<int>(compressed.size()));
      if (compressedSize <= 0)
        return fail(error, "Could not LZ4-compress a planet package block");
      compressed.resize(static_cast<size_t>(compressedSize));
      if (compressed.size() + options.minimumCompressionSavingsBytes < input->payload.size())
      {
        stored = &compressed;
        compression = PlanetPackageCompression::LZ4;
        ++result.compressedBlockCount;
      }
    }
    if (!writeBytes(file, stored->data(), stored->size()))
      return fail(error, "Could not write a planet package block");

    PlanetPackageBlockRecord record{};
    record.key = input->key;
    record.compression = compression;
    record.fileOffset = fileOffset;
    record.storedSize = stored->size();
    record.uncompressedSize = input->payload.size();
    record.checksum = planetPackageChecksum(input->payload.data(), input->payload.size());
    record.hasValueRange = input->hasValueRange;
    record.minimumValue = input->minimumValue;
    record.maximumValue = input->maximumValue;
    records.push_back(record);
    result.uncompressedPayloadBytes += record.uncompressedSize;
    result.storedPayloadBytes += record.storedSize;
    if (!checkedAdd(fileOffset, record.storedSize, fileOffset))
      return fail(error, "Planet package file size overflow");
  }
  result.fileBytes = fileOffset;

  file.seekp(0, std::ios::beg);
  const std::vector<uint8_t> header = serializeHeader(metadata, result.fileBytes, kHeaderSize, result.channelCount, blockDirectoryOffset, result.blockCount, payloadOffset);
  if (!writeBytes(file, header.data(), header.size()))
    return fail(error, "Could not write the planet package header");
  for (const PlanetPackageChannel &channel : channels)
  {
    const std::vector<uint8_t> bytes = serializeChannel(channel);
    if (!writeBytes(file, bytes.data(), bytes.size()))
      return fail(error, "Could not write the planet package channel directory");
  }
  for (const PlanetPackageBlockRecord &record : records)
  {
    const std::vector<uint8_t> bytes = serializeBlock(record);
    if (!writeBytes(file, bytes.data(), bytes.size()))
      return fail(error, "Could not write the planet package block directory");
  }
  file.flush();
  if (!file)
    return fail(error, "Could not finalize the planet package");
  if (statistics != nullptr)
    *statistics = result;
  return true;
}

void PlanetPackageReader::close()
{
  path_.clear();
  metadata_ = {};
  channels_.clear();
  blocks_.clear();
}

bool PlanetPackageReader::open(const std::filesystem::path &path, std::string *error)
{
  close();
  if (error != nullptr)
    error->clear();
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return fail(error, "Could not open planet package");
  file.seekg(0, std::ios::end);
  const std::streamoff end = file.tellg();
  if (end < static_cast<std::streamoff>(kHeaderSize))
    return fail(error, "Planet package is smaller than its header");
  const uint64_t physicalFileSize = static_cast<uint64_t>(end);

  std::array<uint8_t, kHeaderSize> header{};
  if (!readBytesAt(file, 0u, header.data(), header.size()))
    return fail(error, "Could not read planet package header");
  if (!std::equal(kMagic.begin(), kMagic.end(), header.begin()))
    return fail(error, "Planet package magic is invalid");
  if (readU32(header.data() + 8u) != kPlanetPackageVersion)
    return fail(error, "Planet package version is unsupported");
  if (readU32(header.data() + 12u) != kPlanetPackageEndianMarker)
    return fail(error, "Planet package endian marker is invalid");
  if (readU32(header.data() + 16u) != kHeaderSize || readU32(header.data() + 20u) != kChannelRecordSize || readU32(header.data() + 24u) != kBlockRecordSize)
    return fail(error, "Planet package record sizes are unsupported");

  const uint64_t declaredFileSize = readU64(header.data() + 32u);
  const uint64_t channelDirectoryOffset = readU64(header.data() + 40u);
  const uint32_t channelCount = readU32(header.data() + 48u);
  const uint32_t blockCount = readU32(header.data() + 52u);
  const uint64_t blockDirectoryOffset = readU64(header.data() + 56u);
  const uint64_t payloadOffset = readU64(header.data() + 64u);
  uint64_t channelDirectoryBytes = 0u;
  uint64_t blockDirectoryBytes = 0u;
  uint64_t expectedBlockDirectoryOffset = 0u;
  uint64_t directoryEnd = 0u;
  if (declaredFileSize != physicalFileSize || !checkedMultiply(channelCount, kChannelRecordSize, channelDirectoryBytes) || !checkedMultiply(blockCount, kBlockRecordSize, blockDirectoryBytes) ||
      !checkedAdd(channelDirectoryOffset, channelDirectoryBytes, expectedBlockDirectoryOffset) || !checkedAdd(blockDirectoryOffset, blockDirectoryBytes, directoryEnd) || channelDirectoryOffset != kHeaderSize ||
      blockDirectoryOffset != expectedBlockDirectoryOffset || directoryEnd > payloadOffset || payloadOffset > declaredFileSize)
    return fail(error, "Planet package directory layout is invalid");

  PlanetPackageMetadata metadata{};
  metadata.contentHash = readU64(header.data() + 72u);
  metadata.buildHash = readU64(header.data() + 80u);
  metadata.planetRadiusMetres = readF64(header.data() + 88u);
  metadata.heightScaleMetres = readF64(header.data() + 96u);
  metadata.heightBiasMetres = readF64(header.data() + 104u);
  metadata.nominalRenderPatchEdgeMetres = readF64(header.data() + 112u);
  metadata.tileSizePixels = readU32(header.data() + 120u);
  metadata.gutterPixels = readU16(header.data() + 124u);
  metadata.maximumTileLevel = header[126u];
  if (header[127u] != kPlanetPackageCubeFaceCount || !(metadata.planetRadiusMetres > 0.0) || !std::isfinite(metadata.planetRadiusMetres) || !(metadata.heightScaleMetres > 0.0) ||
      !std::isfinite(metadata.heightScaleMetres) || !std::isfinite(metadata.heightBiasMetres) || !(metadata.nominalRenderPatchEdgeMetres > 0.0) || !std::isfinite(metadata.nominalRenderPatchEdgeMetres) ||
      metadata.tileSizePixels == 0u || metadata.maximumTileLevel > 31u)
    return fail(error, "Planet package metadata is invalid");

  if (channelDirectoryBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) || blockDirectoryBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    return fail(error, "Planet package directory cannot be represented in memory");
  std::vector<uint8_t> channelBytes(static_cast<size_t>(channelDirectoryBytes));
  std::vector<uint8_t> blockBytes(static_cast<size_t>(blockDirectoryBytes));
  if (!readBytesAt(file, channelDirectoryOffset, channelBytes.data(), channelBytes.size()) || !readBytesAt(file, blockDirectoryOffset, blockBytes.data(), blockBytes.size()))
    return fail(error, "Could not read planet package directories");

  std::vector<PlanetPackageChannel> channels;
  channels.reserve(channelCount);
  for (uint32_t index = 0u; index < channelCount; ++index)
  {
    const uint8_t *record = channelBytes.data() + static_cast<size_t>(index) * kChannelRecordSize;
    PlanetPackageChannel channel{};
    channel.id = readU32(record + 0u);
    channel.domain = static_cast<PlanetPackageDomain>(readU32(record + 4u));
    channel.format = static_cast<PlanetPackageFormat>(readU32(record + 8u));
    channel.mipPolicy = static_cast<PlanetPackageMipPolicy>(readU32(record + 12u));
    channel.flags = readU32(record + 16u);
    channel.scale = readF64(record + 24u);
    channel.bias = readF64(record + 32u);
    size_t nameLength = 0u;
    while (nameLength < 24u && record[40u + nameLength] != 0u)
      ++nameLength;
    if (nameLength == 24u)
      return fail(error, "Planet package channel name is not terminated");
    channel.name.assign(reinterpret_cast<const char *>(record + 40u), nameLength);
    if (!validDomain(channel.domain) || channel.id == 0u || !validFormat(channel.format) || !validMipPolicy(channel.mipPolicy) || !std::isfinite(channel.scale) || !std::isfinite(channel.bias) || channel.name.empty() ||
        (!channels.empty() && channelOrder(channels.back()) >= channelOrder(channel)))
      return fail(error, "Planet package channel directory is invalid");
    channels.push_back(std::move(channel));
  }

  std::vector<PlanetPackageBlockRecord> blocks;
  blocks.reserve(blockCount);
  for (uint32_t index = 0u; index < blockCount; ++index)
  {
    const uint8_t *serialized = blockBytes.data() + static_cast<size_t>(index) * kBlockRecordSize;
    PlanetPackageBlockRecord block{};
    block.key.domain = static_cast<PlanetPackageDomain>(readU32(serialized + 0u));
    block.key.channelId = readU32(serialized + 4u);
    block.key.tile.face = serialized[8u];
    block.key.tile.level = serialized[9u];
    const uint16_t flags = readU16(serialized + 10u);
    block.key.tile.x = readU32(serialized + 12u);
    block.key.tile.y = readU32(serialized + 16u);
    block.compression = static_cast<PlanetPackageCompression>(readU32(serialized + 20u));
    block.fileOffset = readU64(serialized + 24u);
    block.storedSize = readU64(serialized + 32u);
    block.uncompressedSize = readU64(serialized + 40u);
    block.checksum = readU64(serialized + 48u);
    block.hasValueRange = (flags & kBlockHasValueRange) != 0u;
    block.minimumValue = readF64(serialized + 56u);
    block.maximumValue = readF64(serialized + 64u);
    uint64_t blockEnd = 0u;
    if ((flags & ~kBlockHasValueRange) != 0u || !validBlockKey(block.key, metadata.maximumTileLevel) || !validCompression(block.compression) || !channelExists(channels, block.key.domain, block.key.channelId) ||
        !checkedAdd(block.fileOffset, block.storedSize, blockEnd) || block.fileOffset < payloadOffset || blockEnd > declaredFileSize || block.uncompressedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        block.storedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()) || (block.compression == PlanetPackageCompression::None && block.storedSize != block.uncompressedSize) ||
        (block.hasValueRange && (!std::isfinite(block.minimumValue) || !std::isfinite(block.maximumValue) || block.minimumValue > block.maximumValue)) || (!blocks.empty() && !(blocks.back().key < block.key)))
      return fail(error, "Planet package block directory is invalid");
    blocks.push_back(block);
  }

  path_ = path;
  metadata_ = metadata;
  channels_ = std::move(channels);
  blocks_ = std::move(blocks);
  return true;
}

const PlanetPackageChannel *PlanetPackageReader::findChannel(const PlanetPackageDomain domain, const uint32_t id) const
{
  const auto found = std::lower_bound(
      channels_.begin(),
      channels_.end(),
      std::make_pair(domain, id),
      [](const PlanetPackageChannel &channel, const auto &key)
      {
        return std::make_pair(channel.domain, channel.id) < key;
      });
  return found != channels_.end() && found->domain == domain && found->id == id ? &*found : nullptr;
}

const PlanetPackageBlockRecord *PlanetPackageReader::findBlock(const PlanetPackageBlockKey &key) const
{
  const auto found = std::lower_bound(
      blocks_.begin(),
      blocks_.end(),
      key,
      [](const PlanetPackageBlockRecord &block, const PlanetPackageBlockKey &candidate)
      {
        return block.key < candidate;
      });
  return found != blocks_.end() && found->key == key ? &*found : nullptr;
}

const PlanetPackageBlockRecord *PlanetPackageReader::findBestRasterBlock(const uint32_t channelId, PlanetTileAddress tile) const
{
  if (!validPlanetTileAddress(tile))
    return nullptr;
  for (;;)
  {
    if (const PlanetPackageBlockRecord *record = findBlock(planetRasterBlockKey(channelId, tile)); record != nullptr)
      return record;
    if (tile.level == 0u)
      return nullptr;
    tile = planetTileParent(tile);
  }
}

bool PlanetPackageReader::readBlock(const PlanetPackageBlockKey &key, std::vector<uint8_t> &payload, std::string *error) const
{
  const PlanetPackageBlockRecord *record = findBlock(key);
  if (record == nullptr)
    return fail(error, "Planet package block was not found");
  return readBlock(*record, payload, error);
}

bool PlanetPackageReader::readBlock(const PlanetPackageBlockRecord &record, std::vector<uint8_t> &payload, std::string *error) const
{
  payload.clear();
  if (error != nullptr)
    error->clear();
  if (!isOpen())
    return fail(error, "Planet package reader is not open");
  const PlanetPackageBlockRecord *known = findBlock(record.key);
  if (known == nullptr || known->fileOffset != record.fileOffset || known->storedSize != record.storedSize || known->uncompressedSize != record.uncompressedSize || known->checksum != record.checksum ||
      known->compression != record.compression)
    return fail(error, "Planet package block record does not belong to this reader");
  if (record.storedSize > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) || record.uncompressedSize > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    return fail(error, "Planet package block cannot be represented in memory");

  std::ifstream file(path_, std::ios::binary);
  if (!file)
    return fail(error, "Could not reopen planet package");
  std::vector<uint8_t> stored(static_cast<size_t>(record.storedSize));
  if (!readBytesAt(file, record.fileOffset, stored.data(), stored.size()))
    return fail(error, "Could not read planet package block");
  if (record.compression == PlanetPackageCompression::None)
    payload = std::move(stored);
  else
  {
    payload.resize(static_cast<size_t>(record.uncompressedSize));
    if (record.uncompressedSize == 0u)
    {
      if (record.storedSize != 0u)
        return fail(error, "Empty planet package block has stored data");
    }
    else
    {
      const int decoded = LZ4_decompress_safe(reinterpret_cast<const char *>(stored.data()), reinterpret_cast<char *>(payload.data()), static_cast<int>(stored.size()), static_cast<int>(payload.size()));
      if (decoded < 0 || static_cast<size_t>(decoded) != payload.size())
      {
        payload.clear();
        return fail(error, "Could not LZ4-decompress planet package block");
      }
    }
  }
  if (planetPackageChecksum(payload.data(), payload.size()) != record.checksum)
  {
    payload.clear();
    return fail(error, "Planet package block checksum failed");
  }
  return true;
}

} // namespace procedural
