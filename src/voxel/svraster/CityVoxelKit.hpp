#pragma once

#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "voxel/svraster/CityVoxelVGPage.hpp"
#include "voxel/svraster/SparseVoxelTerrainVirtualGeometrySceneBridge.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace voxel::svraster {

// Many small shared city geometries in ONE virtual-geometry runtime object.
// Each geometry owns a slot: a slice of the object's hierarchy (the terrain
// shared-object layout) and a fixed run of page slots. A placement is an
// instance whose traversal starts at its slot's root, so a thousand
// placements of one wall bay share one slot's pages, and the scene sees one
// object however many distinct bays are resident.
class CityVoxelKit {
public:
  struct Settings {
    std::string objectName = "city-kit";
    uint32_t slots = 8192;            // Distinct resident geometries.
    uint32_t maxNodesPerGeometry = 64;
    uint32_t pagesPerGeometry = 4;
    // False: installs never wait for a page BLAS and cast no ray-traced
    // shadows (far proxies; a street's near houses already fill the budget).
    bool rayTraceCasters = true;
    // Each geometry (one page) casts as one BLAS cut from its own DAG at this
    // error in metres; 0 is its finest level, exact. Per-cluster BLASes over
    // every level cost ~2.4 MB a house, so a street did not fit 768 MB.
    // Negative keeps per-cluster BLASes.
    float rayTracePageCutError = 0.f;
  };
  struct Stats {
    uint32_t residentGeometries = 0, freeSlots = 0, placements = 0;
    uint64_t installs = 0, retirements = 0, rejected = 0;
  };

  bool create(virtualgeometry::VirtualGeometryScene &scene, const Settings &settings,
              const virtualgeometry::QuantizationConfig &quantization,
              const virtualgeometry::AABB &bounds, std::string *error = nullptr);
  bool created() const { return scene_ != nullptr; }

  // Installs a geometry from its .cvscene VG page (an embedded .vg file plus
  // per-cluster city sources). Idempotent per geometry ID.
  bool install(uint64_t geometryId, const CityVoxelVGPageView &page, std::string *error = nullptr);
  bool resident(uint64_t geometryId) const { return slotByGeometry_.count(geometryId) != 0; }
  // False while the BLAS queue is full and this geometry holds no place in
  // it; callers ask nearest first, so the nearest geometries hold the queue.
  bool accepting(uint64_t geometryId) const {
    return resident(geometryId) || pendingSlot_.count(geometryId) != 0 || pendingSlot_.size() < kMaxPending;
  }
  // Placement of a resident geometry; the caller sets city metadata on it.
  bool place(uint64_t geometryId, const math::Vec3f &position, const math::Quatf &rotation,
             virtualgeometry::VirtualGeometryScene::ModelPlacementHandle &out,
             std::string *error = nullptr);
  void unplace(uint64_t geometryId, virtualgeometry::VirtualGeometryScene::ModelPlacementHandle placement);
  // Frees a geometry with no placements; its slot is reused after the
  // scene's release delay because uninstalled pages retire on that clock.
  bool retire(uint64_t geometryId, std::string *error = nullptr);
  uint32_t placementCount(uint64_t geometryId) const;
  // Ends one streaming update: a geometry still waiting on its BLAS that was
  // not asked for again since the last call gives its slot back.
  void endUpdate();
  Stats stats() const;

private:
  struct Slot { uint32_t index = 0; uint32_t placements = 0; uint32_t pages = 0; };
  std::vector<uint32_t> pageSlotsFor(uint32_t slot, uint32_t pages) const;
  virtualgeometry::VirtualGeometryScene *scene_ = nullptr;
  Settings settings_;
  SparseVoxelTerrainVGSharedObjectLayout layout_;
  std::vector<uint32_t> freeSlots_;
  std::unordered_map<uint64_t, Slot> slotByGeometry_;
  // A geometry whose install is waiting on its page BLAS keeps the slot it
  // started in, so the next geometry tries another slot instead of queueing
  // behind the same one (one install a frame). When the queue is full a new
  // geometry is refused, never swapped in: evicting the oldest let a city's
  // thousand requests churn the whole queue every frame, so no BLAS finished.
  static constexpr size_t kMaxPending = 256u;
  struct Pending { uint32_t slot = 0; bool asked = false; };
  std::unordered_map<uint64_t, Pending> pendingSlot_;
  Stats stats_;
};

} // namespace voxel::svraster
