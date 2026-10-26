#include "procedural/world/land/Dendry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace procedural::world
{

namespace
{

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr uint32_t kNoParent = 0xffffffffu;

// Integer-only, for the same reason every other hash in this system is: the
// world must be bit-identical across targets.
uint64_t hashCombine(uint64_t value)
{
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33u;
  return value;
}

uint64_t nodeHash(
    const uint64_t seed, const uint32_t level, const int64_t x, const int64_t y)
{
  uint64_t value = seed ^ (static_cast<uint64_t>(level) * 0x27220a95u);
  value = hashCombine(value ^ static_cast<uint64_t>(x) * 0x9e3779b97f4a7c15ull);
  value = hashCombine(value ^ static_cast<uint64_t>(y) * 0xbf58476d1ce4e5b9ull);
  return value;
}

double unitFrom(const uint64_t value)
{
  return static_cast<double>(value >> 11u) / 9007199254740992.0;
}

// Distance to a segment, and where along it the closest point fell.
double distanceToSegment(
    const Point2 &position, const Point2 &a, const Point2 &b, double &alongOut)
{
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double lengthSquared = dx * dx + dy * dy;
  double t = 0.0;
  if (lengthSquared > 0.0)
    t = std::clamp(
        ((position.x - a.x) * dx + (position.y - a.y) * dy) / lengthSquared,
        0.0, 1.0);
  alongOut = t;
  const double px = a.x + dx * t - position.x;
  const double py = a.y + dy * t - position.y;
  return std::sqrt(px * px + py * py);
}

} // namespace

bool DendrySettings::valid(std::string *error) const
{
  const auto fail = [error](const char *message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (levels == 0u || levels > 12u)
    return fail("dendry levels must be within [1, 12]");
  if (!(baseCellMetres > 0.0))
    return fail("dendry base cell must be positive");
  if (jitter < 0.0 || jitter > 0.5)
    return fail("dendry jitter must be within [0, 0.5]");
  if (disturbance < 0.0)
    return fail("dendry disturbance must not be negative");
  if (inheritance < 0.0)
    return fail("dendry inheritance must not be negative");
  if (angleWeight < 0.0)
    return fail("dendry angle weight must not be negative");
  if (smoothingMetres < 0.0)
    return fail("dendry smoothing must not be negative");
  return true;
}

DendryField::DendryField(DendrySettings settings, ControlFunction control)
    : settings_(settings), control_(std::move(control))
{
}

// Builds the tree in the neighbourhood of a tile.
//
// Level by level, coarsest first. At each level a lattice of nodes is placed -
// one per cell, displaced by the seeding term - and given a value: the control
// function there, disturbed, and pulled down toward whatever channels the
// coarser levels already laid. Each node then links to the neighbour it flows
// to, which is the lowest-valued of its eight neighbours, preferring one that
// continues the direction its own trunk was already going.
//
// A node with no lower neighbour is a root: a local minimum of the control
// function, which is exactly the hook the paper describes for seeding the
// network from the map.
DendryField::Build DendryField::build(
    const Point2 origin, const double sizeMetres) const
{
  Build result{};
  const double margin =
      settings_.baseCellMetres * static_cast<double>(kDependencyCells);
  const double lowX = origin.x - margin;
  const double lowY = origin.y - margin;
  const double highX = origin.x + sizeMetres + margin;
  const double highY = origin.y + sizeMetres + margin;

  std::vector<Node> nodes;
  for (uint32_t level = 0u; level < settings_.levels; ++level)
  {
    const double cell =
        settings_.baseCellMetres / static_cast<double>(1u << level);
    const int64_t firstX = static_cast<int64_t>(std::floor(lowX / cell));
    const int64_t lastX = static_cast<int64_t>(std::ceil(highX / cell));
    const int64_t firstY = static_cast<int64_t>(std::floor(lowY / cell));
    const int64_t lastY = static_cast<int64_t>(std::ceil(highY / cell));
    const int64_t width = lastX - firstX + 1;
    const int64_t height = lastY - firstY + 1;
    if (width <= 0 || height <= 0)
      continue;

    // The coarser levels' channels, as they stand before this level is added.
    // A node near one of them sits lower, so this level's flow is drawn into
    // the trunk rather than running beside it - which is what makes the levels
    // one hierarchy instead of several networks stacked up.
    const double inheritanceRange = cell * 2.0;
    const double disturbance =
        settings_.disturbance / static_cast<double>(1u << level);

    nodes.assign(static_cast<size_t>(width * height), Node{});
    for (int64_t y = firstY; y <= lastY; ++y)
      for (int64_t x = firstX; x <= lastX; ++x)
      {
        Node &node = nodes[static_cast<size_t>(
            (y - firstY) * width + (x - firstX))];
        const uint64_t hash = nodeHash(settings_.seed, level, x, y);
        const double offsetX =
            (unitFrom(hash) - 0.5) * 2.0 * settings_.jitter * cell;
        const double offsetY =
            (unitFrom(hashCombine(hash)) - 0.5) * 2.0 * settings_.jitter * cell;
        node.position = Point2((static_cast<double>(x) + 0.5) * cell + offsetX,
            (static_cast<double>(y) + 0.5) * cell + offsetY);
        double value = control_ ? control_(node.position) : 0.5;
        value += (unitFrom(hashCombine(hash ^ 0x51ed270bull)) - 0.5) *
            disturbance;
        if (level > 0u && settings_.inheritance > 0.0)
        {
          const double toTrunk = nearest(result, node.position).distanceMetres;
          value -= settings_.inheritance *
              std::exp(-toTrunk / inheritanceRange) /
              static_cast<double>(1u << level);
        }
        node.value = value;
      }

    // Parent selection. Two passes so the angle term can read a parent that has
    // already been chosen: the first pass picks purely by descent, the second
    // reconsiders with the direction the candidate itself flows.
    for (int32_t pass = 0; pass < 2; ++pass)
      for (int64_t y = firstY; y <= lastY; ++y)
        for (int64_t x = firstX; x <= lastX; ++x)
        {
          const size_t index =
              static_cast<size_t>((y - firstY) * width + (x - firstX));
          Node &node = nodes[index];
          double bestScore = kInfinity;
          uint32_t best = kNoParent;
          for (int64_t oy = -1; oy <= 1; ++oy)
            for (int64_t ox = -1; ox <= 1; ++ox)
            {
              if (ox == 0 && oy == 0)
                continue;
              const int64_t nx = x + ox;
              const int64_t ny = y + oy;
              if (nx < firstX || nx > lastX || ny < firstY || ny > lastY)
                continue;
              const size_t neighbourIndex = static_cast<size_t>(
                  (ny - firstY) * width + (nx - firstX));
              const Node &neighbour = nodes[neighbourIndex];
              // Strictly lower, so following parents cannot cycle: the value
              // decreases at every step. Ties on index, so the network does not
              // depend on iteration order.
              if (neighbour.value >= node.value)
                continue;
              const double dx = neighbour.position.x - node.position.x;
              const double dy = neighbour.position.y - node.position.y;
              const double span = std::sqrt(dx * dx + dy * dy);
              if (span <= 0.0)
                continue;
              // Steepest descent per unit length, so a slightly lower cell far
              // away does not beat a much lower one nearby.
              double score = (neighbour.value - node.value) / span;
              if (pass == 1 && settings_.angleWeight > 0.0 &&
                  neighbour.parent != kNoParent)
              {
                const Node &grand = nodes[neighbour.parent];
                const double gx = grand.position.x - neighbour.position.x;
                const double gy = grand.position.y - neighbour.position.y;
                const double gspan = std::sqrt(gx * gx + gy * gy);
                if (gspan > 0.0)
                {
                  // +1 when the tributary doubles back on its trunk, 0 when it
                  // continues straight. Penalising the first is what stops the
                  // network reading as the lattice it is built on.
                  const double alignment =
                      (dx * gx + dy * gy) / (span * gspan);
                  score += settings_.angleWeight *
                      std::abs(node.value - neighbour.value) / span *
                      (1.0 - alignment);
                }
              }
              if (score < bestScore ||
                  (score == bestScore && neighbourIndex < best))
              {
                bestScore = score;
                best = static_cast<uint32_t>(neighbourIndex);
              }
            }
          node.parent = best;
        }

    for (const Node &node : nodes)
      if (node.parent != kNoParent)
        result.segments.push_back(DendrySegment{node.position,
          nodes[node.parent].position, node.value, nodes[node.parent].value,
          level});

    // The accelerator is rebuilt after each level because the next level's
    // values query it.
    result.gridOrigin = Point2(lowX, lowY);
    const double span = std::max(highX - lowX, highY - lowY);
    result.gridSide = static_cast<uint32_t>(std::clamp(
        std::floor(std::sqrt(static_cast<double>(result.segments.size()))), 1.0,
        256.0));
    result.gridCell = span / static_cast<double>(result.gridSide);
    const size_t buckets =
        static_cast<size_t>(result.gridSide) * result.gridSide;
    std::vector<uint32_t> counts(buckets, 0u);
    // A segment is registered in every bucket its bounding box touches, so a
    // long trunk is found from anywhere along it.
    const auto forEachBucket = [&](const DendrySegment &segment,
                                   const auto &visit)
    {
      const int32_t last = static_cast<int32_t>(result.gridSide) - 1;
      const auto index = [&](const double value, const double base)
      {
        return std::clamp(
            static_cast<int32_t>(std::floor((value - base) / result.gridCell)),
            0, last);
      };
      const int32_t x0 = index(std::min(segment.from.x, segment.to.x), lowX);
      const int32_t x1 = index(std::max(segment.from.x, segment.to.x), lowX);
      const int32_t y0 = index(std::min(segment.from.y, segment.to.y), lowY);
      const int32_t y1 = index(std::max(segment.from.y, segment.to.y), lowY);
      for (int32_t y = y0; y <= y1; ++y)
        for (int32_t x = x0; x <= x1; ++x)
          visit(static_cast<uint32_t>(y) * result.gridSide +
              static_cast<uint32_t>(x));
    };
    for (const DendrySegment &segment : result.segments)
      forEachBucket(segment, [&counts](const uint32_t bucket) { ++counts[bucket]; });
    result.bucketOffsets.assign(buckets + 1u, 0u);
    for (size_t bucket = 0u; bucket < buckets; ++bucket)
      result.bucketOffsets[bucket + 1u] =
          result.bucketOffsets[bucket] + counts[bucket];
    result.bucketEntries.assign(result.bucketOffsets.back(), 0u);
    std::vector<uint32_t> cursor(
        result.bucketOffsets.begin(), result.bucketOffsets.end() - 1);
    for (size_t index = 0u; index < result.segments.size(); ++index)
      forEachBucket(result.segments[index],
          [&](const uint32_t bucket)
          { result.bucketEntries[cursor[bucket]++] = static_cast<uint32_t>(index); });
  }
  return result;
}

DendrySample DendryField::nearest(
    const Build &build, const Point2 position) const
{
  DendrySample result{};
  result.distanceMetres = kInfinity;
  if (build.segments.empty() || build.gridSide == 0u)
    return result;
  const int32_t last = static_cast<int32_t>(build.gridSide) - 1;
  const int32_t centreX = std::clamp(
      static_cast<int32_t>(
          std::floor((position.x - build.gridOrigin.x) / build.gridCell)),
      0, last);
  const int32_t centreY = std::clamp(
      static_cast<int32_t>(
          std::floor((position.y - build.gridOrigin.y) / build.gridCell)),
      0, last);

  for (int32_t ring = 0; ring <= last; ++ring)
  {
    const int32_t x0 = std::max(0, centreX - ring);
    const int32_t x1 = std::min(last, centreX + ring);
    const int32_t y0 = std::max(0, centreY - ring);
    const int32_t y1 = std::min(last, centreY + ring);
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
      {
        // Only the newly reached shell.
        if (ring > 0 && x > centreX - ring && x < centreX + ring &&
            y > centreY - ring && y < centreY + ring)
          continue;
        const uint32_t bucket =
            static_cast<uint32_t>(y) * build.gridSide + static_cast<uint32_t>(x);
        for (uint32_t entry = build.bucketOffsets[bucket];
             entry < build.bucketOffsets[bucket + 1u]; ++entry)
        {
          const DendrySegment &segment =
              build.segments[build.bucketEntries[entry]];
          double along = 0.0;
          const double distance = distanceToSegment(
              position, segment.from, segment.to, along);
          if (distance >= result.distanceMetres)
            continue;
          result.distanceMetres = distance;
          result.channelValue = segment.fromValue +
              (segment.toValue - segment.fromValue) * along;
          result.level = segment.level;
        }
      }
    // Nothing outside this ring can be nearer than the ring's own inner edge.
    if (result.distanceMetres <= static_cast<double>(ring) * build.gridCell)
      break;
  }
  return result;
}

bool DendryField::sample(
    const Point2 origin,
    const double sizeMetres,
    const uint32_t resolution,
    std::vector<DendrySample> &out,
    std::string *error) const
{
  out.clear();
  if (!settings_.valid(error))
    return false;
  if (!(sizeMetres > 0.0))
  {
    if (error != nullptr)
      *error = "dendry tile size must be positive";
    return false;
  }
  if (resolution == 0u)
  {
    if (error != nullptr)
      *error = "dendry resolution must be positive";
    return false;
  }

  const Build tree = build(origin, sizeMetres);
  out.resize(static_cast<size_t>(resolution) * resolution);
  const double step = sizeMetres / static_cast<double>(resolution);
  for (uint32_t row = 0u; row < resolution; ++row)
    for (uint32_t column = 0u; column < resolution; ++column)
    {
      const Point2 position(
          origin.x + (static_cast<double>(column) + 0.5) * step,
          origin.y + (static_cast<double>(row) + 0.5) * step);
      DendrySample entry = nearest(tree, position);
      // The paper's local smoothing: rounds the field where it meets a channel,
      // so a valley floor is a floor and not a crease.
      if (settings_.smoothingMetres > 0.0)
        entry.distanceMetres =
            std::sqrt(entry.distanceMetres * entry.distanceMetres +
                settings_.smoothingMetres * settings_.smoothingMetres) -
            settings_.smoothingMetres;
      out[static_cast<size_t>(row) * resolution + column] = entry;
    }
  return true;
}

DendrySample DendryField::sampleAt(const Point2 position) const
{
  std::vector<DendrySample> single;
  // A one-sample tile centred on the point: the same construction, so a point
  // query and a baked tile cannot disagree.
  const double size = settings_.baseCellMetres * 0.01;
  if (!sample(Point2(position.x - size * 0.5, position.y - size * 0.5), size, 1u,
          single, nullptr) ||
      single.empty())
    return DendrySample{kInfinity, 0.0, 0u};
  return single.front();
}

double DendryField::distanceAt(const Point2 position) const
{
  return sampleAt(position).distanceMetres;
}

std::vector<DendrySegment> DendryField::network(
    const Point2 origin, const double sizeMetres) const
{
  if (!settings_.valid(nullptr) || !(sizeMetres > 0.0))
    return {};
  return build(origin, sizeMetres).segments;
}

} // namespace procedural::world
