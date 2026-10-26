#include "voxel/svraster/PlanetCbtReference.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace voxel::svraster
{
namespace
{

using Address = procedural::PlanetSurfaceBisectorAddress;

bool sameAddress(const Address &a, const Address &b)
{
  return a.rootHalfedge == b.rootHalfedge && a.depth == b.depth && a.path == b.path;
}

struct VertexPair
{
  uint32_t low = 0u;
  uint32_t high = 0u;

  bool operator==(const VertexPair &other) const
  {
    return low == other.low && high == other.high;
  }
};

struct VertexPairHash
{
  size_t operator()(const VertexPair &edge) const
  {
    const uint64_t value = (static_cast<uint64_t>(edge.low) << 32u) | edge.high;
    return static_cast<size_t>(value ^ (value >> 33u) ^ (value << 11u));
  }
};

VertexPair orderedPair(const uint32_t a, const uint32_t b)
{
  return a < b ? VertexPair{a, b} : VertexPair{b, a};
}

class VertexInterner
{
public:
  explicit VertexInterner(const procedural::PlanetSurfaceHalfedgeMesh &mesh) : mesh_(mesh), next_(static_cast<uint32_t>(mesh.vertices.size() + mesh.faceCentres.size()))
  {
  }

  bool triangle(const Address &address, std::array<uint32_t, 3> &vertices)
  {
    if (!procedural::validPlanetSurfaceBisectorAddress(address) || address.rootHalfedge >= mesh_.vertex.size())
      return false;
    const uint32_t root = address.rootHalfedge;
    if (mesh_.next[root] >= mesh_.vertex.size() || mesh_.vertex[root] >= mesh_.vertices.size() || mesh_.vertex[mesh_.next[root]] >= mesh_.vertices.size() || mesh_.face[root] >= mesh_.faceCentres.size())
      return false;
    vertices = {
      mesh_.vertex[root],
      mesh_.vertex[mesh_.next[root]],
      static_cast<uint32_t>(mesh_.vertices.size()) + mesh_.face[root],
    };
    for (uint32_t level = 0u; level < address.depth; ++level)
    {
      const uint32_t bit = static_cast<uint32_t>((address.path >> (address.depth - level - 1u)) & 1u);
      const auto current = vertices;
      const uint32_t midpoint = intern(current[0], current[1]);
      vertices = bit == 0u ? std::array<uint32_t, 3>{current[0], current[2], midpoint} : std::array<uint32_t, 3>{current[2], current[1], midpoint};
    }
    return true;
  }

private:
  uint32_t intern(const uint32_t a, const uint32_t b)
  {
    const VertexPair key = orderedPair(a, b);
    const auto found = midpoints_.find(key);
    if (found != midpoints_.end())
      return found->second;
    const uint32_t result = next_++;
    midpoints_.emplace(key, result);
    return result;
  }

  const procedural::PlanetSurfaceHalfedgeMesh &mesh_;
  uint32_t next_ = 0u;
  std::unordered_map<VertexPair, uint32_t, VertexPairHash> midpoints_{};
};

struct EdgeOccurrence
{
  uint32_t slot = kPlanetCbtNullSlot;
  uint32_t edge = 0u;
  uint32_t count = 0u;
};

uint32_t &neighborForEdge(PlanetCbtBisectorGPU &bisector, const uint32_t edge)
{
  if (edge == 0u)
    return bisector.twin;
  if (edge == 1u)
    return bisector.next;
  return bisector.previous;
}

uint32_t neighborForEdge(const PlanetCbtBisectorGPU &bisector, const uint32_t edge)
{
  if (edge == 0u)
    return bisector.twin;
  if (edge == 1u)
    return bisector.next;
  return bisector.previous;
}

procedural::SpherePoint normalized(procedural::SpherePoint point)
{
  const double length = std::sqrt(point.x * point.x + point.y * point.y + point.z * point.z);
  if (!(length > 1.0e-15))
    return {0.0, 0.0, 1.0};
  point.x /= length;
  point.y /= length;
  point.z /= length;
  return point;
}

} // namespace

bool planetCbtReferenceTriangle(const procedural::PlanetSurfaceHalfedgeMesh &mesh, const Address &address, procedural::PlanetSurfaceTriangle &triangle)
{
  if (!procedural::validPlanetSurfaceBisectorAddress(address) || address.rootHalfedge >= mesh.vertex.size())
    return false;
  const uint32_t root = address.rootHalfedge;
  if (mesh.next[root] >= mesh.vertex.size() || mesh.vertex[root] >= mesh.vertices.size() || mesh.vertex[mesh.next[root]] >= mesh.vertices.size() || mesh.face[root] >= mesh.faceCentres.size())
    return false;
  triangle.vertices = {
    mesh.vertices[mesh.vertex[root]],
    mesh.vertices[mesh.vertex[mesh.next[root]]],
    mesh.faceCentres[mesh.face[root]],
  };
  for (uint32_t level = 0u; level < address.depth; ++level)
  {
    const uint32_t bit = static_cast<uint32_t>((address.path >> (address.depth - level - 1u)) & 1u);
    const auto current = triangle.vertices;
    const procedural::SpherePoint midpoint{
      (current[0].x + current[1].x) * 0.5,
      (current[0].y + current[1].y) * 0.5,
      (current[0].z + current[1].z) * 0.5,
    };
    triangle.vertices = bit == 0u ? std::array<procedural::SpherePoint, 3>{current[0], current[2], midpoint} : std::array<procedural::SpherePoint, 3>{current[2], current[1], midpoint};
  }
  for (auto &vertex : triangle.vertices)
    vertex = normalized(vertex);
  return true;
}

size_t PlanetCbtReference::AddressHash::operator()(const Address &address) const
{
  uint64_t value = address.path ^ (static_cast<uint64_t>(address.rootHalfedge) << 32u) ^ address.depth;
  value ^= value >> 30u;
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 27u;
  value *= 0x94d049bb133111ebull;
  return static_cast<size_t>(value ^ (value >> 31u));
}

bool PlanetCbtReference::AddressEqual::operator()(const Address &a, const Address &b) const
{
  return sameAddress(a, b);
}

bool PlanetCbtReference::initialize(const procedural::PlanetSurfaceHalfedgeMesh &mesh, const uint32_t treeDepth)
{
  const uint32_t requestedCapacity = planetCbtPoolCapacity(treeDepth);
  if (!procedural::validatePlanetSurfaceHalfedgeMesh(mesh) || mesh.vertex.size() != kPlanetCbtRootBisectorCount || requestedCapacity < mesh.vertex.size())
    return false;
  PlanetCbtReference candidate;
  candidate.mesh_ = mesh;
  candidate.treeDepth_ = treeDepth;
  candidate.pool_.resize(requestedCapacity);
  for (auto &bisector : candidate.pool_)
    bisector.rootHalfedge = kPlanetCbtNullSlot;
  candidate.heap_.resize(planetCbtHeapEntryCount(treeDepth), 0u);
  candidate.active_.reserve(requestedCapacity);
  for (uint32_t root = 0u; root < mesh.vertex.size(); ++root)
    if (candidate.allocate({root, 0u, 0u}) == kPlanetCbtNullSlot)
      return false;
  if (!candidate.rebuildTopology() || !candidate.rebuildHeap() || !candidate.validate())
    return false;
  *this = std::move(candidate);
  return true;
}

uint32_t PlanetCbtReference::allocate(const Address &address)
{
  if (!procedural::validPlanetSurfaceBisectorAddress(address) || active_.find(address) != active_.end())
    return kPlanetCbtNullSlot;
  uint32_t slot = kPlanetCbtNullSlot;
  for (uint32_t index = 0u; index < pool_.size(); ++index)
    if (pool_[index].rootHalfedge == kPlanetCbtNullSlot)
    {
      slot = index;
      break;
    }
  if (slot == kPlanetCbtNullSlot)
    return slot;
  PlanetCbtBisectorGPU bisector{};
  setPlanetCbtBisectorAddress(bisector, address);
  pool_[slot] = bisector;
  active_.emplace(address, slot);
  return slot;
}

void PlanetCbtReference::release(const uint32_t slot)
{
  if (slot >= pool_.size())
    return;
  const Address address = planetCbtBisectorAddress(pool_[slot]);
  const auto found = active_.find(address);
  if (found != active_.end() && found->second == slot)
    active_.erase(found);
  pool_[slot] = {};
  pool_[slot].rootHalfedge = kPlanetCbtNullSlot;
}

bool PlanetCbtReference::rebuildHeap()
{
  if (heap_.size() != planetCbtHeapEntryCount(treeDepth_))
    return false;
  std::fill(heap_.begin(), heap_.end(), 0u);
  for (const auto &entry : active_)
  {
    if (entry.second >= capacity())
      return false;
    if (!planetCbtSetLeafBit(heap_, treeDepth_, entry.second, true))
      return false;
  }
  return planetCbtReduce(heap_, treeDepth_);
}

bool PlanetCbtReference::rebuildTopology(PlanetCbtTopologyError *error)
{
  for (const auto &entry : active_)
  {
    auto &bisector = pool_[entry.second];
    bisector.next = bisector.previous = bisector.twin = kPlanetCbtNullSlot;
  }
  VertexInterner interner(mesh_);
  std::unordered_map<VertexPair, EdgeOccurrence, VertexPairHash> edges;
  edges.reserve(active_.size() * 3u);
  for (const auto &entry : active_)
  {
    std::array<uint32_t, 3> vertices{};
    if (!interner.triangle(entry.first, vertices))
    {
      if (error)
        *error = {entry.first, entry.second, kPlanetCbtNullSlot, "decode", "invalid stable address"};
      return false;
    }
    for (uint32_t edge = 0u; edge < 3u; ++edge)
    {
      const VertexPair key = orderedPair(vertices[edge], vertices[(edge + 1u) % 3u]);
      auto [found, inserted] = edges.emplace(key, EdgeOccurrence{entry.second, edge, 1u});
      if (inserted)
        continue;
      if (found->second.count != 1u)
      {
        if (error)
          *error = {entry.first, entry.second, found->second.slot, "edge", "more than two triangles share an edge"};
        return false;
      }
      neighborForEdge(pool_[entry.second], edge) = found->second.slot;
      neighborForEdge(pool_[found->second.slot], found->second.edge) = entry.second;
      found->second.count = 2u;
    }
  }
  for (const auto &edge : edges)
    if (edge.second.count != 2u)
    {
      const uint32_t slot = edge.second.slot;
      if (error)
        *error = {planetCbtBisectorAddress(pool_[slot]), slot, kPlanetCbtNullSlot, "edge", "edge has no conforming neighbor"};
      return false;
    }
  return true;
}

bool PlanetCbtReference::splitCompatiblePair(const Address &first, const Address &second)
{
  const auto firstIt = active_.find(first);
  const auto secondIt = active_.find(second);
  if (firstIt == active_.end() || secondIt == active_.end() || first.depth >= kPlanetCbtMaximumSubdivisionDepth || second.depth >= kPlanetCbtMaximumSubdivisionDepth || active_.size() + 2u > capacity())
    return false;
  const uint32_t firstSlot = firstIt->second;
  const uint32_t secondSlot = secondIt->second;
  release(firstSlot);
  release(secondSlot);
  for (const Address parent : {first, second})
    for (uint32_t child = 0u; child < 2u; ++child)
      if (allocate(procedural::planetSurfaceBisectorChild(parent, child)) == kPlanetCbtNullSlot)
        return false;
  return rebuildTopology() && rebuildHeap();
}

bool PlanetCbtReference::splitInPlace(const Address &address, const uint32_t recursionBudget)
{
  if (recursionBudget == 0u)
    return false;
  auto found = active_.find(address);
  if (found == active_.end())
    return false;
  const uint32_t slot = found->second;
  const uint32_t neighborSlot = pool_[slot].twin;
  if (neighborSlot >= pool_.size())
    return false;
  Address neighbor = planetCbtBisectorAddress(pool_[neighborSlot]);

  // If our refinement edge is a side of the neighbor, recursively bisect the
  // neighbor's own refinement edge. One of its children then presents this
  // edge as its base, which is the compatibility-chain rule.
  if (pool_[neighborSlot].twin != slot)
  {
    if (!splitInPlace(neighbor, recursionBudget - 1u))
      return false;
    found = active_.find(address);
    if (found == active_.end())
      return false;
    const uint32_t repairedNeighbor = pool_[found->second].twin;
    if (repairedNeighbor >= pool_.size() || pool_[repairedNeighbor].twin != found->second)
      return false;
    neighbor = planetCbtBisectorAddress(pool_[repairedNeighbor]);
  }
  return splitCompatiblePair(address, neighbor);
}

bool PlanetCbtReference::split(const Address &address)
{
  PlanetCbtReference candidate = *this;
  const uint32_t budget = std::max<uint32_t>(64u, activeCount() * 2u);
  if (!candidate.splitInPlace(address, budget) || !candidate.validate())
    return false;
  *this = std::move(candidate);
  return true;
}

bool PlanetCbtReference::mergeInPlace(const Address &address)
{
  if (address.depth == 0u)
    return false;
  const Address parent = procedural::planetSurfaceBisectorParent(address);
  const Address child0 = procedural::planetSurfaceBisectorChild(parent, 0u);
  const Address child1 = procedural::planetSurfaceBisectorChild(parent, 1u);
  const auto first = active_.find(child0);
  const auto second = active_.find(child1);
  if (first == active_.end() || second == active_.end())
    return false;

  // The two halves of the parent's base are child-0.previous and
  // child-1.next. A conservative diamond merge requires their opposite leaves
  // to be active siblings of one parent.
  const uint32_t opposite0Slot = pool_[first->second].previous;
  const uint32_t opposite1Slot = pool_[second->second].next;
  if (opposite0Slot >= pool_.size() || opposite1Slot >= pool_.size() || opposite0Slot == opposite1Slot)
    return false;
  const Address opposite0 = planetCbtBisectorAddress(pool_[opposite0Slot]);
  const Address opposite1 = planetCbtBisectorAddress(pool_[opposite1Slot]);
  if (opposite0.depth == 0u || opposite1.depth == 0u)
    return false;
  const Address oppositeParent = procedural::planetSurfaceBisectorParent(opposite0);
  if (!sameAddress(oppositeParent, procedural::planetSurfaceBisectorParent(opposite1)))
    return false;
  const Address expected0 = procedural::planetSurfaceBisectorChild(oppositeParent, 0u);
  const Address expected1 = procedural::planetSurfaceBisectorChild(oppositeParent, 1u);
  if ((!sameAddress(opposite0, expected0) && !sameAddress(opposite0, expected1)) || (!sameAddress(opposite1, expected0) && !sameAddress(opposite1, expected1)))
    return false;

  const std::array<uint32_t, 4> released{first->second, second->second, opposite0Slot, opposite1Slot};
  for (const uint32_t slot : released)
    release(slot);
  if (allocate(parent) == kPlanetCbtNullSlot || allocate(oppositeParent) == kPlanetCbtNullSlot)
    return false;
  return rebuildTopology() && rebuildHeap();
}

bool PlanetCbtReference::merge(const Address &address)
{
  PlanetCbtReference candidate = *this;
  if (!candidate.mergeInPlace(address) || !candidate.validate())
    return false;
  *this = std::move(candidate);
  return true;
}

bool PlanetCbtReference::contains(const Address &address) const
{
  return active_.find(address) != active_.end();
}

uint32_t PlanetCbtReference::activeCount() const
{
  return static_cast<uint32_t>(active_.size());
}

std::vector<Address> PlanetCbtReference::activeAddresses() const
{
  std::vector<Address> result;
  result.reserve(active_.size());
  for (const auto &entry : active_)
    result.push_back(entry.first);
  std::sort(
      result.begin(),
      result.end(),
      [](const Address &a, const Address &b)
      {
        if (a.rootHalfedge != b.rootHalfedge)
          return a.rootHalfedge < b.rootHalfedge;
        if (a.depth != b.depth)
          return a.depth < b.depth;
        return a.path < b.path;
      });
  return result;
}

bool PlanetCbtReference::validate(PlanetCbtTopologyError *error) const
{
  const auto fail = [error](const Address &address, const uint32_t slot, const uint32_t neighbor, const char *relation, const char *message)
  {
    if (error)
      *error = {address, slot, neighbor, relation, message};
    return false;
  };
  if (capacity() == 0u || pool_.size() != capacity() || heap_.size() != planetCbtHeapEntryCount(treeDepth_))
    return fail({}, kPlanetCbtNullSlot, kPlanetCbtNullSlot, "storage", "invalid CBT storage size");
  std::vector<uint32_t> expectedHeap = heap_;
  const uint32_t leafBase = capacity();
  std::fill(expectedHeap.begin(), expectedHeap.end(), 0u);
  std::vector<uint8_t> occupied(capacity(), 0u);
  for (const auto &entry : active_)
  {
    if (entry.second >= capacity())
      return fail(entry.first, entry.second, kPlanetCbtNullSlot, "address", "active slot is outside the pool");
    const Address stored = planetCbtBisectorAddress(pool_[entry.second]);
    if (occupied[entry.second] || !sameAddress(entry.first, stored) || !procedural::validPlanetSurfaceBisectorAddress(stored) || stored.rootHalfedge >= mesh_.vertex.size())
      return fail(entry.first, entry.second, kPlanetCbtNullSlot, "address", "invalid or duplicate active slot");
    occupied[entry.second] = 1u;
    expectedHeap[leafBase + entry.second] = 1u;
  }
  if (!planetCbtReduce(expectedHeap, treeDepth_) || expectedHeap != heap_ || heap_[1u] != active_.size())
    return fail({}, kPlanetCbtNullSlot, kPlanetCbtNullSlot, "heap", "CBT reduction does not match active slots");

  PlanetCbtReference rebuilt = *this;
  PlanetCbtTopologyError rebuildError{};
  if (!rebuilt.rebuildTopology(&rebuildError))
  {
    if (error)
      *error = std::move(rebuildError);
    return false;
  }
  for (const auto &entry : active_)
  {
    const uint32_t slot = entry.second;
    for (uint32_t edge = 0u; edge < 3u; ++edge)
    {
      const uint32_t neighbor = neighborForEdge(pool_[slot], edge);
      if (neighbor >= capacity() || !occupied[neighbor])
        return fail(entry.first, slot, neighbor, edge == 0u ? "twin" : edge == 1u ? "next" : "previous", "neighbor is not active");
      const auto &opposite = pool_[neighbor];
      if (opposite.twin != slot && opposite.next != slot && opposite.previous != slot)
        return fail(entry.first, slot, neighbor, edge == 0u ? "twin" : edge == 1u ? "next" : "previous", "neighbor relation is not reciprocal");
      if (neighborForEdge(rebuilt.pool_[slot], edge) != neighbor)
        return fail(entry.first, slot, neighbor, "edge", "stored neighbor does not share the decoded edge");
    }
  }
  return true;
}

} // namespace voxel::svraster
