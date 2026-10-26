#include "procedural/WeightedSampleElimination.hpp"

#include "procedural/Geometry.hpp"

#include <algorithm>
#include <cmath>

namespace procedural
{

namespace
{

// Falloff exponent on the neighbour weight. Yuksel's value; higher makes the
// weight care only about the very closest neighbour, lower blurs the ordering
// until elimination stops distinguishing crowded from sparse.
constexpr double kWeightExponent = 8.0;

} // namespace

double WeightedSampleElimination::targetRadius(const uint64_t count, const double regionArea)
{
  if (count == 0u) return 0.0;
  // The radius of a maximal Poisson-disk set of this density: the densest
  // packing of `count` discs in `regionArea` is hexagonal, hence the 2*sqrt(3).
  return std::sqrt(regionArea / (2.0 * std::sqrt(3.0) * static_cast<double>(count)));
}

double WeightedSampleElimination::weightOf(const double distance, const double radius) const
{
  // Measured against twice the radius, which is the distance at which two
  // samples stop interacting at all. Clamped at zero because the neighbour
  // search is inclusive and an even exponent would otherwise turn an
  // out-of-range neighbour back into a positive weight.
  const double t = 1.0 - distance / (2.0 * radius);
  if (t <= 0.0) return 0.0;
  return std::pow(t, kWeightExponent);
}

uint64_t WeightedSampleElimination::buildTree(
    std::vector<uint64_t> &order, const size_t begin, const size_t end, const int depth)
{
  if (begin >= end) return kInvalid;

  // Median split on the alternating axis keeps the tree balanced, which is what
  // bounds the neighbour query. Inserting in input order instead degenerates to
  // a list on sorted input.
  const size_t middle = begin + (end - begin) / 2;
  const int axis = depth % 2;
  std::nth_element(
      order.begin() + begin, order.begin() + middle, order.begin() + end,
      [&](const uint64_t a, const uint64_t b)
      {
        return axis == 0 ? mNodes[a].point.x < mNodes[b].point.x
                         : mNodes[a].point.y < mNodes[b].point.y;
      });

  const uint64_t node = order[middle];
  mNodes[node].left = buildTree(order, begin, middle, depth + 1);
  mNodes[node].right = buildTree(order, middle + 1, end, depth + 1);
  return node;
}

template <typename Visit>
void WeightedSampleElimination::forEachNeighbour(
    const Point2 &target, const double radius, const Visit &visit)
{
  // Explicit stack rather than recursion: the depth is data dependent, and this
  // is the shape a compute port needs anyway.
  struct Entry
  {
    uint64_t node;
    int depth;
  };
  std::vector<Entry> stack;
  stack.push_back({mRoot, 0});

  while (!stack.empty())
  {
    const Entry entry = stack.back();
    stack.pop_back();
    if (entry.node == kInvalid) continue;

    const Node &node = mNodes[entry.node];
    const int axis = entry.depth % 2;
    const double delta =
        axis == 0 ? target.x - node.point.x : target.y - node.point.y;

    if (!node.eliminated)
    {
      const double d = distance(target, node.point);
      if (d > 0.0 && d <= radius) visit(entry.node, d);
    }

    const uint64_t near = delta < 0.0 ? node.left : node.right;
    const uint64_t far = delta < 0.0 ? node.right : node.left;

    stack.push_back({near, entry.depth + 1});
    // The far side can still hold neighbours whenever the splitting plane is
    // closer than the radius. Skipping that test is what silently loses
    // neighbours and leaves weights too low.
    if (std::fabs(delta) <= radius) stack.push_back({far, entry.depth + 1});
  }
}

void WeightedSampleElimination::swapHeap(const uint64_t left, const uint64_t right)
{
  std::swap(mHeap[left], mHeap[right]);
  mNodes[mHeap[left]].index = left;
  mNodes[mHeap[right]].index = right;
}

void WeightedSampleElimination::heapifyDown(uint64_t position)
{
  for (;;)
  {
    const uint64_t left = position * 2u + 1u;
    const uint64_t right = position * 2u + 2u;
    uint64_t largest = position;

    if (left < mSize && mNodes[mHeap[left]].weight > mNodes[mHeap[largest]].weight)
      largest = left;
    if (right < mSize && mNodes[mHeap[right]].weight > mNodes[mHeap[largest]].weight)
      largest = right;
    if (largest == position) return;

    swapHeap(position, largest);
    position = largest;
  }
}

void WeightedSampleElimination::reduceWeight(const uint64_t node, const double amount)
{
  mNodes[node].weight -= amount;
  // A weight only ever decreases here, so the node can only sink.
  if (mNodes[node].index < mSize) heapifyDown(mNodes[node].index);
}

uint64_t WeightedSampleElimination::extractMaxNode()
{
  const uint64_t node = mHeap[0];
  mSize = mSize - 1u;
  swapHeap(0u, mSize);
  heapifyDown(0u);
  return node;
}

std::vector<Point2> WeightedSampleElimination::eliminate(
    const std::vector<Point2> &points, const std::vector<float> &weights,
    const Settings &settings)
{
  if (settings.targetCount >= points.size()) return points;
  if (settings.targetCount == 0u) return {};

  const double radius = targetRadius(settings.targetCount, settings.regionArea);
  if (!(radius > 0.0)) return {};

  mNodes.assign(points.size(), Node{});
  for (size_t i = 0; i < points.size(); i++)
  {
    mNodes[i].point = points[i];
    mNodes[i].weight = 0.0;
  }

  std::vector<uint64_t> order(points.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  mRoot = buildTree(order, 0, order.size(), 0);

  // Initial weights: every sample accumulates its neighbours' contributions.
  // The caller's weights bias that, which is how a sample can be made more or
  // less likely to survive independently of its neighbourhood.
  for (size_t i = 0; i < mNodes.size(); i++)
  {
    double total = 0.0;
    forEachNeighbour(
        mNodes[i].point, 2.0 * radius,
        [&](const uint64_t, const double d) { total += weightOf(d, radius); });
    const double bias = i < weights.size() ? static_cast<double>(weights[i]) : 1.0;
    mNodes[i].weight = total * bias;
  }

  mHeap.resize(mNodes.size());
  for (size_t i = 0; i < mNodes.size(); i++)
  {
    mHeap[i] = i;
    mNodes[i].index = i;
  }
  mSize = mNodes.size();
  for (uint64_t i = mSize / 2u; i-- > 0u;) heapifyDown(i);

  // Remove the most crowded sample, then relieve the crowding it was causing,
  // and repeat. Subtracting after removal is what lets the ordering adapt: a
  // sample that was crowded only by the one just removed becomes a survivor.
  while (mSize > settings.targetCount)
  {
    const uint64_t victim = extractMaxNode();
    mNodes[victim].eliminated = true;

    const Point2 target = mNodes[victim].point;
    forEachNeighbour(
        target, 2.0 * radius,
        [&](const uint64_t neighbour, const double d)
        { reduceWeight(neighbour, weightOf(d, radius)); });
  }

  std::vector<Point2> result;
  result.reserve(mSize);
  for (uint64_t i = 0; i < mSize; i++) result.push_back(mNodes[mHeap[i]].point);
  return result;
}

} // namespace procedural
