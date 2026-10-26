#include "RoadGrowth.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <string>

namespace procedural::world::generator {

namespace {

constexpr double kPi = 3.14159265358979323846;

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

double turn(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

Position lerp(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}

Position projectOnto(Position p, Position a, Position b)
{
  const double l = distance(a, b);

  if (l < 1e-9)
  {
    return a;
  }

  const double t = std::clamp(((p.x - a.x) * (b.x - a.x) + (p.z - a.z) * (b.z - a.z)) / (l * l), 0., 1.);

  return lerp(a, b, t);
}

double wrapAngle(double a)
{
  while (a > kPi)
  {
    a -= 2 * kPi;
  }

  while (a < -kPi)
  {
    a += 2 * kPi;
  }

  return a;
}

struct RoadModule
{
  int delay = 0;
  bool highway = true;
  uint32_t generation = 0;
  Position origin;
  double bearing = 0;
  double length = 0;
  uint64_t order = 0; // Insertion order, for a stable queue.
};

struct Later
{
  bool operator()(const RoadModule &x, const RoadModule &y) const
  {
    return x.delay != y.delay ? x.delay > y.delay : x.order > y.order;
  }
};

} // namespace

RoadGrowth::RoadGrowth(uint64_t domain) : Generator("city.road-growth", domain, "city.road-growth-input", "city.road-network", GenerationScope::Patch)
{
}

bool RoadGrowth::run(const RoadGrowthInput &input, RoadNetwork &output, Ledger &ledger, std::string *error)
{
  return grow(input, output, ledger, error);
}

bool RoadGrowth::grow(const RoadGrowthInput &input, RoadNetwork &output, Ledger &ledger, std::string *error)
{
  if (!input.onLand || !input.covered || !input.terrain || !input.random || input.highwayLength <= 0 || input.streetLength <= 0 || input.maximumSegments == 0)
  {
    if (error != nullptr)
    {
      *error = "road growth needs land, coverage, terrain and random callbacks and positive lengths";
    }

    return false;
  }

  auto density = [&](Position p) {
    return input.density ? std::clamp(input.density(p), 0., 1.) : 1.;
  };

  auto slopeBetween = [&](Position p, Position q) {
    return std::abs(input.terrain(q.x, q.z) - input.terrain(p.x, p.z)) / std::max(1., distance(p, q));
  };

  std::priority_queue<RoadModule, std::vector<RoadModule>, Later> queue;
  uint64_t order = 0;

  for (const auto &seed : input.seeds)
  {
    if (input.onLand(seed.origin))
    {
      RoadModule module;
      module.delay = seed.delay;
      module.highway = seed.highway;
      module.origin = seed.origin;
      module.bearing = seed.bearing;
      module.length = seed.highway ? input.highwayLength : input.streetLength;
      module.order = order++;
      queue.push(module);
    }
  }

  auto &segments = output.segments;

  auto degreeAt = [&](Position p) {
    unsigned degree = 0;

    for (const auto &segment : segments)
    {
      degree += distance(segment.a, p) < 1e-6;
      degree += distance(segment.b, p) < 1e-6;
    }

    return degree;
  };

  auto sharpAt = [&](Position vertex, Position other) {
    for (const auto &segment : segments)
    {
      for (auto [u, v] : {std::make_pair(segment.a, segment.b), std::make_pair(segment.b, segment.a)})
      {
        if (distance(u, vertex) < 1e-6)
        {
          const double ax = v.x - u.x, az = v.z - u.z, bx = other.x - u.x, bz = other.z - u.z;
          const double angle = std::acos(std::clamp((ax * bx + az * bz) / std::max(1e-9, std::hypot(ax, az) * std::hypot(bx, bz)), -1., 1.));

          if (angle < input.minimumAngleRadians)
          {
            return true;
          }
        }
      }
    }

    return false;
  };

  // Global goals (3.2): the bearing a module wants, by road class and pattern.
  auto patternBearing = [&](const RoadModule &module, double proposed) {
    switch (input.pattern)
    {
    case RoadPattern::Raster: {
      // Snap to the nearest multiple of ninety degrees about the raster axis.
      const double relative = wrapAngle(proposed - input.rasterAxisRadians);
      const double snapped = std::round(relative / (kPi / 2)) * (kPi / 2);
      return input.rasterAxisRadians + snapped;
    }
    case RoadPattern::Radial: {
      // Spokes run away from the centre, rings around it: keep whichever the
      // module is closer to.
      const double radial = std::atan2(module.origin.z - input.centre.z, module.origin.x - input.centre.x);
      const double tangential = radial + kPi / 2;
      double best = proposed, bestDelta = 1e30;

      for (double candidate : {radial, radial + kPi, tangential, tangential + kPi})
      {
        const double delta = std::abs(wrapAngle(candidate - proposed));

        if (delta < bestDelta)
        {
          bestDelta = delta;
          best = candidate;
        }
      }

      return best;
    }
    case RoadPattern::Elevation: {
      double best = proposed, bestCost = 1e30;

      for (double deviation : {0., .2, -.2, .4, -.4, .6, -.6})
      {
        const Position end{module.origin.x + std::cos(proposed + deviation) * module.length, 0, module.origin.z + std::sin(proposed + deviation) * module.length};
        const double cost = slopeBetween(module.origin, end) * 12 + std::abs(deviation) * .4;

        if (cost < bestCost)
        {
          bestCost = cost;
          best = proposed + deviation;
        }
      }

      return best;
    }
    default:
      return proposed;
    }
  };

  double fanWeight = 0; // Weight of the densest ray of the last highway goal.

  auto highwayBearing = [&](const RoadModule &module) {
    // Sample the population along rays in a fan; the densest ray wins, with a
    // mild preference for going straight (the paper's basic rule).
    double best = module.bearing, bestWeight = -1;

    for (int k = -4; k <= 4; ++k)
    {
      const double bearing = module.bearing + k * .2;
      double weight = 0;

      for (int step = 1; step <= 4; ++step)
      {
        const Position sample{module.origin.x + std::cos(bearing) * module.length * step, 0, module.origin.z + std::sin(bearing) * module.length * step};
        weight += input.onLand(sample) ? density(sample) / step : 0;
      }

      weight -= std::abs(k) * .05;

      if (weight > bestWeight)
      {
        bestWeight = weight;
        best = bearing;
      }
    }

    fanWeight = bestWeight;
    return patternBearing(module, best);
  };

  // Local constraints (3.3), applied in the paper's order.
  auto localConstraints = [&](RoadModule &module, Position &end, bool &terminal) {
    end = {module.origin.x + std::cos(module.bearing) * module.length, 0, module.origin.z + std::sin(module.bearing) * module.length};
    terminal = false;

    // Legal area: prune, then rotate, then fail.
    bool legal = input.onLand(end) && input.covered(module.origin, end);

    if (!legal)
    {
      for (double fraction : {.8, .65, 1 - input.maximumPruneFraction})
      {
        const Position candidate = lerp(module.origin, end, fraction);

        if (input.onLand(candidate) && input.covered(module.origin, candidate))
        {
          end = candidate;
          legal = true;
          ++output.pruned;
          break;
        }
      }
    }

    if (!legal)
    {
      for (double rotation : {.25, -.25, input.maximumRotationRadians, -input.maximumRotationRadians})
      {
        const Position candidate{module.origin.x + std::cos(module.bearing + rotation) * module.length * .8, 0, module.origin.z + std::sin(module.bearing + rotation) * module.length * .8};

        if (input.onLand(candidate) && input.covered(module.origin, candidate))
        {
          end = candidate;
          module.bearing += rotation;
          legal = true;
          ++output.rotated;
          break;
        }
      }
    }

    if (!legal)
    {
      ++output.failedIllegal;
      return false;
    }

    // Crossing an existing road: cut at the first crossing.
    double earliest = 1;

    for (const auto &segment : segments)
    {
      if (distance(segment.a, module.origin) < 1e-6 || distance(segment.b, module.origin) < 1e-6)
      {
        continue;
      }

      const double d1 = turn(segment.a, segment.b, module.origin), d2 = turn(segment.a, segment.b, end);
      const double d3 = turn(module.origin, end, segment.a), d4 = turn(module.origin, end, segment.b);

      if (d1 * d2 < 0 && d3 * d4 < 0)
      {
        earliest = std::min(earliest, d1 / (d1 - d2));
      }
    }

    if (earliest < 1)
    {
      end = lerp(module.origin, end, earliest);
      terminal = true;
      ++output.cut;
    }

    // An end near a destination snaps to it and stops there.
    for (const auto &point : input.snapPoints)
    {
      if (distance(point, end) < input.intersectionSnapMetres && distance(point, module.origin) > 1e-6)
      {
        end = point;
        terminal = true;
        ++output.snappedCrossing;
      }
    }

    // An end near an existing crossing snaps to it.
    for (const auto &segment : segments)
    {
      for (auto vertex : {segment.a, segment.b})
      {
        if (distance(vertex, end) < input.intersectionSnapMetres && distance(vertex, module.origin) > 1e-6)
        {
          end = vertex;
          terminal = true;
          ++output.snappedCrossing;
        }
      }
    }

    // An end near an existing road is extended to it.
    if (!terminal)
    {
      for (const auto &segment : segments)
      {
        const Position foot = projectOnto(end, segment.a, segment.b);

        if (distance(foot, end) < input.roadSnapMetres && distance(foot, module.origin) > 1e-6 && distance(foot, segment.a) > 1 && distance(foot, segment.b) > 1)
        {
          end = foot;
          terminal = true;
          ++output.snappedRoad;
          break;
        }
      }
    }

    if (distance(module.origin, end) < module.length * .3 || !input.covered(module.origin, end))
    {
      ++output.failedShort;
      return false;
    }

    if (sharpAt(module.origin, end) || sharpAt(end, module.origin))
    {
      ++output.failedAngle;
      return false;
    }

    if (degreeAt(end) >= input.maximumDegree || degreeAt(module.origin) > input.maximumDegree)
    {
      ++output.failedDegree;
      return false;
    }

    return true;
  };

  const uint64_t moduleBudget = uint64_t(input.maximumSegments) * 16;

  while (!queue.empty() && segments.size() < input.maximumSegments && output.modules < moduleBudget)
  {
    ++output.modules;
    RoadModule module = queue.top();
    queue.pop();

    // p1 and p2: delays.
    if (module.delay < 0)
    {
      continue;
    }

    if (module.delay > 0)
    {
      module.delay -= 1;
      module.order = order++;
      queue.push(module);
      continue;
    }

    // Global goal: the bearing the module wants. A highway with no population
    // ahead ends (the paper's highways run between population centres).
    module.bearing = module.highway ? highwayBearing(module) : patternBearing(module, module.bearing);

    if (module.highway && input.terminalDensity > 0 && fanWeight < input.terminalDensity && module.generation > 0)
    {
      continue;
    }

    // p3: local constraints.
    Position end;
    bool terminal = false;

    if (!localConstraints(module, end, terminal))
    {
      continue;
    }

    // p4: the segment and the modules it proposes.
    segments.push_back({module.origin, end, module.highway, module.generation});

    if (terminal)
    {
      continue;
    }

    RoadModule ahead = module;
    ahead.origin = end;
    ahead.generation = module.generation + 1;
    ahead.delay = 0;
    ahead.order = order++;
    queue.push(ahead);

    const double here = density(end);

    for (int side : {-1, 1})
    {
      const double draw = input.random(segments.size() * 4 + uint64_t(side + 1), 97);
      const double sideBearing = module.bearing + side * kPi / 2;
      const Position probe{end.x + std::cos(sideBearing) * module.length, 0, end.z + std::sin(sideBearing) * module.length};
      const double there = input.onLand(probe) ? density(probe) : 0;

      if (module.highway && draw < input.highwayBranchProbability * (0.5 + there))
      {
        RoadModule branch = module;
        branch.origin = end;
        branch.bearing = sideBearing;
        branch.generation = module.generation + 1;
        branch.delay = 1;
        branch.order = order++;
        queue.push(branch);
      }
      else if (draw < input.streetBranchProbability * (input.branchDensityFloor + (1 - input.branchDensityFloor) * std::max(here, there)))
      {
        RoadModule street;
        street.highway = false;
        street.origin = end;
        street.bearing = sideBearing;
        street.length = input.streetLength;
        street.generation = module.generation + 1;
        street.delay = module.highway ? input.streetDelay : 1;
        street.order = order++;
        queue.push(street);
      }
    }
  }

  if (segments.empty())
  {
    ledger.add(LedgerKind::Rejected, 0, "road growth: no segment survived the local constraints");
  }

  if (segments.size() >= input.maximumSegments)
  {
    ledger.add(LedgerKind::Capacity, 0, "road growth: segment budget reached");
  }

  ledger.add(
      LedgerKind::Repaired,
      0,
      "road growth: " + std::to_string(segments.size()) + " segments from " + std::to_string(output.modules) + " modules; pruned " + std::to_string(output.pruned) + ", rotated " + std::to_string(output.rotated) +
          ", cut " + std::to_string(output.cut) + ", snapped " + std::to_string(output.snappedCrossing + output.snappedRoad));
  return true;
}

} // namespace procedural::world::generator
