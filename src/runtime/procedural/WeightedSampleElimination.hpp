#pragma once

#include "procedural/Predicates.hpp"

#include <cstdint>
#include <vector>

namespace procedural
{

// Blue-noise sampling by weighted sample elimination, after Yuksel.
//
// Takes a dense set of samples and removes them one at a time until the
// requested count remains, always removing whichever sample is most crowded by
// its neighbours. The survivors are better separated than any subset chosen
// without looking at the geometry, which is what makes this blue noise rather
// than a decimation.
//
// "Most crowded" is a weight: each sample accumulates a contribution from every
// neighbour within twice the target radius, falling off with distance. Removing
// a sample subtracts its contribution from those neighbours, so the ordering
// adapts as the set thins. A max-heap keyed on weight supplies the next victim
// and a kd-tree answers the neighbour queries.
//
// Unlike the Poisson sampler this does not produce samples, it selects them, so
// the input has to be dense enough that a well-separated subset of the target
// size exists within it.
class WeightedSampleElimination
{
public:
  struct Settings
  {
    // Area the samples cover. Sets the target radius, which is what the weight
    // falloff is measured against; a wrong area gives a correctly ordered but
    // wrongly scaled elimination.
    double regionArea = 1.0;
    // Samples to keep. When this is at least the input size the input is
    // returned unchanged.
    uint64_t targetCount = 0u;
  };

  // Target separation for `count` samples over `regionArea`, which is the
  // radius a maximal Poisson-disk set of that density would have.
  static double targetRadius(uint64_t count, double regionArea);

  // Selects `settings.targetCount` of `points`. `weights` may be empty, in
  // which case every sample starts equally weighted.
  std::vector<Point2> eliminate(
      const std::vector<Point2> &points, const std::vector<float> &weights,
      const Settings &settings);

private:
  // One sample. The kd-tree links and the heap position live together because
  // every weight update needs both: the tree finds the neighbours, the heap
  // position is where the updated weight has to be repaired.
  struct Node
  {
    Point2 point;
    double weight = 0.0;
    uint64_t index = 0u;
    uint64_t left = kInvalid;
    uint64_t right = kInvalid;
    // Eliminated samples stay in the tree and are skipped by queries.
    //
    // Deleting them is what the original did, and it is where its correctness
    // went: a kd-tree deletion moves a point from one node into another and the
    // heap position stored alongside moves with it, so the heap and the tree
    // stop agreeing about which sample is which. Yuksel's method never needs
    // the node gone - only ignored.
    bool eliminated = false;
  };

  static constexpr uint64_t kInvalid = ~uint64_t{0};

  uint64_t buildTree(std::vector<uint64_t> &order, size_t begin, size_t end, int depth);
  // Visits every live sample within `radius` of `target`, excluding `target`
  // itself, calling visit(nodeIndex, distance).
  template <typename Visit>
  void forEachNeighbour(const Point2 &target, double radius, const Visit &visit);
  void swapHeap(uint64_t left, uint64_t right);
  double weightOf(double distance, double radius) const;
  void heapifyDown(uint64_t position);
  void reduceWeight(uint64_t node, double amount);
  uint64_t extractMaxNode();

  std::vector<Node> mNodes;
  // Heap of node indices, ordered by weight. `Node::index` is the inverse: the
  // position of that node in here. The two must be kept in step, which is why
  // every exchange goes through swapNodes.
  std::vector<uint64_t> mHeap;
  uint64_t mRoot = kInvalid;
  uint64_t mSize = 0u;
};

} // namespace procedural
