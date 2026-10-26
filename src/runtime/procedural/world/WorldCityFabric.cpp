#include "WorldCityFabric.hpp"
#include <algorithm>
#include <cmath>
#include <queue>

namespace procedural::world::rewrite {
namespace {
bool fail(std::string *error, const std::string &message)
{
  if (error)
  {
    *error = message;
  }

  return false;
}

double flat(const Position &a, const Position &b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

Position centroid(const std::vector<Position> &ring)
{
  Position c;

  for (const auto &p : ring)
  {
    c.x += p.x / double(ring.size());
    c.y += p.y / double(ring.size());
    c.z += p.z / double(ring.size());
  }

  return c;
}

struct Bounds
{
  double minX = 1e300, minZ = 1e300, maxX = -1e300, maxZ = -1e300;

  explicit Bounds(const std::vector<Position> &ring)
  {
    for (const auto &p : ring)
    {
      minX = std::min(minX, p.x);
      maxX = std::max(maxX, p.x);
      minZ = std::min(minZ, p.z);
      maxZ = std::max(maxZ, p.z);
    }
  }

  bool overlaps(const Bounds &o, double tolerance) const
  {
    return minX < o.maxX - tolerance && o.minX < maxX - tolerance && minZ < o.maxZ - tolerance && o.minZ < maxZ - tolerance;
  }

  bool contains(const Bounds &o, double tolerance) const
  {
    return o.minX >= minX - tolerance && o.maxX <= maxX + tolerance && o.minZ >= minZ - tolerance && o.maxZ <= maxZ + tolerance;
  }
};

// Every lot the base lays out is an axis-aligned rectangle, so a footprint is
// its bounding box drawn in by the inset; overlap is a box test.
std::vector<Position> insetFootprint(const std::vector<Position> &lot, double inset, double level)
{
  const Bounds b(lot);
  const double x0 = b.minX + inset, x1 = b.maxX - inset, z0 = b.minZ + inset, z1 = b.maxZ - inset;

  if (x1 <= x0 || z1 <= z0)
  {
    return {};
  }

  return {{x0, level, z0}, {x1, level, z0}, {x1, level, z1}, {x0, level, z1}};
}

// Nearest point of a segment, in plan.
Position projectOnto(const Position &p, const Position &a, const Position &b, double &distance)
{
  const double dx = b.x - a.x, dz = b.z - a.z, length2 = dx * dx + dz * dz;
  const double t = length2 > 1e-12 ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / length2, 0., 1.) : 0.;
  const Position q{a.x + dx * t, a.y + (b.y - a.y) * t, a.z + dz * t};

  distance = flat(p, q);

  return q;
}

uint32_t workUnitsFor(CityProjectKind kind)
{
  switch (kind)
  {
  case CityProjectKind::Home:
    return 12;
  case CityProjectKind::Shop:
  case CityProjectKind::Workshop:
    return 16;
  case CityProjectKind::Inn:
    return 24;
  case CityProjectKind::Farm:
    return 8;
  case CityProjectKind::Market:
    return 30;
  case CityProjectKind::Church:
    return 40;
  case CityProjectKind::Keep:
    return 80;
  case CityProjectKind::Road:
    return 20;
  case CityProjectKind::Stair:
    return 30;
  case CityProjectKind::Repair:
    return 4;
  case CityProjectKind::Extension:
    return 8;
  case CityProjectKind::Rebuild:
    return 14;
  case CityProjectKind::Manor:
    return 30;
  default:
    return 0;
  }
}

std::optional<CityBuildingUse> useOf(CityProjectKind kind)
{
  switch (kind)
  {
  case CityProjectKind::Home:
    return CityBuildingUse::Home;
  case CityProjectKind::Shop:
    return CityBuildingUse::Shop;
  case CityProjectKind::Inn:
    return CityBuildingUse::Inn;
  case CityProjectKind::Workshop:
    return CityBuildingUse::Workshop;
  case CityProjectKind::Farm:
    return CityBuildingUse::Farm;
  case CityProjectKind::Market:
    return CityBuildingUse::Market;
  case CityProjectKind::Church:
    return CityBuildingUse::Church;
  case CityProjectKind::Keep:
    return CityBuildingUse::Keep;
  case CityProjectKind::Manor:
    return CityBuildingUse::Manor;
  default:
    return std::nullopt;
  }
}

bool buildingWork(CityProjectKind kind)
{
  return kind == CityProjectKind::Repair || kind == CityProjectKind::Extension || kind == CityProjectKind::Rebuild;
}

// The walkable network of one site: open ways, finished stairs, and the
// points that hang off them.
struct Network
{
  struct Edge
  {
    uint32_t a, b;
    double length;
    uint64_t route;
  };

  std::vector<Position> nodes;
  std::vector<Edge> edges;
  std::vector<std::vector<uint32_t>> adjacency;
  std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> keys;
  const CityFabricSite &site;
  const CityFabricSettings &settings;
  std::optional<uint64_t> without;
  std::optional<uint32_t> exit, court;

  bool open(const CityRouteRecord &r, uint64_t id) const
  {
    return !r.closedWeek && without != id;
  }

  Network(const CityFabricSite &site, std::optional<uint64_t> withoutRoute) : site(site), settings(site.settings), without(withoutRoute)
  {
    // The ways meet at T-junctions and crossings that share no vertex, so
    // every segment is split wherever another way touches or crosses it.
    struct Segment
    {
      uint64_t route;
      Position a, b;
    };

    std::vector<Segment> segments;

    for (const auto &[id, r] : site.routes)
    {
      if (!open(r, id))
      {
        continue;
      }

      for (size_t i = 1; i < r.path.size(); ++i)
      {
        segments.push_back({id, r.path[i - 1], r.path[i]});
        node(r.path[i - 1]);
        node(r.path[i]);
      }
    }

    for (size_t i = 0; i < segments.size(); ++i)
    {
      for (size_t j = i + 1; j < segments.size(); ++j)
      {
        const auto &s = segments[i], &t = segments[j];

        if (std::abs(s.a.y - t.a.y) > .3)
        {
          continue;
        }

        for (const auto &p : {t.a, t.b})
        {
          double d = 0;
          const auto q = projectOnto(p, s.a, s.b, d);

          if (d <= .05)
          {
            node(q);
          }
        }

        for (const auto &p : {s.a, s.b})
        {
          double d = 0;
          const auto q = projectOnto(p, t.a, t.b, d);

          if (d <= .05)
          {
            node(q);
          }
        }

        const double d1x = s.b.x - s.a.x, d1z = s.b.z - s.a.z, d2x = t.b.x - t.a.x, d2z = t.b.z - t.a.z;
        const double denominator = d1x * d2z - d1z * d2x;

        if (std::abs(denominator) < 1e-9)
        {
          continue;
        }

        const double u = ((t.a.x - s.a.x) * d2z - (t.a.z - s.a.z) * d2x) / denominator;
        const double v = ((t.a.x - s.a.x) * d1z - (t.a.z - s.a.z) * d1x) / denominator;

        if (u > 0 && u < 1 && v > 0 && v < 1)
        {
          node({s.a.x + d1x * u, s.a.y, s.a.z + d1z * u});
        }
      }
    }

    for (const auto &seg : segments)
    {
      std::vector<std::pair<double, uint32_t>> along;

      for (uint32_t n = 0; n < nodes.size(); ++n)
      {
        if (std::abs(nodes[n].y - seg.a.y) > .3)
        {
          continue;
        }

        double d = 0;
        const auto q = projectOnto(nodes[n], seg.a, seg.b, d);

        if (d <= .05)
        {
          along.push_back({flat(seg.a, q), n});
        }
      }

      std::sort(along.begin(), along.end());

      for (size_t k = 1; k < along.size(); ++k)
      {
        link(along[k - 1].second, along[k].second, seg.route);
      }
    }

    for (const auto &[id, s] : site.structures)
    {
      if (s.state != CityStructureState::Completed || s.kind == CityAccessKind::Deck)
      {
        continue;
      }

      if (s.kind == CityAccessKind::Approach)
      {
        const auto foot = node(s.from);
        const auto head = attach(s.to, s.upperLevel, 8);

        exit = foot;

        if (head)
        {
          link(foot, *head, 0);
          court = head;
        }

        continue;
      }

      const auto lower = attach(s.from, s.lowerLevel, 8), upper = attach(s.to, s.upperLevel, 8);

      if (lower && upper)
      {
        link(*lower, *upper, 0);
      }
    }
  }

  uint32_t node(const Position &p)
  {
    const std::tuple<int64_t, int64_t, int64_t> key{int64_t(std::llround(p.x * 20)), int64_t(std::llround(p.y * 20)), int64_t(std::llround(p.z * 20))};
    const auto it = keys.find(key);

    if (it != keys.end())
    {
      return it->second;
    }

    nodes.push_back(p);
    adjacency.emplace_back();
    keys[key] = uint32_t(nodes.size() - 1);

    return uint32_t(nodes.size() - 1);
  }

  void link(uint32_t a, uint32_t b, uint64_t route)
  {
    if (a == b)
    {
      return;
    }

    edges.push_back({a, b, flat(nodes[a], nodes[b]), route});
    adjacency[a].push_back(uint32_t(edges.size() - 1));
    adjacency[b].push_back(uint32_t(edges.size() - 1));
  }

  // Hangs a point off the nearest open way at its level.
  std::optional<uint32_t> attach(const Position &p, double level, double radius)
  {
    double best = radius;
    std::optional<std::tuple<uint64_t, size_t, Position>> found;

    for (const auto &[id, r] : site.routes)
    {
      if (!open(r, id))
      {
        continue;
      }

      for (size_t i = 1; i < r.path.size(); ++i)
      {
        if (std::abs(r.path[i - 1].y - level) > settings.doorstepMetres + .3)
        {
          continue;
        }

        double d = 0;
        const auto q = projectOnto(p, r.path[i - 1], r.path[i], d);

        if (d <= best)
        {
          best = d;
          found = {id, i, q};
        }
      }
    }

    if (!found)
    {
      return std::nullopt;
    }

    const auto &[id, i, q] = *found;
    const auto &r = site.routes.at(id);
    const auto n = node(q);

    link(n, node(r.path[i - 1]), id);
    link(n, node(r.path[i]), id);

    return n;
  }

  // One search from `from`: the cost of every node and the edge it came by.
  struct Tree
  {
    std::vector<double> cost;
    std::vector<int64_t> via;

    bool reaches(uint32_t n) const
    {
      return n < cost.size() && cost[n] < 1e300;
    }
  };

  Tree tree(uint32_t from) const
  {
    Tree t;

    t.cost.assign(nodes.size(), 1e300);
    t.via.assign(nodes.size(), -1);

    std::priority_queue<std::pair<double, uint32_t>, std::vector<std::pair<double, uint32_t>>, std::greater<>> open;

    t.cost[from] = 0;
    open.push({0, from});

    while (!open.empty())
    {
      const auto [c, n] = open.top();

      open.pop();

      if (c > t.cost[n])
      {
        continue;
      }

      for (auto e : adjacency[n])
      {
        const auto &edge = edges[e];
        const auto other = edge.a == n ? edge.b : edge.a;

        if (c + edge.length < t.cost[other])
        {
          t.cost[other] = c + edge.length;
          t.via[other] = e;
          open.push({t.cost[other], other});
        }
      }
    }

    return t;
  }

  // The routes walked from `n` back to the tree's root.
  std::set<uint64_t> routesOf(const Tree &t, uint32_t n) const
  {
    std::set<uint64_t> routes;

    while (n < t.via.size() && t.via[n] >= 0)
    {
      const auto &edge = edges[size_t(t.via[n])];

      if (edge.route)
      {
        routes.insert(edge.route);
      }

      n = edge.a == n ? edge.b : edge.a;
    }

    return routes;
  }

  // Shortest path; the routes it walks along, or nothing when unreachable.
  std::optional<std::set<uint64_t>> path(uint32_t from, uint32_t to) const
  {
    std::vector<double> cost(nodes.size(), 1e300);
    std::vector<int64_t> via(nodes.size(), -1);
    std::priority_queue<std::pair<double, uint32_t>, std::vector<std::pair<double, uint32_t>>, std::greater<>> open;

    cost[from] = 0;
    open.push({0, from});

    while (!open.empty())
    {
      const auto [c, n] = open.top();

      open.pop();

      if (c > cost[n])
      {
        continue;
      }

      if (n == to)
      {
        break;
      }

      for (auto e : adjacency[n])
      {
        const auto &edge = edges[e];
        const auto other = edge.a == n ? edge.b : edge.a;

        if (c + edge.length < cost[other])
        {
          cost[other] = c + edge.length;
          via[other] = e;
          open.push({cost[other], other});
        }
      }
    }

    if (cost[to] >= 1e300)
    {
      return std::nullopt;
    }

    std::set<uint64_t> routes;

    for (uint32_t n = to; n != from;)
    {
      const auto &edge = edges[size_t(via[n])];

      if (edge.route)
      {
        routes.insert(edge.route);
      }

      n = edge.a == n ? edge.b : edge.a;
    }

    return routes;
  }
};

bool accessComplete(const CityFabricSite &site, uint64_t block)
{
  for (const auto &[id, s] : site.structures)
  {
    if (s.block == block && (s.kind == CityAccessKind::Approach || s.kind == CityAccessKind::Stair))
    {
      return s.state == CityStructureState::Completed;
    }
  }

  return false;
}
} // namespace

uint64_t cityGeometryHash(const CityBuildingVersion &v, CityBuildingUse use)
{
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](int64_t value) {
    for (int i = 0; i < 8; ++i)
    {
      h ^= uint64_t(value >> (i * 8)) & 0xff;
      h *= 1099511628211ull;
    }
  };

  for (const auto &p : v.footprint)
  {
    mix(std::llround(p.x * 1000));
    mix(std::llround(p.y * 1000));
    mix(std::llround(p.z * 1000));
  }

  mix(std::llround(v.base * 1000));
  mix(v.floors);
  mix(int64_t(use));

  return h;
}

const CityBuildingVersion *CityBuilding::active(uint32_t week) const
{
  for (const auto &v : versions)
  {
    if (v.fromWeek <= week && (!v.toWeek || week < *v.toWeek))
    {
      return &v;
    }
  }

  return nullptr;
}

const CityBuildingVersion *CityBuilding::standing() const
{
  for (const auto &v : versions)
  {
    if (!v.toWeek)
    {
      return &v;
    }
  }

  return nullptr;
}

CityFabricLedger::CityFabricLedger(CityFabricSettings settings) : defaults(std::move(settings))
{
}

bool CityFabricLedger::found(HistoryNode settlement, const CityFabricSettings &settings, uint32_t week, uint32_t foundingHouseholds, std::string *error)
{
  if (grounds.count(settlement))
  {
    return fail(error, "the settlement already has a site");
  }

  CityFabricSite site;

  site.settlement = settlement;
  site.foundedWeek = week;
  site.settings = settings;

  auto ground = std::make_shared<CityFabricGround>();

  if (!planCityBase(settings.base, ground->base, error) || !populateCityBase(ground->base, ground->completion, ground->relief, error))
  {
    return false;
  }

  if (ground->relief.platforms.empty())
  {
    return fail(error, "the base has no terrace");
  }

  site.ground = ground;

  const auto &first = site.ground->relief.platforms.front();

  site.exit = first.accessFrom;
  site.court = first.accessTo;

  for (const auto &r : site.ground->completion.routes)
  {
    CityRouteRecord record;

    record.id = r.id;
    record.path = r.path;
    record.halfWidth = r.halfWidth;
    // Six ways per terrace: the contour lane, the ring and the court link
    // are lanes; the short cross alleys start as worn footpaths.
    record.surface = (r.id % 6 == 1 || r.id % 6 == 2 || r.id % 6 == 3) ? CityRouteSurface::Lane : CityRouteSurface::Footpath;
    record.openedWeek = week;
    site.routes[r.id] = record;
  }

  for (size_t tier = 0; tier < site.ground->relief.platforms.size(); ++tier)
  {
    const auto &deck = site.ground->relief.platforms[tier];
    CityAccessStructure access;

    access.id = site.nextStructure++;
    access.block = deck.block;

    if (tier == 0)
    {
      access.kind = CityAccessKind::Approach;
      access.from = deck.accessFrom;
      access.to = deck.accessTo;
      access.state = CityStructureState::Completed;
      access.completedWeek = week;
    }
    else
    {
      access.kind = CityAccessKind::Stair;
      access.from = deck.accessPath.empty() ? deck.accessFrom : deck.accessPath.front();
      access.to = deck.accessPath.empty() ? deck.accessTo : deck.accessPath.back();
      access.state = CityStructureState::Planned;
    }

    access.lowerLevel = access.from.y;
    access.upperLevel = deck.level;

    for (const auto &stair : site.ground->relief.stairs)
    {
      if (flat(stair.b, access.to) < 3 || flat(stair.a, access.from) < 3)
      {
        double previous = stair.a.y;

        for (const auto &[end, top] : stair.bands)
        {
          access.treads += top > previous + 1e-6;
          access.landings += top <= previous + 1e-6;
          previous = top;
        }
      }
    }

    site.structures[access.id] = access;

    CityAccessStructure platform;

    platform.id = site.nextStructure++;
    platform.block = deck.block;
    platform.kind = CityAccessKind::Deck;
    platform.state = CityStructureState::Completed;
    platform.completedWeek = week;
    platform.lowerLevel = deck.foundation;
    platform.upperLevel = deck.level;
    platform.from = platform.to = centroid(deck.boundary);
    site.structures[platform.id] = platform;

    for (const auto &bridge : deck.bridges)
    {
      CityAccessStructure span;

      span.id = site.nextStructure++;
      span.block = deck.block;
      span.kind = CityAccessKind::Bridge;
      span.state = CityStructureState::Completed;
      span.completedWeek = week;
      span.from = bridge.a;
      span.to = bridge.b;
      span.lowerLevel = span.upperLevel = deck.level;

      const Position middle{(bridge.a.x + bridge.b.x) * .5, deck.level, (bridge.a.z + bridge.b.z) * .5};

      span.headroom = deck.level - bridge.thickness - (site.ground->relief.accessGround ? site.ground->relief.accessGround(middle.x, middle.z) : site.ground->relief.openGround(middle.x, middle.z));
      site.structures[span.id] = span;
    }
  }

  for (const auto &region : site.ground->completion.skeleton.regions)
  {
    if (region.kind != CityRegionKind::Lot)
    {
      continue;
    }

    CityParcel parcel;

    parcel.id = region.id;
    parcel.block = region.parent;
    parcel.boundary = region.boundary;
    parcel.area = region.area;
    parcel.level = site.ground->relief.regionLevel.count(region.id) ? site.ground->relief.regionLevel.at(region.id) : region.boundary.front().y;

    const auto centre = centroid(region.boundary);
    double best = settings.attachMetres;

    for (const auto &[id, r] : site.routes)
    {
      for (size_t i = 1; i < r.path.size(); ++i)
      {
        if (std::abs(r.path[i - 1].y - parcel.level) > settings.doorstepMetres)
        {
          continue;
        }

        double d = 0;
        const auto q = projectOnto(centre, r.path[i - 1], r.path[i], d);

        if (d < best)
        {
          best = d;
          parcel.frontage = q;
          parcel.frontageRoute = id;
        }
      }
    }

    site.parcels[parcel.id] = parcel;
  }

  uint32_t homes = 0;

  for (auto &[id, parcel] : site.parcels)
  {
    if (parcel.block != first.block || homes >= foundingHouseholds || !parcel.frontageRoute)
    {
      continue;
    }

    CityBuilding home;

    home.id = site.nextBuilding++;
    home.parcel = id;
    home.use = CityBuildingUse::Home;

    CityBuildingVersion v;

    v.fromWeek = week;
    v.footprint = insetFootprint(parcel.boundary, .9, parcel.level);
    v.base = parcel.level;
    v.floors = 1 + uint32_t(id % 2);
    v.change = "built";
    v.geometryHash = cityGeometryHash(v, home.use);
    home.versions.push_back(v);
    parcel.building = home.id;
    site.buildings[home.id] = home;
    ++homes;
  }

  log->push_back({week, CityFabricEventKind::Founded, settlement, homes, 0, std::to_string(site.parcels.size()) + " parcels on " + std::to_string(site.ground->relief.platforms.size()) + " terraces, " + std::to_string(homes) + " founding homes"});
  grounds.emplace(settlement, std::move(site));

  return true;
}

class CityFabricLedger::Week
{
public:
  Week(CityFabricLedger &ledger, CityFabricSite &site, const CityHistorySnapshot &snapshot, CityHouseholdLedger &homes, CityEconomyLedger &economy, CityRealmLedger &realm)
      : L(ledger), site(site), snapshot(snapshot), homes(homes), economy(economy), realm(realm), week(snapshot.week)
  {
  }

  void run()
  {
    sweep();
    tenancies();
    start();

    const uint32_t elapsed = week - site.foundedWeek;

    if (elapsed && elapsed % 4 == 0)
    {
      progress();
      propose();
    }

    traffic();
    publishHousing();
  }

private:
  CityFabricLedger &L;
  CityFabricSite &site;
  const CityHistorySnapshot &snapshot;
  CityHouseholdLedger &homes;
  CityEconomyLedger &economy;
  CityRealmLedger &realm;
  const uint32_t week;

  void add(CityFabricEventKind kind, uint64_t subject, HouseholdId household, std::string detail)
  {
    L.log->push_back({week, kind, site.settlement, subject, household, std::move(detail)});
  }

  uint32_t standingHomes() const
  {
    uint32_t n = 0;

    for (const auto &[id, b] : site.buildings)
    {
      n += b.use == CityBuildingUse::Home && b.standing() != nullptr;
    }

    return n;
  }

  void publishHousing()
  {
    homes.setHousingCapacity(site.settlement, standingHomes());
  }

  bool housedHere(HouseholdId id) const
  {
    const auto it = homes.households().find(id);

    return it != homes.households().end() && !it->second.dissolvedWeek && it->second.settlement == site.settlement && it->second.housing == CityHousingState::Housed;
  }

  // A project cancelled from outside while its works stood: release the
  // parcel, bring a relocated tenant home, keep whatever already stood.
  void sweep()
  {
    for (const auto &[id, c] : std::map<uint64_t, CityConstruction>(site.constructions))
    {
      const auto p = realm.projects().find(id);
      const bool live = p != realm.projects().end() && (p->second.state == CityProjectState::Building || p->second.state == CityProjectState::Suspended);

      if (live)
      {
        continue;
      }

      if (c.building && site.buildings.count(c.building))
      {
        auto &b = site.buildings.at(c.building);

        if (c.relocatedTo && site.buildings.count(*c.relocatedTo))
        {
          b.tenant = site.buildings.at(*c.relocatedTo).tenant;
          site.buildings.at(*c.relocatedTo).tenant.reset();
          add(CityFabricEventKind::Relocated, b.id, b.tenant.value_or(0), "back home; the works were cancelled");
        }

        if (b.versions.empty())
        {
          site.parcels.at(b.parcel).building.reset();
          site.buildings.erase(c.building);
        }
      }

      site.constructions.erase(id);
      add(CityFabricEventKind::ConstructionRejected, id, 0, "works abandoned; the reservation is released");
    }
  }

  void tenancies()
  {
    std::set<HouseholdId> seated;

    for (auto &[id, b] : site.buildings)
    {
      if (b.tenant && (!housedHere(*b.tenant) || b.use != CityBuildingUse::Home || !b.standing()))
      {
        add(CityFabricEventKind::TenancyReleased, id, *b.tenant, "");
        b.tenant.reset();
      }

      if (b.tenant)
      {
        seated.insert(*b.tenant);
      }
    }

    for (const auto &[hid, h] : homes.households())
    {
      if (!housedHere(hid) || seated.count(hid))
      {
        continue;
      }

      for (auto &[id, b] : site.buildings)
      {
        if (b.use == CityBuildingUse::Home && b.standing() && !b.tenant)
        {
          b.tenant = hid;
          seated.insert(hid);
          add(CityFabricEventKind::TenancyAssigned, id, hid, "");

          break;
        }
      }
    }
  }

  bool openProjectOn(const std::string &siteKey) const
  {
    for (const auto &[id, p] : realm.projects())
    {
      const bool live = p.state == CityProjectState::Proposed || p.state == CityProjectState::Funded || p.state == CityProjectState::Building || p.state == CityProjectState::Suspended;

      if (live && p.intent.settlement == site.settlement && p.intent.site == siteKey)
      {
        return true;
      }
    }

    return false;
  }

  // The free parcel a project takes, or why there is none.
  std::optional<uint64_t> parcelFor(CityProjectKind kind, std::string &why)
  {
    const bool civic = kind == CityProjectKind::Market || kind == CityProjectKind::Church;
    const bool crown = kind == CityProjectKind::Keep || kind == CityProjectKind::Manor;
    std::optional<uint64_t> chosen;
    double bestArea = 0;
    std::optional<uint64_t> lockedBlock;
    const uint64_t firstBlock = site.ground->relief.platforms.front().block;

    for (const auto &[id, parcel] : site.parcels)
    {
      if (parcel.building || !parcel.frontageRoute)
      {
        continue;
      }

      if (!accessComplete(site, parcel.block))
      {
        if (!lockedBlock || parcel.block < *lockedBlock)
        {
          lockedBlock = parcel.block;
        }

        continue;
      }

      if (civic && parcel.block != firstBlock)
      {
        continue;
      }

      if (crown)
      {
        // The highest terrace that can be reached.
        if (chosen && site.parcels.at(*chosen).block > parcel.block)
        {
          continue;
        }

        if (chosen && site.parcels.at(*chosen).block == parcel.block && parcel.area <= bestArea)
        {
          continue;
        }

        chosen = id;
        bestArea = parcel.area;

        continue;
      }

      if (civic ? parcel.area > bestArea : !chosen)
      {
        chosen = id;
        bestArea = parcel.area;
      }
    }

    if (chosen)
    {
      return chosen;
    }

    if (lockedBlock)
    {
      why = "waiting for the stair to terrace " + std::to_string(*lockedBlock);

      const std::string key = "stair-" + std::to_string(*lockedBlock);

      if (!openProjectOn(key))
      {
        CityProjectIntent intent;

        intent.kind = CityProjectKind::Stair;
        intent.settlement = site.settlement;
        intent.sponsor = CitySponsorKind::Treasury;
        intent.priority = 55;
        intent.site = key;
        intent.reason = "terrace " + std::to_string(*lockedBlock) + " has parcels but no stair";

        for (const auto &[realmId, c] : realm.realms())
        {
          if (c.seat == site.settlement && c.actor())
          {
            intent.sponsorPerson = c.actor();
          }
        }

        realm.propose(intent, week);
      }
    }
    else
    {
      why = "no free parcel";
    }

    return std::nullopt;
  }

  std::optional<uint64_t> buildingOf(const CityProject &p) const
  {
    if (p.intent.site && p.intent.site->rfind("building-", 0) == 0)
    {
      return uint64_t(std::stoull(p.intent.site->substr(9)));
    }

    return std::nullopt;
  }

  std::optional<uint64_t> vacantHome(std::optional<uint64_t> except) const
  {
    for (const auto &[id, b] : site.buildings)
    {
      if (b.use == CityBuildingUse::Home && b.standing() && !b.tenant && id != except)
      {
        return id;
      }
    }

    return std::nullopt;
  }

  void start()
  {
    for (const auto &[id, p] : std::map<uint64_t, CityProject>(realm.projects()))
    {
      if (p.state != CityProjectState::Funded || p.intent.settlement != site.settlement || site.constructions.count(id))
      {
        continue;
      }

      const auto kind = p.intent.kind;

      if (kind == CityProjectKind::Wall || kind == CityProjectKind::Palisade)
      {
        continue; // The defence ledger's.
      }

      CityConstruction c;

      c.project = id;
      c.kind = kind;
      c.workUnits = workUnitsFor(kind);
      c.startedWeek = week;

      std::string why;

      if (kind == CityProjectKind::Stair)
      {
        const uint64_t block = p.intent.site && p.intent.site->rfind("stair-", 0) == 0 ? std::stoull(p.intent.site->substr(6)) : 0;

        for (auto &[sid, s] : site.structures)
        {
          if (s.block == block && s.kind == CityAccessKind::Stair && s.state == CityStructureState::Planned)
          {
            c.structure = sid;
          }
        }

        if (!c.structure)
        {
          realm.rejectProject(id, week, "no planned stair for that terrace", economy);
          add(CityFabricEventKind::ConstructionRejected, id, 0, "no planned stair for that terrace");

          continue;
        }
      }
      else if (kind == CityProjectKind::Road)
      {
        std::optional<uint64_t> busiest;

        for (const auto &[rid, r] : site.routes)
        {
          if (!r.closedWeek && r.surface != CityRouteSurface::Street && (!busiest || r.traffic > site.routes.at(*busiest).traffic))
          {
            busiest = rid;
          }
        }

        if (!busiest)
        {
          realm.noteProject(id, "every way is already paved");

          continue;
        }

        c.route = busiest;
      }
      else if (buildingWork(kind))
      {
        const auto target = buildingOf(p);

        if (!target || !site.buildings.count(*target) || !site.buildings.at(*target).standing())
        {
          realm.rejectProject(id, week, "the building is gone", economy);
          add(CityFabricEventKind::ConstructionRejected, id, 0, "the building is gone");

          continue;
        }

        c.building = *target;

        if (kind == CityProjectKind::Rebuild)
        {
          auto &b = site.buildings.at(*target);

          if (b.tenant)
          {
            const auto refuge = vacantHome(*target);

            if (!refuge)
            {
              realm.noteProject(id, "no vacant home to relocate the household");
              add(CityFabricEventKind::Waiting, id, *b.tenant, "no vacant home to relocate the household");

              continue;
            }

            site.buildings.at(*refuge).tenant = b.tenant;
            c.relocatedTo = refuge;
            add(CityFabricEventKind::Relocated, *target, *b.tenant, "to building " + std::to_string(*refuge) + " for the rebuild");
            b.tenant.reset();
          }
        }
      }
      else
      {
        const auto use = useOf(kind);

        if (!use)
        {
          continue;
        }

        const auto parcel = parcelFor(kind, why);

        if (!parcel)
        {
          realm.noteProject(id, why);
          add(CityFabricEventKind::Waiting, id, p.intent.household, why);

          continue;
        }

        CityBuilding b;

        b.id = site.nextBuilding++;
        b.parcel = *parcel;
        b.use = *use;
        b.sponsorHousehold = p.intent.sponsor == CitySponsorKind::Household ? std::optional<HouseholdId>(p.intent.household) : std::nullopt;
        b.sponsorPerson = p.intent.sponsorPerson;
        site.parcels.at(*parcel).building = b.id;
        c.building = b.id;
        site.buildings[b.id] = b;
      }

      // Materials go in at the start; coin follows the work.
      CityStock materials = p.escrow;

      materials[CityGood::Coin] = 0;

      std::string error;

      if (!realm.startBuilding(id, week, &error) || !realm.drawEscrow(id, materials, &error))
      {
        add(CityFabricEventKind::ConstructionRejected, id, 0, error);

        continue;
      }

      site.constructions[id] = c;
      add(CityFabricEventKind::ConstructionStarted, id, p.intent.household, std::string(cityProjectKindName(kind)) + (c.building ? " on building " + std::to_string(c.building) : ""));
    }
  }

  void progress()
  {
    const auto crew = economy.workersAt(site.settlement, {CityOccupation::Labourer, CityOccupation::Carpenter, CityOccupation::Mason});
    uint32_t units = uint32_t(crew.size()) * site.settings.workUnitsPerBuilderPerMonth;
    std::vector<HouseholdId> paid;

    for (auto worker : crew)
    {
      if (const auto home = homes.householdOf(worker))
      {
        paid.push_back(*home);
      }
    }

    for (auto &[id, c] : std::map<uint64_t, CityConstruction>(site.constructions))
    {
      const auto project = realm.projects().find(id);

      if (project == realm.projects().end())
      {
        continue;
      }

      const auto &p = project->second;
      auto &record = site.constructions.at(id);

      // Works begun this very week wait for their first month.
      if ((p.state != CityProjectState::Building && p.state != CityProjectState::Suspended) || record.startedWeek == week)
      {
        continue;
      }

      const uint32_t work = std::min(units, record.workUnits - record.workDone);

      if (work == 0)
      {
        if (p.state == CityProjectState::Building && ++record.idleMonths >= site.settings.idleMonthsBeforeSuspension)
        {
          realm.suspendProject(id, week, "no builders");
          add(CityFabricEventKind::ConstructionSuspended, id, 0, "no builders");
        }

        continue;
      }

      if (p.state == CityProjectState::Suspended)
      {
        realm.resumeProject(id, week);
        add(CityFabricEventKind::ConstructionResumed, id, 0, "");
      }

      record.idleMonths = 0;
      units -= work;
      record.workDone += work;

      // Coin for the work done, split across the crew's households.
      const int64_t coin = p.cost[CityGood::Coin] * int64_t(work) / int64_t(std::max(1u, record.workUnits));
      CityStock draw;

      draw[CityGood::Coin] = std::min(coin, p.escrow[CityGood::Coin]);

      if (draw[CityGood::Coin] > 0 && !paid.empty() && realm.drawEscrow(id, draw))
      {
        const int64_t each = draw[CityGood::Coin] / int64_t(paid.size());
        int64_t remainder = draw[CityGood::Coin] - each * int64_t(paid.size());

        for (auto household : paid)
        {
          economy.payFromEscrow(household, each + (remainder > 0 ? 1 : 0), week);
          remainder = std::max<int64_t>(0, remainder - 1);

          if (record.building)
          {
            site.buildings.at(record.building).builders.insert(household);
          }
        }
      }

      if (record.workDone >= record.workUnits)
      {
        complete(id, record, p);
      }
    }
  }

  std::string spatialCheck(const CityBuilding &b, const CityBuildingVersion &v) const
  {
    const auto &parcel = site.parcels.at(b.parcel);

    if (v.footprint.size() < 3)
    {
      return "the parcel is too small for a footprint";
    }

    const Bounds mine(v.footprint);

    if (!Bounds(parcel.boundary).contains(mine, 1e-6))
    {
      return "the footprint leaves its parcel";
    }

    for (const auto &[id, other] : site.buildings)
    {
      const auto *s = other.standing();

      if (id != b.id && s && Bounds(s->footprint).overlaps(mine, 1e-6))
      {
        return "the footprint overlaps building " + std::to_string(id);
      }
    }

    if (!parcel.frontageRoute)
    {
      return "no way fronts the parcel";
    }

    if (std::abs(parcel.frontage.y - parcel.level) > site.settings.doorstepMetres + 1e-6)
    {
      return "the entrance would stand " + std::to_string(std::abs(parcel.frontage.y - parcel.level)) + " m off its way";
    }

    if (!accessComplete(site, parcel.block))
    {
      return "the terrace has no finished stair";
    }

    return "";
  }

  void complete(uint64_t id, CityConstruction &record, const CityProject &p)
  {
    if (record.structure)
    {
      auto &s = site.structures.at(*record.structure);

      s.state = CityStructureState::Completed;
      s.completedWeek = week;
      s.project = id;
      add(CityFabricEventKind::AccessCompleted, s.id, 0, "stair to terrace " + std::to_string(s.block));
    }
    else if (record.route)
    {
      auto &r = site.routes.at(*record.route);

      r.surface = CityRouteSurface::Street;
      r.project = id;
      add(CityFabricEventKind::RouteUpgraded, r.id, 0, "paved as a street");
    }
    else if (record.building)
    {
      auto &b = site.buildings.at(record.building);
      const auto &parcel = site.parcels.at(b.parcel);
      const auto *old = b.standing();
      CityBuildingVersion v;

      v.version = uint32_t(b.versions.size()) + 1;
      v.fromWeek = week;
      v.project = id;
      v.footprint = old ? old->footprint : insetFootprint(parcel.boundary, .9, parcel.level);
      v.base = parcel.level;
      v.floors = old ? old->floors : (b.use == CityBuildingUse::Keep ? 3 : b.use == CityBuildingUse::Home ? 1 + uint32_t(b.parcel % 2) : 2);

      switch (record.kind)
      {
      case CityProjectKind::Repair:
        v.change = "repaired";
        break;
      case CityProjectKind::Extension:
        v.change = "added-floor";
        ++v.floors;
        break;
      case CityProjectKind::Rebuild:
        v.change = "rebuilt";
        v.footprint = insetFootprint(parcel.boundary, .9, parcel.level);
        v.floors = std::max(2u, v.floors);
        break;
      default:
        v.change = "built";
        break;
      }

      v.geometryHash = cityGeometryHash(v, b.use);

      // The check runs against the world as it stands, the old version excluded.
      const auto why = spatialCheck(b, v);

      if (!why.empty())
      {
        realm.rejectProject(id, week, why, economy);
        add(CityFabricEventKind::ConstructionRejected, id, p.intent.household, why);

        if (record.relocatedTo)
        {
          b.tenant = site.buildings.at(*record.relocatedTo).tenant;
          site.buildings.at(*record.relocatedTo).tenant.reset();
          add(CityFabricEventKind::Relocated, b.id, b.tenant.value_or(0), "back home; the rebuild was rejected");
        }

        if (!old)
        {
          site.parcels.at(b.parcel).building.reset();
          site.buildings.erase(b.id);
        }

        site.constructions.erase(id);

        return;
      }

      if (old)
      {
        for (auto &version : b.versions)
        {
          if (!version.toWeek)
          {
            version.toWeek = week;
          }
        }
      }

      b.versions.push_back(v);
      add(CityFabricEventKind::VersionAdded, b.id, p.intent.household, std::string(v.change) + " v" + std::to_string(v.version));

      if (record.relocatedTo)
      {
        b.tenant = site.buildings.at(*record.relocatedTo).tenant;
        site.buildings.at(*record.relocatedTo).tenant.reset();
        add(CityFabricEventKind::Relocated, b.id, b.tenant.value_or(0), "back home after the rebuild");
      }

      if (!old)
      {
        publish(b, p);
      }
    }

    realm.completeProject(id, week, economy);
    site.constructions.erase(id);
    add(CityFabricEventKind::ConstructionCompleted, id, p.intent.household, cityProjectKindName(record.kind));
  }

  // A new building's capacity into the ledgers that count it.
  void publish(CityBuilding &b, const CityProject &p)
  {
    CityWorkplaceSpec spec;

    spec.settlement = site.settlement;
    spec.owner = p.intent.sponsor == CitySponsorKind::Household ? p.intent.sponsorPerson : std::nullopt;

    switch (b.use)
    {
    case CityBuildingUse::Home:
      publishHousing();
      add(CityFabricEventKind::CapacityPublished, b.id, p.intent.household, "one more home");

      return;
    case CityBuildingUse::Shop:
      spec.occupation = CityOccupation::Merchant;
      spec.slots = 2;
      break;
    case CityBuildingUse::Inn:
      spec.occupation = CityOccupation::Innkeeper;
      spec.slots = 2;
      break;
    case CityBuildingUse::Workshop:
    {
      spec.occupation = CityOccupation::Carpenter;
      spec.slots = p.intent.sponsor == CitySponsorKind::Treasury ? 4 : 2;

      if (p.intent.sponsor == CitySponsorKind::Treasury)
      {
        // Crown works employ the least represented building trade; the ruler's
        // personal occupation must not turn a workshop into an administrative job.
        uint32_t least = UINT32_MAX;
        for (const auto trade : {CityOccupation::Carpenter, CityOccupation::Mason, CityOccupation::Smith}) {
          uint32_t slots = 0;
          for (const auto &[id, workplace] : economy.workplaces())
            if (!workplace.retiredWeek && workplace.settlement == site.settlement && workplace.occupation == trade)
              slots += workplace.slots;
          if (slots < least) {least = slots;spec.occupation = trade;}
        }
      }
      else if (p.intent.sponsorPerson)
      {
        const auto r = economy.livelihoods().find(*p.intent.sponsorPerson);

        if (r != economy.livelihoods().end())
        {
          if (r->second.ambition)
          {
            spec.occupation = *r->second.ambition;
          }
          else if (r->second.trade)
          {
            spec.occupation = *r->second.trade;
          }
          else if (r->second.occupation)
          {
            spec.occupation = *r->second.occupation;
          }
        }
      }

      break;
    }
    case CityBuildingUse::Farm:
      spec.occupation = CityOccupation::Farmer;
      spec.slots = p.intent.sponsor == CitySponsorKind::Treasury ? 6 : 3;
      break;
    case CityBuildingUse::Market:
      spec.occupation = CityOccupation::Merchant;
      spec.slots = 3;
      spec.owner.reset();
      break;
    case CityBuildingUse::Church:
      spec.occupation = CityOccupation::Cleric;
      spec.slots = 1;
      spec.owner.reset();
      break;
    case CityBuildingUse::Keep:
    case CityBuildingUse::Manor:
      spec.occupation = CityOccupation::Guard;
      spec.slots = b.use == CityBuildingUse::Keep ? 4 : 2;
      spec.owner.reset();
      break;
    }

    b.workplace = economy.openWorkplace(spec, week);
    b.occupation = spec.occupation;
    add(CityFabricEventKind::CapacityPublished, b.id, p.intent.household, std::string(cityOccupationName(spec.occupation)) + " x" + std::to_string(spec.slots));
  }

  void propose()
  {
    for (const auto &[id, b] : site.buildings)
    {
      const auto *v = b.standing();

      if (!v || b.use != CityBuildingUse::Home || !b.tenant)
      {
        continue;
      }

      const std::string key = "building-" + std::to_string(id);

      if (openProjectOn(key))
      {
        continue;
      }

      const auto household = homes.households().find(*b.tenant);

      if (household == homes.households().end())
      {
        continue;
      }

      CityProjectIntent intent;

      intent.settlement = site.settlement;
      intent.sponsor = CitySponsorKind::Household;
      intent.household = *b.tenant;
      intent.sponsorPerson = household->second.head;
      intent.site = key;

      const uint32_t sinceVersion = week - v->fromWeek, sinceBuilt = week - b.versions.front().fromWeek;

      if (household->second.members.size() > size_t(v->floors) * site.settings.membersPerFloor)
      {
        intent.kind = CityProjectKind::Extension;
        intent.priority = 35;
        intent.reason = std::to_string(household->second.members.size()) + " people on " + std::to_string(v->floors) + " floor(s)";
      }
      else if (sinceBuilt >= site.settings.rebuildAfterYears * 52 && b.versions.size() >= 3)
      {
        intent.kind = CityProjectKind::Rebuild;
        intent.priority = 15;
        intent.reason = "an old house past repairing";
      }
      else if (sinceVersion >= site.settings.repairAfterYears * 52)
      {
        intent.kind = CityProjectKind::Repair;
        intent.priority = 20;
        intent.reason = std::to_string(sinceVersion / 52) + " years without repair";
      }
      else
      {
        continue;
      }

      realm.propose(intent, week);
    }
  }

  std::optional<uint32_t> target(Network &net, const CityLivelihoodRecord &r) const
  {
    if (r.workplace)
    {
      for (const auto &[id, b] : site.buildings)
      {
        if (b.workplace == r.workplace && b.standing())
        {
          const auto &parcel = site.parcels.at(b.parcel);

          return net.attach(parcel.frontage, parcel.level, site.settings.attachMetres);
        }
      }
    }

    return net.court;
  }

  void traffic()
  {
    // Monthly: the ways wear slowly, and a search per destination is enough.
    const uint32_t elapsed = week - site.foundedWeek;

    if (elapsed % 4 != 0)
    {
      return;
    }

    for (auto &[id, r] : site.routes)
    {
      r.traffic = 0;
    }

    Network net(site, {});
    std::vector<std::pair<uint32_t, uint32_t>> commutes; // Home node, destination node.
    std::map<uint64_t, std::optional<uint32_t>> destinations;

    for (const auto &[id, b] : site.buildings)
    {
      if (!b.tenant || !b.standing())
      {
        continue;
      }

      const auto &parcel = site.parcels.at(b.parcel);
      const auto from = net.attach(parcel.frontage, parcel.level, site.settings.attachMetres);
      const auto household = homes.households().find(*b.tenant);

      if (!from || household == homes.households().end())
      {
        continue;
      }

      for (auto member : household->second.members)
      {
        const auto r = economy.livelihoods().find(member);

        if (r == economy.livelihoods().end() || !r->second.workplace)
        {
          continue;
        }

        auto dest = destinations.find(*r->second.workplace);

        if (dest == destinations.end())
        {
          dest = destinations.emplace(*r->second.workplace, target(net, r->second)).first;
        }

        if (dest->second)
        {
          commutes.push_back({*from, *dest->second});
        }
      }
    }

    std::map<uint32_t, Network::Tree> trees;

    for (const auto &[from, to] : commutes)
    {
      auto tree = trees.find(to);

      if (tree == trees.end())
      {
        tree = trees.emplace(to, net.tree(to)).first;
      }

      for (auto route : net.routesOf(tree->second, from))
      {
        ++site.routes.at(route).traffic;
      }
    }

    for (auto &[id, r] : site.routes)
    {
      if (r.closedWeek || r.surface != CityRouteSurface::Footpath)
      {
        continue;
      }

      r.busyWeeks = r.traffic >= site.settings.footpathTraffic ? r.busyWeeks + 1 : 0;

      if (r.busyWeeks >= site.settings.footpathWeeks)
      {
        r.surface = CityRouteSurface::Lane;
        add(CityFabricEventKind::RouteUpgraded, id, 0, "worn into a lane by " + std::to_string(r.traffic) + " commuters a week");
      }
    }
  }
};

bool CityFabricLedger::observe(const CityHistorySnapshot &snapshot, CityHouseholdLedger &homes, CityEconomyLedger &economy, CityRealmLedger &realm, std::string *error)
{
  if (lastWeek && uint64_t(snapshot.week) != uint64_t(*lastWeek) + 1)
  {
    return fail(error, "fabric observations must be consecutive weeks");
  }

  if (homes.week() != std::optional<uint32_t>(snapshot.week) || economy.week() != homes.week() || realm.week() != homes.week())
  {
    return fail(error, "the fabric follows the household, economy and realm ledgers of the same week");
  }

  const size_t mark = log->size(), homesMark = homes.eventMark(), economyMark = economy.eventMark(), realmMark = realm.eventMark();
  CityFabricLedger next = *this;
  CityHouseholdLedger householdsCopy = homes;
  CityEconomyLedger economyCopy = economy;
  CityRealmLedger realmCopy = realm;

  next.lastWeek = snapshot.week;

  for (auto &[settlement, site] : next.grounds)
  {
    Week week(next, site, snapshot, householdsCopy, economyCopy, realmCopy);

    week.run();
  }

  if (!next.validate(snapshot, householdsCopy, economyCopy, error) || !realmCopy.validate(snapshot, error) || !economyCopy.validate(snapshot, householdsCopy, error))
  {
    log->resize(mark);
    homes.truncateEvents(homesMark);
    economy.truncateEvents(economyMark);
    realm.truncateEvents(realmMark);

    return false;
  }

  *this = std::move(next);
  homes = std::move(householdsCopy);
  economy = std::move(economyCopy);
  realm = std::move(realmCopy);

  if (error)
  {
    error->clear();
  }

  return true;
}

bool CityFabricLedger::reachable(HistoryNode settlement, const CityEconomyLedger &economy, const CityHouseholdLedger &homes, std::string *why, std::optional<uint64_t> withoutRoute) const
{
  const auto it = grounds.find(settlement);

  if (it == grounds.end())
  {
    return fail(why, "no site");
  }

  const auto &site = it->second;
  Network net(site, withoutRoute);

  if (!net.exit || !net.court)
  {
    return fail(why, "the approach does not meet the court");
  }

  // Hang every home and every workplace off the network first, then search
  // once from the court, once from the exit and once per workplace.
  std::map<uint64_t, uint32_t> homeNodes, workNodes;

  for (const auto &[id, b] : site.buildings)
  {
    if (!b.standing())
    {
      continue;
    }

    const auto &parcel = site.parcels.at(b.parcel);

    if (b.tenant)
    {
      const auto from = net.attach(parcel.frontage, parcel.level, site.settings.attachMetres);

      if (!from)
      {
        return fail(why, "home " + std::to_string(id) + " fronts no open way");
      }

      homeNodes[id] = *from;
    }

    if (b.workplace)
    {
      if (const auto to = net.attach(parcel.frontage, parcel.level, site.settings.attachMetres))
      {
        workNodes[*b.workplace] = *to;
      }
    }
  }

  const auto fromCourt = net.tree(*net.court), fromExit = net.tree(*net.exit);
  std::map<uint64_t, Network::Tree> fromWork;

  for (const auto &[id, b] : site.buildings)
  {
    if (!b.tenant || !b.standing())
    {
      continue;
    }

    const auto home = homeNodes.at(id);

    if (!fromCourt.reaches(home) || !fromExit.reaches(home))
    {
      return fail(why, "home " + std::to_string(id) + " cannot reach the court or the exit");
    }

    const auto household = homes.households().find(*b.tenant);

    if (household == homes.households().end())
    {
      continue;
    }

    for (auto member : household->second.members)
    {
      const auto r = economy.livelihoods().find(member);

      if (r == economy.livelihoods().end() || !r->second.workplace)
      {
        continue;
      }

      for (const auto &[wid, w] : site.buildings)
      {
        if (w.workplace != r->second.workplace || !w.standing())
        {
          continue;
        }

        const auto node = workNodes.find(*w.workplace);

        if (node == workNodes.end())
        {
          return fail(why, "home " + std::to_string(id) + " cannot reach workplace building " + std::to_string(wid));
        }

        auto tree = fromWork.find(*w.workplace);

        if (tree == fromWork.end())
        {
          tree = fromWork.emplace(*w.workplace, net.tree(node->second)).first;
        }

        if (!tree->second.reaches(home))
        {
          return fail(why, "home " + std::to_string(id) + " cannot reach workplace building " + std::to_string(wid));
        }
      }
    }
  }

  if (why)
  {
    why->clear();
  }

  return true;
}

bool CityFabricLedger::closeRoute(HistoryNode settlement, uint64_t route, uint32_t week, const CityEconomyLedger &economy, const CityHouseholdLedger &homes, std::string *error)
{
  const auto it = grounds.find(settlement);

  if (it == grounds.end() || !it->second.routes.count(route) || it->second.routes.at(route).closedWeek)
  {
    return fail(error, "no such open way");
  }

  std::string why;

  if (!reachable(settlement, economy, homes, &why, route))
  {
    log->push_back({week, CityFabricEventKind::RouteClosureRefused, settlement, route, 0, why});

    return fail(error, "closing the way would sever access: " + why);
  }

  it->second.routes.at(route).closedWeek = week;
  log->push_back({week, CityFabricEventKind::RouteClosed, settlement, route, 0, ""});

  return true;
}

std::vector<std::pair<uint64_t, const CityBuildingVersion *>> CityFabricLedger::standingAt(HistoryNode settlement, uint32_t week) const
{
  std::vector<std::pair<uint64_t, const CityBuildingVersion *>> result;
  const auto it = grounds.find(settlement);

  if (it == grounds.end())
  {
    return result;
  }

  for (const auto &[id, b] : it->second.buildings)
  {
    if (const auto *v = b.active(week))
    {
      result.push_back({id, v});
    }
  }

  return result;
}

std::vector<CityFabricLedger::CityVersionChange> CityFabricLedger::changesBetween(HistoryNode settlement, uint32_t from, uint32_t to) const
{
  std::vector<CityVersionChange> changes;
  const auto it = grounds.find(settlement);

  if (it == grounds.end())
  {
    return changes;
  }

  for (const auto &[id, b] : it->second.buildings)
  {
    for (const auto &v : b.versions)
    {
      const bool began = v.fromWeek > from && v.fromWeek <= to;
      const bool ended = v.toWeek && *v.toWeek > from && *v.toWeek <= to;

      if (began || ended)
      {
        changes.push_back({id, &v, began, ended});
      }
    }
  }

  return changes;
}

CityCompletionPlan CityFabricLedger::completionAt(HistoryNode settlement, uint32_t week) const
{
  const auto it = grounds.find(settlement);

  if (it == grounds.end())
  {
    return {};
  }

  const auto &site = it->second;
  CityCompletionPlan plan = site.ground->completion;
  std::map<uint64_t, const CityBuilding *> byParcel;

  for (const auto &[id, b] : site.buildings)
  {
    if (b.active(week))
    {
      byParcel[b.parcel] = &b;
    }
  }

  for (auto &lot : plan.lots)
  {
    const auto b = byParcel.find(lot.lot);

    lot.developed = b != byParcel.end();

    if (lot.developed)
    {
      const auto *v = b->second->active(week);

      lot.architecturalAgeYears=(week-b->second->versions.front().fromWeek)/52;
      lot.minimumFloors = lot.maximumFloors = std::max(1u, v->floors);
      lot.merchantHouse = b->second->use == CityBuildingUse::Shop || b->second->use == CityBuildingUse::Market;
      lot.residence = b->second->use == CityBuildingUse::Inn ? CityResidenceKind::Inn : b->second->use == CityBuildingUse::Keep || b->second->use == CityBuildingUse::Manor ? CityResidenceKind::Manor : CityResidenceKind::House;
      // The trade room follows the occupation the building was published
      // for; a farm is a farmstead whoever works it.
      lot.trade = b->second->use == CityBuildingUse::Farm ? "farmstead"
                : b->second->occupation && b->second->use != CityBuildingUse::Keep && b->second->use != CityBuildingUse::Manor && b->second->use != CityBuildingUse::Church ? cityTradeOf(*b->second->occupation)
                : b->second->use == CityBuildingUse::Workshop ? "carpentry" : "";
    }
  }

  plan.routes.clear();

  for (const auto &r : site.ground->completion.routes)
  {
    const auto record = site.routes.find(r.id);

    if (record != site.routes.end() && record->second.openedWeek <= week && (!record->second.closedWeek || week < *record->second.closedWeek))
    {
      plan.routes.push_back(r);
    }
  }

  return plan;
}

CityFabricFacts CityFabricLedger::facts(HistoryNode settlement) const
{
  CityFabricFacts f;
  const auto it = grounds.find(settlement);

  if (it == grounds.end())
  {
    return f;
  }

  const auto &site = it->second;

  for (const auto &[id, b] : site.buildings)
  {
    if (b.standing())
    {
      f.occupiedBlocks.insert(site.parcels.at(b.parcel).block);

      if (b.use == CityBuildingUse::Home)
      {
        ++f.homes;
        f.occupiedHomes += b.tenant.has_value();
        f.vacantHomes += !b.tenant.has_value();
      }
    }
  }

  for (const auto &[id, parcel] : site.parcels)
  {
    if (!parcel.building && parcel.frontageRoute)
    {
      ++f.freeParcels;
      f.reachableFreeParcels += accessComplete(site, parcel.block);
    }
  }

  for (const auto &[id, r] : site.routes)
  {
    if (r.closedWeek)
    {
      continue;
    }

    ++f.routesOpen;
    f.streets += r.surface == CityRouteSurface::Street;
    f.lanes += r.surface == CityRouteSurface::Lane;
    f.footpaths += r.surface == CityRouteSurface::Footpath;
  }

  f.underConstruction = uint32_t(site.constructions.size());

  return f;
}

bool CityFabricLedger::validate(const CityHistorySnapshot &snapshot, const CityHouseholdLedger &homes, const CityEconomyLedger &economy, std::string *error) const
{
  if (lastWeek != std::optional<uint32_t>(snapshot.week))
  {
    return fail(error, "fabric validated against a different week");
  }

  for (const auto &[settlement, site] : grounds)
  {
    const CityReliefSettings limits;
    std::set<HouseholdId> tenants;
    uint32_t standingHomes = 0;

    for (const auto &[id, b] : site.buildings)
    {
      const auto &parcel = site.parcels.at(b.parcel);
      uint32_t active = 0;

      for (const auto &v : b.versions)
      {
        active += !v.toWeek;

        if (v.toWeek && *v.toWeek < v.fromWeek)
        {
          return fail(error, "a building version ends before it begins");
        }
      }

      if (active > 1)
      {
        return fail(error, "a building has two standing versions");
      }

      if (parcel.building != id)
      {
        return fail(error, "a building's parcel does not name it");
      }

      const auto *v = b.standing();

      if (!v)
      {
        if (b.tenant)
        {
          return fail(error, "a household is housed in a building with no standing version");
        }

        continue;
      }

      if (!Bounds(parcel.boundary).contains(Bounds(v->footprint), 1e-6))
      {
        return fail(error, "a standing footprint leaves its parcel");
      }

      for (const auto &[otherId, other] : site.buildings)
      {
        const auto *o = other.standing();

        if (otherId != id && o && Bounds(o->footprint).overlaps(Bounds(v->footprint), 1e-6))
        {
          return fail(error, "two standing footprints overlap");
        }
      }

      if (!accessComplete(site, parcel.block))
      {
        return fail(error, "a building stands on a terrace with no finished stair");
      }

      if (!parcel.frontageRoute || std::abs(parcel.frontage.y - parcel.level) > site.settings.doorstepMetres + 1e-6)
      {
        return fail(error, "a standing building's entrance is not at its way's level");
      }

      if (b.use == CityBuildingUse::Home)
      {
        ++standingHomes;
      }

      if (b.tenant)
      {
        const auto h = homes.households().find(*b.tenant);

        if (b.use != CityBuildingUse::Home || h == homes.households().end() || h->second.dissolvedWeek || h->second.settlement != settlement || h->second.housing != CityHousingState::Housed || !tenants.insert(*b.tenant).second)
        {
          return fail(error, "a tenancy names a household that is not housed here, or twice");
        }
      }
    }

    for (const auto &[hid, h] : homes.households())
    {
      if (!h.dissolvedWeek && h.settlement == settlement && h.housing == CityHousingState::Housed && !tenants.count(hid))
      {
        return fail(error, "a housed household has no home building");
      }
    }

    const auto capacity = homes.policy().housingCapacity.find(settlement);

    if (capacity == homes.policy().housingCapacity.end() || capacity->second != standingHomes)
    {
      return fail(error, "the published housing capacity is not the number of standing homes");
    }

    for (const auto &stair : site.ground->relief.stairs)
    {
      double previous = stair.a.y, previousEnd = 0;
      uint32_t run = 0;

      // Treads and landings sit 4 cm proud of the ground they meet; a band
      // that climbs less than a riser's worth is a landing.
      for (const auto &[end, top] : stair.bands)
      {
        const double rise = top - previous, going = end - previousEnd;

        if (rise > limits.stepRiseMetres + .05)
        {
          return fail(error, "a stair riser exceeds the limit");
        }

        if (rise > .06)
        {
          if (going < limits.stepGoingMetres - 1e-6)
          {
            return fail(error, "a stair going is shorter than the limit");
          }

          if (++run > limits.stepsPerFlight)
          {
            return fail(error, "a flight runs past the landing limit");
          }
        }
        else
        {
          run = 0;
        }

        previous = top;
        previousEnd = end;
      }
    }

    for (const auto &[id, s] : site.structures)
    {
      if (s.kind == CityAccessKind::Bridge && s.state == CityStructureState::Completed && s.headroom < site.settings.minimumHeadroomMetres)
      {
        return fail(error, "a bridge has no headroom");
      }
    }

    for (const auto &[id, r] : site.routes)
    {
      for (size_t i = 1; i < r.path.size(); ++i)
      {
        const double run = flat(r.path[i - 1], r.path[i]);

        if (run > 1e-6 && std::abs(r.path[i].y - r.path[i - 1].y) / run > limits.maximumStreetGrade + 1e-6)
        {
          return fail(error, "a way is steeper than a street may be");
        }
      }
    }

    for (const auto &[id, c] : site.constructions)
    {
      if (c.workDone > c.workUnits)
      {
        return fail(error, "construction did more work than it needed");
      }
    }

    std::string why;

    if (!reachable(settlement, economy, homes, &why))
    {
      return fail(error, "isolated home: " + why);
    }
  }

  if (error)
  {
    error->clear();
  }

  return true;
}

const char *cityFabricEventName(CityFabricEventKind k)
{
  static const char *const names[] = {"founded",  "tenancy-assigned",        "tenancy-released",   "relocated",   "construction-started", "construction-suspended", "construction-resumed",
                                      "construction-completed", "construction-rejected", "capacity-published", "version-added", "building-retired", "route-upgraded", "route-closed",
                                      "route-closure-refused", "access-completed", "waiting"};

  return uint32_t(k) < 17 ? names[uint32_t(k)] : "unknown";
}

const char *cityRouteSurfaceName(CityRouteSurface s)
{
  return s == CityRouteSurface::Footpath ? "footpath" : s == CityRouteSurface::Lane ? "lane" : "street";
}

const char *cityAccessKindName(CityAccessKind k)
{
  static const char *const names[] = {"approach", "stair", "deck", "bridge"};

  return uint32_t(k) < 4 ? names[uint32_t(k)] : "unknown";
}

const char *cityStructureStateName(CityStructureState s)
{
  static const char *const names[] = {"planned", "building", "completed", "retired"};

  return uint32_t(s) < 4 ? names[uint32_t(s)] : "unknown";
}

const char *cityBuildingUseName(CityBuildingUse u)
{
  static const char *const names[] = {"home", "shop", "inn", "workshop", "farm", "market", "church", "keep", "manor"};

  return uint32_t(u) < 9 ? names[uint32_t(u)] : "unknown";
}
} // namespace procedural::world::rewrite
