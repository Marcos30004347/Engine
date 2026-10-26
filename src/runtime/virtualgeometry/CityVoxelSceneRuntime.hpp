#pragma once
#include "voxel/svraster/CityVoxelSceneFormat.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include "voxel/svraster/CityVoxelVGPage.hpp"
#include "virtualgeometry/CityVoxelHardwareMesh.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace virtualgeometry {
// Bounded, file-backed city page residency. A frame publishes complete cells;
// cells awaiting shared assets retain a conservative coarse bounds proxy.
// Page pointers remain valid only until the next update or open.
class CityVoxelSceneRuntime {
public:
  struct Bounds { std::array<double,3> minimum{}, maximum{}; };
  struct Budgets {
    uint64_t metadataBytes=256ull<<20;
    uint64_t instanceBytes=192ull<<20;
    uint64_t geometryEncodedBytes=256ull<<20;
    uint64_t geometryDecodedBytes=256ull<<20;
    uint64_t appearanceBytes=128ull<<20;
    uint64_t traceBytes=256ull<<20;
    uint64_t virtualGeometryBytes=256ull<<20;
    uint64_t fallbackBytes=32ull<<20;
    uint64_t stagingBytes=64ull<<20;
    uint32_t maximumReadsPerFrame=64;
    uint32_t maximumActiveInstances=250000;
    bool requireTrace=true;
    bool requireVirtualGeometry=true;
  };
  struct CellState {
    uint32_t cellIndex=0;
    uint32_t firstInstance=0;
    uint32_t instanceCount=0;
    bool detailed=false;
    Bounds fallbackBounds{};
  };
  struct Statistics {
    uint64_t frame=0;
    uint64_t metadataBytes=0, instanceBytes=0;
    uint64_t geometryEncodedBytes=0, geometryDecodedBytes=0;
    uint64_t appearanceBytes=0, traceBytes=0, virtualGeometryBytes=0;
    uint64_t fallbackBytes=0, stagingPeakBytes=0;
    uint64_t pageFaults=0, pageReads=0, evictions=0;
    uint32_t visibleCells=0, detailedCells=0, fallbackCells=0;
    uint32_t activeInstances=0, distinctResidentGeometry=0, distinctResidentAppearances=0;
  };
  using Visible = std::function<bool(uint32_t,const Bounds&)>;
  bool open(const std::string &, Budgets, const voxel::svraster::CityVoxelSceneLimits &,
            std::string *error=nullptr);
  // `priority` orders visible cells (lower first) for page requests and the
  // instance budget, so a bounded read budget fills in nearest cells first.
  bool update(uint64_t frame, const Visible &, std::string *error=nullptr,
              const std::function<double(uint32_t,const Bounds&)> &priority={});
  bool updateRadius(uint64_t frame, const std::array<double,3> &camera,
                    double radiusMetres, std::string *error=nullptr);
  const voxel::svraster::CityVoxelSceneIndex &index() const { return file_.index(); }
  const std::vector<CellState> &publishedCells() const { return published_; }
  const Bounds &cellBounds(uint32_t i) const { return bounds_.at(i); }
  const std::vector<uint8_t> *residentPage(uint32_t id) const;
  bool residentGeometry(uint32_t index,
      voxel::svraster::CityVoxelCompiledGeometry &,std::string *error=nullptr) const;
  bool residentAppearance(uint32_t index,
      voxel::svraster::CityVoxelCompiledAppearance &,std::string *error=nullptr) const;
  bool residentTrace(uint32_t geometryIndex,
      CityVoxelHardwareMesh &,std::string *error=nullptr) const;
  uint32_t traceGroupCount(uint32_t geometryIndex) const;
  bool residentTraceGroup(uint32_t geometryIndex,uint32_t groupIndex,
      CityVoxelHardwareMesh &,std::string *error=nullptr) const;
  bool residentVirtualGeometry(uint32_t geometryIndex,
      voxel::svraster::CityVoxelVGPageView &,std::string *error=nullptr) const;
  const Statistics &statistics() const { return stats_; }
  const Budgets &budgets() const { return budgets_; }
  uint64_t openGeneration() const { return openGeneration_; }
private:
  struct ResidentPage { std::vector<uint8_t> bytes; uint64_t lastUse=0; };
  bool makeBounds(std::string *error);
  bool ensurePage(uint32_t id, uint64_t frame,
                  const std::unordered_map<uint32_t,uint32_t> &wanted,
                  uint32_t &reads, std::string *error);
  bool hasAssetPages(uint64_t geometryId, uint64_t appearanceId,
                     const std::vector<uint8_t> &ready) const;
  uint64_t &poolBytes(voxel::svraster::CityVoxelPageKind);
  uint64_t poolBudget(voxel::svraster::CityVoxelPageKind) const;
  voxel::svraster::CityVoxelSceneFileReader file_;
  Budgets budgets_{};
  Statistics stats_{};
  uint64_t openGeneration_=0;
  std::vector<Bounds> bounds_;
  std::vector<CellState> published_;
  std::unordered_map<uint32_t,ResidentPage> resident_;
  std::unordered_map<uint64_t,uint32_t> geometryById_, appearanceById_;
  std::unordered_map<uint64_t,std::vector<uint32_t>> tracePagesByGeometry_;
  std::unordered_map<uint64_t,std::vector<uint32_t>> vgPagesByGeometry_;
};
} // namespace virtualgeometry
