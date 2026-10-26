#pragma once

#include "voxel/material/VoxelGeometryIR.hpp"
#include "voxel/material/VoxelMaterialContext.hpp"
#include "voxel/material/VoxelShadingIR.hpp"
#include "voxel/svraster/SparseVoxelMicroMaterial.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace voxel::material
{

using VoxelMaterialHandle = uint16_t;

inline constexpr VoxelMaterialHandle kInvalidVoxelMaterialHandle = 0u;
// The normalized per-voxel context lives in its own 32-bit sidecar, leaving the
// complete 16-bit brick descriptor entry available for stable material slots.
inline constexpr VoxelMaterialHandle kMaximumVoxelMaterialHandle = 0xffffu;

enum class VoxelMaterialSlotPolicy : uint32_t
{
  // Editable materials always receive their own stable slot, even when their
  // current bytes match another material.
  UniqueEditable = 0u,
  // Immutable imported materials may share one slot when descriptor and
  // context requirements match exactly.
  SharedImmutable = 1u,
};

enum class VoxelMaterialChange : uint32_t
{
  None = 0u,
  Geometry = 1u << 0u,
  Shading = 1u << 1u,
  ContextLayout = 1u << 2u,
};

inline constexpr VoxelMaterialChange operator|(const VoxelMaterialChange left, const VoxelMaterialChange right)
{
  return static_cast<VoxelMaterialChange>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

inline VoxelMaterialChange &operator|=(VoxelMaterialChange &left, const VoxelMaterialChange right)
{
  left = left | right;
  return left;
}

inline constexpr bool voxelMaterialChangeContains(const VoxelMaterialChange changes, const VoxelMaterialChange change)
{
  return (static_cast<uint32_t>(changes) & static_cast<uint32_t>(change)) != 0u;
}

inline constexpr bool voxelMaterialChangeRequiresPageRebuild(const VoxelMaterialChange changes)
{
  return voxelMaterialChangeContains(changes, VoxelMaterialChange::ContextLayout);
}

inline constexpr bool voxelMaterialChangeInvalidatesVisibilityHistory(
    const VoxelMaterialChange changes)
{
  return voxelMaterialChangeContains(changes, VoxelMaterialChange::Geometry) ||
      voxelMaterialChangeContains(changes, VoxelMaterialChange::ContextLayout);
}

struct VoxelMaterialDefinition
{
  // Stable package/editor identity. It is intentionally separate from the
  // compact runtime handle assigned by VoxelMaterialManager.
  std::string assetId;
  std::string name;
  svraster::SparseVoxelMicroMaterialGPU descriptor{};
  VoxelMaterialContextRequirements contextRequirements{};
  VoxelMaterialSlotPolicy slotPolicy = VoxelMaterialSlotPolicy::UniqueEditable;
  // Empty retains the specialized legacy shading profile. Non-empty programs
  // are verified and evaluated only in the material pass.
  VoxelShadingProgram shadingProgram{};
  VoxelShadingParameterBlockGPU shadingParameters{};
  // Empty selects the descriptor's specialized built-in GeometryIR program.
  VoxelGeometryProgram geometryProgram{};
  VoxelGeometryParameterBlockGPU geometryParameters{};
  VoxelMaterialFieldBindings fieldBindings{};
  VoxelShadingPaletteGPU shadingPalette{};
};

struct VoxelMaterialSlot
{
  std::string debugName;
  svraster::SparseVoxelMicroMaterialGPU descriptor{};
  VoxelMaterialContextRequirements contextRequirements{};
  VoxelShadingProgram shadingProgram{};
  VoxelShadingParameterBlockGPU shadingParameters{};
  VoxelGeometryProgram geometryProgram{};
  VoxelGeometryParameterBlockGPU geometryParameters{};
  VoxelMaterialFieldBindings fieldBindings{};
  VoxelShadingPaletteGPU shadingPalette{};
  uint64_t geometryRevision = 0u;
  uint64_t shadingRevision = 0u;
  uint64_t contextRevision = 0u;
  bool occupied = false;
  bool immutable = true;
};

// Phase-one owner for stable voxel-material slots. The slot descriptor remains
// the current built-in SparseVoxelMicroMaterialGPU encoding, so existing GPU
// code can consume descriptors() without a visual or ABI change. Later program
// records replace the descriptor payload while preserving these handles and
// revision rules.
class VoxelMaterialManager
{
public:
  VoxelMaterialManager();

  bool registerMaterial(const VoxelMaterialDefinition &definition, VoxelMaterialHandle &handle, std::string *error = nullptr);

  bool importLegacyMaterial(
      const std::string &assetId,
      const std::string &name,
      uint32_t sourceMaterialId,
      const svraster::SparseVoxelMicroMaterialLibrary &library,
      VoxelMaterialHandle &handle,
      VoxelMaterialSlotPolicy policy = VoxelMaterialSlotPolicy::UniqueEditable,
      VoxelMaterialContextRequirements requirements = {},
      std::string *error = nullptr);

  bool updateMaterial(VoxelMaterialHandle handle, const svraster::SparseVoxelMicroMaterialGPU &descriptor, VoxelMaterialContextRequirements requirements, VoxelMaterialChange &changes, std::string *error = nullptr);

  bool updateGeometry(VoxelMaterialHandle handle, const svraster::SparseVoxelMicroDescriptor &geometry, VoxelMaterialChange &changes, std::string *error = nullptr);

  bool updateShading(VoxelMaterialHandle handle, const svraster::SparseVoxelMicroShadingDescriptor &shading, VoxelMaterialChange &changes, std::string *error = nullptr);

  bool updateShadingProgram(VoxelMaterialHandle handle, const VoxelShadingProgram &program, VoxelMaterialChange &changes, std::string *error = nullptr);
  bool updateShadingParameters(VoxelMaterialHandle handle, const VoxelShadingParameterBlockGPU &parameters, VoxelMaterialChange &changes, std::string *error = nullptr);
  bool updateShadingPalette(VoxelMaterialHandle handle, const VoxelShadingPaletteGPU &palette, VoxelMaterialChange &changes, std::string *error = nullptr);
  bool updateGeometryProgram(VoxelMaterialHandle handle, const VoxelGeometryProgram &program, VoxelMaterialChange &changes, std::string *error = nullptr);
  bool updateGeometryParameters(VoxelMaterialHandle handle, const VoxelGeometryParameterBlockGPU &parameters, VoxelMaterialChange &changes, std::string *error = nullptr);
  bool updateFieldBindings(VoxelMaterialHandle handle, const VoxelMaterialFieldBindings &bindings, VoxelMaterialChange &changes, std::string *error = nullptr);

  // Source material IDs are the IDs already carried by
  // SparseVoxelMaterialGPU. Binding them separately keeps stable asset/runtime
  // identity out of the compact source material ABI during migration.
  bool bindSourceMaterial(uint32_t sourceMaterialId, VoxelMaterialHandle handle, std::string *error = nullptr);

  VoxelMaterialHandle handleForAsset(const std::string &assetId) const;
  std::string assetId(VoxelMaterialHandle handle) const;
  VoxelMaterialHandle handleForSourceMaterial(uint32_t sourceMaterialId) const;
  const VoxelMaterialSlot &slot(VoxelMaterialHandle handle) const;
  const svraster::SparseVoxelMicroMaterialGPU &descriptor(VoxelMaterialHandle handle) const;

  // Descriptor index equals VoxelMaterialHandle. Entry zero is the canonical
  // disabled descriptor expected by the current shaders.
  const std::vector<svraster::SparseVoxelMicroMaterialGPU> &descriptors() const
  {
    return descriptors_;
  }

  // Program records are indexed by stable material handle. A zero instruction
  // count selects the existing specialized profile without entering the VM.
  const std::vector<VoxelShadingProgramGPU> &shadingPrograms() const
  {
    return shadingPrograms_;
  }

  const std::vector<VoxelShadingInstructionGPU> &shadingInstructions() const
  {
    return shadingInstructions_;
  }

  const std::vector<VoxelShadingParameterBlockGPU> &shadingParameters() const
  {
    return shadingParameters_;
  }

  const std::vector<VoxelShadingPaletteGPU> &shadingPalettes() const
  {
    return shadingPalettes_;
  }

  const std::vector<VoxelGeometryProgramGPU> &geometryPrograms() const
  {
    return geometryPrograms_;
  }

  const std::vector<VoxelGeometryInstructionGPU> &geometryInstructions() const
  {
    return geometryInstructions_;
  }

  const std::vector<VoxelGeometryParameterBlockGPU> &geometryParameters() const
  {
    return geometryParameters_;
  }

  const std::vector<VoxelMaterialFieldDefaultsGPU> &materialFieldDefaults() const
  {
    return materialFieldDefaults_;
  }

  // Temporal identities incorporate this token, while palette/page handles
  // remain stable across live geometry edits.
  uint64_t geometryIdentity(VoxelMaterialHandle handle) const;

  size_t slotCount() const
  {
    return slots_.size();
  }

  uint64_t contentRevision() const
  {
    return contentRevision_;
  }

  uint64_t geometryRevision() const
  {
    return geometryRevision_;
  }

  uint64_t shadingRevision() const
  {
    return shadingRevision_;
  }

  uint64_t contextRevision() const
  {
    return contextRevision_;
  }

private:
  using SharedSlotKey =
      std::tuple<uint32_t, uint32_t, uint32_t, VoxelShadingProgram,
                 VoxelShadingParameterBlockGPU, VoxelGeometryProgram,
                 VoxelGeometryParameterBlockGPU,
                 VoxelMaterialFieldBindings, VoxelShadingPaletteGPU>;

  bool validHandle(VoxelMaterialHandle handle) const;

  std::vector<VoxelMaterialSlot> slots_;
  std::vector<svraster::SparseVoxelMicroMaterialGPU> descriptors_;
  std::vector<VoxelShadingProgramGPU> shadingPrograms_;
  // Every stable handle owns one fixed maximum-sized range. Live edits replace
  // that range in place, keeping buffer capacity and offsets stable.
  std::vector<VoxelShadingInstructionGPU> shadingInstructions_;
  std::vector<VoxelShadingParameterBlockGPU> shadingParameters_;
  std::vector<VoxelShadingPaletteGPU> shadingPalettes_;
  std::vector<VoxelGeometryProgramGPU> geometryPrograms_;
  std::vector<VoxelGeometryInstructionGPU> geometryInstructions_;
  std::vector<VoxelGeometryParameterBlockGPU> geometryParameters_;
  std::vector<VoxelMaterialFieldDefaultsGPU> materialFieldDefaults_;
  std::map<std::string, VoxelMaterialHandle> assetHandles_;
  std::map<uint32_t, VoxelMaterialHandle> sourceMaterialHandles_;
  std::map<SharedSlotKey, VoxelMaterialHandle> sharedImmutableSlots_;
  uint64_t contentRevision_ = 0u;
  uint64_t geometryRevision_ = 0u;
  uint64_t shadingRevision_ = 0u;
  uint64_t contextRevision_ = 0u;
};

} // namespace voxel::material
