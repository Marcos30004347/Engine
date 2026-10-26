#include "voxel/material/VoxelMaterial.hpp"

#include <algorithm>
#include <cmath>

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

bool validRequirements(const VoxelMaterialContextRequirements requirements)
{
  return (requirements.fields & ~kVoxelMaterialKnownContextFields) == 0u;
}

bool programRequirementsSatisfied(
    const VoxelShadingProgram &program,
    const VoxelMaterialContextRequirements requirements)
{
  return !voxelShadingProgramUsesTerrainSlope(program) ||
      requirements.contains(VoxelMaterialContextField::TerrainSlope);
}

bool geometryRequirementsSatisfied(
    const VoxelGeometryProgram &program,
    const VoxelMaterialContextRequirements requirements)
{
  const uint32_t required = voxelGeometryRequiredContextFields(program);
  return (required & ~requirements.fields) == 0u;
}

bool materialFieldRequirementsSatisfied(
    const uint32_t required, const VoxelMaterialFieldBindings &bindings)
{
  for (uint32_t slot = 0u; slot < kVoxelMaterialFieldCount; ++slot)
    if ((required & (1u << slot)) != 0u &&
        bindings[slot].semantic == VoxelMaterialFieldSemantic::Unused)
      return false;
  return true;
}

bool geometryFieldSamplingSatisfied(
    const uint32_t required, const VoxelMaterialFieldBindings &bindings,
    std::string *error)
{
  for (uint32_t slot = 0u; slot < kVoxelMaterialFieldCount; ++slot)
    if ((required & (1u << slot)) != 0u &&
        bindings[slot].source != VoxelMaterialFieldSource::Constant &&
        bindings[slot].filter != VoxelMaterialFieldFilter::Nearest)
      return fail(
          error,
          "Voxel geometry fields require fixed-mip nearest sampling");
  return true;
}

bool sameGeometry(const svraster::SparseVoxelMicroMaterialGPU &left, const svraster::SparseVoxelMicroMaterialGPU &right)
{
  const auto a = svraster::unpackSparseVoxelMicroGeometry(left);
  const auto b = svraster::unpackSparseVoxelMicroGeometry(right);
  return a.subdivision == b.subdivision && a.maximumCarveDepth == b.maximumCarveDepth && a.carveDepthFraction == b.carveDepthFraction && a.faceMask == b.faceMask && a.geometryProfile == b.geometryProfile &&
         a.materialSeed == b.materialSeed;
}

bool sameShading(const svraster::SparseVoxelMicroMaterialGPU &left, const svraster::SparseVoxelMicroMaterialGPU &right)
{
  const auto a = svraster::unpackSparseVoxelMicroShading(left);
  const auto b = svraster::unpackSparseVoxelMicroShading(right);
  return a.profile == b.profile && a.variationStrength == b.variationStrength && a.parameter0 == b.parameter0 && a.parameter1 == b.parameter1 && a.lodHandoff == b.lodHandoff;
}

} // namespace

VoxelMaterialManager::VoxelMaterialManager()
    : slots_(1u), descriptors_(1u), shadingPrograms_(1u),
      shadingInstructions_(kMaximumVoxelShadingInstructionCount),
      shadingParameters_(1u), shadingPalettes_(1u), geometryPrograms_(1u),
      geometryInstructions_(kMaximumVoxelGeometryInstructionCount),
      geometryParameters_(1u), materialFieldDefaults_(1u)
{
  // Handle zero is immutable, unoccupied, and always resolves to the canonical
  // disabled descriptor.
}

bool VoxelMaterialManager::registerMaterial(const VoxelMaterialDefinition &definition, VoxelMaterialHandle &handle, std::string *error)
{
  handle = kInvalidVoxelMaterialHandle;
  if (error != nullptr)
    error->clear();
  if (definition.assetId.empty())
    return fail(error, "Voxel material asset ID is empty");
  if (!svraster::sparseVoxelMicroMaterialIsValid(definition.descriptor))
    return fail(error, "Voxel material descriptor is invalid");
  if (!validRequirements(definition.contextRequirements))
    return fail(error, "Voxel material context requirements are invalid");
  if (!validateVoxelMaterialFieldBindings(definition.fieldBindings, error))
    return false;
  std::vector<VoxelShadingInstructionGPU> compiledProgram;
  if (!compileVoxelShadingProgram(
          definition.shadingProgram, compiledProgram, error))
    return false;
  if (!programRequirementsSatisfied(
          definition.shadingProgram, definition.contextRequirements))
    return fail(error, "Voxel shading program requires unavailable context: " +
        voxelMaterialContextFieldNames(static_cast<uint32_t>(
            VoxelMaterialContextField::TerrainSlope) &
            ~definition.contextRequirements.fields));
  VoxelGeometryCompilation compiledGeometry{};
  const auto legacyGeometry =
      svraster::unpackSparseVoxelMicroGeometry(definition.descriptor);
  if (!compileVoxelGeometryProgram(
          definition.geometryProgram,
          static_cast<uint32_t>(legacyGeometry.geometryProfile),
          compiledGeometry, error))
    return false;
  if (!geometryRequirementsSatisfied(
          definition.geometryProgram, definition.contextRequirements))
    return fail(error, "Voxel geometry program requires unavailable context: " +
        voxelMaterialContextFieldNames(
            voxelGeometryRequiredContextFields(definition.geometryProgram) &
            ~definition.contextRequirements.fields));
  if (!materialFieldRequirementsSatisfied(
          voxelShadingRequiredMaterialFields(definition.shadingProgram) |
              voxelGeometryRequiredMaterialFields(definition.geometryProgram),
          definition.fieldBindings))
    return fail(error, "Voxel material program reads an undeclared material field");
  if (!geometryFieldSamplingSatisfied(
          voxelGeometryRequiredMaterialFields(definition.geometryProgram),
          definition.fieldBindings, error))
    return false;
  for (const int32_t value : definition.geometryParameters.values)
    if (value < -kVoxelGeometryFixedOne || value > kVoxelGeometryFixedOne)
      return fail(error, "Voxel geometry parameter exceeds the fixed range");

  const auto existingAsset = assetHandles_.find(definition.assetId);
  if (existingAsset != assetHandles_.end())
  {
    const VoxelMaterialSlot &existing = slot(existingAsset->second);
    const bool requestedImmutable = definition.slotPolicy == VoxelMaterialSlotPolicy::SharedImmutable;
    if (existing.descriptor.geometry != definition.descriptor.geometry || existing.descriptor.shading != definition.descriptor.shading || existing.contextRequirements != definition.contextRequirements || existing.shadingProgram != definition.shadingProgram || existing.shadingParameters != definition.shadingParameters || existing.geometryProgram != definition.geometryProgram || existing.geometryParameters != definition.geometryParameters || existing.fieldBindings != definition.fieldBindings || existing.shadingPalette != definition.shadingPalette ||
        existing.immutable != requestedImmutable)
      return fail(error, "Voxel material asset ID is already registered with different contents");
    handle = existingAsset->second;
    return true;
  }

  const bool immutable = definition.slotPolicy == VoxelMaterialSlotPolicy::SharedImmutable;
  const SharedSlotKey sharedKey{
    definition.descriptor.geometry,
    definition.descriptor.shading,
    definition.contextRequirements.fields,
    definition.shadingProgram,
    definition.shadingParameters,
    definition.geometryProgram,
    definition.geometryParameters,
    definition.fieldBindings,
    definition.shadingPalette,
  };
  if (immutable)
  {
    const auto shared = sharedImmutableSlots_.find(sharedKey);
    if (shared != sharedImmutableSlots_.end())
    {
      assetHandles_.emplace(definition.assetId, shared->second);
      handle = shared->second;
      return true;
    }
  }

  if (slots_.size() > kMaximumVoxelMaterialHandle)
    return fail(error, "Voxel material stable slot table is full");

  handle = static_cast<VoxelMaterialHandle>(slots_.size());
  VoxelMaterialSlot material{};
  material.debugName = definition.name.empty() ? definition.assetId : definition.name;
  material.descriptor = definition.descriptor;
  material.contextRequirements = definition.contextRequirements;
  material.shadingProgram = definition.shadingProgram;
  material.shadingParameters = definition.shadingParameters;
  material.geometryProgram = definition.geometryProgram;
  material.geometryParameters = definition.geometryParameters;
  material.fieldBindings = definition.fieldBindings;
  material.shadingPalette = definition.shadingPalette;
  material.geometryRevision = 1u;
  material.shadingRevision = 1u;
  material.contextRevision = 1u;
  material.occupied = true;
  material.immutable = immutable;
  slots_.push_back(material);
  descriptors_.push_back(definition.descriptor);
  VoxelShadingProgramGPU programRecord{};
  programRecord.instructionOffset = static_cast<uint32_t>(handle) *
      kMaximumVoxelShadingInstructionCount;
  programRecord.instructionCount =
      static_cast<uint32_t>(compiledProgram.size());
  programRecord.flags = compiledProgram.empty()
      ? 0u : kVoxelShadingProgramEnabled |
          (voxelShadingAdditionalPaletteMask(definition.shadingProgram)
           << kVoxelShadingProgramPaletteShift) |
          (voxelShadingRequiredMaterialFields(definition.shadingProgram)
           << kVoxelShadingProgramMaterialFieldShift);
  shadingPrograms_.push_back(programRecord);
  shadingInstructions_.resize(
      (static_cast<size_t>(handle) + 1u) *
      kMaximumVoxelShadingInstructionCount);
  std::copy(
      compiledProgram.begin(), compiledProgram.end(),
      shadingInstructions_.begin() + programRecord.instructionOffset);
  shadingParameters_.push_back(definition.shadingParameters);
  shadingPalettes_.push_back(definition.shadingPalette);
  compiledGeometry.gpu.instructionOffset = static_cast<uint32_t>(handle) *
      kMaximumVoxelGeometryInstructionCount;
  geometryPrograms_.push_back(compiledGeometry.gpu);
  geometryInstructions_.resize(
      (static_cast<size_t>(handle) + 1u) *
      kMaximumVoxelGeometryInstructionCount);
  std::copy(
      compiledGeometry.instructions.begin(), compiledGeometry.instructions.end(),
      geometryInstructions_.begin() + compiledGeometry.gpu.instructionOffset);
  geometryParameters_.push_back(definition.geometryParameters);
  materialFieldDefaults_.push_back(
      compileVoxelMaterialFieldDefaults(definition.fieldBindings));
  assetHandles_.emplace(definition.assetId, handle);
  if (immutable)
    sharedImmutableSlots_.emplace(sharedKey, handle);
  ++contentRevision_;
  ++geometryRevision_;
  ++shadingRevision_;
  ++contextRevision_;
  return true;
}

bool VoxelMaterialManager::importLegacyMaterial(
    const std::string &assetId,
    const std::string &name,
    const uint32_t sourceMaterialId,
    const svraster::SparseVoxelMicroMaterialLibrary &library,
    VoxelMaterialHandle &handle,
    const VoxelMaterialSlotPolicy policy,
    const VoxelMaterialContextRequirements requirements,
    std::string *error)
{
  const uint16_t descriptorId = library.descriptorId(sourceMaterialId);
  if (!registerMaterial(
      VoxelMaterialDefinition{
        assetId,
        name,
        library.descriptor(descriptorId),
        requirements,
        policy,
      },
      handle,
      error))
    return false;
  return bindSourceMaterial(sourceMaterialId, handle, error);
}

bool VoxelMaterialManager::updateMaterial(
    const VoxelMaterialHandle handle,
    const svraster::SparseVoxelMicroMaterialGPU &descriptor,
    const VoxelMaterialContextRequirements requirements,
    VoxelMaterialChange &changes,
    std::string *error)
{
  changes = VoxelMaterialChange::None;
  if (error != nullptr)
    error->clear();
  if (!validHandle(handle))
    return fail(error, "Voxel material handle is invalid");
  if (slots_[handle].immutable)
    return fail(error, "Voxel material slot is immutable");
  if (!svraster::sparseVoxelMicroMaterialIsValid(descriptor))
    return fail(error, "Voxel material descriptor is invalid");
  if (!validRequirements(requirements))
    return fail(error, "Voxel material context requirements are invalid");
  if (!programRequirementsSatisfied(slots_[handle].shadingProgram, requirements))
    return fail(
        error,
        "Voxel material context no longer satisfies its shading program");
  if (!geometryRequirementsSatisfied(slots_[handle].geometryProgram, requirements))
    return fail(
        error,
        "Voxel material context no longer satisfies its geometry program");

  VoxelMaterialSlot &material = slots_[handle];
  if (!sameGeometry(material.descriptor, descriptor))
    changes |= VoxelMaterialChange::Geometry;
  if (!sameShading(material.descriptor, descriptor))
    changes |= VoxelMaterialChange::Shading;
  if (material.contextRequirements != requirements)
    changes |= VoxelMaterialChange::ContextLayout;
  if (changes == VoxelMaterialChange::None)
    return true;

  material.descriptor = descriptor;
  material.contextRequirements = requirements;
  descriptors_[handle] = descriptor;
  if (material.geometryProgram.instructions.empty())
  {
    VoxelGeometryCompilation builtIn = compileVoxelGeometryBuiltIn(
        svraster::unpackSparseVoxelMicroGeometry(descriptor));
    builtIn.gpu.instructionOffset = static_cast<uint32_t>(handle) *
        kMaximumVoxelGeometryInstructionCount;
    geometryPrograms_[handle] = builtIn.gpu;
  }
  ++contentRevision_;
  if (voxelMaterialChangeContains(changes, VoxelMaterialChange::Geometry))
  {
    ++material.geometryRevision;
    ++geometryRevision_;
  }
  if (voxelMaterialChangeContains(changes, VoxelMaterialChange::Shading))
  {
    ++material.shadingRevision;
    ++shadingRevision_;
  }
  if (voxelMaterialChangeContains(changes, VoxelMaterialChange::ContextLayout))
  {
    ++material.contextRevision;
    ++contextRevision_;
  }
  return true;
}

bool VoxelMaterialManager::updateGeometry(const VoxelMaterialHandle handle, const svraster::SparseVoxelMicroDescriptor &geometry, VoxelMaterialChange &changes, std::string *error)
{
  if (!validHandle(handle))
  {
    changes = VoxelMaterialChange::None;
    return fail(error, "Voxel material handle is invalid");
  }
  const VoxelMaterialSlot &material = slots_[handle];
  svraster::SparseVoxelMicroMaterialGPU packed{};
  if (!svraster::packSparseVoxelMicroMaterial(geometry, svraster::unpackSparseVoxelMicroShading(material.descriptor), packed, error))
  {
    changes = VoxelMaterialChange::None;
    return false;
  }
  return updateMaterial(handle, packed, material.contextRequirements, changes, error);
}

bool VoxelMaterialManager::updateShading(const VoxelMaterialHandle handle, const svraster::SparseVoxelMicroShadingDescriptor &shading, VoxelMaterialChange &changes, std::string *error)
{
  if (!validHandle(handle))
  {
    changes = VoxelMaterialChange::None;
    return fail(error, "Voxel material handle is invalid");
  }
  const VoxelMaterialSlot &material = slots_[handle];
  svraster::SparseVoxelMicroMaterialGPU packed{};
  if (!svraster::packSparseVoxelMicroMaterial(svraster::unpackSparseVoxelMicroGeometry(material.descriptor), shading, packed, error))
  {
    changes = VoxelMaterialChange::None;
    return false;
  }
  return updateMaterial(handle, packed, material.contextRequirements, changes, error);
}

bool VoxelMaterialManager::updateShadingProgram(
    const VoxelMaterialHandle handle, const VoxelShadingProgram &program,
    VoxelMaterialChange &changes, std::string *error)
{
  changes = VoxelMaterialChange::None;
  if (error != nullptr)
    error->clear();
  if (!validHandle(handle))
    return fail(error, "Voxel material handle is invalid");
  if (slots_[handle].immutable)
    return fail(error, "Voxel material slot is immutable");
  std::vector<VoxelShadingInstructionGPU> compiledProgram;
  if (!compileVoxelShadingProgram(program, compiledProgram, error))
    return false;
  VoxelMaterialSlot &material = slots_[handle];
  if (!programRequirementsSatisfied(program, material.contextRequirements))
    return fail(
        error,
        "Voxel shading program reads terrain slope without declaring it");
  if (!materialFieldRequirementsSatisfied(
          voxelShadingRequiredMaterialFields(program), material.fieldBindings))
    return fail(error, "Voxel shading program reads an undeclared material field");
  if (material.shadingProgram == program)
    return true;

  VoxelShadingProgramGPU record{};
  record.instructionOffset = static_cast<uint32_t>(handle) *
      kMaximumVoxelShadingInstructionCount;
  record.instructionCount = static_cast<uint32_t>(compiledProgram.size());
  record.flags = compiledProgram.empty()
      ? 0u : kVoxelShadingProgramEnabled |
          (voxelShadingAdditionalPaletteMask(program)
           << kVoxelShadingProgramPaletteShift) |
          (voxelShadingRequiredMaterialFields(program)
           << kVoxelShadingProgramMaterialFieldShift);
  auto destination = shadingInstructions_.begin() + record.instructionOffset;
  std::fill(
      destination,
      destination + kMaximumVoxelShadingInstructionCount,
      VoxelShadingInstructionGPU{});
  std::copy(compiledProgram.begin(), compiledProgram.end(), destination);
  shadingPrograms_[handle] = record;
  material.shadingProgram = program;
  changes = VoxelMaterialChange::Shading;
  ++material.shadingRevision;
  ++shadingRevision_;
  ++contentRevision_;
  return true;
}

bool VoxelMaterialManager::updateShadingParameters(
    const VoxelMaterialHandle handle,
    const VoxelShadingParameterBlockGPU &parameters,
    VoxelMaterialChange &changes, std::string *error)
{
  changes = VoxelMaterialChange::None;
  if (error != nullptr)
    error->clear();
  if (!validHandle(handle))
    return fail(error, "Voxel material handle is invalid");
  if (slots_[handle].immutable)
    return fail(error, "Voxel material slot is immutable");
  for (const float value : parameters.values)
    if (!std::isfinite(value))
      return fail(error, "Voxel shading parameter is non-finite");
  if (slots_[handle].shadingParameters == parameters)
    return true;
  slots_[handle].shadingParameters = parameters;
  shadingParameters_[handle] = parameters;
  changes = VoxelMaterialChange::Shading;
  ++slots_[handle].shadingRevision;
  ++shadingRevision_;
  ++contentRevision_;
  return true;
}

bool VoxelMaterialManager::updateShadingPalette(
    const VoxelMaterialHandle handle,
    const VoxelShadingPaletteGPU &palette,
    VoxelMaterialChange &changes, std::string *error)
{
  changes = VoxelMaterialChange::None;
  if (error != nullptr)
    error->clear();
  if (!validHandle(handle))
    return fail(error, "Voxel material handle is invalid");
  if (slots_[handle].immutable)
    return fail(error, "Voxel material slot is immutable");
  if (slots_[handle].shadingPalette == palette)
    return true;
  slots_[handle].shadingPalette = palette;
  shadingPalettes_[handle] = palette;
  changes = VoxelMaterialChange::Shading;
  ++slots_[handle].shadingRevision;
  ++shadingRevision_;
  ++contentRevision_;
  return true;
}

bool VoxelMaterialManager::updateGeometryProgram(
    const VoxelMaterialHandle handle, const VoxelGeometryProgram &program,
    VoxelMaterialChange &changes, std::string *error)
{
  changes = VoxelMaterialChange::None;
  if (error != nullptr)
    error->clear();
  if (!validHandle(handle))
    return fail(error, "Voxel material handle is invalid");
  if (slots_[handle].immutable)
    return fail(error, "Voxel material slot is immutable");
  const auto legacy =
      svraster::unpackSparseVoxelMicroGeometry(slots_[handle].descriptor);
  VoxelGeometryCompilation compiled{};
  if (!compileVoxelGeometryProgram(
          program, static_cast<uint32_t>(legacy.geometryProfile),
          compiled, error))
    return false;
  if (!geometryRequirementsSatisfied(program, slots_[handle].contextRequirements))
    return fail(error, "Voxel geometry program requires unavailable context: " +
        voxelMaterialContextFieldNames(
            voxelGeometryRequiredContextFields(program) &
            ~slots_[handle].contextRequirements.fields));
  if (!materialFieldRequirementsSatisfied(
          voxelGeometryRequiredMaterialFields(program),
          slots_[handle].fieldBindings))
    return fail(error, "Voxel geometry program reads an undeclared material field");
  if (!geometryFieldSamplingSatisfied(
          voxelGeometryRequiredMaterialFields(program),
          slots_[handle].fieldBindings, error))
    return false;
  if (slots_[handle].geometryProgram == program)
    return true;

  compiled.gpu.instructionOffset = static_cast<uint32_t>(handle) *
      kMaximumVoxelGeometryInstructionCount;
  auto destination = geometryInstructions_.begin() +
      compiled.gpu.instructionOffset;
  std::fill(
      destination, destination + kMaximumVoxelGeometryInstructionCount,
      VoxelGeometryInstructionGPU{});
  std::copy(
      compiled.instructions.begin(), compiled.instructions.end(), destination);
  geometryPrograms_[handle] = compiled.gpu;
  slots_[handle].geometryProgram = program;
  changes = VoxelMaterialChange::Geometry;
  ++slots_[handle].geometryRevision;
  ++geometryRevision_;
  ++contentRevision_;
  return true;
}

bool VoxelMaterialManager::updateGeometryParameters(
    const VoxelMaterialHandle handle,
    const VoxelGeometryParameterBlockGPU &parameters,
    VoxelMaterialChange &changes, std::string *error)
{
  changes = VoxelMaterialChange::None;
  if (error != nullptr)
    error->clear();
  if (!validHandle(handle))
    return fail(error, "Voxel material handle is invalid");
  if (slots_[handle].immutable)
    return fail(error, "Voxel material slot is immutable");
  for (const int32_t value : parameters.values)
    if (value < -kVoxelGeometryFixedOne || value > kVoxelGeometryFixedOne)
      return fail(error, "Voxel geometry parameter exceeds the fixed range");
  if (slots_[handle].geometryParameters == parameters)
    return true;
  slots_[handle].geometryParameters = parameters;
  geometryParameters_[handle] = parameters;
  changes = VoxelMaterialChange::Geometry;
  ++slots_[handle].geometryRevision;
  ++geometryRevision_;
  ++contentRevision_;
  return true;
}

bool VoxelMaterialManager::updateFieldBindings(
    const VoxelMaterialHandle handle,
    const VoxelMaterialFieldBindings &bindings,
    VoxelMaterialChange &changes, std::string *error)
{
  changes = VoxelMaterialChange::None;
  if (error != nullptr)
    error->clear();
  if (!validHandle(handle))
    return fail(error, "Voxel material handle is invalid");
  if (slots_[handle].immutable)
    return fail(error, "Voxel material slot is immutable");
  if (!validateVoxelMaterialFieldBindings(bindings, error))
    return false;
  VoxelMaterialSlot &material = slots_[handle];
  const uint32_t shadingFields =
      voxelShadingRequiredMaterialFields(material.shadingProgram);
  const uint32_t geometryFields =
      voxelGeometryRequiredMaterialFields(material.geometryProgram);
  if (!materialFieldRequirementsSatisfied(
          shadingFields | geometryFields, bindings))
    return fail(error, "Voxel material program reads an undeclared material field");
  if (!geometryFieldSamplingSatisfied(geometryFields, bindings, error))
    return false;
  if (material.fieldBindings == bindings)
    return true;

  uint32_t changedFields = 0u;
  for (uint32_t slot = 0u; slot < kVoxelMaterialFieldCount; ++slot)
    if (!(material.fieldBindings[slot] == bindings[slot]))
      changedFields |= 1u << slot;
  material.fieldBindings = bindings;
  materialFieldDefaults_[handle] = compileVoxelMaterialFieldDefaults(bindings);
  ++contentRevision_;
  if ((changedFields & geometryFields) != 0u)
  {
    changes |= VoxelMaterialChange::Geometry;
    ++material.geometryRevision;
    ++geometryRevision_;
  }
  if ((changedFields & shadingFields) != 0u)
  {
    changes |= VoxelMaterialChange::Shading;
    ++material.shadingRevision;
    ++shadingRevision_;
  }
  return true;
}

bool VoxelMaterialManager::bindSourceMaterial(const uint32_t sourceMaterialId, const VoxelMaterialHandle handle, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!validHandle(handle))
    return fail(error, "Voxel material source binding handle is invalid");
  const auto existing = sourceMaterialHandles_.find(sourceMaterialId);
  if (existing != sourceMaterialHandles_.end())
  {
    if (existing->second != handle)
      return fail(error, "Voxel source material ID is already bound to another stable handle");
    return true;
  }
  sourceMaterialHandles_.emplace(sourceMaterialId, handle);
  return true;
}

VoxelMaterialHandle VoxelMaterialManager::handleForAsset(const std::string &assetId) const
{
  const auto found = assetHandles_.find(assetId);
  return found != assetHandles_.end() ? found->second : kInvalidVoxelMaterialHandle;
}

std::string VoxelMaterialManager::assetId(
    const VoxelMaterialHandle handle) const
{
  if (!validHandle(handle))
    return {};
  for (const auto &entry : assetHandles_)
    if (entry.second == handle)
      return entry.first;
  return {};
}

VoxelMaterialHandle VoxelMaterialManager::handleForSourceMaterial(const uint32_t sourceMaterialId) const
{
  const auto found = sourceMaterialHandles_.find(sourceMaterialId);
  return found != sourceMaterialHandles_.end() ? found->second : kInvalidVoxelMaterialHandle;
}

const VoxelMaterialSlot &VoxelMaterialManager::slot(const VoxelMaterialHandle handle) const
{
  return slots_[validHandle(handle) ? handle : 0u];
}

const svraster::SparseVoxelMicroMaterialGPU &VoxelMaterialManager::descriptor(const VoxelMaterialHandle handle) const
{
  return descriptors_[validHandle(handle) ? handle : 0u];
}

uint64_t VoxelMaterialManager::geometryIdentity(
    const VoxelMaterialHandle handle) const
{
  if (!validHandle(handle))
    return 0u;
  // Keep the handle in the low bits for debuggability and avalanche the live
  // revision into the upper identity used by temporal rejection.
  uint64_t value = slots_[handle].geometryRevision ^
      (static_cast<uint64_t>(handle) << 48u);
  value ^= value >> 30u;
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 27u;
  value *= 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}

bool VoxelMaterialManager::validHandle(const VoxelMaterialHandle handle) const
{
  return handle != kInvalidVoxelMaterialHandle && handle < slots_.size() && slots_[handle].occupied;
}

} // namespace voxel::material
