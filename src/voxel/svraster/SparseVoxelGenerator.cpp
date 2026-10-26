#include "voxel/svraster/SparseVoxelGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace voxel::svraster
{
namespace
{

constexpr uint64_t kSplitmixIncrement = 0x9e3779b97f4a7c15ull;
constexpr uint64_t kSplitmixMultiply0 = 0xbf58476d1ce4e5b9ull;
constexpr uint64_t kSplitmixMultiply1 = 0x94d049bb133111ebull;
constexpr uint32_t kMinimumAlbedoChannel = 32u;
constexpr uint32_t kAlbedoChannelSpan = 224u;
constexpr uint32_t kMinimumRoughnessUNorm8 = 20u;
constexpr uint32_t kRoughnessSpan = 236u;
constexpr uint64_t kEmissionEnableMask = 0xfull;

struct GeneratedVoxel
{
  uint64_t path = 0u;
  SparseVoxelPathLevelGPU pathLevel{};
  SparseVoxelCenterLengthGPU centerLength{};
  SparseVoxelMaterialGPU material{};
};

uint32_t byteAt(const uint64_t value, const uint32_t bitOffset)
{
  return static_cast<uint32_t>((value >> bitOffset) & 0xffu);
}

uint32_t mapAlbedoChannel(const uint32_t byte)
{
  return kMinimumAlbedoChannel + ((byte * kAlbedoChannelSpan) >> 8u);
}

std::array<float, 16> identityMatrix()
{
  std::array<float, 16> result{};
  result[0] = 1.0f;
  result[5] = 1.0f;
  result[10] = 1.0f;
  result[15] = 1.0f;
  return result;
}

bool failValidation(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool isFiniteHalf(const uint16_t value)
{
  return (value & 0x7c00u) != 0x7c00u;
}

uint64_t ancestorPath(const uint64_t path, const uint32_t level)
{
  const uint32_t unusedBits = 3u * (kMaxOctreeLevels - level);
  return (path >> unusedBits) << unusedBits;
}

SparseVoxelCenterLengthGPU centerLengthForPath(const uint64_t path, const uint32_t level)
{
  const auto coordinate = decodeOctreePath(path, level);
  const float length = kBootstrapRootExtent / static_cast<float>(uint32_t{1} << level);
  constexpr float rootMinimum = -kBootstrapRootExtent * 0.5f;
  return {{
            rootMinimum + (static_cast<float>(coordinate[0]) + 0.5f) * length,
            rootMinimum + (static_cast<float>(coordinate[1]) + 0.5f) * length,
            rootMinimum + (static_cast<float>(coordinate[2]) + 0.5f) * length,
          },
          length};
}

struct HSVColor
{
  double hue = 0.0;
  double saturation = 0.0;
  double value = 0.0;
};

HSVColor unpackHSV(const uint32_t rgba)
{
  const double red = static_cast<double>(rgba & 0xffu) / 255.0;
  const double green = static_cast<double>((rgba >> 8u) & 0xffu) / 255.0;
  const double blue = static_cast<double>((rgba >> 16u) & 0xffu) / 255.0;
  const double maximum = std::max({red, green, blue});
  const double minimum = std::min({red, green, blue});
  const double chroma = maximum - minimum;
  HSVColor result{0.0, maximum > 0.0 ? chroma / maximum : 0.0, maximum};
  if (chroma <= 0.0)
    return result;
  if (maximum == red)
    result.hue = std::fmod((green - blue) / chroma, 6.0);
  else if (maximum == green)
    result.hue = (blue - red) / chroma + 2.0;
  else
    result.hue = (red - green) / chroma + 4.0;
  result.hue /= 6.0;
  if (result.hue < 0.0)
    result.hue += 1.0;
  return result;
}

uint32_t packHSV(const HSVColor &hsv)
{
  const double hue = hsv.hue - std::floor(hsv.hue);
  const double chroma = hsv.value * hsv.saturation;
  const double hueSector = hue * 6.0;
  const double secondary = chroma * (1.0 - std::abs(std::fmod(hueSector, 2.0) - 1.0));
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;
  if (hueSector < 1.0)
    red = chroma, green = secondary;
  else if (hueSector < 2.0)
    red = secondary, green = chroma;
  else if (hueSector < 3.0)
    green = chroma, blue = secondary;
  else if (hueSector < 4.0)
    green = secondary, blue = chroma;
  else if (hueSector < 5.0)
    red = secondary, blue = chroma;
  else
    red = chroma, blue = secondary;
  const double match = hsv.value - chroma;
  const auto pack = [&](const double channel)
  {
    return static_cast<uint32_t>(std::lround(std::clamp(channel + match, 0.0, 1.0) * 255.0));
  };
  return pack(red) | (pack(green) << 8u) | (pack(blue) << 16u) | 0xff000000u;
}

SparseVoxelMaterialGPU reduceMaterials(
    const std::vector<SparseVoxelMaterialGPU> &children,
    const uint32_t materialId)
{
  SparseVoxelMaterialGPU result{};
  if (children.empty())
    return result;
  std::array<uint64_t, 2> propertySums{};
  std::array<double, 3> emissive{};
  double hueX = 0.0;
  double hueY = 0.0;
  double hueWeightSum = 0.0;
  double saturationSum = 0.0;
  double valueSum = 0.0;
  HSVColor fallbackHue{};
  uint32_t fallbackAlbedo = 0u;
  bool hasFallback = false;
  constexpr double tau = 6.28318530717958647692;
  for (const auto &child : children)
  {
    const HSVColor hsv = unpackHSV(child.albedoRGBA8);
    const double hueWeight = hsv.saturation * hsv.value;
    hueX += std::cos(hsv.hue * tau) * hueWeight;
    hueY += std::sin(hsv.hue * tau) * hueWeight;
    hueWeightSum += hueWeight;
    saturationSum += hsv.saturation;
    valueSum += hsv.value;
    if (!hasFallback || hsv.saturation > fallbackHue.saturation ||
        (hsv.saturation == fallbackHue.saturation && hsv.value > fallbackHue.value) ||
        (hsv.saturation == fallbackHue.saturation && hsv.value == fallbackHue.value && child.albedoRGBA8 < fallbackAlbedo))
    {
      fallbackHue = hsv;
      fallbackAlbedo = child.albedoRGBA8;
      hasFallback = true;
    }
    propertySums[0] += child.roughnessMetallicUNorm8 & 0xffu;
    propertySums[1] += (child.roughnessMetallicUNorm8 >> 8u) & 0xffu;
    emissive[0] += halfToFloat(static_cast<uint16_t>(child.emissiveRG16F));
    emissive[1] += halfToFloat(static_cast<uint16_t>(child.emissiveRG16F >> 16u));
    emissive[2] += halfToFloat(static_cast<uint16_t>(child.emissiveB16F));
    result.flags |= child.flags;
  }
  const uint64_t count = children.size();
  const auto averageByte = [&](const uint64_t sum) { return static_cast<uint32_t>((sum + count / 2u) / count); };
  const double hueCoherence = std::sqrt(hueX * hueX + hueY * hueY);
  HSVColor aggregate{};
  aggregate.hue = hueWeightSum > 0.0 && hueCoherence > hueWeightSum * 1.0e-6
                      ? std::atan2(hueY, hueX) / tau
                      : fallbackHue.hue;
  aggregate.saturation = saturationSum / static_cast<double>(count);
  aggregate.value = valueSum / static_cast<double>(count);
  result.albedoRGBA8 = packHSV(aggregate);
  result.roughnessMetallicUNorm8 = averageByte(propertySums[0]) | (averageByte(propertySums[1]) << 8u);
  result.emissiveRG16F = static_cast<uint32_t>(floatToHalfRoundToNearestEven(static_cast<float>(emissive[0] / count))) |
                         (static_cast<uint32_t>(floatToHalfRoundToNearestEven(static_cast<float>(emissive[1] / count))) << 16u);
  result.emissiveB16F = static_cast<uint32_t>(floatToHalfRoundToNearestEven(static_cast<float>(emissive[2] / count)));
  result.materialId = materialId;
  return result;
}

} // namespace

SparseVoxelMaterialGPU reduceSparseVoxelLODMaterials(
    const std::vector<SparseVoxelMaterialGPU> &children,
    const uint32_t materialId)
{
  return reduceMaterials(children, materialId);
}

uint64_t splitmix64(uint64_t value)
{
  value += kSplitmixIncrement;
  value = (value ^ (value >> 30u)) * kSplitmixMultiply0;
  value = (value ^ (value >> 27u)) * kSplitmixMultiply1;
  return value ^ (value >> 31u);
}

uint64_t encodeOctreePath(const uint32_t x, const uint32_t y, const uint32_t z, const uint32_t level)
{
  if (level == 0u || level > kMaxOctreeLevels)
    return kInvalidOctreePath;

  const uint32_t resolution = 1u << level;
  if (x >= resolution || y >= resolution || z >= resolution)
    return kInvalidOctreePath;

  uint64_t morton = 0u;
  for (uint32_t bit = 0u; bit < level; ++bit)
  {
    const uint64_t octant = (static_cast<uint64_t>((x >> bit) & 1u) << 2u) | (static_cast<uint64_t>((y >> bit) & 1u) << 1u) | static_cast<uint64_t>((z >> bit) & 1u);
    morton |= octant << (3u * bit);
  }
  return morton << (3u * (kMaxOctreeLevels - level));
}

std::array<uint32_t, 3> decodeOctreePath(const uint64_t path, const uint32_t level)
{
  std::array<uint32_t, 3> result{};
  if (!isCanonicalOctreePath(path, level))
    return result;

  uint64_t morton = path >> (3u * (kMaxOctreeLevels - level));
  for (uint32_t bit = 0u; bit < level; ++bit)
  {
    const uint32_t octant = static_cast<uint32_t>(morton & 0x7u);
    result[0] |= ((octant >> 2u) & 1u) << bit;
    result[1] |= ((octant >> 1u) & 1u) << bit;
    result[2] |= (octant & 1u) << bit;
    morton >>= 3u;
  }
  return result;
}

bool isCanonicalOctreePath(const uint64_t path, const uint32_t level)
{
  if (level == 0u || level > kMaxOctreeLevels || (path & ~kOctreePathMask) != 0u)
    return false;
  const uint32_t unusedBits = 3u * (kMaxOctreeLevels - level);
  const uint64_t unusedMask = unusedBits == 0u ? 0u : ((uint64_t{1} << unusedBits) - 1u);
  return (path & unusedMask) == 0u;
}

uint16_t floatToHalfRoundToNearestEven(const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));

  const uint32_t sign = (bits >> 16u) & 0x8000u;
  const uint32_t exponentBits = (bits >> 23u) & 0xffu;
  uint32_t mantissa = bits & 0x007fffffu;

  if (exponentBits == 0xffu)
    return static_cast<uint16_t>(sign | (mantissa == 0u ? 0x7c00u : 0x7e00u));

  int32_t exponent = static_cast<int32_t>(exponentBits) - 127 + 15;
  if (exponent <= 0)
  {
    if (exponent < -10)
      return static_cast<uint16_t>(sign);
    mantissa |= 0x00800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - exponent);
    uint32_t halfMantissa = mantissa >> shift;
    const uint32_t remainder = mantissa & ((uint32_t{1} << shift) - 1u);
    const uint32_t halfway = uint32_t{1} << (shift - 1u);
    if (remainder > halfway || (remainder == halfway && (halfMantissa & 1u) != 0u))
      ++halfMantissa;
    return static_cast<uint16_t>(sign | halfMantissa);
  }

  if (exponent >= 31)
    return static_cast<uint16_t>(sign | 0x7c00u);

  uint32_t halfMantissa = mantissa >> 13u;
  const uint32_t remainder = mantissa & 0x1fffu;
  if (remainder > 0x1000u || (remainder == 0x1000u && (halfMantissa & 1u) != 0u))
  {
    ++halfMantissa;
    if (halfMantissa == 0x400u)
    {
      halfMantissa = 0u;
      ++exponent;
      if (exponent >= 31)
        return static_cast<uint16_t>(sign | 0x7c00u);
    }
  }

  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10u) | halfMantissa);
}

float halfToFloat(const uint16_t value)
{
  const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16u;
  const uint32_t exponent = (value >> 10u) & 0x1fu;
  uint32_t mantissa = value & 0x03ffu;
  uint32_t bits = 0u;

  if (exponent == 0u)
  {
    if (mantissa == 0u)
    {
      bits = sign;
    }
    else
    {
      int32_t normalizedExponent = -14;
      while ((mantissa & 0x0400u) == 0u)
      {
        mantissa <<= 1u;
        --normalizedExponent;
      }
      mantissa &= 0x03ffu;
      bits = sign | (static_cast<uint32_t>(normalizedExponent + 127) << 23u) | (mantissa << 13u);
    }
  }
  else if (exponent == 0x1fu)
  {
    bits = sign | 0x7f800000u | (mantissa << 13u);
  }
  else
  {
    bits = sign | ((exponent - 15u + 127u) << 23u) | (mantissa << 13u);
  }

  float result = 0.0f;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

SparseVoxelMaterialGPU generateExteriorShellMaterial(const uint64_t path, const uint64_t materialSeed)
{
  const uint64_t hAlbedo = splitmix64(path ^ materialSeed);
  const uint64_t hProperties = splitmix64(hAlbedo);
  const uint64_t hEmission = splitmix64(hProperties);

  SparseVoxelMaterialGPU material{};
  const uint32_t red = mapAlbedoChannel(byteAt(hAlbedo, 0u));
  const uint32_t green = mapAlbedoChannel(byteAt(hAlbedo, 8u));
  const uint32_t blue = mapAlbedoChannel(byteAt(hAlbedo, 16u));
  material.albedoRGBA8 = red | (green << 8u) | (blue << 16u) | 0xff000000u;

  const uint32_t roughness = kMinimumRoughnessUNorm8 + ((byteAt(hProperties, 0u) * kRoughnessSpan) >> 8u);
  const uint32_t metallic = byteAt(hProperties, 8u);
  material.roughnessMetallicUNorm8 = roughness | (metallic << 8u);
  material.materialId = static_cast<uint32_t>((hProperties >> 16u) & 0xffffu);

  if ((hEmission & kEmissionEnableMask) == 0u)
  {
    const float redEmission = static_cast<float>(1u + (byteAt(hEmission, 8u) & 0x3fu)) / 16.0f;
    const float greenEmission = static_cast<float>(1u + (byteAt(hEmission, 16u) & 0x3fu)) / 16.0f;
    const float blueEmission = static_cast<float>(1u + (byteAt(hEmission, 24u) & 0x3fu)) / 16.0f;
    material.emissiveRG16F = static_cast<uint32_t>(floatToHalfRoundToNearestEven(redEmission)) | (static_cast<uint32_t>(floatToHalfRoundToNearestEven(greenEmission)) << 16u);
    material.emissiveB16F = static_cast<uint32_t>(floatToHalfRoundToNearestEven(blueEmission));
  }

  return material;
}

BootstrapSparseVoxelModel generateExteriorShell128(const uint64_t materialSeed)
{
  BootstrapSparseVoxelModel model{};
  model.blas.rootCenterExtent = {0.0f, 0.0f, 0.0f, kBootstrapRootExtent};
  model.blas.firstVoxel = 0u;
  model.blas.voxelCount = kExteriorShellVoxelCount;
  model.blas.maxLevel = kExteriorShellLevel;

  std::vector<GeneratedVoxel> generated;
  generated.reserve(kExteriorShellVoxelCount);
  constexpr float voxelLength = kBootstrapRootExtent / static_cast<float>(kExteriorShellResolution);
  constexpr float rootMinimum = -kBootstrapRootExtent * 0.5f;

  for (uint32_t z = 0u; z < kExteriorShellResolution; ++z)
  {
    for (uint32_t y = 0u; y < kExteriorShellResolution; ++y)
    {
      for (uint32_t x = 0u; x < kExteriorShellResolution; ++x)
      {
        if (x != 0u && x + 1u != kExteriorShellResolution && y != 0u && y + 1u != kExteriorShellResolution && z != 0u && z + 1u != kExteriorShellResolution)
          continue;

        GeneratedVoxel voxel{};
        voxel.path = encodeOctreePath(x, y, z, kExteriorShellLevel);
        voxel.pathLevel.pathLo = static_cast<uint32_t>(voxel.path);
        voxel.pathLevel.pathHi = static_cast<uint32_t>(voxel.path >> 32u);
        voxel.pathLevel.level = kExteriorShellLevel;
        voxel.centerLength.center = {
          rootMinimum + (static_cast<float>(x) + 0.5f) * voxelLength,
          rootMinimum + (static_cast<float>(y) + 0.5f) * voxelLength,
          rootMinimum + (static_cast<float>(z) + 0.5f) * voxelLength,
        };
        voxel.centerLength.length = voxelLength;
        voxel.material = generateExteriorShellMaterial(voxel.path, materialSeed);
        generated.push_back(voxel);
      }
    }
  }

  std::sort(
      generated.begin(),
      generated.end(),
      [](const GeneratedVoxel &left, const GeneratedVoxel &right)
      {
        return left.path < right.path;
      });

  model.pathsLevels.reserve(generated.size());
  model.centersLengths.reserve(generated.size());
  model.materials.reserve(generated.size());
  for (const GeneratedVoxel &voxel : generated)
  {
    model.pathsLevels.push_back(voxel.pathLevel);
    model.centersLengths.push_back(voxel.centerLength);
    model.materials.push_back(voxel.material);
  }

  model.instance.modelToWorld = identityMatrix();
  model.instance.worldToModel = identityMatrix();
  model.instance.previousModelToWorld = identityMatrix();
  model.instance.worldBoundsMin = {-1.0f, -1.0f, -1.0f, 0.0f};
  model.instance.worldBoundsMax = {1.0f, 1.0f, 1.0f, 0.0f};
  model.instance.blasIndex = 0u;
  return model;
}

BootstrapSparseVoxelLOD generateExteriorShellLOD128(const uint64_t materialSeed)
{
  const BootstrapSparseVoxelModel leaves = generateExteriorShell128(materialSeed);
  struct NodeData
  {
    SparseVoxelMaterialGPU material{};
  };
  std::array<std::map<uint64_t, NodeData>, kExteriorShellLevel + 1u> levels;
  for (size_t index = 0u; index < leaves.pathsLevels.size(); ++index)
  {
    const auto &pathLevel = leaves.pathsLevels[index];
    const uint64_t path = static_cast<uint64_t>(pathLevel.pathLo) | (static_cast<uint64_t>(pathLevel.pathHi) << 32u);
    levels[kExteriorShellLevel][path].material = leaves.materials[index];
  }
  for (uint32_t childLevel = kExteriorShellLevel; childLevel > 1u; --childLevel)
  {
    std::map<uint64_t, std::vector<SparseVoxelMaterialGPU>> grouped;
    for (const auto &[childPath, child] : levels[childLevel])
      grouped[ancestorPath(childPath, childLevel - 1u)].push_back(child.material);
    for (auto &[parentPath, childMaterials] : grouped)
      levels[childLevel - 1u][parentPath].material =
          reduceSparseVoxelLODMaterials(childMaterials, static_cast<uint32_t>(splitmix64(parentPath)));
  }

  BootstrapSparseVoxelLOD result{};
  std::map<std::pair<uint32_t, uint64_t>, uint32_t> indices;
  for (uint32_t level = 1u; level <= kExteriorShellLevel; ++level)
  {
    for (const auto &[path, node] : levels[level])
    {
      const uint32_t index = static_cast<uint32_t>(result.nodes.size());
      indices[{level, path}] = index;
      result.nodes.push_back({0u, 0u, index, level});
      result.pathsLevels.push_back({static_cast<uint32_t>(path), static_cast<uint32_t>(path >> 32u), level, 0u});
      result.centersLengths.push_back(centerLengthForPath(path, level));
      result.materials.push_back(node.material);
    }
  }
  result.firstRoot = 0u;
  result.rootCount = static_cast<uint32_t>(levels[1].size());
  std::vector<std::vector<uint32_t>> children(result.nodes.size());
  for (uint32_t childIndex = result.rootCount; childIndex < result.nodes.size(); ++childIndex)
  {
    const auto &pathLevel = result.pathsLevels[childIndex];
    const uint64_t path = static_cast<uint64_t>(pathLevel.pathLo) | (static_cast<uint64_t>(pathLevel.pathHi) << 32u);
    children[indices.at({pathLevel.level - 1u, ancestorPath(path, pathLevel.level - 1u)})].push_back(childIndex);
  }
  for (uint32_t index = 0u; index < result.nodes.size(); ++index)
  {
    if (children[index].empty())
      continue;
    result.nodes[index].firstChild = children[index].front();
    result.nodes[index].childCount = static_cast<uint32_t>(children[index].size());
  }
  return result;
}

bool validateBootstrapSparseVoxelLOD(const BootstrapSparseVoxelLOD &hierarchy, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (hierarchy.nodes.empty() || hierarchy.nodes.size() != hierarchy.pathsLevels.size() || hierarchy.nodes.size() != hierarchy.centersLengths.size() || hierarchy.nodes.size() != hierarchy.materials.size())
    return failValidation(error, "LOD hierarchy parallel array sizes differ");
  if (hierarchy.firstRoot + hierarchy.rootCount > hierarchy.nodes.size() || hierarchy.rootCount != 8u)
    return failValidation(error, "LOD root range is invalid");
  std::vector<uint32_t> parentCounts(hierarchy.nodes.size(), 0u);
  uint32_t leafCount = 0u;
  for (uint32_t index = 0u; index < hierarchy.nodes.size(); ++index)
  {
    const auto &node = hierarchy.nodes[index];
    const auto &pathLevel = hierarchy.pathsLevels[index];
    if (node.voxelIndex != index || node.level != pathLevel.level || node.level < 1u || node.level > kExteriorShellLevel || node.childCount > 8u || node.firstChild + node.childCount > hierarchy.nodes.size())
      return failValidation(error, "LOD node metadata is invalid");
    if (node.childCount == 0u)
    {
      if (node.level != kExteriorShellLevel)
        return failValidation(error, "LOD hierarchy contains an early leaf");
      ++leafCount;
    }
    else
    {
      const uint64_t parentPath = static_cast<uint64_t>(pathLevel.pathLo) | (static_cast<uint64_t>(pathLevel.pathHi) << 32u);
      for (uint32_t child = 0u; child < node.childCount; ++child)
      {
        const uint32_t childIndex = node.firstChild + child;
        const auto &childPathLevel = hierarchy.pathsLevels[childIndex];
        const uint64_t childPath = static_cast<uint64_t>(childPathLevel.pathLo) | (static_cast<uint64_t>(childPathLevel.pathHi) << 32u);
        if (hierarchy.nodes[childIndex].level != node.level + 1u || ancestorPath(childPath, node.level) != parentPath)
          return failValidation(error, "LOD child level is invalid");
        ++parentCounts[childIndex];
      }
    }
  }
  for (uint32_t index = 0u; index < hierarchy.nodes.size(); ++index)
  {
    const bool isRoot = index >= hierarchy.firstRoot && index < hierarchy.firstRoot + hierarchy.rootCount;
    if ((isRoot && (hierarchy.nodes[index].level != 1u || parentCounts[index] != 0u)) || (!isRoot && parentCounts[index] != 1u))
      return failValidation(error, "LOD node parent relationship is invalid");
  }
  if (leafCount != kExteriorShellVoxelCount)
    return failValidation(error, "LOD leaf count differs from the exterior shell");
  return true;
}

bool validateBootstrapSparseVoxelModel(const BootstrapSparseVoxelModel &model, std::string *error)
{
  if (error != nullptr)
    error->clear();
  const size_t voxelCount = model.pathsLevels.size();
  if (voxelCount != model.centersLengths.size() || voxelCount != model.materials.size())
    return failValidation(error, "parallel voxel array sizes differ");
  if (voxelCount == 0u)
    return failValidation(error, "bootstrap BLAS is empty");
  if (model.blas.firstVoxel != 0u || model.blas.voxelCount != voxelCount)
    return failValidation(error, "BLAS voxel range does not match resident arrays");
  if (model.blas.maxLevel == 0u || model.blas.maxLevel > kMaxOctreeLevels || model.blas.flags != 0u)
    return failValidation(error, "BLAS level or flags are invalid");
  if (!std::isfinite(model.blas.rootCenterExtent[3]) || model.blas.rootCenterExtent[3] <= 0.0f)
    return failValidation(error, "BLAS root extent is invalid");
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(model.blas.rootCenterExtent[axis]))
      return failValidation(error, "BLAS root center is non-finite");
  }

  std::pair<uint32_t, uint64_t> previousKey{};
  std::set<std::pair<uint32_t, uint64_t>> activePaths;
  uint32_t maximumLevel = 0u;
  for (size_t voxelIndex = 0u; voxelIndex < voxelCount; ++voxelIndex)
  {
    const SparseVoxelPathLevelGPU &pathLevel = model.pathsLevels[voxelIndex];
    const uint64_t path = static_cast<uint64_t>(pathLevel.pathLo) | (static_cast<uint64_t>(pathLevel.pathHi) << 32u);
    if (!isCanonicalOctreePath(path, pathLevel.level))
      return failValidation(error, "voxel path is not canonical");
    if (pathLevel.flags != 0u)
      return failValidation(error, "voxel path flags are unsupported");
    const std::pair<uint32_t, uint64_t> key{pathLevel.level, path};
    if (voxelIndex > 0u && key <= previousKey)
      return failValidation(error, "voxels are not in canonical (level, path) order");
    previousKey = key;
    for (uint32_t ancestorLevel = 1u; ancestorLevel < pathLevel.level; ++ancestorLevel)
    {
      const uint32_t unusedBits = 3u * (kMaxOctreeLevels - ancestorLevel);
      const uint64_t ancestorPath = (path >> unusedBits) << unusedBits;
      if (activePaths.count({ancestorLevel, ancestorPath}) != 0u)
        return failValidation(error, "active voxel path has an active ancestor");
    }
    activePaths.insert(key);
    maximumLevel = std::max(maximumLevel, pathLevel.level);

    const std::array<uint32_t, 3> coordinate = decodeOctreePath(path, pathLevel.level);
    const float length = model.blas.rootCenterExtent[3] / static_cast<float>(uint32_t{1} << pathLevel.level);
    const float rootMinimum = -model.blas.rootCenterExtent[3] * 0.5f;
    const SparseVoxelCenterLengthGPU &centerLength = model.centersLengths[voxelIndex];
    if (!std::isfinite(centerLength.length) || centerLength.length != length)
      return failValidation(error, "voxel length does not match its level");
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float expected = model.blas.rootCenterExtent[axis] + rootMinimum + (static_cast<float>(coordinate[axis]) + 0.5f) * length;
      if (!std::isfinite(centerLength.center[axis]) || centerLength.center[axis] != expected)
        return failValidation(error, "voxel center does not match path decoding");
    }

    const SparseVoxelMaterialGPU &material = model.materials[voxelIndex];
    if ((material.albedoRGBA8 >> 24u) != 0xffu)
      return failValidation(error, "voxel albedo is not opaque");
    if ((material.roughnessMetallicUNorm8 & 0xffff0000u) != 0u)
      return failValidation(error, "voxel scalar material packing is not canonical");
    if ((material.emissiveB16F & 0xffff0000u) != 0u)
      return failValidation(error, "voxel blue emission packing is not canonical");
    const uint16_t emissionR = static_cast<uint16_t>(material.emissiveRG16F);
    const uint16_t emissionG = static_cast<uint16_t>(material.emissiveRG16F >> 16u);
    const uint16_t emissionB = static_cast<uint16_t>(material.emissiveB16F);
    if (!isFiniteHalf(emissionR) || !isFiniteHalf(emissionG) || !isFiniteHalf(emissionB) || halfToFloat(emissionR) < 0.0f || halfToFloat(emissionG) < 0.0f || halfToFloat(emissionB) < 0.0f)
      return failValidation(error, "voxel emission is invalid");
    if (material.flags != 0u ||
        !sparseVoxelMaterialSecondaryLayerIsCanonical(material))
      return failValidation(error, "voxel material flags or secondary layer are invalid");
  }

  if (maximumLevel != model.blas.maxLevel)
    return failValidation(error, "BLAS maxLevel does not match resident voxels");

  if (model.instance.blasIndex != 0u || model.instance.flags != 0u || model.instance.reserved0 != 0u || model.instance.reserved1 != 0u)
    return failValidation(error, "bootstrap instance metadata is invalid");
  for (float value : model.instance.modelToWorld)
  {
    if (!std::isfinite(value))
      return failValidation(error, "bootstrap instance transform is non-finite");
  }
  for (float value : model.instance.worldToModel)
  {
    if (!std::isfinite(value))
      return failValidation(error, "bootstrap inverse transform is non-finite");
  }
  for (float value : model.instance.previousModelToWorld)
  {
    if (!std::isfinite(value))
      return failValidation(error, "bootstrap previous transform is non-finite");
  }
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(model.instance.worldBoundsMin[axis]) || !std::isfinite(model.instance.worldBoundsMax[axis]) || model.instance.worldBoundsMin[axis] > model.instance.worldBoundsMax[axis])
      return failValidation(error, "bootstrap instance bounds are invalid");
  }
  return true;
}

} // namespace voxel::svraster
