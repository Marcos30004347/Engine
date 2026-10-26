#include "procedural/world/MapExport.hpp"
#include "procedural/world/MapTerrainRefinement.hpp"

#include "lz4.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#endif
#include "json.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace procedural::world
{

namespace
{

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void hashBytes(uint64_t &hash, const void *data, const size_t size)
{
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0u; i < size; ++i)
  {
    hash ^= bytes[i];
    hash *= kFnvPrime;
  }
}

void hashString(uint64_t &hash, const std::string &value)
{
  hashBytes(hash, value.data(), value.size());
  const uint8_t terminator = 0u;
  hashBytes(hash, &terminator, 1u);
}

void hashU8(uint64_t &hash, const uint8_t value)
{
  hashBytes(hash, &value, 1u);
}

void hashU32(uint64_t &hash, const uint32_t value)
{
  for (uint32_t shift = 0u; shift < 32u; shift += 8u)
    hashU8(hash, static_cast<uint8_t>(value >> shift));
}

void hashF64(uint64_t &hash, const double value)
{
  const double normalized = value == 0.0 ? 0.0 : value;
  uint64_t bits = 0u;
  std::memcpy(&bits, &normalized, sizeof(bits));
  for (uint32_t shift = 0u; shift < 64u; shift += 8u)
    hashU8(hash, static_cast<uint8_t>(bits >> shift));
}

uint64_t hashData(const std::vector<uint8_t> &data)
{
  uint64_t hash = kFnvOffset;
  hashBytes(hash, data.data(), data.size());
  return hash;
}

size_t storedSize(const MapExportPage &page)
{
  return page.compression == MapExportCompression::None ? page.data.size() : page.storedData.size();
}

void appendU8(std::vector<uint8_t> &out, const uint8_t value)
{
  out.push_back(value);
}

void appendU16(std::vector<uint8_t> &out, const uint16_t value)
{
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8u));
}

void appendU32(std::vector<uint8_t> &out, const uint32_t value)
{
  for (uint32_t shift = 0u; shift < 32u; shift += 8u)
    out.push_back(static_cast<uint8_t>(value >> shift));
}

void appendU64(std::vector<uint8_t> &out, const uint64_t value)
{
  for (uint32_t shift = 0u; shift < 64u; shift += 8u)
    out.push_back(static_cast<uint8_t>(value >> shift));
}

void appendF64(std::vector<uint8_t> &out, const double value)
{
  uint64_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  appendU64(out, bits);
}

void appendPoint(std::vector<uint8_t> &out, const Point2 point)
{
  appendF64(out, point.x);
  appendF64(out, point.y);
}

void appendString(std::vector<uint8_t> &out, const std::string &value)
{
  appendU32(out, static_cast<uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

void appendPoints(std::vector<uint8_t> &out, const std::vector<Point2> &points)
{
  appendU32(out, static_cast<uint32_t>(points.size()));
  for (const Point2 point : points)
    appendPoint(out, point);
}

void appendIds(std::vector<uint8_t> &out, const std::vector<uint32_t> &ids)
{
  appendU32(out, static_cast<uint32_t>(ids.size()));
  for (const uint32_t id : ids)
    appendU32(out, id);
}

uint16_t floatToHalf(const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint16_t sign = static_cast<uint16_t>((bits >> 16u) & 0x8000u);
  const uint32_t exponent = (bits >> 23u) & 0xffu;
  uint32_t mantissa = bits & 0x7fffffu;
  if (exponent == 0xffu)
    return static_cast<uint16_t>(sign | (mantissa == 0u ? 0x7c00u : 0x7e00u));

  const int32_t halfExponent = static_cast<int32_t>(exponent) - 127 + 15;
  if (halfExponent >= 31)
    return static_cast<uint16_t>(sign | 0x7c00u);
  if (halfExponent <= 0)
  {
    if (halfExponent < -10)
      return sign;
    mantissa |= 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - halfExponent);
    uint32_t rounded = mantissa >> shift;
    const uint32_t remainder = mantissa & ((1u << shift) - 1u);
    const uint32_t halfway = 1u << (shift - 1u);
    if (remainder > halfway || (remainder == halfway && (rounded & 1u)))
      ++rounded;
    return static_cast<uint16_t>(sign | rounded);
  }

  uint32_t rounded = mantissa >> 13u;
  const uint32_t remainder = mantissa & 0x1fffu;
  if (remainder > 0x1000u || (remainder == 0x1000u && (rounded & 1u)))
  {
    ++rounded;
    if (rounded == 0x400u)
    {
      rounded = 0u;
      if (halfExponent + 1 >= 31)
        return static_cast<uint16_t>(sign | 0x7c00u);
      return static_cast<uint16_t>(sign | ((halfExponent + 1) << 10u));
    }
  }
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(halfExponent) << 10u) | rounded);
}

float halfToFloat(const uint16_t value)
{
  const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16u;
  uint32_t exponent = (value >> 10u) & 0x1fu;
  uint32_t mantissa = value & 0x03ffu;
  uint32_t bits = 0u;
  if (exponent == 0u)
  {
    if (mantissa == 0u)
      bits = sign;
    else
    {
      int32_t unbiased = -14;
      while ((mantissa & 0x0400u) == 0u)
      {
        mantissa <<= 1u;
        --unbiased;
      }
      mantissa &= 0x03ffu;
      bits = sign | (static_cast<uint32_t>(unbiased + 127) << 23u) | (mantissa << 13u);
    }
  }
  else if (exponent == 31u)
    bits = sign | 0x7f800000u | (mantissa << 13u);
  else
    bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
  float result = 0.0f;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

void appendHalf(std::vector<uint8_t> &out, const double value)
{
  appendU16(out, floatToHalf(static_cast<float>(value)));
}

uint8_t unorm8(const double value)
{
  return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

struct PlacementPotential
{
  double trees = 0.0;
  double shrubs = 0.0;
  double groundcover = 0.0;
  double exposedRock = 0.0;
};

PlacementPotential potentialFor(const Biome biome)
{
  switch (biome)
  {
  case Biome::Ocean:
  case Biome::Lake:
    return {};
  case Biome::Shore:
    return {0.03, 0.15, 0.12, 0.20};
  case Biome::Glacier:
    return {0.0, 0.0, 0.0, 0.85};
  case Biome::Tundra:
    return {0.02, 0.18, 0.50, 0.45};
  case Biome::ColdDesert:
    return {0.01, 0.10, 0.16, 0.65};
  case Biome::SaltFlat:
    return {0.0, 0.02, 0.03, 0.20};
  case Biome::Taiga:
    return {0.90, 0.45, 0.60, 0.20};
  case Biome::TemperateForest:
    return {0.95, 0.65, 0.78, 0.15};
  case Biome::TemperateRainforest:
    return {1.0, 0.80, 0.95, 0.10};
  case Biome::MontaneForest:
    return {0.75, 0.55, 0.58, 0.55};
  case Biome::Mediterranean:
    return {0.45, 0.78, 0.58, 0.35};
  case Biome::Grassland:
    return {0.08, 0.30, 0.95, 0.15};
  case Biome::Steppe:
    return {0.02, 0.40, 0.65, 0.35};
  case Biome::Shrubland:
    return {0.12, 0.85, 0.55, 0.35};
  case Biome::Savanna:
    return {0.30, 0.55, 0.85, 0.20};
  case Biome::TropicalSeasonalForest:
    return {0.85, 0.65, 0.82, 0.15};
  case Biome::TropicalRainforest:
    return {1.0, 0.88, 1.0, 0.08};
  case Biome::SubtropicalForest:
    return {0.90, 0.72, 0.88, 0.10};
  case Biome::Desert:
    return {0.0, 0.15, 0.05, 0.75};
  case Biome::SandDesert:
    return {0.0, 0.03, 0.02, 0.25};
  case Biome::Wetland:
    return {0.15, 0.75, 1.0, 0.03};
  case Biome::Swamp:
    return {0.65, 0.90, 1.0, 0.03};
  case Biome::Mangrove:
    return {0.95, 0.85, 0.80, 0.03};
  case Biome::AlpineMeadow:
    return {0.02, 0.12, 0.65, 0.65};
  case Biome::BareRock:
    return {0.0, 0.02, 0.03, 1.0};
  case Biome::Count:
    break;
  }
  return {};
}

MapExportField makeField(const char *name, const MapExportFormat format, const MapExportFilter filter, const uint32_t resolution, const bool available)
{
  MapExportField field{};
  field.name = name;
  field.format = format;
  field.filter = filter;
  field.width = resolution;
  field.height = resolution;
  field.available = available;
  if (available)
    field.data.reserve(static_cast<size_t>(resolution) * resolution * mapExportTexelBytes(format));
  return field;
}

void describeField(MapExportField &field, std::initializer_list<const char *> components, std::initializer_list<const char *> units)
{
  field.components.clear();
  field.units.clear();
  for (const char *component : components)
    field.components.emplace_back(component);
  for (const char *unit : units)
    field.units.emplace_back(unit);
}

uint16_t loadU16(const uint8_t *bytes)
{
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8u;
}

uint32_t loadU32(const uint8_t *bytes)
{
  return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8u | static_cast<uint32_t>(bytes[2]) << 16u | static_cast<uint32_t>(bytes[3]) << 24u;
}

void storeU16(uint8_t *bytes, const uint16_t value)
{
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8u);
}

void storeU32(uint8_t *bytes, const uint32_t value)
{
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8u);
  bytes[2] = static_cast<uint8_t>(value >> 16u);
  bytes[3] = static_cast<uint8_t>(value >> 24u);
}

std::vector<uint8_t> downsampleField(const std::vector<uint8_t> &source, const uint32_t sourceSide, const MapExportFormat format, const MapExportFilter filter)
{
  const uint32_t targetSide = sourceSide / 2u;
  const uint32_t texelBytes = mapExportTexelBytes(format);
  std::vector<uint8_t> target(static_cast<size_t>(targetSide) * targetSide * texelBytes);
  for (uint32_t y = 0u; y < targetSide; ++y)
    for (uint32_t x = 0u; x < targetSide; ++x)
    {
      const std::array<const uint8_t *, 4> samples{
        source.data() + (static_cast<size_t>(y * 2u) * sourceSide + x * 2u) * texelBytes,
        source.data() + (static_cast<size_t>(y * 2u) * sourceSide + x * 2u + 1u) * texelBytes,
        source.data() + (static_cast<size_t>(y * 2u + 1u) * sourceSide + x * 2u) * texelBytes,
        source.data() + (static_cast<size_t>(y * 2u + 1u) * sourceSide + x * 2u + 1u) * texelBytes};
      uint8_t *destination = target.data() + (static_cast<size_t>(y) * targetSide + x) * texelBytes;
      if (filter == MapExportFilter::Nearest)
      {
        size_t selected = 0u;
        uint32_t selectedCount = 0u;
        for (size_t candidate = 0u; candidate < samples.size(); ++candidate)
        {
          uint32_t count = 0u;
          for (const uint8_t *sample : samples)
            count += std::memcmp(samples[candidate], sample, texelBytes) == 0 ? 1u : 0u;
          if (count > selectedCount || (count == selectedCount && std::lexicographical_compare(samples[candidate], samples[candidate] + texelBytes, samples[selected], samples[selected] + texelBytes)))
          {
            selected = candidate;
            selectedCount = count;
          }
        }
        std::memcpy(destination, samples[selected], texelBytes);
        continue;
      }

      switch (format)
      {
      case MapExportFormat::R16Unorm:
      {
        uint32_t sum = 0u;
        for (const uint8_t *sample : samples)
          sum += loadU16(sample);
        storeU16(destination, static_cast<uint16_t>((sum + 2u) / 4u));
        break;
      }
      case MapExportFormat::R16Float:
      case MapExportFormat::RG16Float:
      {
        const uint32_t components = mapExportComponentCount(format);
        for (uint32_t component = 0u; component < components; ++component)
        {
          double sum = 0.0;
          for (const uint8_t *sample : samples)
            sum += halfToFloat(loadU16(sample + component * 2u));
          storeU16(destination + component * 2u, floatToHalf(static_cast<float>(sum * 0.25)));
        }
        break;
      }
      case MapExportFormat::RGBA8Unorm:
        for (uint32_t component = 0u; component < 4u; ++component)
        {
          uint32_t sum = 0u;
          for (const uint8_t *sample : samples)
            sum += sample[component];
          destination[component] = static_cast<uint8_t>((sum + 2u) / 4u);
        }
        break;
      case MapExportFormat::R8Uint:
      case MapExportFormat::R32Uint:
      case MapExportFormat::RGBA8Uint:
        std::memcpy(destination, samples[0], texelBytes);
        break;
      }
    }
  return target;
}

constexpr size_t kBiomeMixtureSize = 4u;

void encodeBiomeMixture(const std::array<double, 256> &scores, uint8_t *ids, uint8_t *weights)
{
  struct Candidate
  {
    uint8_t id = 0u;
    double score = 0.0;
    double remainder = 0.0;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(static_cast<size_t>(Biome::Count));
  for (uint32_t id = 0u; id < static_cast<uint32_t>(Biome::Count); ++id)
    if (scores[id] > 0.0 && std::isfinite(scores[id]))
      candidates.push_back(Candidate{static_cast<uint8_t>(id), scores[id], 0.0});
  std::stable_sort(
      candidates.begin(),
      candidates.end(),
      [](const Candidate &left, const Candidate &right)
      {
        if (left.score != right.score)
          return left.score > right.score;
        return left.id < right.id;
      });
  if (candidates.size() > kBiomeMixtureSize)
    candidates.resize(kBiomeMixtureSize);
  std::fill(ids, ids + kBiomeMixtureSize, 0u);
  std::fill(weights, weights + kBiomeMixtureSize, 0u);
  if (candidates.empty())
  {
    ids[0] = static_cast<uint8_t>(Biome::Ocean);
    weights[0] = 255u;
    return;
  }

  double total = 0.0;
  for (const Candidate &candidate : candidates)
    total += candidate.score;
  uint32_t quantizedTotal = 0u;
  for (size_t slot = 0u; slot < candidates.size(); ++slot)
  {
    ids[slot] = candidates[slot].id;
    const double exact = candidates[slot].score / total * 255.0;
    weights[slot] = static_cast<uint8_t>(std::floor(exact));
    candidates[slot].remainder = exact - std::floor(exact);
    quantizedTotal += weights[slot];
  }
  std::vector<size_t> remainderOrder(candidates.size());
  for (size_t slot = 0u; slot < candidates.size(); ++slot)
    remainderOrder[slot] = slot;
  std::stable_sort(
      remainderOrder.begin(),
      remainderOrder.end(),
      [&](const size_t left, const size_t right)
      {
        if (candidates[left].remainder != candidates[right].remainder)
          return candidates[left].remainder > candidates[right].remainder;
        return candidates[left].id < candidates[right].id;
      });
  for (uint32_t remainder = quantizedTotal; remainder < 255u; ++remainder)
    ++weights[remainderOrder[(remainder - quantizedTotal) % remainderOrder.size()]];
}

void biomeMixtureFor(const MapGraph &graph, const std::vector<BlendWeight> &blendWeights, uint8_t *ids, uint8_t *weights)
{
  std::array<double, 256> scores{};
  for (const BlendWeight &weight : blendWeights)
    if (weight.node < graph.nodeCount() && weight.weight > 0.0)
      scores[graph.node(weight.node).biome] += weight.weight;
  encodeBiomeMixture(scores, ids, weights);
}

double localHeightVariation(const MapSurfaceField &surface, const Point2 position, const double centreElevation)
{
  const double radius = std::max(1.0, surface.spacingMetres());
  double lowest = centreElevation;
  double highest = centreElevation;
  for (const Point2 offset : {Point2(radius, 0.0), Point2(-radius, 0.0), Point2(0.0, radius), Point2(0.0, -radius)})
  {
    const double elevation = surface.sample(Point2(position.x + offset.x, position.y + offset.y)).elevationMetres;
    lowest = std::min(lowest, elevation);
    highest = std::max(highest, elevation);
  }
  return highest - lowest;
}

void downsampleBiomeMixture(
    const std::vector<uint8_t> &sourceIds,
    const std::vector<uint8_t> &sourceWeights,
    const uint32_t sourceSide,
    std::vector<uint8_t> &targetHard,
    std::vector<uint8_t> &targetIds,
    std::vector<uint8_t> &targetWeights)
{
  const uint32_t targetSide = sourceSide / 2u;
  const size_t targetPixels = static_cast<size_t>(targetSide) * targetSide;
  targetHard.resize(targetPixels);
  targetIds.resize(targetPixels * kBiomeMixtureSize);
  targetWeights.resize(targetPixels * kBiomeMixtureSize);
  for (uint32_t y = 0u; y < targetSide; ++y)
    for (uint32_t x = 0u; x < targetSide; ++x)
    {
      std::array<double, 256> scores{};
      for (uint32_t childY = 0u; childY < 2u; ++childY)
        for (uint32_t childX = 0u; childX < 2u; ++childX)
        {
          const size_t child = (static_cast<size_t>(y * 2u + childY) * sourceSide + x * 2u + childX) * kBiomeMixtureSize;
          for (size_t slot = 0u; slot < kBiomeMixtureSize; ++slot)
          {
            const uint8_t id = sourceIds[child + slot];
            if (id < static_cast<uint8_t>(Biome::Count))
              scores[id] += sourceWeights[child + slot];
          }
        }
      const size_t target = static_cast<size_t>(y) * targetSide + x;
      encodeBiomeMixture(scores, targetIds.data() + target * kBiomeMixtureSize, targetWeights.data() + target * kBiomeMixtureSize);
      targetHard[target] = targetIds[target * kBiomeMixtureSize];
    }
}

std::string pageFileName(const std::string &fieldName, const MapExportPageAddress &address)
{
  std::ostringstream out;
  out << "pages/" << fieldName << "/l_" << std::setw(2) << std::setfill('0') << static_cast<uint32_t>(address.level) << "_x_" << std::setw(5) << address.x << "_y_" << std::setw(5) << address.y << ".bin";
  return out.str();
}

bool encodePageStorage(MapExportPage &page, const bool compress, std::string *error)
{
  page.contentHash = hashData(page.data);
  page.storedHash = page.contentHash;
  if (!compress)
    return true;
  if (page.data.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
  {
    if (error != nullptr)
      *error = "map export page is too large for LZ4 compression";
    return false;
  }
  const int sourceBytes = static_cast<int>(page.data.size());
  const int capacity = LZ4_compressBound(sourceBytes);
  if (capacity <= 0)
  {
    if (error != nullptr)
      *error = "could not size an LZ4 map export page";
    return false;
  }
  std::vector<uint8_t> compressed(static_cast<size_t>(capacity));
  const int compressedBytes = LZ4_compress_default(reinterpret_cast<const char *>(page.data.data()), reinterpret_cast<char *>(compressed.data()), sourceBytes, capacity);
  if (compressedBytes <= 0)
  {
    if (error != nullptr)
      *error = "could not compress an LZ4 map export page";
    return false;
  }
  if (compressedBytes < sourceBytes)
  {
    compressed.resize(static_cast<size_t>(compressedBytes));
    page.compression = MapExportCompression::Lz4;
    page.file += ".lz4";
    page.storedData = std::move(compressed);
    page.storedHash = hashData(page.storedData);
  }
  return true;
}

bool buildPagesFromLevels(MapExportField &field, const std::vector<std::vector<uint8_t>> &levels, const uint32_t pageSize, const uint16_t gutter, const uint8_t maximumLevel, const bool compress, std::string *error)
{
  const uint32_t texelBytes = mapExportTexelBytes(field.format);
  const uint32_t storedSide = pageSize + static_cast<uint32_t>(gutter) * 2u;
  field.pages.clear();
  for (uint8_t level = 0u; level <= maximumLevel; ++level)
  {
    const uint32_t dimension = uint32_t{1u} << level;
    const uint32_t rasterSide = pageSize * dimension;
    const std::vector<uint8_t> &raster = levels[level];
    for (uint32_t y = 0u; y < dimension; ++y)
      for (uint32_t x = 0u; x < dimension; ++x)
      {
        MapExportPage page{};
        page.address = {level, x, y};
        page.file = pageFileName(field.name, page.address);
        page.width = storedSide;
        page.height = storedSide;
        page.data.resize(static_cast<size_t>(storedSide) * storedSide * texelBytes);
        for (uint32_t storedY = 0u; storedY < storedSide; ++storedY)
          for (uint32_t storedX = 0u; storedX < storedSide; ++storedX)
          {
            const int64_t globalX = static_cast<int64_t>(x) * pageSize + storedX - gutter;
            const int64_t globalY = static_cast<int64_t>(y) * pageSize + storedY - gutter;
            const uint32_t sourceX = static_cast<uint32_t>(std::clamp<int64_t>(globalX, 0, static_cast<int64_t>(rasterSide) - 1));
            const uint32_t sourceY = static_cast<uint32_t>(std::clamp<int64_t>(globalY, 0, static_cast<int64_t>(rasterSide) - 1));
            std::memcpy(page.data.data() + (static_cast<size_t>(storedY) * storedSide + storedX) * texelBytes, raster.data() + (static_cast<size_t>(sourceY) * rasterSide + sourceX) * texelBytes, texelBytes);
          }
        if (!encodePageStorage(page, compress, error))
          return false;
        field.pages.push_back(std::move(page));
      }
  }
  return true;
}

bool buildPagePyramid(MapExportField &field, const uint32_t pageSize, const uint16_t gutter, const uint8_t maximumLevel, const bool compress, std::string *error)
{
  if (field.filter == MapExportFilter::SparseMixture)
  {
    if (error != nullptr)
      *error = "sparse mixture fields must build their page pyramids as a pair";
    return false;
  }
  std::vector<std::vector<uint8_t>> levels(maximumLevel + 1u);
  levels[maximumLevel] = field.data;
  uint32_t levelSide = field.width;
  for (uint8_t level = maximumLevel; level > 0u; --level)
  {
    levels[level - 1u] = downsampleField(levels[level], levelSide, field.format, field.filter);
    levelSide /= 2u;
  }
  return buildPagesFromLevels(field, levels, pageSize, gutter, maximumLevel, compress, error);
}

bool buildBiomeMixturePyramids(
    MapExportField &hardField,
    MapExportField &idField,
    MapExportField &weightField,
    const uint32_t pageSize,
    const uint16_t gutter,
    const uint8_t maximumLevel,
    const bool compress,
    std::string *error)
{
  std::vector<std::vector<uint8_t>> hardLevels(maximumLevel + 1u);
  std::vector<std::vector<uint8_t>> idLevels(maximumLevel + 1u);
  std::vector<std::vector<uint8_t>> weightLevels(maximumLevel + 1u);
  hardLevels[maximumLevel] = hardField.data;
  idLevels[maximumLevel] = idField.data;
  weightLevels[maximumLevel] = weightField.data;
  uint32_t levelSide = hardField.width;
  for (uint8_t level = maximumLevel; level > 0u; --level)
  {
    downsampleBiomeMixture(idLevels[level], weightLevels[level], levelSide, hardLevels[level - 1u], idLevels[level - 1u], weightLevels[level - 1u]);
    levelSide /= 2u;
  }
  return buildPagesFromLevels(hardField, hardLevels, pageSize, gutter, maximumLevel, compress, error) && buildPagesFromLevels(idField, idLevels, pageSize, gutter, maximumLevel, compress, error) &&
         buildPagesFromLevels(weightField, weightLevels, pageSize, gutter, maximumLevel, compress, error);
}

void serializeGraph(const MapGraph &graph, std::vector<uint8_t> &out)
{
  out.clear();
  out.insert(out.end(), {'W', 'M', 'G', '1'});
  appendU32(out, 1u);
  appendU64(out, graph.signature());
  const WorldBrief &brief = graph.brief();
  appendU64(out, brief.seed);
  appendF64(out, brief.extentMetres);
  appendF64(out, brief.seaLevelMetres);
  appendF64(out, brief.reliefMetres);
  appendU32(out, static_cast<uint32_t>(brief.tags.size()));
  for (const Tag tag : brief.tags)
    appendU8(out, static_cast<uint8_t>(tag));
  appendU32(out, static_cast<uint32_t>(graph.nodeCount()));
  for (const MapNode &node : graph.nodes())
  {
    appendU32(out, node.id);
    appendU32(out, node.parent);
    appendU8(out, node.level);
    appendU8(out, static_cast<uint8_t>(node.type));
    appendU8(out, static_cast<uint8_t>(node.content));
    appendU8(out, node.biome);
    appendU16(out, node.craton);
    appendU8(out, static_cast<uint8_t>((node.land ? 1u : 0u) | (node.coastal ? 2u : 0u) | (node.shallow ? 4u : 0u)));
    appendU32(out, node.region);
    appendF64(out, node.intent.elevationMetres);
    appendF64(out, node.intent.upliftRate);
    appendF64(out, node.intent.reliefAmplitude);
    appendF64(out, node.intent.roughness);
    appendPoint(out, node.site);
    appendIds(out, node.children);
    appendIds(out, node.neighbours);
    appendPoints(out, node.area);
    appendU32(out, static_cast<uint32_t>(node.boundary.edges.size()));
    for (const EdgeSample &edge : node.boundary.edges)
    {
      appendPoint(out, edge.position);
      appendF64(out, edge.elevationMetres);
      appendF64(out, edge.slope);
    }
    appendU32(out, static_cast<uint32_t>(node.boundary.rivers.size()));
    for (const RiverCrossing &river : node.boundary.rivers)
    {
      appendPoint(out, river.position);
      appendF64(out, river.dischargeCumecs);
      appendPoint(out, river.direction);
    }
  }
}

void serializeFeatures(const std::vector<CoastPolyline> &coastline, const RiverNetwork &rivers, const ContourSet &contours, const RegionMap &regions, std::vector<uint8_t> &out)
{
  out.clear();
  out.insert(out.end(), {'W', 'M', 'F', '1'});
  appendU32(out, 2u);
  appendF64(out, contours.intervalMetres);
  appendU32(out, static_cast<uint32_t>(coastline.size()));
  for (const CoastPolyline &coast : coastline)
  {
    appendU8(out, coast.closed ? 1u : 0u);
    appendPoints(out, coast.points);
  }
  appendU32(out, rivers.riverCells);
  appendU32(out, rivers.mouths);
  appendU32(out, rivers.filledCells);
  appendU32(out, rivers.maximumOrder);
  appendF64(out, rivers.largestDischargeCumecs);
  appendU32(out, static_cast<uint32_t>(rivers.nodes.size()));
  for (const RiverNode &node : rivers.nodes)
  {
    appendU32(out, node.downstream);
    appendF64(out, node.filledElevationMetres);
    appendF64(out, node.drainageAreaSquareMetres);
    appendF64(out, node.dischargeCumecs);
    appendU32(out, node.strahlerOrder);
    appendU8(out, node.river ? 1u : 0u);
  }
  appendU32(out, static_cast<uint32_t>(rivers.segments.size()));
  for (const RiverSegment &segment : rivers.segments)
  {
    appendPoint(out, segment.from);
    appendPoint(out, segment.to);
    appendF64(out, segment.dischargeCumecs);
    appendU32(out, segment.strahlerOrder);
    appendU32(out, segment.node);
    appendU8(out, segment.mouth ? 1u : 0u);
  }
  appendU32(out, static_cast<uint32_t>(rivers.waterfalls.size()));
  for (const Waterfall &waterfall : rivers.waterfalls)
  {
    appendPoint(out, waterfall.position);
    appendF64(out, waterfall.dropMetres);
    appendF64(out, waterfall.gradient);
    appendF64(out, waterfall.dischargeCumecs);
    appendU32(out, waterfall.node);
  }
  appendU32(out, static_cast<uint32_t>(contours.lines.size()));
  for (const ContourPolyline &line : contours.lines)
  {
    appendF64(out, line.elevationMetres);
    appendU8(out, static_cast<uint8_t>((line.closed ? 1u : 0u) | (line.major ? 2u : 0u)));
    appendPoints(out, line.points);
  }
  appendU32(out, static_cast<uint32_t>(regions.regions.size()));
  for (const MapRegion &region : regions.regions)
  {
    appendU32(out, region.id);
    appendIds(out, region.cells);
    appendU32(out, region.seedCell);
    appendPoint(out, region.centroid);
    appendF64(out, region.areaSquareMetres);
    appendU8(out, static_cast<uint8_t>(region.content));
    appendU8(out, static_cast<uint8_t>(region.biome));
    appendString(out, region.name);
    appendF64(out, region.meanElevationMetres);
    appendF64(out, region.reliefMetres);
    appendF64(out, region.meanUpliftRate);
    appendF64(out, region.coastalFraction);
    appendF64(out, region.riverFraction);
    appendU8(out, region.water ? 1u : 0u);
  }
  appendU32(out, static_cast<uint32_t>(regions.rivers.size()));
  for (const NamedRiver &river : regions.rivers)
  {
    appendString(out, river.name);
    appendIds(out, river.stem);
    appendF64(out, river.dischargeCumecs);
    appendU32(out, river.strahlerOrder);
  }
  appendIds(out, regions.cellRegion);
}

class BinaryReader
{
public:
  explicit BinaryReader(const std::vector<uint8_t> &data) : data_(data)
  {
  }

  bool magic(const char expected[4])
  {
    if (!require(4u, "binary magic is truncated"))
      return false;
    if (std::memcmp(data_.data() + offset_, expected, 4u) != 0)
      return fail("binary magic is invalid");
    offset_ += 4u;
    return true;
  }

  bool u8(uint8_t &value)
  {
    if (!require(1u, "uint8 is truncated"))
      return false;
    value = data_[offset_++];
    return true;
  }

  bool u16(uint16_t &value)
  {
    if (!require(2u, "uint16 is truncated"))
      return false;
    value = static_cast<uint16_t>(data_[offset_]) | static_cast<uint16_t>(data_[offset_ + 1u] << 8u);
    offset_ += 2u;
    return true;
  }

  bool u32(uint32_t &value)
  {
    if (!require(4u, "uint32 is truncated"))
      return false;
    value = 0u;
    for (uint32_t byte = 0u; byte < 4u; ++byte)
      value |= static_cast<uint32_t>(data_[offset_ + byte]) << (byte * 8u);
    offset_ += 4u;
    return true;
  }

  bool u64(uint64_t &value)
  {
    if (!require(8u, "uint64 is truncated"))
      return false;
    value = 0u;
    for (uint32_t byte = 0u; byte < 8u; ++byte)
      value |= static_cast<uint64_t>(data_[offset_ + byte]) << (byte * 8u);
    offset_ += 8u;
    return true;
  }

  bool f64(double &value)
  {
    uint64_t bits = 0u;
    if (!u64(bits))
      return false;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) || fail("binary contains a non-finite scalar");
  }

  bool point(Point2 &value)
  {
    return f64(value.x) && f64(value.y);
  }

  bool ids(std::vector<uint32_t> &values)
  {
    uint32_t count = 0u;
    if (!boundedCount(count, 4u, "id array"))
      return false;
    values.resize(count);
    for (uint32_t &value : values)
      if (!u32(value))
        return false;
    return true;
  }

  bool points(std::vector<Point2> &values)
  {
    uint32_t count = 0u;
    if (!boundedCount(count, 16u, "point array"))
      return false;
    values.resize(count);
    for (Point2 &value : values)
      if (!point(value))
        return false;
    return true;
  }

  bool string(std::string &value)
  {
    uint32_t count = 0u;
    if (!u32(count))
      return false;
    if (count > 16u * 1024u * 1024u || !require(count, "string is truncated"))
      return fail("string length is unreasonable or truncated");
    value.assign(reinterpret_cast<const char *>(data_.data() + offset_), count);
    offset_ += count;
    return true;
  }

  bool count(uint32_t &value, const size_t minimumBytes, const char *label)
  {
    return boundedCount(value, minimumBytes, label);
  }

  bool finished() const
  {
    return error_.empty() && offset_ == data_.size();
  }

  const std::string &error() const
  {
    return error_;
  }

  size_t remaining() const
  {
    return data_.size() - offset_;
  }

private:
  bool require(const size_t size, const char *message)
  {
    if (size <= remaining())
      return true;
    return fail(message);
  }

  bool boundedCount(uint32_t &value, const size_t minimumBytes, const char *label)
  {
    if (!u32(value))
      return false;
    constexpr uint32_t maximumEntries = 100000000u;
    if (value > maximumEntries || (minimumBytes > 0u && value > remaining() / minimumBytes))
      return fail(std::string(label) + " count is unreasonable or truncated");
    return true;
  }

  bool fail(const std::string &message)
  {
    if (error_.empty())
      error_ = message;
    return false;
  }

  const std::vector<uint8_t> &data_;
  size_t offset_ = 0u;
  std::string error_;
};

void updateDistance(std::vector<float> &distance, std::vector<float> *value, const uint32_t side, const uint32_t x, const uint32_t y, const int32_t nx, const int32_t ny, const float cost)
{
  if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(side) || ny >= static_cast<int32_t>(side))
    return;
  const size_t at = static_cast<size_t>(y) * side + x;
  const size_t from = static_cast<size_t>(ny) * side + static_cast<uint32_t>(nx);
  const float candidate = distance[from] + cost;
  if (candidate < distance[at])
  {
    distance[at] = candidate;
    if (value != nullptr)
      (*value)[at] = (*value)[from];
  }
}

void distanceTransform(std::vector<float> &distance, std::vector<float> *value, const uint32_t side)
{
  constexpr float diagonal = 1.41421356237f;
  for (uint32_t y = 0u; y < side; ++y)
    for (uint32_t x = 0u; x < side; ++x)
    {
      updateDistance(distance, value, side, x, y, static_cast<int32_t>(x) - 1, static_cast<int32_t>(y), 1.0f);
      updateDistance(distance, value, side, x, y, static_cast<int32_t>(x), static_cast<int32_t>(y) - 1, 1.0f);
      updateDistance(distance, value, side, x, y, static_cast<int32_t>(x) - 1, static_cast<int32_t>(y) - 1, diagonal);
      updateDistance(distance, value, side, x, y, static_cast<int32_t>(x) + 1, static_cast<int32_t>(y) - 1, diagonal);
    }
  for (uint32_t y = side; y-- > 0u;)
    for (uint32_t x = side; x-- > 0u;)
    {
      updateDistance(distance, value, side, x, y, static_cast<int32_t>(x) + 1, static_cast<int32_t>(y), 1.0f);
      updateDistance(distance, value, side, x, y, static_cast<int32_t>(x), static_cast<int32_t>(y) + 1, 1.0f);
      updateDistance(distance, value, side, x, y, static_cast<int32_t>(x) + 1, static_cast<int32_t>(y) + 1, diagonal);
      updateDistance(distance, value, side, x, y, static_cast<int32_t>(x) - 1, static_cast<int32_t>(y) + 1, diagonal);
    }
}

std::string hex64(const uint64_t value)
{
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}

std::string manifestFor(const MapExportBundle &bundle)
{
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<double>::max_digits10);
  out << "{\n"
      << "  \"schema\": \"engine.world-map\",\n"
      << "  \"schemaVersion\": " << bundle.schemaVersion << ",\n"
      << "  \"seed\": " << bundle.seed << ",\n"
      << "  \"extentMetres\": " << bundle.extentMetres << ",\n"
      << "  \"resolution\": [" << bundle.resolution << ", " << bundle.resolution << "],\n"
      << "  \"fieldEncoding\": \"raw\",\n"
      << "  \"byteOrder\": \"little-endian\",\n"
      << "  \"origin\": \"lower-left\",\n"
      << "  \"texelRegistration\": \"cell-centre\",\n"
      << "  \"pagePyramid\": {\"addressing\": \"planar-quadtree\", "
      << "\"interiorSizePixels\": " << bundle.pageSizePixels << ", \"gutterPixels\": " << bundle.gutterPixels << ", \"maximumLevel\": " << static_cast<uint32_t>(bundle.maximumPageLevel)
      << ", \"ancestorFallback\": true},\n"
      << "  \"graph\": {\"file\": \"graph.bin\", \"format\": \"WMG1\", \"signature\": \"" << hex64(bundle.graphSignature) << "\"},\n"
      << "  \"features\": {\"file\": \"features.bin\", \"format\": \"WMF1\", \"contentHash\": \"" << hex64(hashData(bundle.featureData)) << "\"},\n"
      << "  \"bundleSignature\": \"" << hex64(bundle.bundleSignature) << "\",\n  \"fields\": [\n";
  for (size_t i = 0u; i < bundle.fields.size(); ++i)
  {
    const MapExportField &field = bundle.fields[i];
    out << "    {\"name\": \"" << field.name << "\", \"available\": " << (field.available ? "true" : "false") << ", \"format\": \"" << mapExportFormatName(field.format) << "\", \"filter\": \""
        << mapExportFilterName(field.filter) << "\", \"samplingGroup\": " << (field.samplingGroup.empty() ? "null" : "\"" + field.samplingGroup + "\"") << ", \"size\": [" << field.width << ", " << field.height
        << "], \"components\": [";
    for (size_t component = 0u; component < field.components.size(); ++component)
      out << (component == 0u ? "" : ", ") << "\"" << field.components[component] << "\"";
    out << "], \"units\": [";
    for (size_t component = 0u; component < field.units.size(); ++component)
      out << (component == 0u ? "" : ", ") << "\"" << field.units[component] << "\"";
    out << "], \"scale\": [";
    for (size_t component = 0u; component < 4u; ++component)
      out << (component == 0u ? "" : ", ") << field.scale[component];
    out << "], \"offset\": [";
    for (size_t component = 0u; component < 4u; ++component)
      out << (component == 0u ? "" : ", ") << field.offset[component];
    out << "], \"contentHash\": " << (field.available ? "\"" + hex64(field.contentHash) + "\"" : "null") << ", \"uncompressedBytes\": " << (field.available ? std::to_string(field.data.size()) : "0") << ", \"pages\": [";
    for (size_t pageIndex = 0u; pageIndex < field.pages.size(); ++pageIndex)
    {
      const MapExportPage &page = field.pages[pageIndex];
      out << (pageIndex == 0u ? "" : ", ") << "{\"level\": " << static_cast<uint32_t>(page.address.level) << ", \"x\": " << page.address.x << ", \"y\": " << page.address.y << ", \"file\": \"" << page.file
          << "\", \"compression\": \"" << mapExportCompressionName(page.compression) << "\", \"size\": [" << page.width << ", " << page.height << "], \"contentHash\": \"" << hex64(page.contentHash)
          << "\", \"storedHash\": \"" << hex64(page.storedHash) << "\", \"uncompressedBytes\": " << page.data.size() << ", \"storedBytes\": " << storedSize(page) << "}";
    }
    out << "]}" << (i + 1u == bundle.fields.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
  return out.str();
}

uint64_t signatureFor(const MapExportBundle &bundle)
{
  uint64_t hash = kFnvOffset;
  hashU32(hash, bundle.pageSizePixels);
  hashU32(hash, bundle.gutterPixels);
  hashU8(hash, bundle.maximumPageLevel);
  hashBytes(hash, bundle.graphData.data(), bundle.graphData.size());
  hashBytes(hash, bundle.featureData.data(), bundle.featureData.size());
  for (const MapExportField &field : bundle.fields)
  {
    hashString(hash, field.name);
    hashU8(hash, static_cast<uint8_t>(field.format));
    hashU8(hash, static_cast<uint8_t>(field.filter));
    hashString(hash, field.samplingGroup);
    hashU8(hash, field.available ? 1u : 0u);
    for (const std::string &component : field.components)
      hashString(hash, component);
    for (const std::string &unit : field.units)
      hashString(hash, unit);
    for (const double scale : field.scale)
      hashF64(hash, scale);
    for (const double offset : field.offset)
      hashF64(hash, offset);
    hashBytes(hash, field.data.data(), field.data.size());
    for (const MapExportPage &page : field.pages)
    {
      hashU8(hash, page.address.level);
      hashU32(hash, page.address.x);
      hashU32(hash, page.address.y);
      hashString(hash, page.file);
      hashU8(hash, static_cast<uint8_t>(page.compression));
      hashBytes(hash, page.data.data(), page.data.size());
      if (page.compression != MapExportCompression::None)
        hashBytes(hash, page.storedData.data(), page.storedData.size());
    }
  }
  return hash;
}

bool writeFile(const std::filesystem::path &path, const void *data, const size_t size, std::string *error)
{
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file)
  {
    if (error != nullptr)
      *error = "could not open " + path.string() + " for writing";
    return false;
  }
  file.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
  if (!file)
  {
    if (error != nullptr)
      *error = "could not write " + path.string();
    return false;
  }
  return true;
}

bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &data, std::string *error)
{
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
  {
    if (error != nullptr)
      *error = "could not open " + path.string() + " for reading";
    return false;
  }
  const std::streampos end = file.tellg();
  if (end < 0)
  {
    if (error != nullptr)
      *error = "could not measure " + path.string();
    return false;
  }
  data.resize(static_cast<size_t>(end));
  file.seekg(0, std::ios::beg);
  if (!data.empty())
    file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!file)
  {
    if (error != nullptr)
      *error = "could not read " + path.string();
    return false;
  }
  return true;
}

bool safePagePath(const std::string &path)
{
  return path.rfind("pages/", 0u) == 0u && path.find("..") == std::string::npos && !std::filesystem::path(path).is_absolute();
}

bool parseFormat(const std::string &name, MapExportFormat &format)
{
  static const std::array<MapExportFormat, 7> formats{
    MapExportFormat::R8Uint, MapExportFormat::R16Float, MapExportFormat::R16Unorm, MapExportFormat::R32Uint, MapExportFormat::RG16Float, MapExportFormat::RGBA8Uint, MapExportFormat::RGBA8Unorm};
  for (const MapExportFormat candidate : formats)
    if (name == mapExportFormatName(candidate))
    {
      format = candidate;
      return true;
    }
  return false;
}

bool parseFilter(const std::string &name, MapExportFilter &filter)
{
  if (name == "nearest")
  {
    filter = MapExportFilter::Nearest;
    return true;
  }
  if (name == "linear")
  {
    filter = MapExportFilter::Linear;
    return true;
  }
  if (name == "sparse-mixture")
  {
    filter = MapExportFilter::SparseMixture;
    return true;
  }
  return false;
}

bool parseCompression(const std::string &name, MapExportCompression &compression)
{
  if (name == "none")
  {
    compression = MapExportCompression::None;
    return true;
  }
  if (name == "lz4")
  {
    compression = MapExportCompression::Lz4;
    return true;
  }
  return false;
}

uint64_t parseHex64(const std::string &value)
{
  if (value.size() != 16u)
    throw std::runtime_error("a content hash is not 16 hexadecimal digits");
  size_t consumed = 0u;
  const uint64_t parsed = std::stoull(value, &consumed, 16);
  if (consumed != value.size())
    throw std::runtime_error("a content hash contains non-hexadecimal data");
  return parsed;
}

uint64_t readU64(const std::vector<uint8_t> &data, const size_t offset)
{
  uint64_t value = 0u;
  for (uint32_t byte = 0u; byte < 8u; ++byte)
    value |= static_cast<uint64_t>(data[offset + byte]) << (byte * 8u);
  return value;
}

} // namespace

const char *mapExportFormatName(const MapExportFormat format)
{
  switch (format)
  {
  case MapExportFormat::R8Uint:
    return "R8_UINT";
  case MapExportFormat::R16Float:
    return "R16_FLOAT";
  case MapExportFormat::R16Unorm:
    return "R16_UNORM";
  case MapExportFormat::R32Uint:
    return "R32_UINT";
  case MapExportFormat::RG16Float:
    return "RG16_FLOAT";
  case MapExportFormat::RGBA8Uint:
    return "RGBA8_UINT";
  case MapExportFormat::RGBA8Unorm:
    return "RGBA8_UNORM";
  }
  return "UNKNOWN";
}

const char *mapExportFilterName(const MapExportFilter filter)
{
  switch (filter)
  {
  case MapExportFilter::Nearest:
    return "nearest";
  case MapExportFilter::Linear:
    return "linear";
  case MapExportFilter::SparseMixture:
    return "sparse-mixture";
  }
  return "unknown";
}

const char *mapExportCompressionName(const MapExportCompression compression)
{
  return compression == MapExportCompression::Lz4 ? "lz4" : "none";
}

bool operator==(const MapExportPageAddress &left, const MapExportPageAddress &right)
{
  return left.level == right.level && left.x == right.x && left.y == right.y;
}

bool validMapExportPageAddress(const MapExportPageAddress &address, const uint8_t maximumLevel)
{
  if (maximumLevel > 31u || address.level > maximumLevel)
    return false;
  const uint32_t dimension = uint32_t{1u} << address.level;
  return address.x < dimension && address.y < dimension;
}

MapExportPageAddress mapExportPageParent(const MapExportPageAddress &address)
{
  if (address.level == 0u || address.level > 31u)
    return address;
  return {static_cast<uint8_t>(address.level - 1u), address.x >> 1u, address.y >> 1u};
}

uint64_t mapExportPageMorton(const MapExportPageAddress &address)
{
  if (!validMapExportPageAddress(address, address.level))
    return std::numeric_limits<uint64_t>::max();
  uint64_t morton = 0u;
  for (uint32_t bit = 0u; bit < address.level; ++bit)
  {
    morton |= static_cast<uint64_t>((address.x >> bit) & 1u) << (bit * 2u);
    morton |= static_cast<uint64_t>((address.y >> bit) & 1u) << (bit * 2u + 1u);
  }
  return morton;
}

uint32_t mapExportTexelBytes(const MapExportFormat format)
{
  switch (format)
  {
  case MapExportFormat::R8Uint:
    return 1u;
  case MapExportFormat::R16Float:
  case MapExportFormat::R16Unorm:
    return 2u;
  case MapExportFormat::R32Uint:
  case MapExportFormat::RG16Float:
  case MapExportFormat::RGBA8Uint:
  case MapExportFormat::RGBA8Unorm:
    return 4u;
  }
  return 0u;
}

uint32_t mapExportComponentCount(const MapExportFormat format)
{
  switch (format)
  {
  case MapExportFormat::R8Uint:
  case MapExportFormat::R16Float:
  case MapExportFormat::R16Unorm:
  case MapExportFormat::R32Uint:
    return 1u;
  case MapExportFormat::RG16Float:
    return 2u;
  case MapExportFormat::RGBA8Uint:
  case MapExportFormat::RGBA8Unorm:
    return 4u;
  }
  return 0u;
}

const MapExportPage *MapExportField::page(const MapExportPageAddress &address) const
{
  const auto found = std::find_if(
      pages.begin(),
      pages.end(),
      [&](const MapExportPage &candidate)
      {
        return candidate.address == address;
      });
  return found != pages.end() && found->address == address ? &*found : nullptr;
}

const MapExportPage *MapExportField::bestPage(MapExportPageAddress address) const
{
  if (!validMapExportPageAddress(address, address.level))
    return nullptr;
  for (;;)
  {
    if (const MapExportPage *found = page(address); found != nullptr)
      return found;
    if (address.level == 0u)
      return nullptr;
    address = mapExportPageParent(address);
  }
}

const MapExportField *MapExportBundle::field(const std::string &name) const
{
  const auto found = std::find_if(
      fields.begin(),
      fields.end(),
      [&](const MapExportField &candidate)
      {
        return candidate.name == name;
      });
  return found == fields.end() ? nullptr : &*found;
}

MapExportField *MapExportBundle::field(const std::string &name)
{
  const auto found = std::find_if(
      fields.begin(),
      fields.end(),
      [&](const MapExportField &candidate)
      {
        return candidate.name == name;
      });
  return found == fields.end() ? nullptr : &*found;
}

bool MapExportBundle::validate(std::string *error) const
{
  const auto fail = [&](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (schemaVersion != 4u || resolution < 2u || !(extentMetres > 0.0) || pageSizePixels == 0u || gutterPixels == 0u || gutterPixels > 16u || maximumPageLevel > 8u ||
      static_cast<uint64_t>(pageSizePixels) * (uint64_t{1u} << maximumPageLevel) != resolution)
    return fail("map export bundle header is invalid");
  if (graphData.size() < 16u || featureData.size() < 8u || std::memcmp(graphData.data(), "WMG1", 4u) != 0 || std::memcmp(featureData.data(), "WMF1", 4u) != 0)
    return fail("map export graph or feature payload is invalid");
  std::unordered_set<std::string> names;
  std::unordered_set<std::string> pagePaths;
  for (const MapExportField &entry : fields)
  {
    const bool safeName = !entry.name.empty() && std::all_of(
                                                     entry.name.begin(),
                                                     entry.name.end(),
                                                     [](const char character)
                                                     {
                                                       return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
                                                     });
    if (!safeName || !names.insert(entry.name).second)
      return fail("map export field names must be unique and non-empty");
    if (entry.width != resolution || entry.height != resolution)
      return fail("map export fields are not co-registered");
    if (static_cast<uint8_t>(entry.format) > static_cast<uint8_t>(MapExportFormat::RGBA8Unorm) || static_cast<uint8_t>(entry.filter) > static_cast<uint8_t>(MapExportFilter::SparseMixture))
      return fail("map export field uses an invalid format or filter");
    if (entry.components.size() != mapExportComponentCount(entry.format) || entry.units.size() != entry.components.size())
      return fail("map export field component metadata is incomplete");
    if ((entry.filter == MapExportFilter::SparseMixture) != !entry.samplingGroup.empty())
      return fail("sparse mixture fields must declare exactly one sampling group");
    const size_t wanted = static_cast<size_t>(resolution) * resolution * mapExportTexelBytes(entry.format);
    if (entry.available && entry.data.size() != wanted)
      return fail("available map export field has the wrong payload size");
    if (!entry.available && (!entry.data.empty() || !entry.pages.empty() || entry.contentHash != 0u))
      return fail("unavailable map export field carries a payload");
    if (entry.available && hashData(entry.data) != entry.contentHash)
      return fail("map export field content hash does not match");
    if (entry.available)
    {
      uint64_t expectedPageCount = 0u;
      for (uint8_t level = 0u; level <= maximumPageLevel; ++level)
        expectedPageCount += uint64_t{1u} << (level * 2u);
      if (entry.pages.size() != expectedPageCount)
        return fail("map export field page pyramid is incomplete");
      std::unordered_set<uint64_t> pageKeys;
      const uint32_t storedSide = pageSizePixels + gutterPixels * 2u;
      const size_t pageBytes = static_cast<size_t>(storedSide) * storedSide * mapExportTexelBytes(entry.format);
      for (const MapExportPage &page : entry.pages)
      {
        const uint64_t pageKey = (static_cast<uint64_t>(page.address.level) << 56u) | mapExportPageMorton(page.address);
        if (!validMapExportPageAddress(page.address, maximumPageLevel) || !pageKeys.insert(pageKey).second || !pagePaths.insert(page.file).second || !safePagePath(page.file) || page.width != storedSide ||
            page.height != storedSide || page.data.size() != pageBytes || hashData(page.data) != page.contentHash)
          return fail("map export page metadata or content is invalid");
        switch (page.compression)
        {
        case MapExportCompression::None:
          if (!page.storedData.empty() || page.storedHash != page.contentHash)
            return fail("raw map export page has invalid stored metadata");
          break;
        case MapExportCompression::Lz4:
          if (page.storedData.empty() || page.storedData.size() >= page.data.size() || hashData(page.storedData) != page.storedHash)
            return fail("compressed map export page has invalid stored data");
          break;
        default:
          return fail("map export page uses unsupported compression");
        }
      }
      if (entry.page({0u, 0u, 0u}) == nullptr)
        return fail("map export field has no level-zero fallback page");
      const uint32_t texelBytes = mapExportTexelBytes(entry.format);
      for (const MapExportPage &page : entry.pages)
        if (page.address.level == maximumPageLevel)
          for (uint32_t y = 0u; y < pageSizePixels; ++y)
          {
            const size_t pageOffset = (static_cast<size_t>(y + gutterPixels) * storedSide + gutterPixels) * texelBytes;
            const size_t fieldOffset = (static_cast<size_t>(page.address.y * pageSizePixels + y) * resolution + page.address.x * pageSizePixels) * texelBytes;
            if (std::memcmp(page.data.data() + pageOffset, entry.data.data() + fieldOffset, static_cast<size_t>(pageSizePixels) * texelBytes) != 0)
              return fail("map export finest page does not reconstruct its field");
          }
    }
    for (size_t component = 0u; component < 4u; ++component)
      if (!std::isfinite(entry.scale[component]) || !std::isfinite(entry.offset[component]))
        return fail("map export field scale or offset is not finite");
  }
  static const std::array<const char *, 31> required{
    "base_elevation",  "uplift",         "relief_roughness",    "climate",           "region_id",         "biome_id",           "biome_mix_ids", "biome_mix_weights", "landform_id", "water_type", "water_level",
    "river",           "coast_distance", "placement_potential", "refined_elevation", "erosion_wear_flow", "erosion_deposition", "substrate_id",  "soil_depth",        "soil_layers", "roads",      "settlement_activity",
    "object_exclusion", "rock_potential", "rock_type", "exposed_bedrock", "vegetation_potential", "vegetation_type", "canopy_density", "ground_density", "riparian_mask"};
  for (const char *name : required)
    if (field(name) == nullptr)
      return fail(std::string("map export is missing required field ") + name);

  const MapExportField *hardBiome = field("biome_id");
  const MapExportField *biomeIds = field("biome_mix_ids");
  const MapExportField *biomeWeights = field("biome_mix_weights");
  if (hardBiome == nullptr || biomeIds == nullptr || biomeWeights == nullptr || hardBiome->format != MapExportFormat::R8Uint || hardBiome->filter != MapExportFilter::Nearest ||
      biomeIds->format != MapExportFormat::RGBA8Uint || biomeWeights->format != MapExportFormat::RGBA8Unorm || biomeIds->filter != MapExportFilter::SparseMixture ||
      biomeWeights->filter != MapExportFilter::SparseMixture || biomeIds->samplingGroup != "biome_mixture" || biomeWeights->samplingGroup != "biome_mixture")
    return fail("biome mixture fields do not declare the schema contract");
  const auto validMixture = [](const uint8_t hard, const uint8_t *ids, const uint8_t *weights)
  {
    uint32_t total = 0u;
    for (size_t slot = 0u; slot < kBiomeMixtureSize; ++slot)
    {
      total += weights[slot];
      if (weights[slot] > 0u && ids[slot] >= static_cast<uint8_t>(Biome::Count))
        return false;
      if (slot > 0u && weights[slot] > weights[slot - 1u])
        return false;
      for (size_t previous = 0u; previous < slot; ++previous)
        if (weights[slot] > 0u && weights[previous] > 0u && ids[slot] == ids[previous])
          return false;
    }
    return total == 255u && hard == ids[0] && weights[0] > 0u;
  };
  const size_t biomePixels = static_cast<size_t>(resolution) * resolution;
  for (size_t pixel = 0u; pixel < biomePixels; ++pixel)
    if (!validMixture(hardBiome->data[pixel], biomeIds->data.data() + pixel * kBiomeMixtureSize, biomeWeights->data.data() + pixel * kBiomeMixtureSize))
      return fail("full-resolution biome mixture is invalid");
  for (const MapExportPage &hardPage : hardBiome->pages)
  {
    const MapExportPage *idPage = biomeIds->page(hardPage.address);
    const MapExportPage *weightPage = biomeWeights->page(hardPage.address);
    if (idPage == nullptr || weightPage == nullptr)
      return fail("biome mixture page pyramids are not co-addressed");
    const size_t pagePixels = static_cast<size_t>(hardPage.width) * hardPage.height;
    for (size_t pixel = 0u; pixel < pagePixels; ++pixel)
      if (!validMixture(hardPage.data[pixel], idPage->data.data() + pixel * kBiomeMixtureSize, weightPage->data.data() + pixel * kBiomeMixtureSize))
        return fail("paged biome mixture is invalid");
  }
  if (readU64(graphData, 8u) != graphSignature)
    return fail("map export graph signature metadata does not match graph.bin");
  if (signatureFor(*this) != bundleSignature)
    return fail("map export bundle signature does not match");
  if (manifestJson.empty())
    return fail("map export manifest is empty");
  return true;
}

bool buildMapExportBundle(
    const MapGraph &graph,
    const SiteIndex &index,
    const MapSurfaceField &surface,
    const ClimateMap &climate,
    const RiverNetwork &rivers,
    const std::vector<CoastPolyline> &coastline,
    const ContourSet &contours,
    const RegionMap &regions,
    const BlendSettings &blend,
    const HeightBlendSettings &heightBlend,
    const MapExportSettings &settings,
    MapExportBundle &bundle,
    std::string *error)
{
  bundle = MapExportBundle{};
  const auto fail = [&](const std::string &message)
  {
    bundle = MapExportBundle{};
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (graph.nodeCount() == 0u || index.empty() || surface.empty())
    return fail("map export requires a graph, site index and shared surface");
  if (settings.resolution < 2u || settings.resolution > 4096u)
    return fail("map export resolution must be between 2 and 4096");
  if (settings.pageSizePixels == 0u || settings.pageSizePixels > settings.resolution || settings.gutterPixels == 0u || settings.gutterPixels > 16u || settings.resolution % settings.pageSizePixels != 0u)
    return fail("map export page size or gutter is invalid");
  const uint32_t pageRatio = settings.resolution / settings.pageSizePixels;
  if ((pageRatio & (pageRatio - 1u)) != 0u)
    return fail("map export resolution/page size must be a power of two");
  if (climate.cells.size() != graph.nodeCount())
    return fail("map export climate does not cover the graph");
  if (!(graph.brief().extentMetres > 0.0) || std::abs(surface.extentMetres() - graph.brief().extentMetres) > 1.0e-6)
    return fail("map export inputs do not share an extent");

  bundle.resolution = settings.resolution;
  bundle.pageSizePixels = settings.pageSizePixels;
  bundle.gutterPixels = settings.gutterPixels;
  uint32_t remainingPageRatio = pageRatio;
  while (remainingPageRatio > 1u)
  {
    ++bundle.maximumPageLevel;
    remainingPageRatio >>= 1u;
  }
  bundle.extentMetres = graph.brief().extentMetres;
  bundle.seed = graph.brief().seed;
  bundle.graphSignature = graph.signature();
  serializeGraph(graph, bundle.graphData);
  serializeFeatures(coastline, rivers, contours, regions, bundle.featureData);

  double minimumElevation = std::numeric_limits<double>::infinity();
  double maximumElevation = -std::numeric_limits<double>::infinity();
  for (uint32_t y = 0u; y < surface.gridSide(); ++y)
    for (uint32_t x = 0u; x < surface.gridSide(); ++x)
    {
      minimumElevation = std::min(minimumElevation, surface.at(x, y).elevationMetres);
      maximumElevation = std::max(maximumElevation, surface.at(x, y).elevationMetres);
    }
  const double elevationScale = std::max(1.0e-9, maximumElevation - minimumElevation);

  bundle.fields.push_back(makeField("base_elevation", MapExportFormat::R16Unorm, MapExportFilter::Linear, settings.resolution, true));
  bundle.fields.back().scale[0] = elevationScale;
  bundle.fields.back().offset[0] = minimumElevation;
  describeField(bundle.fields.back(), {"elevation"}, {"metres"});
  bundle.fields.push_back(makeField("uplift", MapExportFormat::R16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"uplift"}, {"metres_per_year"});
  bundle.fields.push_back(makeField("relief_roughness", MapExportFormat::RG16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"relief", "roughness"}, {"metres", "normalized"});
  bundle.fields.push_back(makeField("climate", MapExportFormat::RG16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"temperature", "moisture"}, {"celsius", "normalized"});
  bundle.fields.push_back(makeField("region_id", MapExportFormat::R32Uint, MapExportFilter::Nearest, settings.resolution, true));
  describeField(bundle.fields.back(), {"region_id"}, {"index"});
  bundle.fields.push_back(makeField("biome_id", MapExportFormat::R8Uint, MapExportFilter::Nearest, settings.resolution, true));
  describeField(bundle.fields.back(), {"biome_id"}, {"enum"});
  bundle.fields.push_back(makeField("biome_mix_ids", MapExportFormat::RGBA8Uint, MapExportFilter::SparseMixture, settings.resolution, true));
  bundle.fields.back().samplingGroup = "biome_mixture";
  describeField(bundle.fields.back(), {"biome_id_0", "biome_id_1", "biome_id_2", "biome_id_3"}, {"enum", "enum", "enum", "enum"});
  bundle.fields.push_back(makeField("biome_mix_weights", MapExportFormat::RGBA8Unorm, MapExportFilter::SparseMixture, settings.resolution, true));
  bundle.fields.back().samplingGroup = "biome_mixture";
  describeField(bundle.fields.back(), {"weight_0", "weight_1", "weight_2", "weight_3"}, {"normalized", "normalized", "normalized", "normalized"});
  bundle.fields.push_back(makeField("landform_id", MapExportFormat::R8Uint, MapExportFilter::Nearest, settings.resolution, true));
  describeField(bundle.fields.back(), {"landform_id"}, {"enum"});
  bundle.fields.push_back(makeField("water_type", MapExportFormat::R8Uint, MapExportFilter::Nearest, settings.resolution, true));
  describeField(bundle.fields.back(), {"water_type"}, {"enum_0_land_1_ocean_2_lake"});
  bundle.fields.push_back(makeField("water_level", MapExportFormat::R16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"water_level"}, {"metres"});
  bundle.fields.push_back(makeField("river", MapExportFormat::RG16Float, MapExportFilter::Linear, settings.resolution, true));
  bundle.fields.back().scale[0] = bundle.extentMetres;
  bundle.fields.back().scale[1] = std::max(1.0, rivers.largestDischargeCumecs);
  describeField(bundle.fields.back(), {"distance", "discharge"}, {"metres", "cubic_metres_per_second"});
  bundle.fields.push_back(makeField("coast_distance", MapExportFormat::R16Float, MapExportFilter::Linear, settings.resolution, true));
  bundle.fields.back().scale[0] = bundle.extentMetres;
  describeField(bundle.fields.back(), {"signed_distance"}, {"metres_negative_in_water"});
  bundle.fields.push_back(makeField("placement_potential", MapExportFormat::RGBA8Unorm, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"trees", "shrubs", "groundcover", "exposed_rock"}, {"normalized", "normalized", "normalized", "normalized"});
  bundle.fields.push_back(makeField("refined_elevation", MapExportFormat::R16Unorm, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"elevation"}, {"metres"});
  bundle.fields.push_back(makeField("erosion_wear_flow", MapExportFormat::RG16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"wear", "flow"}, {"normalized", "normalized"});
  bundle.fields.push_back(makeField("erosion_deposition", MapExportFormat::R16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"deposition"}, {"normalized"});
  bundle.fields.push_back(makeField("substrate_id", MapExportFormat::R8Uint, MapExportFilter::Nearest, settings.resolution, true));
  describeField(bundle.fields.back(), {"substrate_id"}, {"enum"});
  bundle.fields.push_back(makeField("soil_depth", MapExportFormat::R16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"soil_depth"}, {"metres"});
  bundle.fields.push_back(makeField("soil_layers", MapExportFormat::RGBA8Unorm, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"bedrock", "regolith", "alluvium", "organic"}, {"normalized", "normalized", "normalized", "normalized"});
  bundle.fields.push_back(makeField("rock_potential", MapExportFormat::RGBA8Unorm, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"exposed_bedrock", "boulders", "scree", "cliff"}, {"normalized", "normalized", "normalized", "normalized"});
  bundle.fields.push_back(makeField("rock_type", MapExportFormat::R8Uint, MapExportFilter::Nearest, settings.resolution, false));
  describeField(bundle.fields.back(), {"rock_type"}, {"enum"});
  bundle.fields.push_back(makeField("exposed_bedrock", MapExportFormat::R16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"exposed_bedrock"}, {"normalized"});
  bundle.fields.push_back(makeField("vegetation_potential", MapExportFormat::RGBA8Unorm, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"trees", "shrubs", "groundcover", "riparian"}, {"normalized", "normalized", "normalized", "normalized"});
  bundle.fields.push_back(makeField("vegetation_type", MapExportFormat::R8Uint, MapExportFilter::Nearest, settings.resolution, false));
  describeField(bundle.fields.back(), {"vegetation_type"}, {"enum"});
  bundle.fields.push_back(makeField("canopy_density", MapExportFormat::R16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"canopy_density"}, {"normalized"});
  bundle.fields.push_back(makeField("ground_density", MapExportFormat::R16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"ground_density"}, {"normalized"});
  bundle.fields.push_back(makeField("riparian_mask", MapExportFormat::R16Float, MapExportFilter::Linear, settings.resolution, true));
  describeField(bundle.fields.back(), {"riparian"}, {"normalized"});
  bundle.fields.push_back(makeField("roads", MapExportFormat::R8Uint, MapExportFilter::Nearest, settings.resolution, false));
  describeField(bundle.fields.back(), {"road_type"}, {"enum"});
  bundle.fields.push_back(makeField("settlement_activity", MapExportFormat::R8Uint, MapExportFilter::Nearest, settings.resolution, false));
  describeField(bundle.fields.back(), {"activity"}, {"normalized"});
  bundle.fields.push_back(makeField("object_exclusion", MapExportFormat::R8Uint, MapExportFilter::Nearest, settings.resolution, false));
  describeField(bundle.fields.back(), {"excluded"}, {"boolean"});

  MapExportField &elevationField = *bundle.field("base_elevation");
  MapExportField &upliftField = *bundle.field("uplift");
  MapExportField &reliefField = *bundle.field("relief_roughness");
  MapExportField &climateField = *bundle.field("climate");
  MapExportField &regionField = *bundle.field("region_id");
  MapExportField &biomeField = *bundle.field("biome_id");
  MapExportField &biomeMixIdsField = *bundle.field("biome_mix_ids");
  MapExportField &biomeMixWeightsField = *bundle.field("biome_mix_weights");
  MapExportField &landformField = *bundle.field("landform_id");
  MapExportField &waterTypeField = *bundle.field("water_type");
  MapExportField &waterLevelField = *bundle.field("water_level");
  MapExportField &riverField = *bundle.field("river");
  MapExportField &coastField = *bundle.field("coast_distance");
  MapExportField &placementField = *bundle.field("placement_potential");
  MapExportField &refinedElevationField = *bundle.field("refined_elevation");
  MapExportField &erosionField = *bundle.field("erosion_wear_flow");
  MapExportField &depositionField = *bundle.field("erosion_deposition");
  MapExportField &substrateField = *bundle.field("substrate_id");
  MapExportField &soilDepthField = *bundle.field("soil_depth");
  MapExportField &soilLayersField = *bundle.field("soil_layers");
  MapExportField &rockPotentialField = *bundle.field("rock_potential");
  MapExportField &exposedBedrockField = *bundle.field("exposed_bedrock");
  MapExportField &vegetationPotentialField = *bundle.field("vegetation_potential");
  MapExportField &canopyDensityField = *bundle.field("canopy_density");
  MapExportField &groundDensityField = *bundle.field("ground_density");
  MapExportField &riparianMaskField = *bundle.field("riparian_mask");

  std::vector<BlendWeight> weights;
  const uint32_t side = settings.resolution;
  for (uint32_t y = 0u; y < side; ++y)
    for (uint32_t x = 0u; x < side; ++x)
    {
      const Point2 position((static_cast<double>(x) + 0.5) / side * bundle.extentMetres, (static_cast<double>(y) + 0.5) / side * bundle.extentMetres);
      const MapSurfaceSample scalar = surface.sample(position);
      const double normalizedElevation = (scalar.elevationMetres - minimumElevation) / elevationScale;
      appendU16(elevationField.data, static_cast<uint16_t>(std::lround(std::clamp(normalizedElevation, 0.0, 1.0) * 65535.0)));
      appendHalf(upliftField.data, scalar.upliftRateMetresPerYear);
      appendHalf(reliefField.data, scalar.reliefAmplitudeMetres);
      appendHalf(reliefField.data, scalar.roughness);

      blendAt(graph, index, position, blend, weights);
      if (weights.empty())
        return fail("map export could not sample the graph");
      const MapNode &nearest = graph.node(weights.front().node);
      double temperature = 0.0;
      double moisture = 0.0;
      for (const BlendWeight &weight : weights)
      {
        const ClimateCell &cell = climate.cells[weight.node];
        temperature += weight.weight * cell.temperatureCelsius;
        moisture += weight.weight * cell.moisture;
      }
      appendHalf(climateField.data, temperature);
      appendHalf(climateField.data, std::clamp(moisture, 0.0, 1.0));

      std::vector<BlendWeight> biomeWeights = weights;
      bool land = scalar.elevationMetres >= graph.brief().seaLevelMetres;
      if (nearest.type == CellType::Lake)
        land = false;
      restrictBlendToSurface(graph, land, biomeWeights);
      applyCellInfluenceWeights(graph, heightBlend.cellWeightStrength, biomeWeights);
      shapeBlendByHeight(graph, scalar.elevationMetres, localHeightVariation(surface, position, scalar.elevationMetres), graph.brief().reliefMetres, heightBlend, biomeWeights);
      std::array<uint8_t, kBiomeMixtureSize> biomeIds{};
      std::array<uint8_t, kBiomeMixtureSize> biomeMixWeights{};
      biomeMixtureFor(graph, biomeWeights, biomeIds.data(), biomeMixWeights.data());

      PlacementPotential potential{};
      for (size_t slot = 0u; slot < kBiomeMixtureSize; ++slot)
      {
        const double weight = biomeMixWeights[slot] / 255.0;
        const PlacementPotential candidate = potentialFor(static_cast<Biome>(biomeIds[slot]));
        potential.trees += weight * candidate.trees;
        potential.shrubs += weight * candidate.shrubs;
        potential.groundcover += weight * candidate.groundcover;
        potential.exposedRock += weight * candidate.exposedRock;
      }
      const MapNode &classified = graph.node(biomeWeights.front().node);
      appendU32(regionField.data, classified.region);
      appendU8(biomeField.data, biomeIds[0]);
      biomeMixIdsField.data.insert(biomeMixIdsField.data.end(), biomeIds.begin(), biomeIds.end());
      biomeMixWeightsField.data.insert(biomeMixWeightsField.data.end(), biomeMixWeights.begin(), biomeMixWeights.end());
      appendU8(landformField.data, static_cast<uint8_t>(classified.type));
      uint8_t waterType = 0u;
      if (!land)
        waterType = classified.type == CellType::Lake ? 2u : 1u;
      appendU8(waterTypeField.data, waterType);
      const double waterLevel = classified.type == CellType::Lake ? classified.intent.elevationMetres : (!land ? graph.brief().seaLevelMetres : 0.0);
      appendHalf(waterLevelField.data, waterLevel);
      appendU8(placementField.data, unorm8(potential.trees));
      appendU8(placementField.data, unorm8(potential.shrubs));
      appendU8(placementField.data, unorm8(potential.groundcover));
      appendU8(placementField.data, unorm8(potential.exposedRock));
    }

  const size_t pixelCount = static_cast<size_t>(side) * side;
  const float infinity = std::numeric_limits<float>::infinity();
  std::vector<float> coastDistance(pixelCount, infinity);
  for (uint32_t y = 0u; y < side; ++y)
    for (uint32_t x = 0u; x < side; ++x)
    {
      const size_t at = static_cast<size_t>(y) * side + x;
      const bool water = waterTypeField.data[at] != 0u;
      const auto differs = [&](const int32_t nx, const int32_t ny)
      {
        if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(side) || ny >= static_cast<int32_t>(side))
          return false;
        return (waterTypeField.data[static_cast<size_t>(ny) * side + static_cast<uint32_t>(nx)] != 0u) != water;
      };
      if (differs(static_cast<int32_t>(x) - 1, static_cast<int32_t>(y)) || differs(static_cast<int32_t>(x) + 1, static_cast<int32_t>(y)) || differs(static_cast<int32_t>(x), static_cast<int32_t>(y) - 1) ||
          differs(static_cast<int32_t>(x), static_cast<int32_t>(y) + 1))
        coastDistance[at] = 0.0f;
    }
  distanceTransform(coastDistance, nullptr, side);
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    double normalized = std::isfinite(coastDistance[pixel]) ? static_cast<double>(coastDistance[pixel]) / side : 1.0;
    if (waterTypeField.data[pixel] != 0u)
      normalized = -normalized;
    appendHalf(coastField.data, normalized);
  }

  std::vector<float> riverDistance(pixelCount, infinity);
  std::vector<float> riverDischarge(pixelCount, 0.0f);
  const double largestDischarge = std::max(1.0, rivers.largestDischargeCumecs);
  for (const RiverSegment &segment : rivers.segments)
  {
    const double ax = segment.from.x / bundle.extentMetres * side - 0.5;
    const double ay = segment.from.y / bundle.extentMetres * side - 0.5;
    const double bx = segment.to.x / bundle.extentMetres * side - 0.5;
    const double by = segment.to.y / bundle.extentMetres * side - 0.5;
    const uint32_t steps = std::max(1u, static_cast<uint32_t>(std::ceil(std::max(std::abs(bx - ax), std::abs(by - ay)) * 2.0)));
    for (uint32_t step = 0u; step <= steps; ++step)
    {
      const double t = static_cast<double>(step) / steps;
      const int32_t x = static_cast<int32_t>(std::lround(ax + (bx - ax) * t));
      const int32_t y = static_cast<int32_t>(std::lround(ay + (by - ay) * t));
      if (x < 0 || y < 0 || x >= static_cast<int32_t>(side) || y >= static_cast<int32_t>(side))
        continue;
      const size_t at = static_cast<size_t>(y) * side + static_cast<uint32_t>(x);
      riverDistance[at] = 0.0f;
      riverDischarge[at] = std::max(riverDischarge[at], static_cast<float>(segment.dischargeCumecs / largestDischarge));
    }
  }
  distanceTransform(riverDistance, &riverDischarge, side);
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    const double normalizedDistance = std::isfinite(riverDistance[pixel]) ? static_cast<double>(riverDistance[pixel]) / side : 1.0;
    appendHalf(riverField.data, normalizedDistance);
    appendHalf(riverField.data, riverDischarge[pixel]);
  }

  TerrainRefinementInput refinementInput{};
  refinementInput.side = side;
  refinementInput.extentMetres = bundle.extentMetres;
  refinementInput.seaLevelMetres = graph.brief().seaLevelMetres;
  refinementInput.elevationMetres.resize(pixelCount);
  refinementInput.moisture.resize(pixelCount);
  refinementInput.biome = biomeField.data;
  refinementInput.waterType = waterTypeField.data;
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    refinementInput.elevationMetres[pixel] = static_cast<double>(loadU16(elevationField.data.data() + pixel * 2u)) / 65535.0 * elevationField.scale[0] + elevationField.offset[0];
    refinementInput.moisture[pixel] = halfToFloat(loadU16(climateField.data.data() + pixel * 4u + 2u));
  }
  TerrainRefinementMaps refinement;
  if (!refineTerrainRaster(refinementInput, TerrainRefinementSettings{}, refinement, nullptr, error))
    return fail(error != nullptr && !error->empty() ? *error : "terrain refinement failed");
  double minimumRefined = std::numeric_limits<double>::infinity();
  double maximumRefined = -std::numeric_limits<double>::infinity();
  for (const double elevation : refinement.elevationMetres)
  {
    minimumRefined = std::min(minimumRefined, elevation);
    maximumRefined = std::max(maximumRefined, elevation);
  }
  refinedElevationField.scale[0] = std::max(1.0e-9, maximumRefined - minimumRefined);
  refinedElevationField.offset[0] = minimumRefined;
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    appendU16(refinedElevationField.data, static_cast<uint16_t>(std::lround(std::clamp((refinement.elevationMetres[pixel] - minimumRefined) / refinedElevationField.scale[0], 0.0, 1.0) * 65535.0)));
    appendHalf(erosionField.data, refinement.wear[pixel]);
    appendHalf(erosionField.data, refinement.flow[pixel]);
    appendHalf(depositionField.data, refinement.deposition[pixel]);
    appendU8(substrateField.data, refinement.substrate[pixel]);
    appendHalf(soilDepthField.data, refinement.soilDepthMetres[pixel]);
    for (const uint8_t layer : refinement.soilLayers[pixel])
      appendU8(soilLayersField.data, layer);

    const double depthResponse = std::clamp(static_cast<double>(refinement.soilDepthMetres[pixel]) / 2.0, 0.10, 1.0);
    placementField.data[pixel * 4u] = static_cast<uint8_t>(std::lround(placementField.data[pixel * 4u] * depthResponse));
    placementField.data[pixel * 4u + 2u] = static_cast<uint8_t>(std::lround(placementField.data[pixel * 4u + 2u] * std::clamp(depthResponse + refinement.soilLayers[pixel][3] / 255.0, 0.0, 1.0)));
    placementField.data[pixel * 4u + 3u] = std::max(placementField.data[pixel * 4u + 3u], refinement.soilLayers[pixel][0]);

    const bool water = waterTypeField.data[pixel] != 0u;
    const auto substrate = static_cast<GroundSubstrate>(refinement.substrate[pixel]);
    const double bedrockLayer = refinement.soilLayers[pixel][0] / 255.0;
    const double organicLayer = refinement.soilLayers[pixel][3] / 255.0;
    const double soilSparse = 1.0 - std::clamp(static_cast<double>(refinement.soilDepthMetres[pixel]) / 1.5, 0.0, 1.0);
    const double wear = std::clamp(static_cast<double>(refinement.wear[pixel]), 0.0, 1.0);
    const double exposedBedrock = water ? 0.0 : std::clamp(std::max(placementField.data[pixel * 4u + 3u] / 255.0, bedrockLayer * 0.85), 0.0, 1.0);
    const double boulders = water ? 0.0 : std::clamp(exposedBedrock * 0.45 + (substrate == GroundSubstrate::Bedrock ? 0.25 : 0.0) + wear * 0.25, 0.0, 1.0);
    const double scree = water ? 0.0 : std::clamp((substrate == GroundSubstrate::Scree ? 0.75 : 0.0) + exposedBedrock * 0.25 + wear * 0.20, 0.0, 1.0);
    const double cliff = water ? 0.0 : std::clamp(bedrockLayer * 0.60 + wear * 0.35 + soilSparse * 0.20, 0.0, 1.0);
    appendU8(rockPotentialField.data, unorm8(exposedBedrock));
    appendU8(rockPotentialField.data, unorm8(boulders));
    appendU8(rockPotentialField.data, unorm8(scree));
    appendU8(rockPotentialField.data, unorm8(cliff));
    appendHalf(exposedBedrockField.data, exposedBedrock);

    const double riverNormalizedDistance = std::isfinite(riverDistance[pixel]) ? static_cast<double>(riverDistance[pixel]) / side : 1.0;
    const double riparian = water ? 0.0 : std::clamp(1.0 - riverNormalizedDistance / 0.10, 0.0, 1.0) * std::clamp(0.35 + refinementInput.moisture[pixel], 0.0, 1.0);
    const double treePotential = water ? 0.0 : placementField.data[pixel * 4u] / 255.0;
    const double shrubPotential = water ? 0.0 : placementField.data[pixel * 4u + 1u] / 255.0;
    const double groundcoverPotential = water ? 0.0 : placementField.data[pixel * 4u + 2u] / 255.0;
    appendU8(vegetationPotentialField.data, unorm8(treePotential));
    appendU8(vegetationPotentialField.data, unorm8(shrubPotential));
    appendU8(vegetationPotentialField.data, unorm8(groundcoverPotential));
    appendU8(vegetationPotentialField.data, unorm8(riparian));
    appendHalf(canopyDensityField.data, std::clamp(treePotential * 0.85 + shrubPotential * 0.25, 0.0, 1.0));
    appendHalf(groundDensityField.data, std::clamp(groundcoverPotential + shrubPotential * 0.15 + organicLayer * 0.25 + riparian * 0.15, 0.0, 1.0));
    appendHalf(riparianMaskField.data, riparian);
  }

  for (MapExportField &field : bundle.fields)
    if (field.available)
      field.contentHash = hashData(field.data);

  if (!buildBiomeMixturePyramids(biomeField, biomeMixIdsField, biomeMixWeightsField, bundle.pageSizePixels, bundle.gutterPixels, bundle.maximumPageLevel, settings.compressFields, error))
    return fail(error != nullptr && !error->empty() ? *error : "could not build biome mixture page pyramids");
  for (MapExportField &field : bundle.fields)
    if (field.available && &field != &biomeField && &field != &biomeMixIdsField && &field != &biomeMixWeightsField)
      if (!buildPagePyramid(field, bundle.pageSizePixels, bundle.gutterPixels, bundle.maximumPageLevel, settings.compressFields, error))
        return fail(error != nullptr && !error->empty() ? *error : "could not build map export page pyramid");
  bundle.bundleSignature = signatureFor(bundle);
  bundle.manifestJson = manifestFor(bundle);
  std::string validationError;
  if (!bundle.validate(&validationError))
    return fail("built map export did not validate: " + validationError);
  return true;
}

bool writeMapExportBundle(const MapExportBundle &bundle, const std::string &directory, std::string *error)
{
  std::string validationError;
  if (!bundle.validate(&validationError))
  {
    if (error != nullptr)
      *error = "cannot write invalid map export: " + validationError;
    return false;
  }
  if (directory.empty())
  {
    if (error != nullptr)
      *error = "map export directory is empty";
    return false;
  }
  const std::filesystem::path root(directory);
  std::error_code filesystemError;
  std::filesystem::create_directories(root / "pages", filesystemError);
  if (filesystemError)
  {
    if (error != nullptr)
      *error = "could not create map export directory: " + filesystemError.message();
    return false;
  }
  if (!writeFile(root / "graph.bin", bundle.graphData.data(), bundle.graphData.size(), error) || !writeFile(root / "features.bin", bundle.featureData.data(), bundle.featureData.size(), error))
    return false;
  for (const MapExportField &field : bundle.fields)
    for (const MapExportPage &page : field.pages)
    {
      std::filesystem::create_directories((root / page.file).parent_path(), filesystemError);
      if (filesystemError)
      {
        if (error != nullptr)
          *error = "could not create map export page directory: " + filesystemError.message();
        return false;
      }
      if (!writeFile(root / page.file, page.compression == MapExportCompression::None ? static_cast<const void *>(page.data.data()) : static_cast<const void *>(page.storedData.data()), storedSize(page), error))
        return false;
    }
  return writeFile(root / "manifest.json", bundle.manifestJson.data(), bundle.manifestJson.size(), error);
}

bool readMapExportManifest(const std::string &directory, MapExportBundle &bundle, std::string *error)
{
  bundle = MapExportBundle{};
  const auto fail = [&](const std::string &message)
  {
    bundle = MapExportBundle{};
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (directory.empty())
    return fail("map export directory is empty");
  std::vector<uint8_t> manifestBytes;
  std::string ioError;
  if (!readFile(std::filesystem::path(directory) / "manifest.json", manifestBytes, &ioError))
    return fail(ioError);
  bundle.manifestJson.assign(manifestBytes.begin(), manifestBytes.end());

  try
  {
    const nlohmann::json manifest = nlohmann::json::parse(bundle.manifestJson);
    if (manifest.at("schema").get<std::string>() != "engine.world-map" || manifest.at("fieldEncoding").get<std::string>() != "raw" || manifest.at("byteOrder").get<std::string>() != "little-endian" ||
        manifest.at("origin").get<std::string>() != "lower-left" || manifest.at("texelRegistration").get<std::string>() != "cell-centre")
      return fail("map export manifest uses an unsupported registration");
    bundle.schemaVersion = manifest.at("schemaVersion").get<uint32_t>();
    bundle.seed = manifest.at("seed").get<uint64_t>();
    bundle.extentMetres = manifest.at("extentMetres").get<double>();
    const auto &resolution = manifest.at("resolution");
    if (!resolution.is_array() || resolution.size() != 2u || resolution[0].get<uint32_t>() != resolution[1].get<uint32_t>())
      return fail("map export manifest resolution is not square");
    bundle.resolution = resolution[0].get<uint32_t>();
    const auto &pyramid = manifest.at("pagePyramid");
    if (pyramid.at("addressing").get<std::string>() != "planar-quadtree" || !pyramid.at("ancestorFallback").get<bool>())
      return fail("map export manifest uses unsupported page addressing");
    bundle.pageSizePixels = pyramid.at("interiorSizePixels").get<uint32_t>();
    bundle.gutterPixels = pyramid.at("gutterPixels").get<uint16_t>();
    bundle.maximumPageLevel = pyramid.at("maximumLevel").get<uint8_t>();
    bundle.graphSignature = parseHex64(manifest.at("graph").at("signature").get<std::string>());
    bundle.bundleSignature = parseHex64(manifest.at("bundleSignature").get<std::string>());
    if (bundle.schemaVersion != 4u || bundle.resolution == 0u || bundle.pageSizePixels == 0u || bundle.gutterPixels == 0u || bundle.maximumPageLevel > 8u ||
        static_cast<uint64_t>(bundle.pageSizePixels) * (uint64_t{1u} << bundle.maximumPageLevel) != bundle.resolution)
      return fail("map export manifest page pyramid is invalid");

    const auto &fields = manifest.at("fields");
    if (!fields.is_array())
      return fail("map export fields are not an array");
    bundle.fields.reserve(fields.size());
    for (const auto &source : fields)
    {
      MapExportField field{};
      field.name = source.at("name").get<std::string>();
      field.available = source.at("available").get<bool>();
      if (!parseFormat(source.at("format").get<std::string>(), field.format) || !parseFilter(source.at("filter").get<std::string>(), field.filter))
        return fail("map export field uses an unsupported format or filter");
      const auto &size = source.at("size");
      if (!size.is_array() || size.size() != 2u)
        return fail("map export field size is invalid");
      field.width = size[0].get<uint32_t>();
      field.height = size[1].get<uint32_t>();
      field.components = source.at("components").get<std::vector<std::string>>();
      field.units = source.at("units").get<std::vector<std::string>>();
      if (!source.at("samplingGroup").is_null())
        field.samplingGroup = source.at("samplingGroup").get<std::string>();
      const auto &scale = source.at("scale");
      const auto &offset = source.at("offset");
      if (!scale.is_array() || scale.size() != 4u || !offset.is_array() || offset.size() != 4u)
        return fail("map export field transform is invalid");
      for (size_t component = 0u; component < 4u; ++component)
      {
        field.scale[component] = scale[component].get<double>();
        field.offset[component] = offset[component].get<double>();
      }
      const auto &pages = source.at("pages");
      if (!field.available)
      {
        if (!source.at("contentHash").is_null() || source.at("uncompressedBytes").get<uint64_t>() != 0u || !pages.empty())
          return fail("unavailable map export field has storage metadata");
        bundle.fields.push_back(std::move(field));
        continue;
      }
      field.contentHash = parseHex64(source.at("contentHash").get<std::string>());
      const uint64_t fieldBytes = static_cast<uint64_t>(field.width) * field.height * mapExportTexelBytes(field.format);
      if (field.width != bundle.resolution || field.height != bundle.resolution || fieldBytes == 0u || source.at("uncompressedBytes").get<uint64_t>() != fieldBytes || !pages.is_array())
        return fail("map export field byte counts are invalid");
      for (const auto &pageSource : pages)
      {
        MapExportPage page{};
        page.address.level = pageSource.at("level").get<uint8_t>();
        page.address.x = pageSource.at("x").get<uint32_t>();
        page.address.y = pageSource.at("y").get<uint32_t>();
        page.file = pageSource.at("file").get<std::string>();
        if (!validMapExportPageAddress(page.address, bundle.maximumPageLevel) || !safePagePath(page.file) || !parseCompression(pageSource.at("compression").get<std::string>(), page.compression))
          return fail("map export page address, path or compression is invalid");
        const auto &pageSize = pageSource.at("size");
        if (!pageSize.is_array() || pageSize.size() != 2u)
          return fail("map export page size is invalid");
        page.width = pageSize[0].get<uint32_t>();
        page.height = pageSize[1].get<uint32_t>();
        page.contentHash = parseHex64(pageSource.at("contentHash").get<std::string>());
        page.storedHash = parseHex64(pageSource.at("storedHash").get<std::string>());
        const uint64_t decodedBytes = static_cast<uint64_t>(page.width) * page.height * mapExportTexelBytes(field.format);
        const uint64_t storedBytes = pageSource.at("storedBytes").get<uint64_t>();
        if (page.width != bundle.pageSizePixels + bundle.gutterPixels * 2u || page.height != page.width || decodedBytes == 0u || pageSource.at("uncompressedBytes").get<uint64_t>() != decodedBytes || storedBytes == 0u ||
            storedBytes > decodedBytes || (page.compression == MapExportCompression::None && storedBytes != decodedBytes))
          return fail("map export page byte counts are invalid");
        field.pages.push_back(std::move(page));
      }
      const size_t expectedPages = (static_cast<size_t>(uint64_t{1u} << ((bundle.maximumPageLevel + 1u) * 2u)) - 1u) / 3u;
      if (field.pages.size() != expectedPages || field.page({0u, 0u, 0u}) == nullptr)
        return fail("map export field page pyramid is incomplete");
      for (uint8_t level = 0u; level <= bundle.maximumPageLevel; ++level)
      {
        const uint32_t dimension = uint32_t{1u} << level;
        for (uint32_t y = 0u; y < dimension; ++y)
          for (uint32_t x = 0u; x < dimension; ++x)
            if (field.page({level, x, y}) == nullptr)
              return fail("map export field page pyramid has an address hole");
      }
      bundle.fields.push_back(std::move(field));
    }
  }
  catch (const std::exception &exception)
  {
    return fail(std::string("could not parse map export manifest: ") + exception.what());
  }

  const MapExportField *biomeIds = bundle.field("biome_mix_ids");
  const MapExportField *biomeWeights = bundle.field("biome_mix_weights");
  if (biomeIds == nullptr || biomeWeights == nullptr || biomeIds->samplingGroup != "biome_mixture" || biomeWeights->samplingGroup != "biome_mixture")
    return fail("map export manifest has no semantic biome pair");
  return true;
}

void releaseMapExportBundleTransportCopies(MapExportBundle &bundle)
{
  for (MapExportField &field : bundle.fields)
  {
    std::vector<uint8_t>().swap(field.data);
    for (MapExportPage &page : field.pages)
      std::vector<uint8_t>().swap(page.storedData);
  }
}

bool readMapExportBundle(const std::string &directory, MapExportBundle &bundle, std::string *error)
{
  bundle = MapExportBundle{};
  const auto fail = [&](const std::string &message)
  {
    bundle = MapExportBundle{};
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (directory.empty())
    return fail("map export directory is empty");
  const std::filesystem::path root(directory);
  std::vector<uint8_t> manifestBytes;
  std::string ioError;
  if (!readFile(root / "manifest.json", manifestBytes, &ioError))
    return fail(ioError);
  bundle.manifestJson.assign(manifestBytes.begin(), manifestBytes.end());

  try
  {
    const nlohmann::json manifest = nlohmann::json::parse(bundle.manifestJson);
    if (manifest.at("schema").get<std::string>() != "engine.world-map" || manifest.at("fieldEncoding").get<std::string>() != "raw" || manifest.at("byteOrder").get<std::string>() != "little-endian" ||
        manifest.at("origin").get<std::string>() != "lower-left" || manifest.at("texelRegistration").get<std::string>() != "cell-centre")
      return fail("map export manifest uses an unsupported registration");
    bundle.schemaVersion = manifest.at("schemaVersion").get<uint32_t>();
    bundle.seed = manifest.at("seed").get<uint64_t>();
    bundle.extentMetres = manifest.at("extentMetres").get<double>();
    const auto &resolution = manifest.at("resolution");
    if (!resolution.is_array() || resolution.size() != 2u || resolution[0].get<uint32_t>() != resolution[1].get<uint32_t>())
      return fail("map export manifest resolution is not square");
    bundle.resolution = resolution[0].get<uint32_t>();
    const auto &pyramid = manifest.at("pagePyramid");
    if (pyramid.at("addressing").get<std::string>() != "planar-quadtree" || !pyramid.at("ancestorFallback").get<bool>())
      return fail("map export manifest uses unsupported page addressing");
    bundle.pageSizePixels = pyramid.at("interiorSizePixels").get<uint32_t>();
    bundle.gutterPixels = pyramid.at("gutterPixels").get<uint16_t>();
    bundle.maximumPageLevel = pyramid.at("maximumLevel").get<uint8_t>();
    const auto &graph = manifest.at("graph");
    const auto &features = manifest.at("features");
    if (graph.at("file").get<std::string>() != "graph.bin" || graph.at("format").get<std::string>() != "WMG1" || features.at("file").get<std::string>() != "features.bin" ||
        features.at("format").get<std::string>() != "WMF1")
      return fail("map export manifest uses unsupported graph payloads");
    bundle.graphSignature = parseHex64(graph.at("signature").get<std::string>());
    bundle.bundleSignature = parseHex64(manifest.at("bundleSignature").get<std::string>());
    if (!readFile(root / "graph.bin", bundle.graphData, &ioError) || !readFile(root / "features.bin", bundle.featureData, &ioError))
      return fail(ioError);
    if (hashData(bundle.featureData) != parseHex64(features.at("contentHash").get<std::string>()))
      return fail("map export features content hash does not match");

    const auto &fields = manifest.at("fields");
    if (!fields.is_array())
      return fail("map export fields are not an array");
    bundle.fields.reserve(fields.size());
    for (const auto &source : fields)
    {
      MapExportField field{};
      field.name = source.at("name").get<std::string>();
      field.available = source.at("available").get<bool>();
      if (!parseFormat(source.at("format").get<std::string>(), field.format) || !parseFilter(source.at("filter").get<std::string>(), field.filter))
        return fail("map export field uses an unsupported format or filter");
      const auto &size = source.at("size");
      if (!size.is_array() || size.size() != 2u)
        return fail("map export field size is invalid");
      field.width = size[0].get<uint32_t>();
      field.height = size[1].get<uint32_t>();
      field.components = source.at("components").get<std::vector<std::string>>();
      field.units = source.at("units").get<std::vector<std::string>>();
      if (!source.at("samplingGroup").is_null())
        field.samplingGroup = source.at("samplingGroup").get<std::string>();
      const auto &scale = source.at("scale");
      const auto &offset = source.at("offset");
      if (!scale.is_array() || scale.size() != 4u || !offset.is_array() || offset.size() != 4u)
        return fail("map export field transform is invalid");
      for (size_t component = 0u; component < 4u; ++component)
      {
        field.scale[component] = scale[component].get<double>();
        field.offset[component] = offset[component].get<double>();
      }
      if (field.available)
      {
        field.contentHash = parseHex64(source.at("contentHash").get<std::string>());
        const uint64_t uncompressedBytes = source.at("uncompressedBytes").get<uint64_t>();
        const uint64_t wantedBytes = static_cast<uint64_t>(field.width) * field.height * mapExportTexelBytes(field.format);
        if (uncompressedBytes != wantedBytes || uncompressedBytes > static_cast<uint64_t>(std::numeric_limits<int>::max()))
          return fail("map export field byte counts are invalid");
        const auto &pages = source.at("pages");
        if (!pages.is_array())
          return fail("map export field pages are not an array");
        field.pages.reserve(pages.size());
        for (const auto &pageSource : pages)
        {
          MapExportPage page{};
          page.address.level = pageSource.at("level").get<uint8_t>();
          page.address.x = pageSource.at("x").get<uint32_t>();
          page.address.y = pageSource.at("y").get<uint32_t>();
          page.file = pageSource.at("file").get<std::string>();
          if (!safePagePath(page.file) || !parseCompression(pageSource.at("compression").get<std::string>(), page.compression))
            return fail("map export page path or compression is invalid");
          const auto &pageSize = pageSource.at("size");
          if (!pageSize.is_array() || pageSize.size() != 2u)
            return fail("map export page size is invalid");
          page.width = pageSize[0].get<uint32_t>();
          page.height = pageSize[1].get<uint32_t>();
          page.contentHash = parseHex64(pageSource.at("contentHash").get<std::string>());
          page.storedHash = parseHex64(pageSource.at("storedHash").get<std::string>());
          const uint64_t decodedSize = pageSource.at("uncompressedBytes").get<uint64_t>();
          const uint64_t encodedSize = pageSource.at("storedBytes").get<uint64_t>();
          const uint64_t expectedPageSize = static_cast<uint64_t>(page.width) * page.height * mapExportTexelBytes(field.format);
          if (decodedSize != expectedPageSize || decodedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()) || encodedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()))
            return fail("map export page byte counts are invalid");
          if (page.compression == MapExportCompression::None)
          {
            if (encodedSize != decodedSize || !readFile(root / page.file, page.data, &ioError))
              return fail(encodedSize != decodedSize ? "raw map export page byte counts do not match" : ioError);
          }
          else
          {
            if (!readFile(root / page.file, page.storedData, &ioError))
              return fail(ioError);
            if (page.storedData.size() != encodedSize || hashData(page.storedData) != page.storedHash)
              return fail("compressed map export page storage does not match");
            page.data.resize(static_cast<size_t>(decodedSize));
            const int decodedBytes =
                LZ4_decompress_safe(reinterpret_cast<const char *>(page.storedData.data()), reinterpret_cast<char *>(page.data.data()), static_cast<int>(page.storedData.size()), static_cast<int>(page.data.size()));
            if (decodedBytes != static_cast<int>(page.data.size()))
              return fail("could not decompress an LZ4 map export page");
          }
          if (storedSize(page) != encodedSize || hashData(page.data) != page.contentHash)
            return fail("map export page bytes do not match the manifest");
          field.pages.push_back(std::move(page));
        }

        field.data.assign(static_cast<size_t>(uncompressedBytes), 0u);
        const uint32_t texelBytes = mapExportTexelBytes(field.format);
        const uint32_t storedSide = bundle.pageSizePixels + bundle.gutterPixels * 2u;
        for (const MapExportPage &page : field.pages)
          if (page.address.level == bundle.maximumPageLevel)
            for (uint32_t y = 0u; y < bundle.pageSizePixels; ++y)
            {
              const size_t sourceOffset = (static_cast<size_t>(y + bundle.gutterPixels) * storedSide + bundle.gutterPixels) * texelBytes;
              const size_t destinationOffset = (static_cast<size_t>(page.address.y * bundle.pageSizePixels + y) * field.width + page.address.x * bundle.pageSizePixels) * texelBytes;
              if (sourceOffset + static_cast<size_t>(bundle.pageSizePixels) * texelBytes > page.data.size() || destinationOffset + static_cast<size_t>(bundle.pageSizePixels) * texelBytes > field.data.size())
                return fail("map export finest page lies outside its field");
              std::memcpy(field.data.data() + destinationOffset, page.data.data() + sourceOffset, static_cast<size_t>(bundle.pageSizePixels) * texelBytes);
            }
        if (hashData(field.data) != field.contentHash)
          return fail("reconstructed map export field hash does not match");
      }
      else if (!source.at("contentHash").is_null() || source.at("uncompressedBytes").get<uint64_t>() != 0u || !source.at("pages").empty())
        return fail("unavailable map export field has storage metadata");
      bundle.fields.push_back(std::move(field));
    }
  }
  catch (const std::exception &exception)
  {
    return fail(std::string("could not parse map export manifest: ") + exception.what());
  }

  std::string validationError;
  if (!bundle.validate(&validationError))
    return fail("loaded map export did not validate: " + validationError);
  return true;
}

bool readMapExportPage(const std::string &directory, const MapExportField &field, const MapExportPage &page, std::vector<uint8_t> &decoded, std::string *error)
{
  decoded.clear();
  if (directory.empty() || !field.available || !safePagePath(page.file) || page.width == 0u || page.height == 0u)
  {
    if (error != nullptr)
      *error = "map export streaming page metadata is invalid";
    return false;
  }
  const uint64_t expectedSize = static_cast<uint64_t>(page.width) * page.height * mapExportTexelBytes(field.format);
  if (expectedSize == 0u || expectedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()))
  {
    if (error != nullptr)
      *error = "map export streaming page byte count is invalid";
    return false;
  }

  std::vector<uint8_t> stored;
  if (!readFile(std::filesystem::path(directory) / page.file, stored, error))
    return false;
  if (hashData(stored) != page.storedHash)
  {
    if (error != nullptr)
      *error = "map export streaming page stored hash does not match";
    return false;
  }
  if (page.compression == MapExportCompression::None)
    decoded = std::move(stored);
  else if (page.compression == MapExportCompression::Lz4)
  {
    if (stored.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
      if (error != nullptr)
        *error = "compressed map export streaming page is too large";
      return false;
    }
    decoded.resize(static_cast<size_t>(expectedSize));
    const int decodedBytes = LZ4_decompress_safe(reinterpret_cast<const char *>(stored.data()), reinterpret_cast<char *>(decoded.data()), static_cast<int>(stored.size()), static_cast<int>(decoded.size()));
    if (decodedBytes != static_cast<int>(decoded.size()))
    {
      decoded.clear();
      if (error != nullptr)
        *error = "could not decompress an LZ4 map export streaming page";
      return false;
    }
  }
  else
  {
    if (error != nullptr)
      *error = "map export streaming page compression is invalid";
    return false;
  }
  if (decoded.size() != expectedSize || hashData(decoded) != page.contentHash)
  {
    decoded.clear();
    if (error != nullptr)
      *error = "map export streaming page decoded hash does not match";
    return false;
  }
  return true;
}

bool decodeMapExportGraph(const MapExportBundle &bundle, MapGraph &graph, std::string *error)
{
  graph = MapGraph{};
  const auto fail = [&](const std::string &message)
  {
    graph = MapGraph{};
    if (error != nullptr)
      *error = message;
    return false;
  };
  std::string validationError;
  if (!bundle.validate(&validationError))
    return fail("cannot decode invalid map export: " + validationError);

  try
  {
    BinaryReader reader(bundle.graphData);
    uint32_t version = 0u;
    uint64_t encodedSignature = 0u;
    WorldBrief brief{};
    if (!reader.magic("WMG1") || !reader.u32(version) || version != 1u || !reader.u64(encodedSignature) || !reader.u64(brief.seed) || !reader.f64(brief.extentMetres) || !reader.f64(brief.seaLevelMetres) ||
        !reader.f64(brief.reliefMetres))
      return fail("could not decode WMG1 header: " + reader.error());
    uint32_t tagCount = 0u;
    if (!reader.count(tagCount, 1u, "tag array") || tagCount > static_cast<uint32_t>(Tag::Count))
      return fail("WMG1 tag array is invalid: " + reader.error());
    brief.tags.resize(tagCount);
    for (Tag &tag : brief.tags)
    {
      uint8_t value = 0u;
      if (!reader.u8(value) || value >= static_cast<uint8_t>(Tag::Count))
        return fail("WMG1 contains an invalid tag");
      tag = static_cast<Tag>(value);
    }
    if (!brief.valid() || brief.seed != bundle.seed || brief.extentMetres != bundle.extentMetres || encodedSignature != bundle.graphSignature)
      return fail("WMG1 brief does not match the manifest");

    uint32_t nodeCount = 0u;
    if (!reader.count(nodeCount, 76u, "node array") || nodeCount == 0u)
      return fail("WMG1 node array is invalid: " + reader.error());
    MapGraph decoded;
    decoded.setBrief(brief);
    for (uint32_t slot = 0u; slot < nodeCount; ++slot)
    {
      MapNode node{};
      uint8_t type = 0u;
      uint8_t content = 0u;
      uint8_t flags = 0u;
      if (!reader.u32(node.id) || !reader.u32(node.parent) || !reader.u8(node.level) || !reader.u8(type) || !reader.u8(content) || !reader.u8(node.biome) || !reader.u16(node.craton) || !reader.u8(flags) ||
          !reader.u32(node.region) || !reader.f64(node.intent.elevationMetres) || !reader.f64(node.intent.upliftRate) || !reader.f64(node.intent.reliefAmplitude) || !reader.f64(node.intent.roughness) ||
          !reader.point(node.site) || !reader.ids(node.children) || !reader.ids(node.neighbours) || !reader.points(node.area))
        return fail("could not decode WMG1 node: " + reader.error());
      if (node.id != slot || type >= static_cast<uint8_t>(CellType::Count) || content >= static_cast<uint8_t>(CellType::Count) || node.biome >= static_cast<uint8_t>(Biome::Count) || (flags & ~7u))
        return fail("WMG1 node contains an invalid id, enum or flag");
      node.type = static_cast<CellType>(type);
      node.content = static_cast<CellType>(content);
      node.land = (flags & 1u) != 0u;
      node.coastal = (flags & 2u) != 0u;
      node.shallow = (flags & 4u) != 0u;

      uint32_t edgeCount = 0u;
      if (!reader.count(edgeCount, 32u, "boundary edge array"))
        return fail("could not decode WMG1 boundary edges: " + reader.error());
      node.boundary.edges.resize(edgeCount);
      for (EdgeSample &edge : node.boundary.edges)
        if (!reader.point(edge.position) || !reader.f64(edge.elevationMetres) || !reader.f64(edge.slope))
          return fail("could not decode WMG1 boundary edge: " + reader.error());
      uint32_t crossingCount = 0u;
      if (!reader.count(crossingCount, 40u, "river crossing array"))
        return fail("could not decode WMG1 river crossings: " + reader.error());
      node.boundary.rivers.resize(crossingCount);
      for (RiverCrossing &crossing : node.boundary.rivers)
        if (!reader.point(crossing.position) || !reader.f64(crossing.dischargeCumecs) || !reader.point(crossing.direction))
          return fail("could not decode WMG1 river crossing: " + reader.error());
      decoded.addNode(std::move(node));
    }
    if (!reader.finished())
      return fail(reader.error().empty() ? "WMG1 has trailing bytes" : "could not decode WMG1: " + reader.error());
    if (!decoded.validate(&validationError))
      return fail("decoded WMG1 graph is invalid: " + validationError);
    if (decoded.signature() != encodedSignature)
      return fail("decoded WMG1 graph signature does not match");
    graph = std::move(decoded);
    return true;
  }
  catch (const std::exception &exception)
  {
    return fail(std::string("could not decode WMG1: ") + exception.what());
  }
}

bool decodeMapExportFeatures(const MapExportBundle &bundle, MapExportFeatures &features, std::string *error)
{
  features = MapExportFeatures{};
  const auto fail = [&](const std::string &message)
  {
    features = MapExportFeatures{};
    if (error != nullptr)
      *error = message;
    return false;
  };
  std::string validationError;
  if (!bundle.validate(&validationError))
    return fail("cannot decode invalid map export: " + validationError);

  try
  {
    BinaryReader reader(bundle.featureData);
    uint32_t version = 0u;
    MapExportFeatures decoded;
    if (!reader.magic("WMF1") || !reader.u32(version) || version != 2u || !reader.f64(decoded.contours.intervalMetres))
      return fail("could not decode WMF1 header: " + reader.error());

    uint32_t count = 0u;
    if (!reader.count(count, 5u, "coastline array"))
      return fail("could not decode WMF1 coastlines: " + reader.error());
    decoded.coastline.resize(count);
    for (CoastPolyline &coast : decoded.coastline)
    {
      uint8_t closed = 0u;
      if (!reader.u8(closed) || closed > 1u || !reader.points(coast.points))
        return fail("could not decode WMF1 coastline: " + reader.error());
      coast.closed = closed != 0u;
    }

    if (!reader.u32(decoded.rivers.riverCells) || !reader.u32(decoded.rivers.mouths) || !reader.u32(decoded.rivers.filledCells) || !reader.u32(decoded.rivers.maximumOrder) ||
        !reader.f64(decoded.rivers.largestDischargeCumecs) || !reader.count(count, 33u, "river node array"))
      return fail("could not decode WMF1 river header: " + reader.error());
    decoded.rivers.nodes.resize(count);
    for (RiverNode &node : decoded.rivers.nodes)
    {
      uint8_t river = 0u;
      if (!reader.u32(node.downstream) || !reader.f64(node.filledElevationMetres) || !reader.f64(node.drainageAreaSquareMetres) || !reader.f64(node.dischargeCumecs) || !reader.u32(node.strahlerOrder) ||
          !reader.u8(river) || river > 1u)
        return fail("could not decode WMF1 river node: " + reader.error());
      node.river = river != 0u;
    }
    if (!reader.count(count, 41u, "river segment array"))
      return fail("could not decode WMF1 river segments: " + reader.error());
    decoded.rivers.segments.resize(count);
    for (RiverSegment &segment : decoded.rivers.segments)
    {
      uint8_t mouth = 0u;
      if (!reader.point(segment.from) || !reader.point(segment.to) || !reader.f64(segment.dischargeCumecs) || !reader.u32(segment.strahlerOrder) || !reader.u32(segment.node) || !reader.u8(mouth) || mouth > 1u)
        return fail("could not decode WMF1 river segment: " + reader.error());
      segment.mouth = mouth != 0u;
    }
    if (!reader.count(count, 44u, "waterfall array"))
      return fail("could not decode WMF1 waterfalls: " + reader.error());
    decoded.rivers.waterfalls.resize(count);
    for (Waterfall &waterfall : decoded.rivers.waterfalls)
      if (!reader.point(waterfall.position) || !reader.f64(waterfall.dropMetres) || !reader.f64(waterfall.gradient) || !reader.f64(waterfall.dischargeCumecs) || !reader.u32(waterfall.node))
        return fail("could not decode WMF1 waterfall: " + reader.error());

    if (!reader.count(count, 13u, "contour array"))
      return fail("could not decode WMF1 contours: " + reader.error());
    decoded.contours.lines.resize(count);
    for (ContourPolyline &line : decoded.contours.lines)
    {
      uint8_t flags = 0u;
      if (!reader.f64(line.elevationMetres) || !reader.u8(flags) || (flags & ~3u) || !reader.points(line.points))
        return fail("could not decode WMF1 contour: " + reader.error());
      line.closed = (flags & 1u) != 0u;
      line.major = (flags & 2u) != 0u;
    }

    if (!reader.count(count, 75u, "region array"))
      return fail("could not decode WMF1 regions: " + reader.error());
    decoded.regions.regions.resize(count);
    for (MapRegion &region : decoded.regions.regions)
    {
      uint8_t content = 0u;
      uint8_t biome = 0u;
      uint8_t water = 0u;
      if (!reader.u32(region.id) || !reader.ids(region.cells) || !reader.u32(region.seedCell) || !reader.point(region.centroid) || !reader.f64(region.areaSquareMetres) || !reader.u8(content) || !reader.u8(biome) ||
          !reader.string(region.name) || !reader.f64(region.meanElevationMetres) || !reader.f64(region.reliefMetres) || !reader.f64(region.meanUpliftRate) || !reader.f64(region.coastalFraction) ||
          !reader.f64(region.riverFraction) || !reader.u8(water) || content >= static_cast<uint8_t>(CellType::Count) || biome >= static_cast<uint8_t>(Biome::Count) || water > 1u)
        return fail("could not decode WMF1 region: " + reader.error());
      region.content = static_cast<CellType>(content);
      region.biome = static_cast<Biome>(biome);
      region.water = water != 0u;
    }

    if (!reader.count(count, 20u, "named river array"))
      return fail("could not decode WMF1 named rivers: " + reader.error());
    decoded.regions.rivers.resize(count);
    for (NamedRiver &river : decoded.regions.rivers)
      if (!reader.string(river.name) || !reader.ids(river.stem) || !reader.f64(river.dischargeCumecs) || !reader.u32(river.strahlerOrder))
        return fail("could not decode WMF1 named river: " + reader.error());
    if (!reader.ids(decoded.regions.cellRegion))
      return fail("could not decode WMF1 cell-region lookup: " + reader.error());
    if (!reader.finished())
      return fail(reader.error().empty() ? "WMF1 has trailing bytes" : "could not decode WMF1: " + reader.error());

    const size_t nodes = decoded.rivers.nodes.size();
    if (decoded.regions.cellRegion.size() != nodes)
      return fail("WMF1 cell-region lookup does not cover the graph");
    for (const RiverNode &node : decoded.rivers.nodes)
      if (node.downstream != kInvalidNode && node.downstream >= nodes)
        return fail("WMF1 river downstream id is out of range");
    for (const RiverSegment &segment : decoded.rivers.segments)
      if (segment.node >= nodes)
        return fail("WMF1 river segment node id is out of range");
    for (const Waterfall &waterfall : decoded.rivers.waterfalls)
      if (waterfall.node >= nodes)
        return fail("WMF1 waterfall node id is out of range");
    for (const MapRegion &region : decoded.regions.regions)
    {
      if (region.id >= decoded.regions.regions.size() || (region.seedCell != kInvalidNode && region.seedCell >= nodes))
        return fail("WMF1 region id or seed is out of range");
      for (const uint32_t cell : region.cells)
        if (cell >= nodes)
          return fail("WMF1 region cell id is out of range");
    }
    for (const NamedRiver &river : decoded.regions.rivers)
      for (const uint32_t cell : river.stem)
        if (cell >= nodes)
          return fail("WMF1 named-river cell id is out of range");
    features = std::move(decoded);
    return true;
  }
  catch (const std::exception &exception)
  {
    return fail(std::string("could not decode WMF1: ") + exception.what());
  }
}

} // namespace procedural::world
