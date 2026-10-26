#include "WorldSuitability.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <set>

namespace procedural::world::rewrite {
size_t WorldSuitability::cellAt(double x, double z) const
{
  if (cells_.empty() || !std::isfinite(x) || !std::isfinite(z) || x < settings_.minX || z < settings_.minZ || x >= settings_.maxX || z >= settings_.maxZ)
  {
    return SIZE_MAX;
  }
  return size_t((z - settings_.minZ) / settings_.spacingMetres) * columns_ + size_t((x - settings_.minX) / settings_.spacingMetres);
}

std::vector<uint32_t> WorldSuitability::neighbors(uint32_t i) const
{
  std::vector<uint32_t> out;
  if (i >= cells_.size())
  {
    return out;
  }
  if (i % columns_)
  {
    out.push_back(i - 1);
  }
  if (i % columns_ + 1 < columns_)
  {
    out.push_back(i + 1);
  }
  if (i >= columns_)
  {
    out.push_back(i - columns_);
  }
  if (i + columns_ < cells_.size())
  {
    out.push_back(i + columns_);
  }
  return out;
}

bool WorldSuitability::build(
    const WorldPhysicalTerrain &physical,
    const WorldRiverChannels &channels,
    const Field &soil,
    const Field &moisture,
    const std::vector<Position> &seeds,
    const SuitabilitySettings &s,
    uint64_t revision,
    std::string *error,
    const std::function<bool(double, double)> &additionalWater)
{
  auto fail = [&](const char *message) {
    if (error)
    {
      *error = message;
    }
    return false;
  };
  if (!soil || !moisture || !revision || seeds.empty() || seeds.size() > 4096)
  {
    return fail("Missing suitability dependencies or access seeds");
  }
  for (double v :
       {s.minX,
        s.minZ,
        s.maxX,
        s.maxZ,
        s.spacingMetres,
        s.maximumBuildGrade,
        s.maximumTravelGrade,
        s.maximumReliefMetres,
        s.brinkSetbackMetres,
        s.waterSetbackMetres,
        s.floodReachMetres,
        s.floodRiseMetres,
        s.minimumSoil,
        s.maximumMoisture})
  {
    if (!std::isfinite(v) || std::abs(v) > 1e8)
    {
      return fail("Non-finite suitability settings");
    }
  }
  if (s.spacingMetres < 1 || s.spacingMetres > 20 || s.maxX <= s.minX || s.maxZ <= s.minZ || s.maximumBuildGrade <= 0 || s.maximumTravelGrade < s.maximumBuildGrade || s.maximumReliefMetres <= 0 ||
      s.brinkSetbackMetres < 0 || s.waterSetbackMetres < 0 || s.floodReachMetres < s.waterSetbackMetres || s.floodRiseMetres < 0 || s.minimumSoil < 0 || s.minimumSoil > 1 || s.maximumMoisture < 0 ||
      s.maximumMoisture > 1)
  {
    return fail("Invalid suitability settings");
  }
  double nx = (s.maxX - s.minX) / s.spacingMetres, nz = (s.maxZ - s.minZ) / s.spacingMetres;
  if (nx * nz > 65536 || nx < 1 || nz < 1 || std::abs(nx - std::round(nx)) > 1e-8 || std::abs(nz - std::round(nz)) > 1e-8)
  {
    return fail("Suitability domain must fit a bounded whole-cell grid");
  }
  if (!channels.validateReservations(physical, error))
  {
    return false;
  }
  WorldSuitability result;
  result.settings_ = s;
  result.columns_ = uint32_t(std::round(nx));
  result.rows_ = uint32_t(std::round(nz));
  result.cells_.resize(result.columns_ * result.rows_);
  auto base = [&](double x, double z) {
    PhysicalSample p;
    return physical.sample(x, z, p) ? p.height : std::numeric_limits<double>::quiet_NaN();
  };
  auto surface = [&](double x, double z, ChannelSample &p) {
    return channels.sample(x, z, base, p);
  };
  for (uint32_t id = 0; id < result.cells_.size(); ++id)
  {
    auto &c = result.cells_[id];
    c.center = {s.minX + (id % result.columns_ + .5) * s.spacingMetres, 0, s.minZ + (id / result.columns_ + .5) * s.spacingMetres};
    double heights[9], lo = 1e30, hi = -1e30;
    c.soil = 1;
    c.moisture = 0;
    bool featureAccessible = true;
    for (int j = 0; j < 3; ++j)
    {
      for (int i = 0; i < 3; ++i)
      {
        double x = c.center.x + (i - 1) * s.spacingMetres * .5, z = c.center.z + (j - 1) * s.spacingMetres * .5;
        ChannelSample water;
        PhysicalSample p;
        SplineSurfaceSample original;
        double fertility = soil(x, z), wetness = moisture(x, z);
        if (!surface(x, z, water) || !physical.sample(x, z, p) || !physical.beforeModifications(x, z, original) || !std::isfinite(fertility) || !std::isfinite(wetness) || fertility < 0 || fertility > 1 || wetness < 0 ||
            wetness > 1)
        {
          return fail("Unresolved terrain or invalid soil/moisture field");
        }
        heights[j * 3 + i] = water.terrainHeight;
        lo = std::min(lo, water.terrainHeight);
        hi = std::max(hi, water.terrainHeight);
        c.soil = std::min(c.soil, fertility);
        c.moisture = std::max(c.moisture, wetness);
        if (water.wet || (additionalWater && additionalWater(x, z)))
        {
          c.reasons |= Wet;
        }
        if (p.rampId)
        {
          c.reasons |= Ramp;
        }
        if (!p.rampId && (p.region == SplineRegion::FreeFace || p.region == SplineRegion::WeatheredFace))
        {
          c.reasons |= Cliff;
        }
        if (p.region == SplineRegion::Apron)
        {
          c.reasons |= Talus;
        }
        if (p.region == SplineRegion::Brink)
        {
          c.reasons |= Brink;
        }
        if (original.region == SplineRegion::Cap && original.featureId && !physical.reachableFeatures().count(original.featureId))
        {
          featureAccessible = false;
        }
        for (auto &feature : physical.landforms().features())
        {
          SplineStation station;
          if (!physical.landforms().station(feature.id, x, z, station))
          {
            return fail("Unresolved brink distance");
          }
          if (station.distanceMetres <= s.brinkSetbackMetres)
          {
            c.reasons |= Brink;
          }
        }
        for (size_t reach = 0; reach < channels.reaches().size(); ++reach)
        {
          const auto &r = channels.reaches()[reach];
          double station = 0, distance = r.path.project(x, z, station);
          ChannelSection section;
          double level;
          if (!channels.section(reach, station, section, level))
          {
            return fail("Unresolved flood profile");
          }
          double bank = std::max(0., distance - section.waterWidth * .5);
          c.waterDistance = std::min(c.waterDistance, bank);
          if (bank <= s.waterSetbackMetres || (bank <= s.floodReachMetres && water.terrainHeight <= level + s.floodRiseMetres))
          {
            c.reasons |= Flood;
          }
        }
      }
    }
    c.center.y = heights[4];
    c.relief = hi - lo;
    for (int j = 0; j < 3; ++j)
    {
      for (int i = 0; i < 3; ++i)
      {
        if (i < 2)
        {
          c.slope = std::max(c.slope, std::abs(heights[j * 3 + i + 1] - heights[j * 3 + i]) / (s.spacingMetres * .5));
        }
        if (j < 2)
        {
          c.slope = std::max(c.slope, std::abs(heights[j * 3 + i + 3] - heights[j * 3 + i]) / (s.spacingMetres * .5));
        }
      }
    }
    // Bound the gradient magnitude even on a diagonal hillside.
    c.slope *= std::sqrt(2.);
    if (c.slope > s.maximumBuildGrade)
    {
      c.reasons |= Steep;
    }
    if (c.relief > s.maximumReliefMetres)
    {
      c.reasons |= Rough;
    }
    if (c.soil < s.minimumSoil)
    {
      c.reasons |= PoorSoil;
    }
    if (c.moisture > s.maximumMoisture)
    {
      c.reasons |= Saturated;
    }
    c.walkable = featureAccessible && !(c.reasons & (Wet | Cliff)) && c.slope <= s.maximumTravelGrade;
    if (!featureAccessible)
    {
      c.reasons |= Inaccessible;
    }
    c.score = .45 * c.soil + .2 * (1 - std::abs(c.moisture - .5) * 2) + .2 * std::max(0., 1 - c.slope / s.maximumBuildGrade) + .15 * std::exp(-c.waterDistance / 150);
  }
  // Resolve walk connectivity, checking the final surface along every portal.
  uint32_t component = 0;
  for (uint32_t id = 0; id < result.cells_.size(); ++id)
  {
    if (!result.cells_[id].walkable || result.cells_[id].component != UINT32_MAX)
    {
      continue;
    }
    std::queue<uint32_t> queue;
    queue.push(id);
    result.cells_[id].component = component;
    while (!queue.empty())
    {
      auto current = queue.front();
      queue.pop();
      for (auto next : result.neighbors(current))
      {
        auto &n = result.cells_[next];
        if (!n.walkable || n.component != UINT32_MAX)
        {
          continue;
        }
        auto a = result.cells_[current].center, b = n.center;
        double previous = a.y;
        bool connected = true;
        int probes = int(std::ceil(s.spacingMetres));
        for (int k = 1; k <= probes; ++k)
        {
          double t = double(k) / probes;
          ChannelSample p;
          if (!surface(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t, p) || p.wet || (additionalWater && additionalWater(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t)) ||
              std::abs(p.terrainHeight - previous) > s.maximumTravelGrade * s.spacingMetres / probes + 1e-8)
          {
            connected = false;
            break;
          }
          previous = p.terrainHeight;
        }
        if (connected)
        {
          n.component = component;
          queue.push(next);
        }
      }
    }
    ++component;
  }
  std::set<uint32_t> accessible;
  for (auto seed : seeds)
  {
    size_t id = result.cellAt(seed.x, seed.z);
    if (id == SIZE_MAX || !result.cells_[id].walkable)
    {
      return fail("Access seed must lie on traversable final terrain");
    }
    accessible.insert(result.cells_[id].component);
  }
  for (auto &c : result.cells_)
  {
    if (!accessible.count(c.component))
    {
      c.reasons |= Inaccessible;
    }
  }
  // Fingerprint semantic sampled inputs plus explicit upstream dependency revision.
  uint64_t hash = 1469598103934665603ULL;
  auto mix = [&](uint64_t v) {
    for (int i = 0; i < 8; ++i)
    {
      hash ^= (v >> (i * 8)) & 255;
      hash *= 1099511628211ULL;
    }
  };
  auto number = [&](double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    mix(bits);
  };
  mix(revision);
  mix(result.columns_);
  mix(result.rows_);
  number(s.spacingMetres);
  for (auto &c : result.cells_)
  {
    for (double v : {c.center.x, c.center.y, c.center.z, c.slope, c.relief, c.soil, c.moisture, c.waterDistance, c.score})
    {
      number(v);
    }
    mix(c.reasons);
    mix(c.component);
    mix(c.walkable);
  }
  result.fingerprint_ = hash;
  *this = std::move(result);
  return true;
}

namespace {
bool valid(const SettlementSettings &s)
{
  for (double v : {s.townAreaMetres2, s.villageAreaMetres2, s.expansionBandMetres, s.minimumExpansionRatio, s.minimumCompactness, s.maximumAspect, s.maximumTownDistanceMetres, s.minimumSettlementGapMetres})
  {
    if (!std::isfinite(v) || v < 0 || v > 1e7)
    {
      return false;
    }
  }
  return s.townAreaMetres2 > 0 && s.villageAreaMetres2 > 0 && s.minimumExpansionRatio <= 2 && s.minimumCompactness <= 1 && s.maximumAspect >= 1 && s.candidateStrideCells > 0 && s.maximumCandidates > 0 &&
         s.maximumCandidates <= 1024 && s.towns <= 32 && s.villages <= 128;
}
} // namespace

bool evaluateSettlement(const WorldSuitability &world, uint32_t seed, bool town, const SettlementSettings &s, SettlementCandidate &out, std::string *error)
{
  auto fail = [&](const char *message) {
    if (error)
    {
      *error = message;
    }
    return false;
  };
  if (!valid(s) || seed >= world.cells().size())
  {
    return fail("Invalid settlement candidate/settings");
  }
  const auto &cells = world.cells();
  double spacing = world.settings().spacingMetres, unit = spacing * spacing;
  size_t target = size_t(std::ceil((town ? s.townAreaMetres2 : s.villageAreaMetres2) / unit));
  if (target > 4096 || s.expansionBandMetres / spacing > 32)
  {
    return fail("Settlement footprint budget exceeded");
  }
  SettlementCandidate candidate;
  candidate.seed = seed;
  candidate.town = town;
  candidate.reasons = cells[seed].reasons;
  if (candidate.reasons)
  {
    out = candidate;
    return true;
  }
  using Entry = std::pair<double, uint32_t>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
  std::vector<uint8_t> state(cells.size());
  queue.push({0, seed});
  state[seed] = 1;
  while (!queue.empty() && candidate.core.size() < target)
  {
    auto id = queue.top().second;
    queue.pop();
    state[id] = 2;
    candidate.core.push_back(id);
    candidate.score += cells[id].score;
    for (auto next : world.neighbors(id))
    {
      if (!state[next] && !cells[next].reasons && cells[next].component == cells[seed].component)
      {
        auto p = cells[next].center, a = cells[seed].center;
        double key = ((p.x - a.x) * (p.x - a.x) + (p.z - a.z) * (p.z - a.z)) / (target * unit) - cells[next].score * .5;
        queue.push({key, next});
        state[next] = 1;
      }
    }
  }
  if (candidate.core.size() < target)
  {
    candidate.reasons |= InsufficientArea;
  }
  if (!candidate.core.empty())
  {
    candidate.score /= candidate.core.size();
  }
  double perimeter = 0, minX = 1e30, minZ = 1e30, maxX = -1e30, maxZ = -1e30;
  for (auto id : candidate.core)
  {
    auto p = cells[id].center;
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minZ = std::min(minZ, p.z);
    maxZ = std::max(maxZ, p.z);
    int inside = 0;
    for (auto next : world.neighbors(id))
    {
      inside += state[next] == 2;
    }
    perimeter += (4 - inside) * spacing;
  }
  if (!candidate.core.empty())
  {
    double width = maxX - minX + spacing, height = maxZ - minZ + spacing;
    double compactness = 4 * 3.141592653589793 * candidate.core.size() * unit / (perimeter * perimeter);
    if (compactness < s.minimumCompactness || std::max(width / height, height / width) > s.maximumAspect)
    {
      candidate.reasons |= Narrow;
    }
  }
  // Geodesic expansion stays on suitable connected ground; no reservation crosses a hazard.
  std::queue<std::pair<uint32_t, int>> band;
  std::vector<bool> visited(cells.size());
  for (auto id : candidate.core)
  {
    band.push({id, 0});
    visited[id] = true;
  }
  int rings = int(std::ceil(s.expansionBandMetres / spacing));
  while (!band.empty())
  {
    auto [id, depth] = band.front();
    band.pop();
    if (depth >= rings)
    {
      continue;
    }
    for (auto next : world.neighbors(id))
    {
      if (!visited[next] && !cells[next].reasons && cells[next].component == cells[seed].component)
      {
        visited[next] = true;
        candidate.expansion.push_back(next);
        band.push({next, depth + 1});
      }
    }
  }
  if (candidate.expansion.size() < candidate.core.size() * s.minimumExpansionRatio)
  {
    candidate.reasons |= ExpansionBlocked;
  }
  std::sort(candidate.core.begin(), candidate.core.end());
  std::sort(candidate.expansion.begin(), candidate.expansion.end());
  out = std::move(candidate);
  return true;
}

bool reserveSettlements(const WorldSuitability &world, const SettlementSettings &s, SettlementProduct &out, std::string *error)
{
  if (!valid(s) || world.cells().empty())
  {
    if (error)
    {
      *error = "Invalid settlement reservation inputs";
    }
    return false;
  }
  SettlementProduct result;
  result.suitabilityFingerprint = world.fingerprint();
  std::vector<bool> occupied(world.cells().size());
  uint64_t nextId = 1, gapComparisons = 0;
  for (bool town : {true, false})
  {
    std::vector<SettlementCandidate> candidates;
    for (uint32_t z = 0; z < world.rows(); z += s.candidateStrideCells)
    {
      for (uint32_t x = 0; x < world.columns(); x += s.candidateStrideCells)
      {
        if (candidates.size() >= s.maximumCandidates)
        {
          if (error)
          {
            *error = "Candidate budget exceeded; choose a coarser stride";
          }
          return false;
        }
        SettlementCandidate c;
        if (!evaluateSettlement(world, z * world.columns() + x, town, s, c, error))
        {
          return false;
        }
        candidates.push_back(std::move(c));
      }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
      return a.score != b.score ? a.score > b.score : a.seed < b.seed;
    });
    uint32_t wanted = town ? s.towns : s.villages, accepted = 0;
    for (auto &c : candidates)
    {
      if (c.reasons)
      {
        result.candidates.push_back(std::move(c));
        continue;
      }
      if (accepted >= wanted)
      {
        c.reasons |= Quota;
        result.candidates.push_back(std::move(c));
        continue;
      }
      for (auto id : c.core)
      {
        if (occupied[id])
        {
          c.reasons |= Occupied;
        }
      }
      for (auto id : c.expansion)
      {
        if (occupied[id])
        {
          c.reasons |= Occupied;
        }
      }
      for (const auto &existing : result.reservations)
      {
        for (auto id : c.core)
        {
          if (c.reasons & TooClose)
          {
            break;
          }
          for (auto other : existing.core)
          {
            if (++gapComparisons > 16000000)
            {
              if (error)
              {
                *error = "Settlement separation comparison budget exceeded";
              }
              return false;
            }
            auto a = world.cells()[id].center, b = world.cells()[other].center;
            double dx = std::max(0., std::abs(a.x - b.x) - world.settings().spacingMetres), dz = std::max(0., std::abs(a.z - b.z) - world.settings().spacingMetres);
            if (std::hypot(dx, dz) < s.minimumSettlementGapMetres)
            {
              c.reasons |= TooClose;
              break;
            }
          }
        }
      }
      uint64_t parent = 0;
      auto center = world.cells()[c.seed].center;
      if (!town)
      {
        double best = s.maximumTownDistanceMetres;
        for (const auto &r : result.reservations)
        {
          if (r.town && r.component == world.cells()[c.seed].component)
          {
            double distance = std::hypot(center.x - r.center.x, center.z - r.center.z);
            if (distance < best)
            {
              best = distance;
              parent = r.id;
            }
          }
        }
        if (!parent)
        {
          c.reasons |= NoTown;
        }
      }
      if (!c.reasons && accepted >= wanted)
      {
        c.reasons |= Quota;
      }
      if (!c.reasons && accepted < wanted)
      {
        SettlementReservation r;
        r.id = nextId++;
        r.parentTown = parent;
        r.town = town;
        r.center = center;
        r.areaMetres2 = c.core.size() * world.settings().spacingMetres * world.settings().spacingMetres;
        r.score = c.score;
        r.component = world.cells()[c.seed].component;
        r.core = c.core;
        r.expansion = c.expansion;
        for (auto id : c.core)
        {
          occupied[id] = true;
        }
        for (auto id : c.expansion)
        {
          occupied[id] = true;
        }
        result.reservations.push_back(std::move(r));
        ++accepted;
      }
      result.candidates.push_back(std::move(c));
    }
    if (town)
    {
      result.unfilledTowns = wanted - accepted;
    }
    else
    {
      result.unfilledVillages = wanted - accepted;
    }
  }
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
