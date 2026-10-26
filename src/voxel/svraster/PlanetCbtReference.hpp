#pragma once

#include "voxel/svraster/PlanetCbt.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace voxel::svraster
{

struct PlanetCbtTopologyError
{
  procedural::PlanetSurfaceBisectorAddress address{};
  uint32_t poolSlot = kPlanetCbtNullSlot;
  uint32_t neighborSlot = kPlanetCbtNullSlot;
  std::string relation;
  std::string message;
};

// Independent decoder used to keep the future WGSL path honest. It implements
// the bisection matrices directly rather than calling the existing hierarchy
// decoder that it is compared against in tests.
bool planetCbtReferenceTriangle(const procedural::PlanetSurfaceHalfedgeMesh &mesh, const procedural::PlanetSurfaceBisectorAddress &address, procedural::PlanetSurfaceTriangle &triangle);

class PlanetCbtReference
{
public:
  bool initialize(const procedural::PlanetSurfaceHalfedgeMesh &mesh, uint32_t treeDepth);

  // A request is transactional. Compatibility-chain propagation may touch
  // several bisectors, but failure (including pool exhaustion) leaves every
  // byte of the prior pool and heap intact.
  bool split(const procedural::PlanetSurfaceBisectorAddress &address);
  bool merge(const procedural::PlanetSurfaceBisectorAddress &address);

  bool validate(PlanetCbtTopologyError *error = nullptr) const;
  bool contains(const procedural::PlanetSurfaceBisectorAddress &address) const;
  std::vector<procedural::PlanetSurfaceBisectorAddress> activeAddresses() const;

  const procedural::PlanetSurfaceHalfedgeMesh &mesh() const
  {
    return mesh_;
  }
  const std::vector<PlanetCbtBisectorGPU> &pool() const
  {
    return pool_;
  }
  const std::vector<uint32_t> &heap() const
  {
    return heap_;
  }
  uint32_t treeDepth() const
  {
    return treeDepth_;
  }
  uint32_t capacity() const
  {
    return planetCbtPoolCapacity(treeDepth_);
  }
  uint32_t activeCount() const;
  uint32_t freeCount() const
  {
    return capacity() - activeCount();
  }

private:
  struct AddressHash
  {
    size_t operator()(const procedural::PlanetSurfaceBisectorAddress &address) const;
  };
  struct AddressEqual
  {
    bool operator()(const procedural::PlanetSurfaceBisectorAddress &a, const procedural::PlanetSurfaceBisectorAddress &b) const;
  };

  using ActiveMap = std::unordered_map<procedural::PlanetSurfaceBisectorAddress, uint32_t, AddressHash, AddressEqual>;

  bool splitInPlace(const procedural::PlanetSurfaceBisectorAddress &address, uint32_t recursionBudget);
  bool splitCompatiblePair(const procedural::PlanetSurfaceBisectorAddress &first, const procedural::PlanetSurfaceBisectorAddress &second);
  bool mergeInPlace(const procedural::PlanetSurfaceBisectorAddress &address);
  bool rebuildTopology(PlanetCbtTopologyError *error = nullptr);
  bool rebuildHeap();
  uint32_t allocate(const procedural::PlanetSurfaceBisectorAddress &address);
  void release(uint32_t slot);

  procedural::PlanetSurfaceHalfedgeMesh mesh_{};
  uint32_t treeDepth_ = 0u;
  std::vector<PlanetCbtBisectorGPU> pool_{};
  std::vector<uint32_t> heap_{};
  ActiveMap active_{};
};

} // namespace voxel::svraster
