#include "voxel/material/VoxelMaterialAsset.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace voxel::material
{
namespace
{

bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

constexpr uint64_t kVoxelMaterialFileMagic = 0x3154414d4c585656ull;
constexpr size_t kVoxelMaterialFileHeaderBytes = 32u;
constexpr uint32_t kMaximumVoxelMaterialStringBytes = 1u << 20u;

void appendU32(std::vector<uint8_t> &bytes, const uint32_t value)
{
  for (uint32_t shift = 0u; shift < 32u; shift += 8u)
    bytes.push_back(static_cast<uint8_t>(value >> shift));
}

void appendU64(std::vector<uint8_t> &bytes, const uint64_t value)
{
  for (uint32_t shift = 0u; shift < 64u; shift += 8u)
    bytes.push_back(static_cast<uint8_t>(value >> shift));
}

void appendFloat(std::vector<uint8_t> &bytes, const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  appendU32(bytes, bits);
}

void appendString(std::vector<uint8_t> &bytes, const std::string &value)
{
  appendU32(bytes, static_cast<uint32_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

uint64_t payloadChecksum(const uint8_t *data, const size_t size)
{
  uint64_t hash = 1469598103934665603ull;
  for (size_t index = 0u; index < size; ++index)
  {
    hash ^= data[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

struct AssetReader
{
  const std::vector<uint8_t> &bytes;
  size_t offset = 0u;

  bool readU32(uint32_t &value)
  {
    if (offset > bytes.size() || 4u > bytes.size() - offset)
      return false;
    value = static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
    offset += 4u;
    return true;
  }

  bool readU64(uint64_t &value)
  {
    value = 0u;
    if (offset > bytes.size() || 8u > bytes.size() - offset)
      return false;
    for (uint32_t index = 0u; index < 8u; ++index)
      value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8u);
    offset += 8u;
    return true;
  }

  bool readFloat(float &value)
  {
    uint32_t bits = 0u;
    if (!readU32(bits))
      return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }

  bool readString(std::string &value)
  {
    uint32_t size = 0u;
    if (!readU32(size) || size > kMaximumVoxelMaterialStringBytes ||
        offset > bytes.size() || size > bytes.size() - offset)
      return false;
    value.assign(
        reinterpret_cast<const char *>(bytes.data() + offset), size);
    offset += size;
    return true;
  }
};

void hashBytes(uint64_t &hash, const void *data, const size_t size)
{
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0u; index < size; ++index)
  {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
}

template <typename T>
void hashValue(uint64_t &hash, const T &value)
{
  hashBytes(hash, &value, sizeof(value));
}

void hashString(uint64_t &hash, const std::string &value)
{
  const uint64_t size = value.size();
  hashValue(hash, size);
  hashBytes(hash, value.data(), value.size());
}

} // namespace

bool VoxelMaterialSurfaceDefaults::operator==(
    const VoxelMaterialSurfaceDefaults &other) const
{
  return primaryAlbedoRGBA8 == other.primaryAlbedoRGBA8 &&
      secondaryAlbedoRGBA8 == other.secondaryAlbedoRGBA8 &&
      tertiaryAlbedoRGBA8 == other.tertiaryAlbedoRGBA8 &&
      quaternaryAlbedoRGBA8 == other.quaternaryAlbedoRGBA8 &&
      roughness == other.roughness && metallic == other.metallic &&
      emissive == other.emissive;
}

bool VoxelMaterialAsset::validate(std::string *error) const
{
  if (error != nullptr)
    error->clear();
  if (version != kVoxelMaterialAssetVersion)
    return fail(error, "Unsupported voxel material asset version");
  if (assetId.empty())
    return fail(error, "Voxel material asset ID is empty");
  if (!std::isfinite(surface.roughness) || surface.roughness < 0.0f ||
      surface.roughness > 1.0f || !std::isfinite(surface.metallic) ||
      surface.metallic < 0.0f || surface.metallic > 1.0f ||
      !std::isfinite(surface.emissive) || surface.emissive < 0.0f)
    return fail(error, "Voxel material surface defaults are invalid");
  if ((contextRequirements.fields & ~kVoxelMaterialKnownContextFields) != 0u)
    return fail(error, "Voxel material context requirements are invalid");
  svraster::SparseVoxelMicroMaterialGPU packed{};
  if (!svraster::packSparseVoxelMicroMaterial(
          geometry, legacyShading, packed, error))
    return false;
  if (!verifyVoxelShadingProgram(shadingProgram, error))
    return false;
  if (!verifyVoxelGeometryProgram(geometryProgram, error))
    return false;
  if (!validateVoxelMaterialFieldBindings(fieldBindings, error))
    return false;
  for (const float value : shadingParameters.values)
    if (!std::isfinite(value))
      return fail(error, "Voxel shading parameter is non-finite");
  for (const int32_t value : geometryParameters.values)
    if (value < -kVoxelGeometryFixedOne || value > kVoxelGeometryFixedOne)
      return fail(error, "Voxel geometry parameter exceeds the fixed range");
  if (voxelShadingProgramUsesTerrainSlope(shadingProgram) &&
      !contextRequirements.contains(VoxelMaterialContextField::TerrainSlope))
    return fail(
        error, "Voxel shading program reads undeclared terrain slope context");
  if ((voxelGeometryRequiredContextFields(geometryProgram) &
       ~contextRequirements.fields) != 0u)
  {
    if (error != nullptr)
      *error = "Voxel geometry program requires unavailable context: " +
          voxelMaterialContextFieldNames(
              voxelGeometryRequiredContextFields(geometryProgram) &
              ~contextRequirements.fields);
    return false;
  }
  const uint32_t requiredMaterialFields =
      voxelShadingRequiredMaterialFields(shadingProgram) |
      voxelGeometryRequiredMaterialFields(geometryProgram);
  for (uint32_t slot = 0u; slot < kVoxelMaterialFieldCount; ++slot)
    if ((requiredMaterialFields & (1u << slot)) != 0u &&
        fieldBindings[slot].semantic == VoxelMaterialFieldSemantic::Unused)
      return fail(error, "Voxel material program reads an undeclared material field");
  return true;
}

bool VoxelMaterialAsset::definition(
    VoxelMaterialDefinition &result, std::string *error) const
{
  result = {};
  if (!validate(error))
    return false;
  svraster::SparseVoxelMicroMaterialGPU packed{};
  if (!svraster::packSparseVoxelMicroMaterial(
          geometry, legacyShading, packed, error))
    return false;
  result = {assetId, name, packed, contextRequirements, slotPolicy,
            shadingProgram, shadingParameters, geometryProgram,
            geometryParameters, fieldBindings,
            VoxelShadingPaletteGPU{{surface.tertiaryAlbedoRGBA8,
                                    surface.quaternaryAlbedoRGBA8}}};
  return true;
}

uint64_t VoxelMaterialAsset::contentHash() const
{
  uint64_t hash = 1469598103934665603ull;
  hashValue(hash, version);
  hashString(hash, assetId);
  hashString(hash, name);
  hashValue(hash, surface.primaryAlbedoRGBA8);
  hashValue(hash, surface.secondaryAlbedoRGBA8);
  hashValue(hash, surface.tertiaryAlbedoRGBA8);
  hashValue(hash, surface.quaternaryAlbedoRGBA8);
  hashValue(hash, surface.roughness);
  hashValue(hash, surface.metallic);
  hashValue(hash, surface.emissive);
  svraster::SparseVoxelMicroMaterialGPU packed{};
  std::string ignored;
  svraster::packSparseVoxelMicroMaterial(
      geometry, legacyShading, packed, &ignored);
  hashValue(hash, packed.geometry);
  hashValue(hash, packed.shading);
  hashValue(hash, contextRequirements.fields);
  hashValue(hash, slotPolicy);
  const uint64_t instructionCount = shadingProgram.instructions.size();
  hashValue(hash, instructionCount);
  for (const VoxelShadingInstruction &instruction :
       shadingProgram.instructions)
  {
    hashValue(hash, instruction.opcode);
    hashValue(hash, instruction.destination);
    hashValue(hash, instruction.source0);
    hashValue(hash, instruction.source1);
    hashValue(hash, instruction.immediate0);
    hashValue(hash, instruction.immediate1);
  }
  for (const float value : shadingParameters.values)
    hashValue(hash, value);
  hashValue(hash, geometryProgram.domain);
  hashValue(hash, geometryProgram.executionHint);
  const uint64_t geometryInstructionCount = geometryProgram.instructions.size();
  hashValue(hash, geometryInstructionCount);
  for (const VoxelGeometryInstruction &instruction :
       geometryProgram.instructions)
  {
    hashValue(hash, instruction.opcode);
    hashValue(hash, instruction.destination);
    hashValue(hash, instruction.source0);
    hashValue(hash, instruction.source1);
    hashValue(hash, instruction.immediate);
  }
  for (const int32_t value : geometryParameters.values)
    hashValue(hash, value);
  for (const VoxelMaterialFieldBinding &binding : fieldBindings)
  {
    hashValue(hash, binding.semantic);
    hashValue(hash, binding.source);
    hashValue(hash, binding.mapping);
    hashValue(hash, binding.filter);
    hashValue(hash, binding.addressMode);
    hashValue(hash, binding.defaultValue);
    hashValue(hash, binding.scale);
    hashValue(hash, binding.bias);
    hashString(hash, binding.textureAssetId);
  }
  return hash;
}

bool VoxelMaterialCompilerCache::compile(
    const VoxelMaterialAsset &asset,
    const VoxelMaterialCompiledAsset *&result, std::string *error)
{
  result = nullptr;
  if (error != nullptr)
    error->clear();
  const uint64_t hash = asset.contentHash();
  const auto found = entries_.find(hash);
  if (found != entries_.end())
  {
    ++hitCount_;
    result = &found->second;
    return true;
  }

  VoxelMaterialCompiledAsset compiled{};
  compiled.sourceHash = hash;
  if (!asset.definition(compiled.definition, error))
    return false;
  const auto legacyGeometry =
      svraster::unpackSparseVoxelMicroGeometry(compiled.definition.descriptor);
  if (!compileVoxelGeometryProgram(
          asset.geometryProgram,
          static_cast<uint32_t>(legacyGeometry.geometryProfile),
          compiled.geometry, error))
    return false;
  if (!compileVoxelShadingProgram(
          asset.shadingProgram, compiled.shadingInstructions, error))
    return false;
  auto inserted = entries_.emplace(hash, std::move(compiled));
  ++missCount_;
  result = &inserted.first->second;
  return true;
}

void VoxelMaterialCompilerCache::clear()
{
  entries_.clear();
  hitCount_ = 0u;
  missCount_ = 0u;
}

bool serializeVoxelMaterialAsset(
    const VoxelMaterialAsset &asset, std::vector<uint8_t> &bytes,
    std::string *error)
{
  bytes.clear();
  if (!asset.validate(error))
    return false;
  if (asset.assetId.size() > kMaximumVoxelMaterialStringBytes ||
      asset.name.size() > kMaximumVoxelMaterialStringBytes)
    return fail(error, "Voxel material string exceeds the file limit");

  std::vector<uint8_t> payload;
  appendString(payload, asset.assetId);
  appendString(payload, asset.name);
  appendU32(payload, asset.surface.primaryAlbedoRGBA8);
  appendU32(payload, asset.surface.secondaryAlbedoRGBA8);
  appendFloat(payload, asset.surface.roughness);
  appendFloat(payload, asset.surface.metallic);
  appendFloat(payload, asset.surface.emissive);
  svraster::SparseVoxelMicroMaterialGPU legacy{};
  if (!svraster::packSparseVoxelMicroMaterial(
          asset.geometry, asset.legacyShading, legacy, error))
    return false;
  appendU32(payload, legacy.geometry);
  appendU32(payload, legacy.shading);
  appendU32(payload, asset.contextRequirements.fields);
  appendU32(payload, static_cast<uint32_t>(asset.slotPolicy));
  appendU32(payload, static_cast<uint32_t>(asset.shadingProgram.instructions.size()));
  for (const VoxelShadingInstruction &instruction :
       asset.shadingProgram.instructions)
  {
    appendU32(payload, static_cast<uint32_t>(instruction.opcode));
    appendU32(payload, instruction.destination);
    appendU32(payload, instruction.source0);
    appendU32(payload, instruction.source1);
    appendFloat(payload, instruction.immediate0);
    appendFloat(payload, instruction.immediate1);
  }
  for (const float value : asset.shadingParameters.values)
    appendFloat(payload, value);
  appendU32(payload, static_cast<uint32_t>(asset.geometryProgram.domain));
  appendU32(payload, static_cast<uint32_t>(asset.geometryProgram.executionHint));
  appendU32(payload, static_cast<uint32_t>(asset.geometryProgram.instructions.size()));
  for (const VoxelGeometryInstruction &instruction :
       asset.geometryProgram.instructions)
  {
    appendU32(payload, static_cast<uint32_t>(instruction.opcode));
    appendU32(payload, instruction.destination);
    appendU32(payload, instruction.source0);
    appendU32(payload, instruction.source1);
    appendU32(payload, static_cast<uint32_t>(instruction.immediate));
  }
  for (const int32_t value : asset.geometryParameters.values)
    appendU32(payload, static_cast<uint32_t>(value));
  for (const VoxelMaterialFieldBinding &binding : asset.fieldBindings)
  {
    appendU32(payload, static_cast<uint32_t>(binding.semantic));
    appendU32(payload, static_cast<uint32_t>(binding.source));
    appendU32(payload, static_cast<uint32_t>(binding.mapping));
    appendU32(payload, static_cast<uint32_t>(binding.filter));
    appendU32(payload, static_cast<uint32_t>(binding.addressMode));
    appendFloat(payload, binding.defaultValue);
    appendFloat(payload, binding.scale);
    appendFloat(payload, binding.bias);
    appendString(payload, binding.textureAssetId);
  }
  appendU32(payload, asset.surface.tertiaryAlbedoRGBA8);
  appendU32(payload, asset.surface.quaternaryAlbedoRGBA8);

  appendU64(bytes, kVoxelMaterialFileMagic);
  appendU32(bytes, kVoxelMaterialFileVersion);
  appendU32(bytes, asset.version);
  appendU64(bytes, payload.size());
  appendU64(bytes, payloadChecksum(payload.data(), payload.size()));
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return true;
}

bool deserializeVoxelMaterialAsset(
    const std::vector<uint8_t> &bytes, VoxelMaterialAsset &asset,
    std::string *error)
{
  asset = {};
  if (error != nullptr)
    error->clear();
  if (bytes.size() < kVoxelMaterialFileHeaderBytes)
    return fail(error, "Voxel material file header is truncated");
  AssetReader reader{bytes};
  uint64_t magic = 0u;
  uint32_t fileVersion = 0u;
  uint32_t assetVersion = 0u;
  uint64_t payloadSize = 0u;
  uint64_t checksum = 0u;
  if (!reader.readU64(magic) || !reader.readU32(fileVersion) ||
      !reader.readU32(assetVersion) || !reader.readU64(payloadSize) ||
      !reader.readU64(checksum))
    return fail(error, "Voxel material file header is truncated");
  if (magic != kVoxelMaterialFileMagic ||
      fileVersion != kVoxelMaterialFileVersion)
    return fail(error, "Unsupported voxel material file version");
  if (assetVersion == 0u || assetVersion > kVoxelMaterialAssetVersion)
    return fail(error, "Unsupported voxel material asset version");
  if (payloadSize != bytes.size() - reader.offset ||
      checksum != payloadChecksum(bytes.data() + reader.offset,
                                  static_cast<size_t>(payloadSize)))
    return fail(error, "Voxel material file checksum or size is invalid");

  VoxelMaterialAsset result{};
  // Version-one assets had no material fields; version two had no auxiliary
  // palette colours. Both migrate to the current bounded defaults.
  result.version = kVoxelMaterialAssetVersion;
  uint32_t legacyGeometry = 0u;
  uint32_t legacyShading = 0u;
  uint32_t slotPolicy = 0u;
  uint32_t shadingCount = 0u;
  if (!reader.readString(result.assetId) || !reader.readString(result.name) ||
      !reader.readU32(result.surface.primaryAlbedoRGBA8) ||
      !reader.readU32(result.surface.secondaryAlbedoRGBA8) ||
      !reader.readFloat(result.surface.roughness) ||
      !reader.readFloat(result.surface.metallic) ||
      !reader.readFloat(result.surface.emissive) ||
      !reader.readU32(legacyGeometry) || !reader.readU32(legacyShading) ||
      !reader.readU32(result.contextRequirements.fields) ||
      !reader.readU32(slotPolicy) || !reader.readU32(shadingCount) ||
      shadingCount > kMaximumVoxelShadingInstructionCount || slotPolicy > 1u)
    return fail(error, "Voxel material payload is corrupt");
  result.slotPolicy = static_cast<VoxelMaterialSlotPolicy>(slotPolicy);
  const svraster::SparseVoxelMicroMaterialGPU legacy{
    legacyGeometry, legacyShading};
  result.geometry = svraster::unpackSparseVoxelMicroGeometry(legacy);
  result.legacyShading = svraster::unpackSparseVoxelMicroShading(legacy);
  result.shadingProgram.instructions.resize(shadingCount);
  for (uint32_t index = 0u; index < shadingCount; ++index)
  {
    auto &instruction = result.shadingProgram.instructions[index];
    uint32_t opcode = 0u;
    if (!reader.readU32(opcode) || !reader.readU32(instruction.destination) ||
        !reader.readU32(instruction.source0) ||
        !reader.readU32(instruction.source1) ||
        !reader.readFloat(instruction.immediate0) ||
        !reader.readFloat(instruction.immediate1))
      return fail(error, "Voxel material shading program is truncated");
    instruction.opcode = static_cast<VoxelShadingOpcode>(opcode);
  }
  for (float &value : result.shadingParameters.values)
    if (!reader.readFloat(value))
      return fail(error, "Voxel material shading parameters are truncated");
  uint32_t domain = 0u;
  uint32_t execution = 0u;
  uint32_t geometryCount = 0u;
  if (!reader.readU32(domain) || !reader.readU32(execution) ||
      !reader.readU32(geometryCount) ||
      domain > static_cast<uint32_t>(VoxelGeometryDomain::Volume) ||
      execution > static_cast<uint32_t>(VoxelGeometryExecutionHint::Cached) ||
      geometryCount > kMaximumVoxelGeometryInstructionCount)
    return fail(error, "Voxel material geometry header is corrupt");
  result.geometryProgram.domain = static_cast<VoxelGeometryDomain>(domain);
  result.geometryProgram.executionHint =
      static_cast<VoxelGeometryExecutionHint>(execution);
  result.geometryProgram.instructions.resize(geometryCount);
  for (uint32_t index = 0u; index < geometryCount; ++index)
  {
    auto &instruction = result.geometryProgram.instructions[index];
    uint32_t opcode = 0u;
    uint32_t immediate = 0u;
    if (!reader.readU32(opcode) || !reader.readU32(instruction.destination) ||
        !reader.readU32(instruction.source0) ||
        !reader.readU32(instruction.source1) || !reader.readU32(immediate))
      return fail(error, "Voxel material geometry program is truncated");
    instruction.opcode = static_cast<VoxelGeometryOpcode>(opcode);
    instruction.immediate = static_cast<int32_t>(immediate);
  }
  for (int32_t &value : result.geometryParameters.values)
  {
    uint32_t bits = 0u;
    if (!reader.readU32(bits))
      return fail(error, "Voxel material geometry parameters are truncated");
    value = static_cast<int32_t>(bits);
  }
  if (assetVersion >= 2u)
  {
    for (VoxelMaterialFieldBinding &binding : result.fieldBindings)
    {
      uint32_t semantic = 0u;
      uint32_t source = 0u;
      uint32_t mapping = 0u;
      uint32_t filter = 0u;
      uint32_t addressMode = 0u;
      if (!reader.readU32(semantic) || !reader.readU32(source) ||
          !reader.readU32(mapping) || !reader.readU32(filter) ||
          !reader.readU32(addressMode) ||
          !reader.readFloat(binding.defaultValue) ||
          !reader.readFloat(binding.scale) || !reader.readFloat(binding.bias) ||
          !reader.readString(binding.textureAssetId))
        return fail(error, "Voxel material field bindings are truncated");
      binding.semantic = static_cast<VoxelMaterialFieldSemantic>(semantic);
      binding.source = static_cast<VoxelMaterialFieldSource>(source);
      binding.mapping = static_cast<VoxelMaterialFieldMapping>(mapping);
      binding.filter = static_cast<VoxelMaterialFieldFilter>(filter);
      binding.addressMode =
          static_cast<VoxelMaterialFieldAddressMode>(addressMode);
    }
  }
  if (assetVersion >= 3u &&
      (!reader.readU32(result.surface.tertiaryAlbedoRGBA8) ||
       !reader.readU32(result.surface.quaternaryAlbedoRGBA8)))
    return fail(error, "Voxel material palette is truncated");
  if (reader.offset != bytes.size())
    return fail(error, "Voxel material payload has trailing data");
  if (!result.validate(error))
    return false;
  asset = std::move(result);
  return true;
}

bool writeVoxelMaterialAsset(
    const std::string &path, const VoxelMaterialAsset &asset,
    std::string *error)
{
  std::vector<uint8_t> bytes;
  if (!serializeVoxelMaterialAsset(asset, bytes, error))
    return false;
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream)
    return fail(error, "Could not open voxel material file for writing");
  stream.write(
      reinterpret_cast<const char *>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
  if (!stream)
    return fail(error, "Could not write voxel material file");
  return true;
}

bool readVoxelMaterialAsset(
    const std::string &path, VoxelMaterialAsset &asset, std::string *error)
{
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return fail(error, "Could not open voxel material file");
  const std::vector<uint8_t> bytes{
    std::istreambuf_iterator<char>(stream),
    std::istreambuf_iterator<char>()};
  if (!stream.eof() && stream.fail())
    return fail(error, "Could not read voxel material file");
  return deserializeVoxelMaterialAsset(bytes, asset, error);
}

bool adaptLegacyVoxelMaterialAsset(
    const std::string &assetId, const std::string &name,
    const svraster::SparseVoxelMicroMaterialGPU &legacy,
    VoxelMaterialAsset &asset, std::string *error)
{
  asset = {};
  asset.assetId = assetId;
  asset.name = name;
  asset.geometry = svraster::unpackSparseVoxelMicroGeometry(legacy);
  asset.legacyShading = svraster::unpackSparseVoxelMicroShading(legacy);
  return asset.validate(error);
}

bool loadVoxelMaterialAsset(
    VoxelMaterialManager &manager, VoxelMaterialCompilerCache &cache,
    const VoxelMaterialAsset &asset, VoxelMaterialHandle &handle,
    std::string *error)
{
  const VoxelMaterialCompiledAsset *compiled = nullptr;
  if (!cache.compile(asset, compiled, error))
    return false;
  return manager.registerMaterial(compiled->definition, handle, error);
}

bool hotReloadVoxelMaterialAsset(
    VoxelMaterialManager &manager, VoxelMaterialCompilerCache &cache,
    const VoxelMaterialHandle handle, const VoxelMaterialAsset &asset,
    VoxelMaterialChange &changes, std::string *error)
{
  changes = VoxelMaterialChange::None;
  const VoxelMaterialCompiledAsset *compiled = nullptr;
  if (!cache.compile(asset, compiled, error))
    return false;
  if (manager.handleForAsset(asset.assetId) != handle)
    return fail(error, "Voxel material hot reload asset ID does not match the stable slot");
  const VoxelMaterialSlot &oldSlot = manager.slot(handle);
  const VoxelMaterialContextRequirements transitional{
    oldSlot.contextRequirements.fields |
    compiled->definition.contextRequirements.fields};
  VoxelMaterialChange local{};
  if (!manager.updateMaterial(
          handle, compiled->definition.descriptor, transitional, local, error))
    return false;
  changes |= local;
  VoxelMaterialFieldBindings transitionalFields =
      compiled->definition.fieldBindings;
  const uint32_t oldRequiredFields =
      voxelShadingRequiredMaterialFields(oldSlot.shadingProgram) |
      voxelGeometryRequiredMaterialFields(oldSlot.geometryProgram);
  for (uint32_t slot = 0u; slot < kVoxelMaterialFieldCount; ++slot)
    if ((oldRequiredFields & (1u << slot)) != 0u &&
        transitionalFields[slot].semantic == VoxelMaterialFieldSemantic::Unused)
      transitionalFields[slot] = oldSlot.fieldBindings[slot];
  if (!manager.updateFieldBindings(
          handle, transitionalFields, local, error))
    return false;
  changes |= local;
  if (!manager.updateShadingProgram(
          handle, compiled->definition.shadingProgram, local, error))
    return false;
  changes |= local;
  if (!manager.updateShadingParameters(
          handle, compiled->definition.shadingParameters, local, error))
    return false;
  changes |= local;
  if (!manager.updateShadingPalette(
          handle, compiled->definition.shadingPalette, local, error))
    return false;
  changes |= local;
  if (!manager.updateGeometryProgram(
          handle, compiled->definition.geometryProgram, local, error))
    return false;
  changes |= local;
  if (!manager.updateGeometryParameters(
          handle, compiled->definition.geometryParameters, local, error))
    return false;
  changes |= local;
  if (transitionalFields != compiled->definition.fieldBindings &&
      !manager.updateFieldBindings(
          handle, compiled->definition.fieldBindings, local, error))
    return false;
  changes |= local;
  if (transitional != compiled->definition.contextRequirements &&
      !manager.updateMaterial(
          handle, compiled->definition.descriptor,
          compiled->definition.contextRequirements, local, error))
    return false;
  changes |= local;
  return true;
}

} // namespace voxel::material
