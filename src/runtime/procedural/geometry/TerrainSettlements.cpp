#include "procedural/geometry/TerrainSettlements.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

namespace procedural::geometry
{
namespace
{
// A response curve that peaks at `ideal` and falls away over `falloff`, rather
// than a threshold. The difference matters: a threshold puts every town exactly
// on the bank, a curve puts them a sensible walk back from it.
double peak(const double value, const double ideal, const double falloff)
{
  if (!(falloff > 0)) return value == ideal ? 1.0 : 0.0;
  const double t = (value - ideal) / falloff;
  return std::exp(-t * t);
}
// Monotone falloff from zero, for criteria where nearer is simply better.
double decay(const double value, const double falloff)
{
  if (!(falloff > 0)) return 0.0;
  const double t = std::max(0.0, value) / falloff;
  return std::exp(-t * t);
}
double smoothstep(double t) { t = std::clamp(t, 0.0, 1.0); return t * t * (3 - 2 * t); }
constexpr double kPi = 3.14159265358979323846;
uint64_t mix(uint64_t n)
{
  n ^= n >> 30; n *= 0xbf58476d1ce4e5b9ull; n ^= n >> 27;
  n *= 0x94d049bb133111ebull; return n ^ (n >> 31);
}
double unitRandom(uint64_t n) { return double(mix(n) >> 11) / double(1ull << 53); }

// Polygon area, signed: positive counter-clockwise.
double signedArea(const std::vector<Point2> &polygon)
{
  double total = 0;
  for (size_t k = 0, n = polygon.size(); k < n; ++k)
  {
    const Point2 &a = polygon[k], &b = polygon[(k + 1u) % n];
    total += a.x * b.y - b.x * a.y;
  }
  return total * .5;
}
Point2 centroidOf(const std::vector<Point2> &polygon)
{
  const double area = signedArea(polygon);
  if (std::abs(area) < 1e-9)
  {
    Point2 mean{};
    for (const Point2 &p : polygon) { mean.x += p.x; mean.y += p.y; }
    const double inverse = polygon.empty() ? 0.0 : 1.0 / double(polygon.size());
    return {mean.x * inverse, mean.y * inverse};
  }
  double cx = 0, cy = 0;
  for (size_t k = 0, n = polygon.size(); k < n; ++k)
  {
    const Point2 &a = polygon[k], &b = polygon[(k + 1u) % n];
    const double cross = a.x * b.y - b.x * a.y;
    cx += (a.x + b.x) * cross; cy += (a.y + b.y) * cross;
  }
  return {cx / (6.0 * area), cy / (6.0 * area)};
}
// Clip a convex polygon to the half-plane of points at least as close to `keep`
// as to `against` - one Voronoi bisector. Sutherland-Hodgman is exact here
// because the subject stays convex: it starts as a box and every cut is a
// half-plane, so the intersection is convex at every step. That is the whole
// reason the wards are built by clipping a box rather than by clipping a
// hand-drawn outline, which would not be convex and would fold.
std::vector<Point2> clipToBisector(const std::vector<Point2> &polygon, const Point2 keep, const Point2 against,
    const double keepWeight = 0.0, const double againstWeight = 0.0)
{
  // side(p) >= 0 keeps p, and is linear: |p-against|^2 - |p-keep|^2. The
  // weights make this a power diagram rather than a plain Voronoi, and the
  // term stays linear, so a weighted cell costs exactly what an unweighted one
  // costs. That is what lets wards differ in size: an unweighted Voronoi with
  // relaxed seeds is a honeycomb, and a honeycomb is unmistakably generated.
  const double nx = 2.0 * (keep.x - against.x), ny = 2.0 * (keep.y - against.y);
  const double offset = against.x * against.x + against.y * against.y -
      keep.x * keep.x - keep.y * keep.y + keepWeight - againstWeight;
  const auto side = [&](const Point2 p) { return nx * p.x + ny * p.y + offset; };
  std::vector<Point2> result;
  result.reserve(polygon.size() + 2u);
  for (size_t k = 0, n = polygon.size(); k < n; ++k)
  {
    const Point2 &a = polygon[k], &b = polygon[(k + 1u) % n];
    const double sa = side(a), sb = side(b);
    if (sa >= 0) result.push_back(a);
    if ((sa >= 0) != (sb >= 0))
    {
      const double t = sa / (sa - sb);
      result.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
    }
  }
  return result;
}
// Vertices are quantised to a centimetre so that the same bisector, clipped
// once for each of the two cells that share it, produces the same key from both
// sides. Without that the union boundary cannot be found at all: every interior
// edge would look unshared.
uint64_t vertexKey(const Point2 p)
{
  const auto qx = uint64_t(int64_t(std::llround(p.x * 100.0)) & 0xFFFFFFFF);
  const auto qy = uint64_t(int64_t(std::llround(p.y * 100.0)) & 0xFFFFFFFF);
  return (qx << 32) | qy;
}
uint64_t edgeKey(const Point2 a, const Point2 b)
{
  const uint64_t ka = vertexKey(a), kb = vertexKey(b);
  return mix(std::min(ka, kb)) ^ (mix(std::max(ka, kb)) * 0x9E3779B97F4A7C15ull);
}
// Chain a set of directed boundary edges into rings, and return the largest.
//
// The union boundary of a set of Voronoi cells is not always one loop: a
// selection can pinch to a single shared vertex, or enclose a ward that was
// refused for being wet, and both leave a vertex with two ways out. Walking
// blindly there fails, which is why half the settlements produced no plan at
// all on the first attempt. At a branch, take the sharpest right turn - the
// standard rule for staying on the outer face of a counter-clockwise traversal
// - and keep every loop that closes, so a hole yields a second ring rather than
// destroying the first.
std::vector<Point2> chainBoundary(const std::vector<std::pair<Point2, Point2>> &edges)
{
  if (edges.empty()) return {};
  std::unordered_map<uint64_t, std::vector<size_t>> outgoing;
  for (size_t k = 0; k < edges.size(); ++k) outgoing[vertexKey(edges[k].first)].push_back(k);
  std::vector<bool> used(edges.size(), false);
  std::vector<Point2> largest;
  double widest = 0;
  for (size_t start = 0; start < edges.size(); ++start)
  {
    if (used[start]) continue;
    std::vector<Point2> ring{edges[start].first};
    size_t current = start;
    used[start] = true;
    for (size_t guard = 0; guard <= edges.size(); ++guard)
    {
      const Point2 at = edges[current].second;
      if (vertexKey(at) == vertexKey(ring.front())) break;
      ring.push_back(at);
      const auto found = outgoing.find(vertexKey(at));
      if (found == outgoing.end()) { ring.clear(); break; }
      const double inx = at.x - edges[current].first.x, iny = at.y - edges[current].first.y;
      size_t next = SIZE_MAX; double sharpest = 0;
      for (const size_t candidate : found->second)
      {
        if (used[candidate]) continue;
        const double outx = edges[candidate].second.x - at.x, outy = edges[candidate].second.y - at.y;
        // atan2 of the turn, measured so that the sharpest right wins.
        const double turn = std::atan2(inx * outy - iny * outx, inx * outx + iny * outy);
        if (next == SIZE_MAX || turn < sharpest) { next = candidate; sharpest = turn; }
      }
      if (next == SIZE_MAX) { ring.clear(); break; }
      used[next] = true; current = next;
    }
    if (ring.size() < 3u) continue;
    const double area = std::abs(signedArea(ring));
    if (area > widest) { widest = area; largest = std::move(ring); }
  }
  return largest;
}

// Smooth a ring towards its own chords, then drop the vertices that are barely
// turning. A wall built straight off a Voronoi boundary has a corner every
// twenty metres, which is not what a curtain wall looks like: masonry is built
// in long straight runs between towers. `keep` protects the gates, which must
// not be smoothed away or moved off the road that arrives at them.
std::vector<Point2> dressRing(const std::vector<Point2> &ring, const double factor,
    const uint32_t rounds, const double minimumRunMetres, const double minimumTurnDegrees,
    const std::unordered_set<uint64_t> &keep)
{
  if (ring.size() < 4u) return ring;
  std::vector<Point2> shape = ring;
  for (uint32_t round = 0; round < rounds; ++round)
  {
    std::vector<Point2> next = shape;
    for (size_t k = 0, n = shape.size(); k < n; ++k)
    {
      if (keep.count(vertexKey(shape[k]))) continue;
      const Point2 &a = shape[(k + n - 1u) % n], &b = shape[(k + 1u) % n];
      // Watabou's smoothVertex is (prev + v*f + next) / (2 + f), which in lerp
      // terms is 2/(2+f) of the way to the chord midpoint - so a *smaller* f
      // smooths *harder*. Passing f straight in as the lerp amount inverted
      // that: at 180 wards his f is 0.22 and means a 0.90 lerp, and we were
      // moving 0.22. Four times too weak, which is the wall full of small turns.
      const double toward = 2.0 / (2.0 + factor);
      next[k] = {shape[k].x + ((a.x + b.x) * .5 - shape[k].x) * toward,
          shape[k].y + ((a.y + b.y) * .5 - shape[k].y) * toward};
    }
    shape.swap(next);
  }
  // Collapse the short runs and the shallow turns into straight masonry.
  const double turnLimit = std::cos(minimumTurnDegrees * kPi / 180.0);
  std::vector<Point2> dressed;
  for (size_t k = 0, n = shape.size(); k < n; ++k)
  {
    const Point2 &here = shape[k];
    if (keep.count(vertexKey(here))) { dressed.push_back(here); continue; }
    if (dressed.size() >= 1u && distance(dressed.back(), here) < minimumRunMetres) continue;
    const Point2 &before = dressed.empty() ? shape[(k + n - 1u) % n] : dressed.back();
    const Point2 &after = shape[(k + 1u) % n];
    const double ax = here.x - before.x, ay = here.y - before.y;
    const double bx = after.x - here.x, by = after.y - here.y;
    const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
    if (la > 1e-6 && lb > 1e-6 && (ax * bx + ay * by) / (la * lb) > turnLimit) continue;
    dressed.push_back(here);
  }
  return dressed.size() >= 3u ? dressed : shape;
}

// Separable box blur, used for the criteria that are about a catchment rather
// than a point.
void blur(std::vector<float> &field, const uint32_t side, const int radius)
{
  if (radius <= 0) return;
  std::vector<float> scratch(field.size());
  const double inverse = 1.0 / double(2 * radius + 1);
  for (uint32_t y = 0; y < side; ++y)
    for (uint32_t x = 0; x < side; ++x)
    {
      double total = 0;
      for (int k = -radius; k <= radius; ++k)
        total += field[size_t(y) * side + uint32_t(std::clamp(int(x) + k, 0, int(side) - 1))];
      scratch[size_t(y) * side + x] = float(total * inverse);
    }
  for (uint32_t y = 0; y < side; ++y)
    for (uint32_t x = 0; x < side; ++x)
    {
      double total = 0;
      for (int k = -radius; k <= radius; ++k)
        total += scratch[size_t(uint32_t(std::clamp(int(y) + k, 0, int(side) - 1))) * side + x];
      field[size_t(y) * side + x] = float(total * inverse);
    }
}
} // namespace

SettlementNetwork planSettlements(const double extentMetres, const SettlementFields &fields,
    const SettlementSettings &settings)
{
  SettlementNetwork network;
  if (!settings.enabled) return network;
  if (!(extentMetres > 0) || settings.resolution < 8u)
    throw std::invalid_argument("invalid settlement domain");
  if (!fields.elevationMetres || !fields.slope || !fields.waterType)
    throw std::invalid_argument("settlement siting needs at least elevation, slope and water type");

  const uint32_t side = settings.resolution;
  const size_t count = size_t(side) * side;
  const double cell = extentMetres / side;
  const auto centre = [&](const uint32_t x, const uint32_t y)
  { return Point2((x + .5) * cell, (y + .5) * cell); };
  const auto sample = [](const std::function<double(Point2)> &fn, const Point2 p, const double fallback)
  { return fn ? fn(p) : fallback; };

  // --- hinterland criteria, blurred over the catchment ---------------------
  std::vector<float> timber(count, 0.f), arable(count, 0.f);
  std::vector<uint8_t> buildable(count, 0u);
  std::vector<float> terrain(count, 0.f);
  for (uint32_t y = 0; y < side; ++y)
    for (uint32_t x = 0; x < side; ++x)
    {
      const size_t id = size_t(y) * side + x;
      const Point2 p = centre(x, y);
      terrain[id] = float(sample(fields.elevationMetres, p, 0.0));
      const bool dry = fields.waterType(p) == 0u;
      timber[id] = dry ? float(std::clamp(sample(fields.treeCover, p, .3), 0.0, 1.0)) : 0.f;
      arable[id] = dry ? float(std::clamp(sample(fields.soilQuality, p, .3), 0.0, 1.0)) : 0.f;
      const double slope = fields.slope(p);
      const double freeboard = terrain[id] - sample(fields.waterLevelMetres, p, 0.0);
      const double temperature = sample(fields.temperatureCelsius, p, 12.0);
      const double moisture = sample(fields.moisture, p, .5);
      buildable[id] = dry && slope <= settings.maximumSlope &&
          freeboard >= settings.minimumFreeboardMetres &&
          temperature >= settings.minimumTemperature &&
          moisture >= settings.minimumMoisture ? 1u : 0u;
    }
  const int catchment = std::max(1, int(std::lround(settings.catchmentRadiusMetres / cell)));
  blur(timber, side, catchment);
  blur(arable, side, catchment);

  // --- suitability ---------------------------------------------------------
  network.suitability.assign(count, 0.f);
  network.suitabilitySide = side;
  for (uint32_t y = 0; y < side; ++y)
    for (uint32_t x = 0; x < side; ++x)
    {
      const size_t id = size_t(y) * side + x;
      if (!buildable[id]) continue;
      const Point2 p = centre(x, y);

      // Fresh water, but only from a reach that is actually a river.
      const double riverDistance = sample(fields.riverDistanceMetres, p, 1e9);
      const double discharge = sample(fields.riverDischarge, p, 0.0);
      const double river = discharge >= settings.waterMinimumDischarge
          ? peak(riverDistance, settings.waterIdealDistanceMetres, settings.waterFalloffMetres) : 0.0;
      // Harbour is a separate peak, not a competitor: a coastal town and a
      // river town are both good, and a river mouth is better than either.
      const double harbour = decay(std::max(0.0, sample(fields.coastDistanceMetres, p, 1e9)),
          settings.harbourFalloffMetres);
      const double water = std::clamp(river + harbour * .85, 0.0, 1.35);
      if (water <= .02) continue;

      // Defensible: a band at the foot of high ground, using the local relief
      // above the surroundings rather than absolute height.
      double highest = terrain[id];
      const int look = std::max(1, int(std::lround(1800.0 / cell)));
      for (int oy = -look; oy <= look; oy += std::max(1, look / 2))
        for (int ox = -look; ox <= look; ox += std::max(1, look / 2))
        {
          const uint32_t sx = uint32_t(std::clamp(int(x) + ox, 0, int(side) - 1));
          const uint32_t sy = uint32_t(std::clamp(int(y) + oy, 0, int(side) - 1));
          highest = std::max<double>(highest, terrain[size_t(sy) * side + sx]);
        }
      const double relief = peak(highest - terrain[id], settings.reliefIdealMetres, settings.reliefFalloffMetres);

      // Multiplied, after Horizon Zero Dawn's composition of placement
      // densities: every criterion can veto, none can carry the site alone.
      const double score = water
          * (.35 + .65 * smoothstep(timber[id] * 1.4))
          * (.30 + .70 * smoothstep(arable[id] * 1.4))
          * (.55 + .45 * relief)
          * (.60 + .40 * (1.0 - smoothstep(fields.slope(p) / std::max(1e-6, settings.maximumSlope))));
      network.suitability[id] = float(std::clamp(score, 0.0, 1.0));
    }

  // --- tiered greedy scatter ----------------------------------------------
  // Largest tier first, each site clearing the exclusion radius of everything
  // already accepted. Same footprint rule as the asset placement in Horizon
  // Zero Dawn and as the landform placement already in this codebase.
  std::vector<uint32_t> order(count);
  for (uint32_t i = 0; i < count; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](const uint32_t a, const uint32_t b)
  {
    if (network.suitability[a] != network.suitability[b]) return network.suitability[a] > network.suitability[b];
    return a < b; // total order keeps the result reproducible
  });
  for (uint32_t tier = 0; tier < uint32_t(SettlementTier::Count); ++tier)
  {
    const auto &rules = settings.tiers[tier];
    uint32_t placed = 0;
    for (const uint32_t id : order)
    {
      if (rules.maximumCount && placed >= rules.maximumCount) break;
      const double score = network.suitability[id];
      if (score < rules.minimumSuitability) break; // sorted, so nothing later qualifies
      const Point2 p = centre(id % side, id / side);
      bool clear = true;
      for (const auto &site : network.sites)
      {
        // A candidate clears *its own* tier's radius against everything already
        // placed. Taking the larger of the two radii instead would let a city
        // sterilise its whole hinterland, when what a city should have is
        // satellites: villages may sit a village-radius from one, cities are
        // kept apart from each other by their own much larger radius.
        if (distance(p, site.position) < rules.exclusionRadiusMetres) { clear = false; break; }
      }
      if (!clear) continue;
      network.sites.push_back({p, SettlementTier(tier), score, uint32_t(network.sites.size())});
      ++placed;
    }
  }
  if (!settings.buildRoads || network.sites.size() < 2u) return network;

  // --- roads ---------------------------------------------------------------
  // Following Benes, Wilkie and Krivanek, "Procedural Modelling of Urban Road
  // Networks". Three things from that paper matter here and none of them were
  // in the previous version:
  //
  //  - the pathfinding lattice connects a vertex to the nearest vertex in each
  //    direction within a radius, not to its eight or sixteen touching
  //    neighbours. Eight neighbours can express eight headings, so every route
  //    was a staircase; a radius of four expresses forty.
  //  - an edge's weight is LENGTH * max(1, O1 * E_max) where E_max is the
  //    steepest slope found by marching along the edge. Sampling the endpoints
  //    instead, as before, lets a road hop a ridge for free because both ends
  //    are flat.
  //  - an edge whose slope exceeds a cutoff is discarded outright. A soft
  //    penalty means a road will always cross a mountain if the detour is long
  //    enough; a real one will not, and this is what makes routes go round.
  const uint32_t roadSide = std::max(settings.resolution, settings.roadResolution);
  const size_t roadCount = size_t(roadSide) * roadSide;
  const double roadCell = extentMetres / roadSide;
  const auto roadCentre = [&](const uint32_t x, const uint32_t y)
  { return Point2((x + .5) * roadCell, (y + .5) * roadCell); };
  std::vector<float> height(roadCount, 0.f);
  std::vector<uint8_t> wet(roadCount, 0u);
  std::vector<float> travel(roadCount, 1.f);
  for (uint32_t y = 0; y < roadSide; ++y)
    for (uint32_t x = 0; x < roadSide; ++x)
    {
      const size_t id = size_t(y) * roadSide + x;
      const Point2 p = roadCentre(x, y);
      height[id] = float(fields.elevationMetres ? fields.elevationMetres(p) : 0.0);
      wet[id] = fields.waterType && fields.waterType(p) != 0u ? 1u : 0u;
    }

  // The stencil: one offset per distinct heading within the radius, which is
  // every coprime pair. A non-coprime offset such as (2,2) is the same heading
  // as (1,1) and only duplicates work.
  struct Step { int dx, dy; double length; };
  std::vector<Step> stencil;
  {
    const int radius = int(std::max(1u, settings.roadNeighbourRadius));
    const auto divisor = [](int a, int b) { while (b) { const int t = a % b; a = b; b = t; } return a < 0 ? -a : a; };
    for (int dy = -radius; dy <= radius; ++dy)
      for (int dx = -radius; dx <= radius; ++dx)
      {
        if (!dx && !dy) continue;
        if (divisor(dx, dy) != 1) continue;
        stencil.push_back({dx, dy, std::hypot(double(dx), double(dy))});
      }
  }
  const auto nearestCell = [&](const Point2 p)
  {
    const auto x = uint32_t(std::clamp(p.x / roadCell, 0.0, double(roadSide - 1)));
    const auto y = uint32_t(std::clamp(p.y / roadCell, 0.0, double(roadSide - 1)));
    return size_t(y) * roadSide + x;
  };

  // Which pairs to route. Each site takes its nearest already-placed site of
  // its own tier or larger, which grows the hierarchy - villages hang off
  // towns, towns off cities. That alone is a spanning tree, so a second pass
  // adds the Gabriel edges: a pair gets a road when no third site sits inside
  // the circle having that pair as its diameter. That is the standard test for
  // "these two are neighbours with nobody between them", and it is what puts
  // the bypasses and the triangles into the network.
  std::vector<std::pair<uint32_t, uint32_t>> pairs;
  std::vector<double> nearestOfTier(network.sites.size(), 0.0);
  for (size_t index = 1; index < network.sites.size(); ++index)
  {
    const auto &site = network.sites[index];
    size_t target = SIZE_MAX; double nearest = 0;
    for (size_t other = 0; other < index; ++other)
    {
      if (network.sites[other].tier > site.tier) continue;
      const double d = distance(site.position, network.sites[other].position);
      if (target == SIZE_MAX || d < nearest) { target = other; nearest = d; }
    }
    if (target == SIZE_MAX) continue;
    nearestOfTier[index] = nearest;
    pairs.emplace_back(uint32_t(index), uint32_t(target));
  }
  for (size_t a = 0; a < network.sites.size(); ++a)
    for (size_t b = a + 1; b < network.sites.size(); ++b)
    {
      const double span = distance(network.sites[a].position, network.sites[b].position);
      const double limit = std::max(nearestOfTier[a], nearestOfTier[b]) * settings.roadLoopFactor;
      if (!(span > 0) || (limit > 0 && span > limit)) continue;
      const Point2 middle((network.sites[a].position.x + network.sites[b].position.x) * .5,
          (network.sites[a].position.y + network.sites[b].position.y) * .5);
      bool gabriel = true;
      for (size_t other = 0; other < network.sites.size() && gabriel; ++other)
        if (other != a && other != b) gabriel = distance(network.sites[other].position, middle) >= span * .5;
      if (!gabriel) continue;
      const auto already = std::find_if(pairs.begin(), pairs.end(), [&](const auto &pair)
      { return (pair.first == a && pair.second == b) || (pair.first == b && pair.second == a); });
      if (already == pairs.end()) pairs.emplace_back(uint32_t(a), uint32_t(b));
    }

  // Generation stamps rather than clearing a million floats per route, and the
  // search is confined to a box around the pair: almost every route is short
  // next to the world, and the wide stencil is only affordable this way.
  std::vector<uint32_t> route(roadCount, UINT32_MAX);
  std::vector<float> best(roadCount, 0.f);
  std::vector<uint32_t> stamp(roadCount, 0u);
  uint32_t generation = 0u;
  for (const auto &pair : pairs)
  {
    const size_t from = nearestCell(network.sites[pair.first].position);
    const size_t to = nearestCell(network.sites[pair.second].position);
    if (from == to) continue;
    ++generation;
    const int fx = int(from % roadSide), fy = int(from / roadSide);
    const int tx = int(to % roadSide), ty = int(to / roadSide);
    const int pad = int(std::abs(fx - tx) + std::abs(fy - ty)) / 2 + 24;
    const int loX = std::max(0, std::min(fx, tx) - pad), hiX = std::min(int(roadSide) - 1, std::max(fx, tx) + pad);
    const int loY = std::max(0, std::min(fy, ty) - pad), hiY = std::min(int(roadSide) - 1, std::max(fy, ty) + pad);

    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    best[from] = 0.f; stamp[from] = generation; route[from] = UINT32_MAX;
    queue.emplace(0.f, uint32_t(from));
    bool arrived = false;
    while (!queue.empty())
    {
      const auto [cost, id] = queue.top(); queue.pop();
      if (id == to) { arrived = true; break; }
      if (stamp[id] != generation || cost > best[id]) continue;
      const int x = int(id % roadSide), y = int(id / roadSide);
      for (const Step &move : stencil)
      {
        const int nx = x + move.dx, ny = y + move.dy;
        if (nx < loX || ny < loY || nx > hiX || ny > hiY) continue;
        const auto next = uint32_t(ny) * roadSide + uint32_t(nx);
        // March the edge: the steepest step along it is what the road has to
        // climb, and any water under it has to be bridged.
        const int steps = std::max(std::abs(move.dx), std::abs(move.dy));
        double steepest = 0; double crossing = 0;
        float previous = height[id];
        for (int k = 1; k <= steps; ++k)
        {
          const int mx = x + (move.dx * k) / steps, my = y + (move.dy * k) / steps;
          const auto at = uint32_t(my) * roadSide + uint32_t(mx);
          const double run = move.length * roadCell / steps;
          steepest = std::max(steepest, std::abs(double(height[at] - previous)) / std::max(1e-6, run));
          if (wet[at]) crossing += run;
          previous = height[at];
        }
        if (steepest > settings.roadMaximumSlope) continue;
        if (crossing > settings.roadMaximumSpanMetres) continue;
        const double metres = move.length * roadCell;
        // Curvature. Galin et al. make this a third dimension of the search -
        // position crossed with heading - because without it "the generated
        // path often have too many shape turns", and their own measurements
        // show a route 998 m long with the constraint against 1,464 m without.
        // The full state costs a heading bin per cell; taking the heading of
        // the best path found so far is the cheap form of the same idea, and it
        // is what stops a road zig-zagging between two lattice directions that
        // cost the same.
        double bend = 0.0;
        if (route[id] != UINT32_MAX)
        {
          const int px = int(route[id] % roadSide), py = int(route[id] / roadSide);
          const double inX = double(x - px), inY = double(y - py);
          const double inLength = std::hypot(inX, inY);
          if (inLength > 1e-6)
          {
            const double outLength = std::max(1e-6, double(move.length));
            const double turn = (inX * move.dx + inY * move.dy) / (inLength * outLength);
            // Zero going straight on, rising as the heading changes, and
            // impassable at a hairpin.
            bend = settings.roadTurnPenalty * (1.0 - std::clamp(turn, -1.0, 1.0));
          }
        }
        const double weight = metres * (std::max(1.0, settings.roadSlopePenalty * steepest) + bend) * travel[next] +
            crossing * settings.roadWaterCrossingPenalty;
        const float step = float(weight);
        if (stamp[next] == generation && best[id] + step >= best[next]) continue;
        best[next] = best[id] + step; route[next] = id; stamp[next] = generation;
        queue.emplace(best[next], next);
      }
    }
    if (!arrived) continue;
    SettlementRoad road;
    road.from = pair.first; road.to = pair.second;
    // The class is the lower of the two ranks it joins: a road is only as
    // important as the smaller place at its end.
    const auto rank = std::max(uint32_t(network.sites[pair.first].tier),
        uint32_t(network.sites[pair.second].tier));
    road.kind = rank == 0u ? RoadClass::Highway : rank == 1u ? RoadClass::Road : RoadClass::Lane;
    road.widthMetres = settings.roadWidthMetres[std::min<size_t>(rank, 2u)];
    for (uint32_t at = uint32_t(to); at != UINT32_MAX; at = route[at])
    {
      road.path.push_back(roadCentre(at % roadSide, at / roadSide));
      // Laying a road makes the corridor cheaper, so the next route prefers to
      // join it rather than run beside it. This is what makes the network read
      // as grown instead of as a star from every town.
      travel[at] = float(std::max(.15, travel[at] * settings.roadReuseDiscount));
      if (at == uint32_t(from)) break;
    }
    std::reverse(road.path.begin(), road.path.end());
    if (road.path.size() < 2u) continue;
    for (uint32_t round = 0; round < settings.roadSmoothing && road.path.size() > 2u; ++round)
    {
      std::vector<Point2> rounded{road.path.front()};
      for (size_t k = 0; k + 1 < road.path.size(); ++k)
      {
        const Point2 &a = road.path[k], &b = road.path[k + 1];
        rounded.push_back({a.x + (b.x - a.x) * .25, a.y + (b.y - a.y) * .25});
        rounded.push_back({a.x + (b.x - a.x) * .75, a.y + (b.y - a.y) * .75});
      }
      rounded.push_back(road.path.back());
      road.path.swap(rounded);
    }
    for (size_t k = 1; k < road.path.size(); ++k) road.lengthMetres += distance(road.path[k - 1], road.path[k]);
    network.roads.push_back(std::move(road));
  }

  // --- built form ----------------------------------------------------------
  // Watabou's construction, not a rosette. Scatter ward seeds, take the Voronoi
  // cells, select a contiguous set of them, and let the union boundary of the
  // selection be the outline and the wall. Everything then derives from the
  // partition: the streets are the shared edges, the districts are the cells
  // themselves. The previous plan drew outline, gates, streets and districts as
  // radius-by-angle, and a plan drawn by angle can only ever be a rosette -
  // which is precisely the arbitrariness Watabou describes moving away from.
  for (const auto &site : network.sites)
  {
    const auto tier = uint32_t(site.tier);
    SettlementPlan plan;
    plan.site = site.id;
    plan.radiusMetres = settings.builtRadiusMetres[tier];
    const uint64_t seed = mix(settings.seed ^ (uint64_t(site.id + 1u) * 0x9E3779B97F4A7C15ull));
    const double reach = plan.radiusMetres * 1.25;
    const double spacing = std::max(18.0, std::sqrt(settings.wardAreaSquareMetres));

    // Seeds on a jittered lattice over the disc. A lattice with jitter, rather
    // than uniform random, is what keeps the wards a consistent size - pure
    // Poisson leaves clumps, and a clump of Voronoi cells is a clump of slivers.
    // Seeds on a Fermat spiral, which is Watabou's: the angle advances by
    // sqrt(i) and the radius by a constant step, so the arc gap between
    // successive seeds grows as sqrt(r). Wards therefore come out small and
    // dense in the core and large towards the edge - which is what a town
    // actually looks like, and what a jittered lattice cannot produce however
    // it is weighted. The lattice version needed a power diagram to break its
    // uniformity; this needs nothing, because it was never uniform.
    std::vector<Point2> seeds;
    std::vector<double> grades;
    // The radius law is what sets how ward size varies with distance from the
    // centre. With r proportional to i - a constant radial step, which is what
    // Watabou uses at his much smaller counts - the innermost cell's area falls
    // off as 1/N, and at the several hundred wards a world-scale city needs
    // that is a knot of slivers at the middle with visible spiral arms. With r
    // proportional to i^a the area at rank i goes as i^(2a-1), so a = .58 over
    // 440 wards gives an inner ward about .46 of the mean and an outer one
    // about 1.16 - a two-and-a-half-fold spread, growing outwards, with the
    // mean still the requested ward area.
    const auto wanted = size_t(std::max(6.0, kPi * reach * reach / settings.wardAreaSquareMetres));
    const double turn = 2.0 * kPi * unitRandom(seed + 17u);
    // The golden angle, rather than a fixed multiple of sqrt(i): it is the one
    // divergence that leaves no arm visible at any count.
    constexpr double kGolden = 2.39996322972865332;
    for (size_t i = 0; i < wanted; ++i)
    {
      const double angle = turn + double(i) * kGolden;
      const double radius = i == 0 ? 0.0
          : reach * std::pow(double(i) / double(wanted), .58) * (.88 + .24 * unitRandom(seed + i * 3u + 21u));
      seeds.push_back({site.position.x + std::cos(angle) * radius, site.position.y + std::sin(angle) * radius});
      grades.push_back(.78 + .34 * unitRandom(seed + i + 77u));
    }
    if (seeds.size() < 4u) { if(getenv("PLAN_DEBUG")) fprintf(stderr,"skip seeds %zu\n", seeds.size()); continue; }
    // The weight of a power cell is a squared length, so a grade of 1.3 makes a
    // ward about a third wider than its neighbours. Bounded well below the
    // spacing, since a weight larger than that deletes the cell outright.
    std::vector<double> weights(seeds.size());
    for (size_t self = 0; self < seeds.size(); ++self)
    { const double r = (grades[self] - 1.0) * spacing * .42; weights[self] = r * std::abs(r); }

    // Voronoi by half-plane clipping. Clipping against every other seed is
    // O(n^2), so the loop stops once the remaining seeds are further away than
    // twice the current cell's reach: no seed beyond that can cut it.
    // The domain the cells are cut from is the disc, not a bounding box. A box
    // leaves every peripheral cell running out to the corner, so its centroid
    // sits far outside the town and Lloyd drags the seed out after it. On a
    // city with a large interior that is survivable; on a village, where almost
    // every cell is peripheral, it collapsed the whole place to a single ward -
    // which is what 157 of 329 settlements did. The rim is jittered so that
    // where a ward does reach it, the outline inherits an irregular edge rather
    // than a machined arc.
    std::vector<Point2> domain;
    {
      constexpr uint32_t kDomain = 26u;
      for (uint32_t step = 0; step < kDomain; ++step)
      {
        const double angle = step * 2.0 * kPi / kDomain;
        const double rim = reach * (.92 + .26 * unitRandom(seed + 2000u + step)) / std::cos(kPi / kDomain);
        domain.push_back({site.position.x + std::cos(angle) * rim, site.position.y + std::sin(angle) * rim});
      }
    }
    std::vector<std::vector<Point2>> cells;
    const auto buildCells = [&]()
    {
      cells.assign(seeds.size(), {});
      std::vector<std::pair<double, size_t>> order;
      for (size_t self = 0; self < seeds.size(); ++self)
      {
        order.clear(); order.reserve(seeds.size());
        for (size_t other = 0; other < seeds.size(); ++other)
          if (other != self) order.emplace_back(distance(seeds[self], seeds[other]), other);
        std::sort(order.begin(), order.end());
        std::vector<Point2> cell = domain;
        for (const auto &[gap, other] : order)
        {
          double furthest = 0;
          for (const Point2 &corner : cell) furthest = std::max(furthest, distance(corner, seeds[self]));
          if (gap > 2.0 * furthest + spacing) break;
          cell = clipToBisector(cell, seeds[self], seeds[other], weights[self], weights[other]);
          if (cell.size() < 3u) break;
        }
        cells[self] = std::move(cell);
      }
    };
    buildCells();
    // Lloyd relaxation. Two rounds takes the worst slivers out without driving
    // the pattern to a honeycomb, which would read as generated immediately.
    // Relax only the innermost handful. Watabou relaxes three central cells and
    // the citadel and nothing else; relaxing the whole set converges on a
    // hexagonal lattice, which is the honeycomb this has to avoid.
    const size_t relaxed = std::min<size_t>(seeds.size(), 4u);
    for (uint32_t round = 0; round < settings.wardRelaxation; ++round)
    {
      for (size_t self = 0; self < relaxed; ++self)
        if (cells[self].size() >= 3u)
        {
          // A light pull only. Full Lloyd relaxation converges on a hexagonal
          // lattice, which is exactly the honeycomb this has to avoid; the
          // point is to remove slivers, not to make the cells equal.
          const Point2 middle = centroidOf(cells[self]);
          seeds[self] = middle;
        }
      buildCells();
    }

    // Selection: which cells are built on at all. A radial falloff sets the
    // size, but ground decides the shape - a cell in the river or on a cliff is
    // never built, so the town grows along the dry gentle ground the way a real
    // one does, and the outline comes out of that rather than being drawn.
    // The roads that serve this place, for the ribbon suburb below.
    std::vector<const SettlementRoad *> serving;
    for (const auto &road : network.roads)
      if (road.from == site.id || road.to == site.id) serving.push_back(&road);
    const auto toRoad = [&](const Point2 at)
    {
      double nearestRoad = std::numeric_limits<double>::max();
      for (const SettlementRoad *road : serving)
        for (const Point2 &q : road->path) nearestRoad = std::min(nearestRoad, distance(at, q));
      return nearestRoad;
    };
    std::vector<uint8_t> chosen(seeds.size(), 0u);
    for (size_t self = 0; self < seeds.size(); ++self)
    {
      if (cells[self].size() < 3u) continue;
      const Point2 at = centroidOf(cells[self]);
      const double radial = distance(at, site.position) / plan.radiusMetres;
      // Growth past the core follows the roads out. An even annulus of suburb
      // is the one thing the reference photographs never show: what is outside
      // the gate is a ribbon along the highway, and the ground between two
      // highways stays fields right up to the wall.
      const double ribbon = .52 * decay(toRoad(at), plan.radiusMetres * .22);
      const double edge = .62 + .30 * unitRandom(seed + 4000u + self) + ribbon;
      if (radial > edge) continue;
      if (fields.waterType && fields.waterType(at) != 0u) continue;
      if (fields.slope && fields.slope(at) > settings.maximumSlope * 1.9) continue;
      chosen[self] = 1u;
    }
    // Adjacency, from the shared bisector edges. This is also the street graph.
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> shared;
    for (size_t self = 0; self < seeds.size(); ++self)
      for (size_t k = 0, n = cells[self].size(); n >= 3u && k < n; ++k)
      {
        auto &entry = shared.try_emplace(edgeKey(cells[self][k], cells[self][(k + 1u) % n]),
            UINT32_MAX, UINT32_MAX).first->second;
        (entry.first == UINT32_MAX ? entry.first : entry.second) = uint32_t(self);
      }
    std::vector<std::vector<uint32_t>> neighbours(seeds.size());
    for (const auto &[key, pair] : shared)
      if (pair.second != UINT32_MAX)
      { neighbours[pair.first].push_back(pair.second); neighbours[pair.second].push_back(pair.first); }

    // Keep only the component containing the centre, so a town is one place and
    // not a scatter of islands across the river.
    size_t core = SIZE_MAX; double nearest = 0;
    for (size_t self = 0; self < seeds.size(); ++self)
    {
      if (!chosen[self]) continue;
      const double gap = distance(centroidOf(cells[self]), site.position);
      if (core == SIZE_MAX || gap < nearest) { core = self; nearest = gap; }
    }
    if (core == SIZE_MAX) { if(getenv("PLAN_DEBUG")) fprintf(stderr,"skip core\n"); continue; }
    std::vector<uint8_t> reached(seeds.size(), 0u);
    std::vector<uint32_t> stack{uint32_t(core)};
    reached[core] = 1u;
    while (!stack.empty())
    {
      const uint32_t at = stack.back(); stack.pop_back();
      for (const uint32_t next : neighbours[at])
        if (chosen[next] && !reached[next]) { reached[next] = 1u; stack.push_back(next); }
    }
    chosen = reached;
    std::vector<uint32_t> wards;
    for (size_t self = 0; self < seeds.size(); ++self) if (chosen[self]) wards.push_back(uint32_t(self));
    if (wards.size() < 2u) { if(getenv("PLAN_DEBUG")) fprintf(stderr,"skip wards %zu\n", wards.size()); continue; }

    // The wall encloses the inner wards. Ranking by distance from the centre
    // keeps the enclosed set contiguous, and a fill pass takes in any ward the
    // ranking left as a notch - a wall does not detour around one house.
    std::vector<uint8_t> walled(seeds.size(), 0u);
    if (site.tier <= settings.walledBelow)
    {
      std::vector<std::pair<double, uint32_t>> byReach;
      for (const uint32_t ward : wards)
        byReach.emplace_back(distance(centroidOf(cells[ward]), site.position), ward);
      std::sort(byReach.begin(), byReach.end());
      const size_t enclosed = std::max<size_t>(1u, size_t(byReach.size() * settings.wallFraction));
      for (size_t k = 0; k < enclosed; ++k) walled[byReach[k].second] = 1u;
      for (const uint32_t ward : wards)
      {
        if (walled[ward]) continue;
        uint32_t inside = 0u, total = 0u;
        for (const uint32_t next : neighbours[ward]) if (chosen[next]) { ++total; inside += walled[next]; }
        if (total >= 3u && inside >= total - 1u) walled[ward] = 1u;
      }
    }

    // Outline and wall are union boundaries: an edge shared by two members is
    // interior, an edge with one member is on the rim.
    const auto boundaryOf = [&](const std::vector<uint8_t> &member)
    {
      std::vector<std::pair<Point2, Point2>> rim;
      for (size_t self = 0; self < seeds.size(); ++self)
      {
        if (!member[self] || cells[self].size() < 3u) continue;
        const bool clockwise = signedArea(cells[self]) < 0;
        for (size_t k = 0, n = cells[self].size(); k < n; ++k)
        {
          Point2 a = cells[self][k], b = cells[self][(k + 1u) % n];
          if (clockwise) std::swap(a, b);
          const auto found = shared.find(edgeKey(a, b));
          const bool interior = found != shared.end() && found->second.second != UINT32_MAX &&
              member[found->second.first] && member[found->second.second];
          if (!interior) rim.emplace_back(a, b);
        }
      }
      return chainBoundary(rim);
    };
    plan.outline = boundaryOf(chosen);
    if (plan.outline.size() < 3u) { if(getenv("PLAN_DEBUG")) fprintf(stderr,"skip outline wards=%zu\n", wards.size()); continue; }
    if (site.tier <= settings.walledBelow) plan.wall = boundaryOf(walled);

    // Streets are the shared edges between two built wards - the partition's
    // own skeleton, which is why they meet at junctions and enclose blocks
    // instead of converging on one point.
    std::unordered_set<uint64_t> emitted;
    for (const uint32_t ward : wards)
      for (size_t k = 0, n = cells[ward].size(); n >= 3u && k < n; ++k)
      {
        const Point2 &a = cells[ward][k], &b = cells[ward][(k + 1u) % n];
        const auto found = shared.find(edgeKey(a, b));
        if (found == shared.end() || found->second.second == UINT32_MAX) continue;
        if (!chosen[found->second.first] || !chosen[found->second.second]) continue;
        if (!emitted.insert(edgeKey(a, b)).second) continue;
        plan.streets.push_back({a, b});
      }

    // Gates, by Watabou's rule: a gate is a rim vertex with more than one
    // adjacent ward on the inside, because that is the only kind a street can
    // actually run inward from. A vertex with a single inner ward is a corner
    // of the wall, not a way through it. Candidates are then ranked by how
    // close a road that serves the place passes, and neighbouring candidates
    // are suppressed so no two gates end up adjacent.
    std::vector<Point2> &rim = plan.wall.empty() ? plan.outline : plan.wall;
    const std::vector<uint8_t> &enclosed = plan.wall.empty() ? chosen : walled;
    std::unordered_map<uint64_t, uint32_t> innerAt;
    for (size_t self = 0; self < seeds.size(); ++self)
      if (enclosed[self])
        for (const Point2 &q : cells[self]) ++innerAt[vertexKey(q)];
    std::vector<std::pair<double, size_t>> candidates;
    for (size_t k = 0; k < rim.size(); ++k)
    {
      const auto found = innerAt.find(vertexKey(rim[k]));
      if (found == innerAt.end() || found->second < 2u) continue;
      double closest = std::numeric_limits<double>::max();
      for (const SettlementRoad *road : serving)
        for (const Point2 &q : road->path)
          if (distance(q, site.position) > plan.radiusMetres * .8)
            closest = std::min(closest, distance(q, rim[k]));
      candidates.emplace_back(closest, k);
    }
    std::sort(candidates.begin(), candidates.end());
    std::vector<uint8_t> taken(rim.size(), 0u);
    const auto apart = std::max<size_t>(1u, rim.size() / std::max(3u, settings.minimumGates * 2u));
    for (const auto &[gap, k] : candidates)
    {
      bool crowded = false;
      for (size_t back = 0; back <= apart && !crowded; ++back)
        crowded = taken[(k + back) % rim.size()] || taken[(k + rim.size() - back) % rim.size()];
      if (crowded) continue;
      taken[k] = 1u;
      plan.gates.push_back(rim[k]);
    }
    for (uint32_t extra = uint32_t(plan.gates.size()); extra < settings.minimumGates && !rim.empty(); ++extra)
      plan.gates.push_back(rim[(extra * rim.size()) / settings.minimumGates]);

    // Dress the rim into masonry: smooth towards the chords, then collapse the
    // short runs and shallow turns into straight walls between corners. The
    // gates are protected, so a gate stays on the road that arrives at it.
    {
      std::unordered_set<uint64_t> protectedVertices;
      for (const Point2 &gate : plan.gates) protectedVertices.insert(vertexKey(gate));
      const double factor = std::min(1.0, 40.0 / double(std::max<size_t>(1u, wards.size())));
      rim = dressRing(rim, factor, settings.wallSmoothing, settings.wallRunMetres,
          settings.wallTurnDegrees, protectedVertices);
    }

    // Kinds. Placed against each other rather than rolled independently: the
    // castle takes the high ground on the rim, the market the middle, the craft
    // quarter the low wet ground, and the good addresses sit by the market and
    // away from the crafts.
    const auto elevation = [&](const uint32_t ward)
    { return fields.elevationMetres ? fields.elevationMetres(centroidOf(cells[ward])) : 0.0; };
    uint32_t market = UINT32_MAX, castle = UINT32_MAX, temple = UINT32_MAX;
    double bestMarket = 0, bestCastle = 0;
    for (const uint32_t ward : wards)
    {
      const bool inside = plan.wall.empty() || walled[ward];
      const double gap = distance(centroidOf(cells[ward]), site.position);
      if (inside && (market == UINT32_MAX || gap < bestMarket)) { market = ward; bestMarket = gap; }
      bool onRim = false;
      for (const uint32_t next : neighbours[ward]) onRim = onRim || !chosen[next] || (!plan.wall.empty() && !walled[next]);
      if (site.tier == SettlementTier::City && inside && onRim)
      {
        const double height = elevation(ward);
        if (castle == UINT32_MAX || height > bestCastle) { castle = ward; bestCastle = height; }
      }
    }
    if (market != UINT32_MAX && site.tier != SettlementTier::Village)
      for (const uint32_t next : neighbours[market])
        if (chosen[next] && next != castle && (plan.wall.empty() || walled[next])) { temple = next; break; }
    // The craft quarter is a quarter: it is grown outwards from the lowest
    // built ward through its neighbours, because the tanners and the mills are
    // all on the same water. Taking the lowest fifth by rank instead scattered
    // single workshops across the town, which is not how a trade clusters.
    std::unordered_set<uint32_t> crafts;
    {
      uint32_t lowest = UINT32_MAX; double least = 0;
      for (const uint32_t ward : wards)
      {
        if (ward == market || ward == castle || ward == temple) continue;
        const double height = elevation(ward);
        if (lowest == UINT32_MAX || height < least) { lowest = ward; least = height; }
      }
      const size_t quarter = std::max<size_t>(1u, wards.size() / 6u);
      if (lowest != UINT32_MAX)
      {
        crafts.insert(lowest);
        std::vector<uint32_t> front{lowest};
        while (!front.empty() && crafts.size() < quarter)
        {
          const uint32_t at = front.front();
          front.erase(front.begin());
          for (const uint32_t next : neighbours[at])
          {
            if (crafts.size() >= quarter) break;
            if (!chosen[next] || next == market || next == castle || next == temple) continue;
            if (crafts.insert(next).second) front.push_back(next);
          }
        }
      }
    }
    // The good addresses are by the market and away from the crafts, and there
    // are two of them. Handing the title to every neighbour of the market put a
    // ring of them round the centre, which is the rosette again in a new guise.
    std::unordered_set<uint32_t> gentry;
    if (market != UINT32_MAX && site.tier != SettlementTier::Village)
    {
      std::vector<std::pair<double, uint32_t>> candidates;
      for (const uint32_t next : neighbours[market])
        if (chosen[next] && next != castle && next != temple && !crafts.count(next))
          candidates.emplace_back(-elevation(next), next);
      std::sort(candidates.begin(), candidates.end());
      for (size_t k = 0; k < std::min<size_t>(2u, candidates.size()); ++k) gentry.insert(candidates[k].second);
    }

    for (const uint32_t ward : wards)
    {
      SettlementDistrict district;
      district.boundary = cells[ward];
      district.insideWall = !plan.wall.empty() && walled[ward];
      if (ward == castle) district.kind = DistrictKind::Castle;
      else if (ward == market) district.kind = DistrictKind::Market;
      else if (ward == temple) district.kind = DistrictKind::Temple;
      else if (!plan.wall.empty() && !walled[ward]) district.kind = DistrictKind::Farmstead;
      else if (crafts.count(ward)) district.kind = DistrictKind::Craft;
      else if (gentry.count(ward)) district.kind = DistrictKind::Gentry;
      else district.kind = DistrictKind::Common;
      plan.districts.push_back(std::move(district));
    }
    // Main streets: gate to the middle, along ward boundaries. Dijkstra over
    // the ward-corner graph, which is the same graph the streets already are -
    // so an artery is a real route through the blocks rather than a line drawn
    // from the gate to the centre through whatever was in the way.
    {
      std::unordered_map<uint64_t, Point2> corner;
      std::unordered_map<uint64_t, std::vector<uint64_t>> links;
      for (const uint32_t ward : wards)
        for (size_t k = 0, n = cells[ward].size(); n >= 3u && k < n; ++k)
        {
          const Point2 &a = cells[ward][k], &b = cells[ward][(k + 1u) % n];
          const uint64_t ka = vertexKey(a), kb = vertexKey(b);
          corner[ka] = a; corner[kb] = b;
          links[ka].push_back(kb); links[kb].push_back(ka);
        }
      uint64_t middle = 0u; double best = 0;
      const Point2 heart = market != UINT32_MAX ? centroidOf(cells[market]) : site.position;
      for (const auto &[key, at] : corner)
      { const double gap = distance(at, heart); if (!middle || gap < best) { middle = key; best = gap; } }
      for (const Point2 &gate : plan.gates)
      {
        const uint64_t start = vertexKey(gate);
        if (!corner.count(start) || !middle) continue;
        std::unordered_map<uint64_t, double> cost{{start, 0.0}};
        std::unordered_map<uint64_t, uint64_t> came;
        std::priority_queue<std::pair<double, uint64_t>, std::vector<std::pair<double, uint64_t>>,
            std::greater<>> queue;
        queue.emplace(0.0, start);
        while (!queue.empty())
        {
          const auto [spent, at] = queue.top(); queue.pop();
          if (at == middle) break;
          const auto found = cost.find(at);
          if (found == cost.end() || spent > found->second) continue;
          for (const uint64_t next : links[at])
          {
            const double step = spent + distance(corner[at], corner[next]);
            const auto seen = cost.find(next);
            if (seen != cost.end() && step >= seen->second) continue;
            cost[next] = step; came[next] = at;
            queue.emplace(step, next);
          }
        }
        if (!cost.count(middle)) continue;
        std::vector<Point2> artery;
        for (uint64_t at = middle;; at = came[at])
        { artery.push_back(corner[at]); if (at == start) break; if (!came.count(at)) { artery.clear(); break; } }
        if (artery.size() >= 2u) { std::reverse(artery.begin(), artery.end()); plan.arteries.push_back(std::move(artery)); }
      }
    }
    // Watabou's `Model.optimizeJunctions`, which is what makes a ward read as a
    // city block rather than as a fish scale.
    //
    // A Voronoi cell has a scatter of very short edges around each junction,
    // because three cells almost never meet at one point. He merges any pair of
    // ward vertices closer than 8 units - 16% of his own city radius, and about
    // a third of a typical ward edge - and does it across every ward that shares
    // the vertex, so the partition stays closed. What is left is a polygon with
    // fewer and much longer edges.
    //
    // It matters twice over: the wards themselves stop looking like scales, and
    // anything that follows ward corners - the wall, the arteries, and every
    // street - stops twitching once per short edge.
    {
      const double weld = std::sqrt(std::max(1.0, settings.wardAreaSquareMetres))
          * settings.wardWeldFraction;
      if (weld > 0)
      {
        // Collect every vertex of every polygon that belongs to this plan, so a
        // vertex shared by two wards moves once and identically.
        std::vector<std::vector<Point2> *> rings;
        rings.push_back(&plan.outline);
        if (!plan.wall.empty()) rings.push_back(&plan.wall);
        for (auto &district : plan.districts) rings.push_back(&district.boundary);
        for (auto &street : plan.streets) rings.push_back(&street);
        // Quantise to find coincident vertices, then union-find the pairs that
        // are within the weld distance.
        std::vector<Point2> points;
        std::vector<std::pair<size_t, size_t>> where;   // ring, index
        for (size_t r = 0; r < rings.size(); ++r)
          for (size_t i = 0; i < rings[r]->size(); ++i)
          { points.push_back((*rings[r])[i]); where.emplace_back(r, i); }
        if (!points.empty())
        {
          std::vector<uint32_t> parent(points.size());
          for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
          const std::function<uint32_t(uint32_t)> find = [&](uint32_t i)
          { while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; };
          // A grid so this stays linear in the number of vertices.
          const double bucket = std::max(weld, 1.0);
          std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
          const auto key = [&](const Point2 q)
          { return (uint64_t(uint32_t(int32_t(std::floor(q.x / bucket)))) << 32)
                 ^ uint64_t(uint32_t(int32_t(std::floor(q.y / bucket)))); };
          for (uint32_t i = 0; i < points.size(); ++i) grid[key(points[i])].push_back(i);
          for (uint32_t i = 0; i < points.size(); ++i)
          {
            const int64_t cx = int64_t(std::floor(points[i].x / bucket));
            const int64_t cy = int64_t(std::floor(points[i].y / bucket));
            for (int64_t dy = -1; dy <= 1; ++dy)
              for (int64_t dx = -1; dx <= 1; ++dx)
              {
                const auto found = grid.find((uint64_t(uint32_t(int32_t(cx + dx))) << 32)
                    ^ uint64_t(uint32_t(int32_t(cy + dy))));
                if (found == grid.end()) continue;
                for (const uint32_t j : found->second)
                  if (j > i && distance(points[i], points[j]) < weld)
                  { const uint32_t a = find(i), b = find(j); if (a != b) parent[a] = b; }
              }
          }
          // Each group moves to its own centroid.
          std::unordered_map<uint32_t, std::pair<Point2, uint32_t>> centres;
          for (uint32_t i = 0; i < points.size(); ++i)
          {
            auto &entry = centres[find(i)];
            entry.first.x += points[i].x; entry.first.y += points[i].y; ++entry.second;
          }
          for (auto &entry : centres)
            if (entry.second.second > 1u)
            { entry.second.first.x /= entry.second.second; entry.second.first.y /= entry.second.second; }
          for (uint32_t i = 0; i < points.size(); ++i)
          {
            const auto &at = centres[find(i)].first;
            (*rings[where[i].first])[where[i].second] = at;
          }
          // Welding leaves duplicate and near-duplicate vertices behind.
          for (auto *ring : rings)
          {
            std::vector<Point2> tidy;
            tidy.reserve(ring->size());
            for (size_t i = 0, n = ring->size(); i < n; ++i)
              if (tidy.empty() || distance(tidy.back(), (*ring)[i]) > 1e-6) tidy.push_back((*ring)[i]);
            while (tidy.size() > 1u && distance(tidy.front(), tidy.back()) <= 1e-6) tidy.pop_back();
            if (tidy.size() >= 3u || ring->size() < 3u) ring->swap(tidy);
          }
          // A ward that collapsed is no longer a ward.
          plan.districts.erase(std::remove_if(plan.districts.begin(), plan.districts.end(),
              [](const SettlementDistrict &d) { return d.boundary.size() < 3u; }), plan.districts.end());
        }
      }
    }
    network.plans.push_back(std::move(plan));
  }

  // A road ends at a gate - at every town it meets, not only the two it was
  // routed between. Routing is site to site, so a road between two towns drove
  // straight over any third town on the way and out the far side, which is what
  // put roads on top of the city. Each route is now split into the runs that
  // lie outside every built outline, and each run's end is planted on the
  // nearest gate of the town it ran into. What happens between those gates is
  // the town's own street network's business.
  {
    const auto inside = [](const std::vector<Point2> &outline, const Point2 at)
    {
      bool in = false;
      for (size_t i = 0, j = outline.size() - 1u; i < outline.size(); j = i++)
        if ((outline[i].y > at.y) != (outline[j].y > at.y) &&
            at.x < (outline[j].x - outline[i].x) * (at.y - outline[i].y) /
                (outline[j].y - outline[i].y) + outline[i].x)
          in = !in;
      return in;
    };
    // Which plan, if any, covers a point. Settlements are far apart, so a scan
    // bounded by the built radius is enough.
    const auto coveredBy = [&](const Point2 at) -> const SettlementPlan *
    {
      for (const auto &plan : network.plans)
      {
        if (plan.outline.size() < 3u) continue;
        const auto site = std::find_if(network.sites.begin(), network.sites.end(),
            [&](const SettlementSite &s) { return s.id == plan.site; });
        if (site == network.sites.end()) continue;
        if (distance(site->position, at) > plan.radiusMetres * 1.6) continue;
        if (inside(plan.outline, at)) return &plan;
      }
      return nullptr;
    };
    const auto nearestGate = [](const SettlementPlan &plan, const Point2 at)
    {
      Point2 gate = at; double closest = 0;
      for (size_t g = 0; g < plan.gates.size(); ++g)
      { const double d = distance(plan.gates[g], at);
        if (!g || d < closest) { closest = d; gate = plan.gates[g]; } }
      return gate;
    };
    std::vector<SettlementRoad> kept;
    kept.reserve(network.roads.size());
    // A run names the two places it actually joins. Splitting a route at every
    // town it meets while leaving both halves labelled with the route's
    // original endpoints makes each fragment claim a site it does not reach -
    // the far end of a half sits a town away from the site it names, and
    // anything that trusts the labels (the ward that counts the roads it
    // serves, the gate a run is planted on) is then working from a lie.
    const auto emit = [&](std::vector<Point2> path, uint32_t from, uint32_t to,
        const SettlementRoad &source)
    {
      if (path.size() < 2u) return;
      SettlementRoad piece;
      piece.from = from; piece.to = to; piece.path = std::move(path);
      piece.kind = source.kind; piece.widthMetres = source.widthMetres;
      for (size_t k = 1; k < piece.path.size(); ++k)
        piece.lengthMetres += distance(piece.path[k - 1], piece.path[k]);
      kept.push_back(std::move(piece));
    };
    for (const auto &road : network.roads)
    {
      std::vector<Point2> run;
      const SettlementPlan *entered = nullptr;
      // Which place this run leaves from: the route's own origin for the first
      // run, and thereafter the town the previous run ended in.
      uint32_t origin = road.from;
      for (const Point2 &at : road.path)
      {
        const SettlementPlan *here = coveredBy(at);
        if (!here) { run.push_back(at); entered = nullptr; continue; }
        // First point inside a town: close the run on that town's gate.
        if (!entered)
        {
          entered = here;
          if (!run.empty() && !here->gates.empty()) run.push_back(nearestGate(*here, run.back()));
          emit(std::move(run), origin, here->site, road);
          run.clear();
          origin = here->site;
        }
      }
      // The tail resumes from the gate it left by, and ends at the route's own
      // destination.
      if (run.size() >= 2u)
      {
        const SettlementPlan *left = coveredBy(run.front());
        if (!left)
        {
          // Find the town the run emerged from, if any, and start on its gate.
          for (const auto &plan : network.plans)
            if (plan.outline.size() >= 3u && !plan.gates.empty() &&
                distance(nearestGate(plan, run.front()), run.front()) < plan.radiusMetres * .55)
            { run.insert(run.begin(), nearestGate(plan, run.front())); origin = plan.site; break; }
        }
        emit(std::move(run), origin, road.to, road);
      }
    }
    network.roads.swap(kept);
  }

  return network;
}
}
