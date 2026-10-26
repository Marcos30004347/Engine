#pragma once
#include "WorldCoverDetail.hpp"
#include "WorldSurfaceRefinement.hpp"
#include <memory>
#include <mutex>
namespace procedural::world::rewrite
{
struct RuntimeDependencies
{
  uint64_t road = 0, discharge = 0, spline = 0, settlement = 0, cover = 0;
  bool operator==(const RuntimeDependencies &b) const
  {
    return road == b.road && discharge == b.discharge && spline == b.spline && settlement == b.settlement && cover == b.cover;
  }
};
struct RuntimeCoverPage
{
  uint64_t revision = 0;
  RuntimeDependencies dependencies;
  CoverPage page;
};
struct RuntimeWorldAssets
{
  RuntimeDependencies dependencies;
  std::vector<RefinedWorldSurfaces> levels; // Level 0 supplies collision for every display LOD.
  std::vector<RuntimeCoverPage> coverPages; // One page per tile, with its gutter.
  std::vector<CoverDefinition> coverDefinitions;
  std::vector<std::vector<RuntimeCoverPage>> coverMips; // Additional footprint levels, each covers every tile.
  SplineFaceMesh structures;                // Explicit cliff/wall/bridge faces, retained at every LOD.
  std::vector<uint32_t> structuralMaterials;
};
struct RuntimeSnapshot
{
  uint64_t ticket = 0;
  std::shared_ptr<const RuntimeWorldAssets> assets;
  std::vector<uint32_t> tileLevels;
};
struct RuntimeSurfaceHit
{
  double height = 0;
  uint64_t revision = 0, face = 0;
  uint32_t material = 0;
};
// Levels must have exactly the same canonical map, including seam stations and heights.
// Dry ground interiors may simplify. Engineered faces and ground touching water
// bounds retain level-0 triangles and errors exactly. No skirts or moving seams.
bool validateRuntimeAssets(const RuntimeWorldAssets &, std::string *error = nullptr);
// Compile progressively coarser interiors, then reapply the measured surface-error contract.
bool buildRuntimeLods(
    const WorldFinalPatches &,
    const PatchSurface &,
    const std::vector<SurfaceFeatureSample> &,
    const std::vector<SurfaceErrorSettings> &,
    const std::vector<SurfaceRefinement> &,
    RuntimeDependencies,
    std::vector<RuntimeCoverPage>,
    RuntimeWorldAssets &,
    std::string *error = nullptr);
// Collision and water readers use the same pinned snapshot as the renderer.
bool queryRuntimeSurface(const RuntimeSnapshot &, double x, double z, uint32_t layer, RuntimeSurfaceHit &, std::string *error = nullptr);
class WorldRuntimePublication
{
public:
  // Begins a CPU publication transaction. Deep-copy input so external mutation cannot affect readers.
  // Existing snapshot remains visible until every requested tile completes.
  bool begin(const RuntimeWorldAssets &, std::vector<uint32_t> tileLevels, uint64_t maximumResidentTriangles, uint64_t &ticket, std::string *error = nullptr);
  bool complete(uint64_t ticket, uint32_t tile, RuntimeDependencies, std::string *error = nullptr);
  bool cancel(uint64_t ticket);
  std::shared_ptr<const RuntimeSnapshot> acquire() const;

private:
  mutable std::mutex mutex_;
  uint64_t next_ = 0;
  std::shared_ptr<const RuntimeSnapshot> visible_;
  std::shared_ptr<RuntimeSnapshot> pending_;
  std::vector<bool> ready_;
};
// Bounded portable JSON for dependency/LOD selection replay; actual assets are revalidated.
// This does not serialize/reconstruct a procedural world or GPU allocations.
bool saveRuntimeSelection(const RuntimeSnapshot &, std::string &, std::string *error = nullptr);
bool loadRuntimeSelection(const std::string &, const RuntimeWorldAssets &, std::vector<uint32_t> &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
