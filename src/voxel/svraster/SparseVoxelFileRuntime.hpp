#pragma once

#include "voxel/svraster/SparseVoxelSceneManager.hpp"
#include "voxel/svraster/SparseVoxelStreaming.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelFileRuntimeOptions
{
  // Zero means enough physical storage for every logical node. This remains a
  // bounded allocation because the validated file header supplies nodeCount.
  uint32_t physicalNodeCapacity = 0u;
  uint32_t maxInFlightReads = 2u;
  uint32_t maxAdmissionsPerUpdate = 2u;
  uint64_t maxStoredBytesPerUpdate = 8u * 1024u * 1024u;
  bool loadAllPages = false;
};

struct SparseVoxelFileRuntimeUpdate
{
  std::vector<SparseVoxelPageInstallBatch> installs;
  uint32_t completedReads = 0u;
  uint32_t failedReads = 0u;
};

// Metadata-first, dependency-aware VSVO session. Opening is transactional: a
// rejected replacement leaves the currently open file and resident cut intact.
// Page payloads are read only by update() after metadata validation succeeds.
class SparseVoxelFileRuntime
{
public:
  bool openMetadata(const std::string &path, const SparseVoxelFileRuntimeOptions &options = {}, std::string *error = nullptr);
  bool requestPage(uint32_t pageIndex, uint32_t priority, uint32_t instanceIndex, uint64_t frameIndex, std::string *error = nullptr);
  bool requestPages(
      const std::vector<uint32_t> &requestBits,
      const std::vector<uint32_t> &priorities,
      const std::vector<uint32_t> &instances,
      uint64_t frameIndex,
      std::string *error = nullptr);
  bool requestAllPages(uint64_t frameIndex, std::string *error = nullptr);
  bool update(SparseVoxelFileRuntimeUpdate &update, std::string *error = nullptr);

  bool isOpen() const { return open_; }
  bool rootsResident() const;
  bool fullyResident() const;
  const std::string &path() const { return path_; }
  const VirtualSparseVoxelSceneMetadata &metadata() const { return metadata_; }
  const SparseVoxelPageResidency &residency() const { return residency_; }
  SparseVoxelPageResidency &residency() { return residency_; }

  // Builds the metadata/TLAS scene from resident root payloads only. Fine-page
  // payloads stay in the physical residency arrays used by raster traversal.
  bool makeRootScene(SparseVoxelScene &scene, std::string *error = nullptr) const;

private:
  bool fail(std::string *error, const std::string &message) const;

  bool open_ = false;
  std::string path_;
  SparseVoxelFileRuntimeOptions options_{};
  VirtualSparseVoxelSceneMetadata metadata_{};
  SparseVoxelPageResidency residency_{};
  std::unique_ptr<SparseVoxelAsyncPageReader> reader_;
};

} // namespace voxel::svraster
