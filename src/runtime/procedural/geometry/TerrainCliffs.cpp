#include "procedural/Geometry.hpp"
#include "procedural/geometry/TerrainMaterial.hpp"
#include "procedural/PlanetGeology.hpp"
#include "procedural/geometry/TerrainMeshBuild.hpp"
#include "procedural/world/MapEdges.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace procedural::geometry
{

namespace
{

uint64_t mix(uint64_t value)
{
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33u;
  return value;
}

double unitNoise(const uint64_t seed, const uint64_t id)
{
  return static_cast<double>(mix(seed ^ (id * 0x9e3779b9ull)) >> 11u) /
      static_cast<double>(1ull << 53u);
}

// The rock a face is cut into, and therefore the angle it stands at.
//
// `PlanetGeology.hpp` already models this and the map generator has never been
// able to see it: the suite is fixed by tectonics, and cratons *are* tectonics,
// so a craton index is the honest bridge until a real lithology stage exists.
PlanetLithology lithologyOf(const uint16_t craton, const double elevationMetres)
{
  static constexpr PlanetRockSuite kSuites[4] = {PlanetRockSuite::Platform,
      PlanetRockSuite::Basement, PlanetRockSuite::Platform,
      PlanetRockSuite::Volcanic};
  const PlanetRockSuite suite =
      kSuites[static_cast<uint32_t>(craton == 0xffffu ? 0u : craton) % 4u];
  const float phase =
      static_cast<float>((static_cast<uint32_t>(craton) % 7u) * 30u);
  return planetStratumLithology(
      suite, static_cast<float>(elevationMetres), phase);
}

// Value noise over a lattice, smoothly interpolated. A pure function of
// position, which is the whole requirement: a displacement that depends on
// anything else moves two copies of a shared vertex to two different places.
double valueNoise(const uint64_t seed, const double x, const double y)
{
  const double fx = std::floor(x);
  const double fy = std::floor(y);
  const auto ix = static_cast<int64_t>(fx);
  const auto iy = static_cast<int64_t>(fy);
  const double tx = x - fx;
  const double ty = y - fy;
  const auto smooth = [](const double t) { return t * t * (3.0 - 2.0 * t); };
  const auto corner = [seed](const int64_t cx, const int64_t cy)
  {
    return unitNoise(seed,
        static_cast<uint64_t>(cx * 73856093ll) ^
            static_cast<uint64_t>(cy * 19349663ll));
  };
  const double sx = smooth(tx);
  const double sy = smooth(ty);
  const double top = corner(ix, iy) + (corner(ix + 1, iy) - corner(ix, iy)) * sx;
  const double bottom =
      corner(ix, iy + 1) + (corner(ix + 1, iy + 1) - corner(ix, iy + 1)) * sx;
  return top + (bottom - top) * sy;
}

// Fractal value noise in [-1, 1].
double fractalNoise(const uint64_t seed, const double x, const double y,
    const double wavelength, const uint32_t octaves)
{
  double sum = 0.0;
  double amplitude = 1.0;
  double total = 0.0;
  double frequency = 1.0 / std::max(1.0, wavelength);
  for (uint32_t octave = 0u; octave < std::max(1u, octaves); ++octave)
  {
    sum += (valueNoise(seed + octave * 0x9e37u, x * frequency, y * frequency) -
               0.5) *
        2.0 * amplitude;
    total += amplitude;
    amplitude *= 0.5;
    frequency *= 2.0;
  }
  return total > 0.0 ? sum / total : 0.0;
}

struct ChainRun
{
  std::vector<Point2> points;
  bool closed = false;
};

// Joins boundary edges into runs by shared endpoint. A block's boundary is one
// or more closed loops; a run that fails to close is kept open rather than
// forced, because forcing it invents an edge that is not there.
std::vector<ChainRun> joinRuns(std::vector<std::pair<Point2, Point2>> edges)
{
  std::vector<ChainRun> runs;
  const auto key = [](const Point2 &p)
  {
    return std::pair<int64_t, int64_t>(
        static_cast<int64_t>(std::llround(p.x * 1000.0)),
        static_cast<int64_t>(std::llround(p.y * 1000.0)));
  };
  std::multimap<std::pair<int64_t, int64_t>, size_t> byStart;
  std::vector<uint8_t> used(edges.size(), 0u);
  for (size_t index = 0u; index < edges.size(); ++index)
  {
    byStart.emplace(key(edges[index].first), index);
    byStart.emplace(key(edges[index].second), index);
  }
  for (size_t index = 0u; index < edges.size(); ++index)
  {
    if (used[index] != 0u)
      continue;
    used[index] = 1u;
    ChainRun run{};
    run.points = {edges[index].first, edges[index].second};
    for (bool extended = true; extended;)
    {
      extended = false;
      const auto range = byStart.equal_range(key(run.points.back()));
      for (auto entry = range.first; entry != range.second; ++entry)
      {
        const size_t candidate = entry->second;
        if (used[candidate] != 0u)
          continue;
        const auto &edge = edges[candidate];
        const bool forward = key(edge.first) == key(run.points.back());
        used[candidate] = 1u;
        run.points.push_back(forward ? edge.second : edge.first);
        extended = true;
        break;
      }
    }
    if (run.points.size() >= 3u &&
        key(run.points.front()) == key(run.points.back()))
    {
      run.points.pop_back();
      run.closed = true;
    }
    if (run.points.size() >= 2u)
      runs.push_back(std::move(run));
  }
  return runs;
}

// Resamples a run by arc length so brink and toe share one parameterisation.
std::vector<Point2> resampleRun(
    const ChainRun &run, const double spacing)
{
  std::vector<Point2> walk = run.points;
  if (run.closed && !walk.empty())
    walk.push_back(walk.front());
  double total = 0.0;
  for (size_t index = 0u; index + 1u < walk.size(); ++index)
    total += distance(walk[index], walk[index + 1u]);
  if (!(total > 0.0))
    return {};
  const auto steps = static_cast<uint32_t>(
      std::max<int64_t>(2, std::llround(total / std::max(1.0, spacing))));
  std::vector<Point2> output;
  output.reserve(steps + 1u);
  const double step = total / static_cast<double>(steps);
  size_t segment = 0u;
  double walked = 0.0;
  for (uint32_t emitted = 0u; emitted <= steps; ++emitted)
  {
    const double target = std::min(total, static_cast<double>(emitted) * step);
    while (segment + 1u < walk.size())
    {
      const double length = distance(walk[segment], walk[segment + 1u]);
      if (walked + length >= target || length <= 0.0)
        break;
      walked += length;
      ++segment;
    }
    if (segment + 1u >= walk.size())
    {
      output.push_back(walk.back());
      continue;
    }
    const double length = distance(walk[segment], walk[segment + 1u]);
    const double t = length > 0.0 ? std::clamp((target - walked) / length, 0.0, 1.0) : 0.0;
    output.push_back(Point2(
        walk[segment].x + t * (walk[segment + 1u].x - walk[segment].x),
        walk[segment].y + t * (walk[segment + 1u].y - walk[segment].y)));
  }
  if (run.closed && output.size() > 1u)
    output.pop_back();
  return output;
}

} // namespace

CliffGeometry buildCliffs(
    const world::GeneratedWorld &world, const CliffSettings &settings)
{
  CliffGeometry geometry{};
  if (world.plateaus.blocks.empty())
    return geometry;

  const double weld = std::max(world.extentMetres * 1.0e-7, 1.0e-4);
  const world::MapEdgeSet edges = world::buildEdgeSet(world.graph, weld);

  const auto blockOf = [&world](const uint32_t cell) -> uint32_t
  {
    if (cell == world::kInvalidNode || cell >= world.plateaus.cellBlock.size())
      return kBaseTerrace;
    const uint32_t block = world.plateaus.cellBlock[cell];
    return block == world::kInvalidNode ? kBaseTerrace : block;
  };
  const auto surfaceOf = [&world, &blockOf](const uint32_t cell) -> double
  {
    if (cell == world::kInvalidNode)
      return 0.0;
    const uint32_t block = blockOf(cell);
    if (block != kBaseTerrace)
      return world.plateaus.blocks[block].elevationMetres;
    return world.graph.node(cell).intent.elevationMetres;
  };

  // A boundary is keyed by *which two terraces meet across it*, not by the
  // block alone. A block that sits partly on open ground and partly on another
  // block has two different risers and two different ways up, and averaging
  // them into one number is what made the terrace graph a star instead of a
  // hierarchy.
  struct Boundary
  {
    std::vector<std::pair<Point2, Point2>> edges;
    double lowerSum = 0.0;
    uint32_t lowerCount = 0u;
  };
  std::map<std::pair<uint32_t, uint32_t>, Boundary> boundaries;

  for (const world::MapBoundaryEdge &edge : edges.edges)
  {
    if (edge.worldBorder)
      continue;
    const uint32_t blockA = blockOf(edge.cellA);
    const uint32_t blockB = blockOf(edge.cellB);
    if (blockA == blockB)
      continue;
    const double surfaceA = surfaceOf(edge.cellA);
    const double surfaceB = surfaceOf(edge.cellB);
    // The upper side owns the face. If neither side is a block there is no
    // plateau here, only a natural slope, and the terrain already expresses it.
    const bool aIsUpper = surfaceA >= surfaceB;
    const uint32_t upper = aIsUpper ? blockA : blockB;
    const uint32_t lower = aIsUpper ? blockB : blockA;
    if (upper == kBaseTerrace)
      continue;
    const uint32_t lowerCell = aIsUpper ? edge.cellB : edge.cellA;
    if (lowerCell == world::kInvalidNode)
      continue;
    const world::MapNode &lowerNode = world.graph.node(lowerCell);
    if (lowerNode.refined() || !lowerNode.land)
      continue;
    Boundary &boundary = boundaries[{upper, lower}];
    boundary.edges.emplace_back(
        edges.vertices[edge.from].position, edges.vertices[edge.to].position);
    boundary.lowerSum += aIsUpper ? surfaceB : surfaceA;
    ++boundary.lowerCount;
  }

  double riserTotal = 0.0;
  double setbackTotal = 0.0;
  uint64_t setbackSamples = 0u;

  for (const auto &entry : boundaries)
  {
    const uint32_t upperBlock = entry.first.first;
    const uint32_t lowerTerrace = entry.first.second;
    const world::PlateauBlock &block = world.plateaus.blocks[upperBlock];
    if (entry.second.lowerCount == 0u)
      continue;
    const double lowerSurface =
        entry.second.lowerSum / static_cast<double>(entry.second.lowerCount);
    const double riser = block.elevationMetres - lowerSurface;
    // Two terraces on the same rung are one terrace: there is no cliff between
    // them and you simply walk across. Without this they are separate nodes
    // with no edge, and a block flush against its neighbour reads as
    // unreachable when in fact nothing is in the way.
    if (riser <= settings.flushJoinMetres)
    {
      TerraceEdge flush{};
      flush.from = lowerTerrace;
      flush.to = upperBlock;
      flush.gradient = 0.0;
      flush.widthMetres = 0.0;
      if (!entry.second.edges.empty())
        flush.position = entry.second.edges.front().first;
      geometry.terraces.edges.push_back(flush);
      continue;
    }

    const uint16_t craton = block.cells.empty()
        ? uint16_t{0}
        : world.graph.node(block.cells.front()).craton;
    const PlanetLithology rock = lithologyOf(craton, block.elevationMetres);
    const PlanetLithologyProperties &properties =
        kPlanetLithologyTable[static_cast<uint32_t>(rock)];

    for (const ChainRun &run : joinRuns(entry.second.edges))
    {
      std::vector<Point2> brink = resampleRun(run, settings.spacingMetres);
      if (brink.size() < 2u)
        continue;

      const size_t count = brink.size();
      std::vector<Point2> toe(count);
      std::vector<double> setbacks(count, 0.0);

      double runLength = 0.0;
      for (size_t index = 0u; index + 1u < count; ++index)
        runLength += distance(brink[index], brink[index + 1u]);

      // Ramp sites. A ramp is a stretch where the setback is widened until
      // riser/setback falls under the walkable bound, so siting one is choosing
      // a centre and a width, not building anything new.
      std::vector<double> rampWeight(count, 0.0);
      if (settings.cutRamps && runLength > 0.0)
      {
        const uint32_t rampCount = std::max(1u, settings.ramps.rampsPerRun);
        // A boundary shorter than the nominal ramp is still ground someone has
        // to be able to get onto, so the ramp narrows to fit rather than being
        // skipped.
        const double width =
            std::min(settings.ramps.rampWidthMetres, runLength * 0.6);
        const auto halfSpan = std::max<size_t>(1u,
            static_cast<size_t>(
                width * 0.5 / std::max(1.0, settings.spacingMetres)));
        for (uint32_t ramp = 0u; ramp < rampCount; ++ramp)
        {
          const double pick = unitNoise(settings.seed ^ 0x7f4a7c15u,
              (static_cast<uint64_t>(upperBlock) * 131ull +
                  static_cast<uint64_t>(lowerTerrace) * 7717ull) * 31ull + ramp);
          const auto centre = static_cast<size_t>(pick * static_cast<double>(count));
          for (size_t offset = 0u; offset <= halfSpan; ++offset)
          {
            const double t =
                static_cast<double>(offset) / static_cast<double>(halfSpan);
            const double weight =
                0.5 * (1.0 + std::cos(t * 3.14159265358979323846));
            for (const long direction : {1L, -1L})
            {
              const auto at = static_cast<size_t>(
                  (static_cast<long>(centre) +
                      direction * static_cast<long>(offset) +
                      static_cast<long>(count) * 4L) %
                  static_cast<long>(count));
              rampWeight[at] = std::max(rampWeight[at], weight);
            }
          }
        }
      }

      for (size_t index = 0u; index < count; ++index)
      {
        const Point2 &previous = brink[(index + count - 1u) % count];
        const Point2 &next = brink[(index + 1u) % count];
        double tx = next.x - previous.x;
        double tz = next.y - previous.y;
        const double length = std::max(1.0e-9, std::hypot(tx, tz));
        tx /= length;
        tz /= length;
        const double nx = tz;
        const double nz = -tx;

        // The rock decides the angle. A low-frequency wobble keeps a face from
        // reading as extruded, but it modulates the tabled repose rather than
        // replacing it.
        const double wobble = 0.75 +
            0.5 * unitNoise(settings.seed ^ 0x2545f491u,
                      static_cast<uint64_t>(upperBlock) * 7919ull + index);
        const double angle = std::clamp(
            static_cast<double>(properties.talusAngleDegrees) * wobble, 12.0, 82.0);
        double setback =
            riser / std::tan(angle * 3.14159265358979323846 / 180.0);
        setback *= settings.setbackScale;
        const double localSize = distance(brink[index], next);
        setback = std::clamp(setback, 0.0,
            std::max(1.0, localSize * 8.0 * settings.maximumSetbackFraction));
        // Buttresses and notches, applied *before* the ramp widening. A
        // notch multiplies the setback down, and running it afterwards shrinks
        // a ramp back below the walkable bound - which reads as a block that
        // has a ramp and is still unreachable.
        // Buttresses and notches: a lobe that pushes the toe out and a chimney
        // that pulls it in, from the same smooth field so neighbouring points
        // move together and the face gains structure instead of noise.
        if (settings.beautify && settings.detail.featureStrength > 0.0)
        {
          const double feature = fractalNoise(settings.detail.seed ^ 0x5bd1e995u,
              brink[index].x, brink[index].y,
              settings.detail.featureWavelengthMetres, 2u);
          setback *= 1.0 + feature * settings.detail.featureStrength;
          setback = std::max(setback, 0.05 * riser);
        }
        if (rampWeight[index] > 0.0)
        {
          const double walkable =
              riser / std::max(0.01, settings.ramps.maximumWalkableGradient);
          setback = std::max(setback,
              setback + (walkable - setback) * rampWeight[index]);
        }
        setbacks[index] = setback;
        toe[index] = Point2(
            brink[index].x + nx * setback, brink[index].y + nz * setback);
        setbackTotal += setback;
        ++setbackSamples;
      }

      // Beautification. The brink is warped through a smooth field of position
      // and jittered vertically; the toe follows the same field, so the two
      // stay parallel and the ruled strip between them stays watertight.
      std::vector<double> brinkHeights(count, block.elevationMetres);
      std::vector<double> toeHeights(count, lowerSurface);
      if (settings.beautify)
      {
        const EdgeDetailSettings &detail = settings.detail;
        for (size_t index = 0u; index < count; ++index)
        {
          const double warpX = fractalNoise(detail.seed ^ 0x1234567u,
              brink[index].x, brink[index].y, detail.warpWavelengthMetres,
              detail.warpOctaves);
          const double warpY = fractalNoise(detail.seed ^ 0x7654321u,
              brink[index].x, brink[index].y, detail.warpWavelengthMetres,
              detail.warpOctaves);
          const Point2 shift(warpX * detail.warpAmplitudeMetres,
              warpY * detail.warpAmplitudeMetres);
          // Both chains take the same displacement, evaluated at the brink, so
          // the face cannot shear.
          brink[index] = Point2(brink[index].x + shift.x, brink[index].y + shift.y);
          toe[index] = Point2(toe[index].x + shift.x, toe[index].y + shift.y);
          brinkHeights[index] = block.elevationMetres +
              fractalNoise(detail.seed ^ 0xabcdefu, brink[index].x, brink[index].y,
                  detail.warpWavelengthMetres * 0.5, 2u) *
                  riser * detail.brinkJitterFraction;
          toeHeights[index] = lowerSurface +
              fractalNoise(detail.seed ^ 0xfedcbau, toe[index].x, toe[index].y,
                  detail.warpWavelengthMetres, 2u) *
                  riser * detail.toeJitterFraction;
        }
      }

      const double upper = block.elevationMetres;
      const double lower = lowerSurface;

      Chain brinkChain{};
      brinkChain.kind = ChainKind::Brink;
      brinkChain.points = brink;
      brinkChain.closed = run.closed;
      brinkChain.elevationMetres = upper;
      brinkChain.elevations = brinkHeights;
      brinkChain.hasElevation = true;
      geometry.chains.push_back(std::move(brinkChain));

      Chain toeChain{};
      toeChain.kind = ChainKind::Toe;
      toeChain.points = toe;
      toeChain.closed = run.closed;
      toeChain.elevationMetres = lower;
      toeChain.elevations = toeHeights;
      toeChain.hasElevation = true;
      geometry.chains.push_back(std::move(toeChain));

      const size_t spans = run.closed ? count : count - 1u;
      for (size_t index = 0u; index < spans; ++index)
      {
        const size_t next = (index + 1u) % count;
        StepQuad quad{};
        quad.brinkA = brink[index];
        quad.brinkB = brink[next];
        quad.toeA = toe[index];
        quad.toeB = toe[next];
        quad.brinkAHeight = brinkHeights[index];
        quad.brinkBHeight = brinkHeights[next];
        quad.toeAHeight = toeHeights[index];
        quad.toeBHeight = toeHeights[next];
        // Role first, place second. A cliff face is rock whatever biome it
        // stands in; a ramp's walking surface is cover even though it is part
        // of the same step; the shoulders either side of that ramp stay rock.
        // The ramp is the case that proves the rule.
        const double gradient = riser /
            std::max(1.0e-6, 0.5 * (setbacks[index] + setbacks[next]));
        quad.walkable = gradient <=
            settings.ramps.maximumWalkableGradient * (1.0 + 1.0e-9);
        quad.role = quad.walkable ? SurfaceRole::RampSurface
                                  : (gradient < 0.6 ? SurfaceRole::Talus
                                                    : SurfaceRole::Face);
        quad.lithology = rock;
        quad.cover = world::biomeOf(world.graph.node(
            block.cells.empty() ? 0u : block.cells.front()));
        quad.material = settings.faceMaterial;
        geometry.quads.push_back(quad);
        geometry.totalBrinkLengthMetres += distance(brink[index], brink[next]);
      }

      // The gentlest point of the face is the ramp, and it is an edge between
      // *these two terraces* - which may be two blocks, not a block and the
      // ground. Tolerance because a ramp sets the setback to exactly
      // riser/bound, and riser/(riser/bound) is not exactly bound in floating
      // point: without it every ramp fails the test it was built to pass.
      double gentlest = 1.0e30;
      size_t gentlestAt = 0u;
      for (size_t index = 0u; index < count; ++index)
      {
        if (!(setbacks[index] > 0.0))
          continue;
        const double gradient = riser / setbacks[index];
        if (gradient < gentlest)
        {
          gentlest = gradient;
          gentlestAt = index;
        }
      }
      if (gentlest <= settings.ramps.maximumWalkableGradient * (1.0 + 1.0e-9))
      {
        TerraceEdge edge{};
        edge.from = lowerTerrace;
        edge.to = upperBlock;
        edge.gradient = gentlest;
        edge.widthMetres =
            std::min(settings.ramps.rampWidthMetres, runLength * 0.6);
        edge.position = brink[gentlestAt];
        geometry.terraces.edges.push_back(edge);
        geometry.terraces.steepestRampGradient =
            std::max(geometry.terraces.steepestRampGradient, gentlest);
      }

      ++geometry.brinkCount;
      riserTotal += riser;
    }
  }

  // Reachability is a traversal from the base terrace, not a count of ramps.
  // A block whose only ramp climbs from another block that is itself unreachable
  // is unreachable, and only walking the graph says so.
  geometry.terraces.blockCount =
      static_cast<uint32_t>(world.plateaus.blocks.size());
  std::vector<std::vector<uint32_t>> adjacency(world.plateaus.blocks.size());
  std::vector<uint32_t> fromBase;
  for (const TerraceEdge &edge : geometry.terraces.edges)
  {
    if (edge.from == kBaseTerrace && edge.to < adjacency.size())
    {
      fromBase.push_back(edge.to);
      continue;
    }
    if (edge.from < adjacency.size() && edge.to < adjacency.size())
    {
      adjacency[edge.from].push_back(edge.to);
      adjacency[edge.to].push_back(edge.from);
    }
  }
  std::vector<uint8_t> reached(world.plateaus.blocks.size(), 0u);
  std::vector<uint32_t> stack = fromBase;
  for (const uint32_t block : fromBase)
    reached[block] = 1u;
  while (!stack.empty())
  {
    const uint32_t block = stack.back();
    stack.pop_back();
    for (const uint32_t next : adjacency[block])
      if (reached[next] == 0u)
      {
        reached[next] = 1u;
        stack.push_back(next);
      }
  }
  for (uint32_t block = 0u; block < reached.size(); ++block)
    if (reached[block] == 0u)
      geometry.terraces.unreachableBlocks.push_back(block);

  if (geometry.brinkCount != 0u)
    geometry.meanRiserMetres = riserTotal / static_cast<double>(geometry.brinkCount);
  if (setbackSamples != 0u)
    geometry.meanSetbackMetres =
        setbackTotal / static_cast<double>(setbackSamples);
  return geometry;
}

} // namespace procedural::geometry
