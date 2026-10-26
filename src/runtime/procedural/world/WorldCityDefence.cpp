#include "WorldCityDefence.hpp"
#include <algorithm>
#include <cmath>

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

// Where two plan segments meet, if they do, including a touch at an end.
std::optional<Position> meet(const Position &a, const Position &b, const Position &c, const Position &d)
{
  const double d1x = b.x - a.x, d1z = b.z - a.z, d2x = d.x - c.x, d2z = d.z - c.z;
  const double denominator = d1x * d2z - d1z * d2x;

  if (std::abs(denominator) < 1e-9)
  {
    return std::nullopt;
  }

  const double u = ((c.x - a.x) * d2z - (c.z - a.z) * d2x) / denominator;
  const double v = ((c.x - a.x) * d1z - (c.z - a.z) * d1x) / denominator;

  if (u < -1e-9 || u > 1 + 1e-9 || v < -1e-9 || v > 1 + 1e-9)
  {
    return std::nullopt;
  }

  return Position{a.x + d1x * u, a.y + (b.y - a.y) * u, a.z + d1z * u};
}

double pointToSegment(const Position &p, const Position &a, const Position &b)
{
  const double dx = b.x - a.x, dz = b.z - a.z, length2 = dx * dx + dz * dz;
  const double t = length2 > 1e-12 ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / length2, 0., 1.) : 0.;

  return std::hypot(p.x - (a.x + dx * t), p.z - (a.z + dz * t));
}

bool segmentCrossesBox(const Position &a, const Position &b, const std::vector<Position> &ring)
{
  double minX = 1e300, minZ = 1e300, maxX = -1e300, maxZ = -1e300;

  for (const auto &p : ring)
  {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minZ = std::min(minZ, p.z);
    maxZ = std::max(maxZ, p.z);
  }

  if (cityInsideBoundary(ring, a) || cityInsideBoundary(ring, b))
  {
    return true;
  }

  const Position corners[] = {{minX, 0, minZ}, {maxX, 0, minZ}, {maxX, 0, maxZ}, {minX, 0, maxZ}};

  for (int i = 0; i < 4; ++i)
  {
    if (meet(a, b, corners[i], corners[(i + 1) % 4]))
    {
      return true;
    }
  }

  return false;
}

std::array<CityWallFace, 6> prismFaces(Position a, Position b, double width, double height)
{
  const double length = flat(a, b);
  const double nx = length > 1e-9 ? -(b.z - a.z) * width * .5 / length : 0;
  const double nz = length > 1e-9 ? (b.x - a.x) * width * .5 / length : 0;
  Position al{a.x + nx, a.y, a.z + nz}, ar{a.x - nx, a.y, a.z - nz};
  Position bl{b.x + nx, b.y, b.z + nz}, br{b.x - nx, b.y, b.z - nz};
  Position alt = al, art = ar, blt = bl, brt = br;

  alt.y += height;
  art.y += height;
  blt.y += height;
  brt.y += height;

  return {{{CityWallFaceKind::Outside, {al, bl, blt, alt}},
           {CityWallFaceKind::Inside, {br, ar, art, brt}},
           {CityWallFaceKind::Up, {alt, blt, brt, art}},
           {CityWallFaceKind::Bottom, {ar, br, bl, al}},
           {CityWallFaceKind::Left, {ar, al, alt, art}},
           {CityWallFaceKind::Right, {bl, br, brt, blt}}}};
}

std::vector<std::pair<Position, Position>> accessLines(const CityFabricSite &site)
{
  std::vector<std::pair<Position, Position>> lines;

  for (const auto &[id, s] : site.structures)
  {
    if ((s.kind == CityAccessKind::Approach || s.kind == CityAccessKind::Stair) && s.state != CityStructureState::Retired)
    {
      lines.push_back({s.from, s.to});
    }
  }

  return lines;
}
} // namespace

bool cityInsideBoundary(const std::vector<Position> &ring, const Position &p)
{
  bool in = false;

  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
  {
    const auto &a = ring[j], &b = ring[i];

    if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x)
    {
      in = !in;
    }
  }

  return in;
}

std::vector<CityBoundaryCrossing> cityBoundaryCrossings(const CityFabricSite &site, const std::vector<Position> &boundary)
{
  std::vector<CityBoundaryCrossing> crossings;
  auto record = [&](uint64_t route, size_t edge, const Position &at) {
    for (const auto &c : crossings)
    {
      if (flat(c.position, at) < .5)
      {
        return;
      }
    }

    crossings.push_back({route, edge, at});
  };

  for (size_t e = 0; e < boundary.size(); ++e)
  {
    const auto &a = boundary[e], &b = boundary[(e + 1) % boundary.size()];

    for (const auto &[id, r] : site.routes)
    {
      if (r.closedWeek)
      {
        continue;
      }

      for (size_t i = 1; i < r.path.size(); ++i)
      {
        if (const auto at = meet(a, b, r.path[i - 1], r.path[i]))
        {
          record(id, e, *at);
        }
      }
    }

    for (const auto &[from, to] : accessLines(site))
    {
      if (const auto at = meet(a, b, from, to))
      {
        record(0, e, *at);
      }
    }
  }

  return crossings;
}

bool CityDefenceCircuit::closed() const
{
  if (segments.empty())
  {
    return false;
  }

  for (const auto &s : segments)
  {
    if (s.state != CityStructureState::Completed)
    {
      return false;
    }
  }

  for (const auto &g : gates)
  {
    if (g.state != CityStructureState::Completed)
    {
      return false;
    }
  }

  return true;
}

CityDefenceLedger::CityDefenceLedger(CityDefenceSettings settings) : rules(std::move(settings))
{
}

class CityDefenceLedger::Week
{
public:
  Week(CityDefenceLedger &ledger, CityDefenceSite &site, const CityFabricSite &ground, const CityHouseholdLedger &homes, CityEconomyLedger &economy, CityRealmLedger &realm, uint32_t week)
      : L(ledger), site(site), ground(ground), homes(homes), economy(economy), realm(realm), week(week)
  {
  }

  void run()
  {
    sweep();
    start();

    const uint32_t elapsed = week - ground.foundedWeek;

    if (elapsed && elapsed % 4 == 0)
    {
      progress();
    }

    retireInner();
    stage();
  }

private:
  CityDefenceLedger &L;
  CityDefenceSite &site;
  const CityFabricSite &ground;
  const CityHouseholdLedger &homes;
  CityEconomyLedger &economy;
  CityRealmLedger &realm;
  const uint32_t week;

  void add(CityDefenceEventKind kind, uint64_t subject, std::string detail)
  {
    L.log->push_back({week, kind, site.settlement, subject, std::move(detail)});
  }

  bool underWorks(const CityDefenceCircuit &c) const
  {
    const auto p = realm.projects().find(c.project);

    return p != realm.projects().end() && (p->second.state == CityProjectState::Building || p->second.state == CityProjectState::Suspended);
  }

  void sweep()
  {
    for (auto &c : site.circuits)
    {
      if (c.lifecycle == CityWallLifecycle::Planned && !c.abandoned && !underWorks(c))
      {
        c.abandoned = true;
        add(CityDefenceEventKind::CircuitAbandoned, c.id, "works stopped with " + std::to_string(std::count_if(c.segments.begin(), c.segments.end(), [](const auto &s) { return s.state == CityStructureState::Completed; })) + " of " + std::to_string(c.segments.size()) + " segments built");
      }
    }
  }

  std::set<uint64_t> occupiedBlocks() const
  {
    std::set<uint64_t> blocks;

    for (const auto &[id, b] : ground.buildings)
    {
      if (b.standing())
      {
        blocks.insert(ground.parcels.at(b.parcel).block);
      }
    }

    return blocks;
  }

  void start()
  {
    for (const auto &[id, p] : std::map<uint64_t, CityProject>(realm.projects()))
    {
      if (p.state != CityProjectState::Funded || p.intent.settlement != site.settlement || (p.intent.kind != CityProjectKind::Wall && p.intent.kind != CityProjectKind::Palisade))
      {
        continue;
      }

      bool known = false;

      for (const auto &c : site.circuits)
      {
        known = known || c.project == id;
      }

      if (known)
      {
        continue;
      }

      const auto blocks = occupiedBlocks();

      if (blocks.empty())
      {
        realm.noteProject(id, "nothing stands to be enclosed");

        continue;
      }

      // A stone wall may replace a palisade around the same terraces; a
      // second wall or palisade around what is already enclosed may not.
      for (const auto &c : site.circuits)
      {
        const bool comparable = p.intent.kind == CityProjectKind::Palisade || !c.palisade;

        if (comparable && !c.abandoned && c.lifecycle != CityWallLifecycle::Absorbed && std::includes(c.enclosedBlocks.begin(), c.enclosedBlocks.end(), blocks.begin(), blocks.end()))
        {
          realm.rejectProject(id, week, "the standing circuit already encloses every occupied terrace", economy);
          add(CityDefenceEventKind::Rejected, id, "nothing new to enclose");
          known = true;
        }
      }

      if (known)
      {
        continue;
      }

      plan(id, p, blocks);
    }
  }

  void plan(uint64_t project, const CityProject &p, const std::set<uint64_t> &blocks)
  {
    double minX = 1e300, minZ = 1e300, maxX = -1e300, maxZ = -1e300;

    for (const auto &deck : ground.ground->relief.platforms)
    {
      if (!blocks.count(deck.block))
      {
        continue;
      }

      for (const auto &q : deck.boundary)
      {
        minX = std::min(minX, q.x);
        maxX = std::max(maxX, q.x);
        minZ = std::min(minZ, q.z);
        maxZ = std::max(maxZ, q.z);
      }
    }

    const double m = L.rules.marginMetres;
    CityDefenceCircuit c;

    c.id = site.nextId++;
    c.project = project;
    c.era = uint32_t(site.circuits.size()) + 1;
    c.palisade = p.intent.kind == CityProjectKind::Palisade;
    c.boundary = {{minX - m, 0, minZ - m}, {maxX + m, 0, minZ - m}, {maxX + m, 0, maxZ + m}, {minX - m, 0, maxZ + m}};
    c.enclosedBlocks = blocks;

    for (auto &q : c.boundary)
    {
      q.y = ground.ground->relief.openGround(q.x, q.z);
    }

    const auto crossings = cityBoundaryCrossings(ground, c.boundary);
    double sinceTower = 0;

    for (size_t e = 0; e < c.boundary.size(); ++e)
    {
      const auto &a = c.boundary[e], &b = c.boundary[(e + 1) % c.boundary.size()];
      const double length = flat(a, b);
      const uint32_t pieces = std::max(1u, uint32_t(std::ceil(length / L.rules.segmentMetres)));

      for (uint32_t k = 0; k < pieces; ++k)
      {
        CityWallSegmentRecord s;

        s.id = site.nextId++;
        s.a = {a.x + (b.x - a.x) * k / pieces, 0, a.z + (b.z - a.z) * k / pieces};
        s.b = {a.x + (b.x - a.x) * (k + 1) / pieces, 0, a.z + (b.z - a.z) * (k + 1) / pieces};
        s.a.y = ground.ground->relief.openGround(s.a.x, s.a.z);
        s.b.y = ground.ground->relief.openGround(s.b.x, s.b.z);
        s.workUnits = L.rules.workUnitsPerSegment;

        // A tower at every corner and wherever the spacing runs out.
        if (k == 0 || sinceTower >= L.rules.towerSpacingMetres)
        {
          CityTowerRecord t;

          t.id = site.nextId++;
          t.segment = s.id;
          t.position = s.a;
          t.workUnits = L.rules.workUnitsPerTower;
          c.towers.push_back(t);
          sinceTower = 0;
        }

        for (const auto &x : crossings)
        {
          if (x.edge == e && pointToSegment(x.position, s.a, s.b) < .05)
          {
            CityGateRecord g;

            g.id = site.nextId++;
            g.segment = s.id;
            g.route = x.route;
            g.position = x.position;
            g.position.y = ground.ground->relief.openGround(g.position.x, g.position.z);
            g.workUnits = L.rules.workUnitsPerGate;
            c.gates.push_back(g);
          }
        }

        sinceTower += flat(s.a, s.b);
        c.segments.push_back(s);
      }
    }

    CityStock materials = p.escrow;

    materials[CityGood::Coin] = 0;

    std::string error;

    if (!realm.startBuilding(project, week, &error) || !realm.drawEscrow(project, materials, &error))
    {
      add(CityDefenceEventKind::Rejected, project, error);

      return;
    }

    add(CityDefenceEventKind::CircuitPlanned, c.id, std::string(c.palisade ? "palisade" : "wall") + " of " + std::to_string(c.segments.size()) + " segments, " + std::to_string(c.gates.size()) + " gates, " + std::to_string(c.towers.size()) + " towers around " + std::to_string(blocks.size()) + " terrace(s)");
    site.circuits.push_back(std::move(c));
  }

  uint32_t totalUnits(const CityDefenceCircuit &c) const
  {
    uint32_t n = 0;

    for (const auto &s : c.segments)
    {
      n += s.workUnits;
    }

    for (const auto &g : c.gates)
    {
      n += g.workUnits;
    }

    for (const auto &t : c.towers)
    {
      n += t.workUnits;
    }

    return n;
  }

  template <typename Record> uint32_t work(Record &r, uint32_t &units, CityDefenceEventKind done, uint64_t id)
  {
    if (r.state == CityStructureState::Completed || units == 0)
    {
      return 0;
    }

    const uint32_t w = std::min(units, r.workUnits - r.workDone);

    r.workDone += w;
    units -= w;
    r.state = r.workDone >= r.workUnits ? CityStructureState::Completed : CityStructureState::Building;

    if (r.state == CityStructureState::Completed)
    {
      r.completedWeek = week;
      add(done, id, "");
    }

    return w;
  }

  void progress()
  {
    const auto crew = economy.workersAt(site.settlement, {CityOccupation::Mason, CityOccupation::Labourer, CityOccupation::Carpenter});
    uint32_t units = uint32_t(crew.size()) * L.rules.workUnitsPerBuilderPerMonth;
    std::vector<HouseholdId> paid;

    for (auto worker : crew)
    {
      if (const auto home = homes.householdOf(worker))
      {
        paid.push_back(*home);
      }
    }

    for (auto &c : site.circuits)
    {
      if (c.abandoned || c.lifecycle != CityWallLifecycle::Planned || !underWorks(c))
      {
        continue;
      }

      const auto &p = realm.projects().at(c.project);
      uint32_t done = 0;

      for (auto &s : c.segments)
      {
        done += work(s, units, CityDefenceEventKind::SegmentCompleted, s.id);
      }

      for (auto &g : c.gates)
      {
        if (std::find_if(c.segments.begin(), c.segments.end(), [&](const auto &s) { return s.id == g.segment && s.state == CityStructureState::Completed; }) != c.segments.end())
        {
          done += work(g, units, CityDefenceEventKind::GateCompleted, g.id);
        }
      }

      for (auto &t : c.towers)
      {
        if (std::find_if(c.segments.begin(), c.segments.end(), [&](const auto &s) { return s.id == t.segment && s.state == CityStructureState::Completed; }) != c.segments.end())
        {
          done += work(t, units, CityDefenceEventKind::TowerCompleted, t.id);
        }
      }

      if (done == 0)
      {
        if (p.state == CityProjectState::Building && ++c.idleMonths >= L.rules.idleMonthsBeforeSuspension)
        {
          realm.suspendProject(c.project, week, "no builders");
          add(CityDefenceEventKind::WorksSuspended, c.id, "no builders");
        }

        continue;
      }

      if (p.state == CityProjectState::Suspended)
      {
        realm.resumeProject(c.project, week);
        add(CityDefenceEventKind::WorksResumed, c.id, "");
      }

      c.idleMonths = 0;

      const int64_t coin = p.cost[CityGood::Coin] * int64_t(done) / int64_t(std::max(1u, totalUnits(c)));
      CityStock draw;

      draw[CityGood::Coin] = std::min(coin, p.escrow[CityGood::Coin]);

      if (draw[CityGood::Coin] > 0 && !paid.empty() && realm.drawEscrow(c.project, draw))
      {
        const int64_t each = draw[CityGood::Coin] / int64_t(paid.size());
        int64_t remainder = draw[CityGood::Coin] - each * int64_t(paid.size());

        for (auto household : paid)
        {
          economy.payFromEscrow(household, each + (remainder > 0 ? 1 : 0), week);
          remainder = std::max<int64_t>(0, remainder - 1);
        }
      }

      bool towersDone = true;

      for (const auto &t : c.towers)
      {
        towersDone = towersDone && t.state == CityStructureState::Completed;
      }

      if (c.closed() && towersDone)
      {
        c.lifecycle = CityWallLifecycle::Standing;
        c.closedWeek = week;
        realm.completeProject(c.project, week, economy);
        add(CityDefenceEventKind::CircuitClosed, c.id, std::to_string(c.segments.size()) + " segments and " + std::to_string(c.gates.size()) + " gates closed");
      }
    }
  }

  // An inner circuit retires only once the outer one is closed and gated.
  void retireInner()
  {
    for (const auto &outer : site.circuits)
    {
      if (outer.lifecycle != CityWallLifecycle::Standing)
      {
        continue;
      }

      for (auto &inner : site.circuits)
      {
        if (inner.id != outer.id && inner.lifecycle == CityWallLifecycle::Standing && inner.era < outer.era && (inner.palisade || !outer.palisade) && std::includes(outer.enclosedBlocks.begin(), outer.enclosedBlocks.end(), inner.enclosedBlocks.begin(), inner.enclosedBlocks.end()))
        {
          inner.lifecycle = CityWallLifecycle::Absorbed;
          inner.absorbedWeek = week;
          realm.retireProject(inner.project, week, "absorbed by the outer circuit");
          add(CityDefenceEventKind::CircuitAbsorbed, inner.id, "kept as a district boundary inside circuit " + std::to_string(outer.id));
        }
      }
    }
  }

  void stage()
  {
    bool manor = false, keep = false;

    for (const auto &[id, b] : ground.buildings)
    {
      manor = manor || (b.use == CityBuildingUse::Manor && b.standing());
      keep = keep || (b.use == CityBuildingUse::Keep && b.standing());
    }

    bool fence = false, wall = false, castle = false;

    for (const auto &c : site.circuits)
    {
      if (c.lifecycle != CityWallLifecycle::Standing)
      {
        continue;
      }

      fence = true;
      wall = wall || !c.palisade;
      castle = castle || (!c.palisade && c.towers.size() >= 4);
    }

    CityDefenceStage next = CityDefenceStage::None;

    if (keep && wall && castle)
    {
      next = CityDefenceStage::Castle;
    }
    else if (keep && wall)
    {
      next = CityDefenceStage::Curtain;
    }
    else if (keep)
    {
      next = CityDefenceStage::Keep;
    }
    else if (manor && fence)
    {
      next = CityDefenceStage::FortifiedManor;
    }
    else if (manor)
    {
      next = CityDefenceStage::Manor;
    }

    if (next != site.stage)
    {
      site.stage = next;
      add(CityDefenceEventKind::StageReached, uint64_t(next), cityDefenceStageName(next));
    }
  }
};

bool CityDefenceLedger::observe(const CityHistorySnapshot &snapshot, const CityHouseholdLedger &homes, CityEconomyLedger &economy, CityRealmLedger &realm, const CityFabricLedger &fabric, std::string *error)
{
  if (lastWeek && uint64_t(snapshot.week) != uint64_t(*lastWeek) + 1)
  {
    return fail(error, "defence observations must be consecutive weeks");
  }

  if (fabric.week() != std::optional<uint32_t>(snapshot.week) || realm.week() != fabric.week() || economy.week() != fabric.week())
  {
    return fail(error, "the defences follow the fabric, realm and economy of the same week");
  }

  const size_t mark = log->size(), economyMark = economy.eventMark(), realmMark = realm.eventMark();
  CityDefenceLedger next = *this;
  CityEconomyLedger economyCopy = economy;
  CityRealmLedger realmCopy = realm;

  next.lastWeek = snapshot.week;

  for (const auto &[settlement, ground] : fabric.sites())
  {
    auto &site = next.grounds[settlement];

    site.settlement = settlement;

    Week week(next, site, ground, homes, economyCopy, realmCopy, snapshot.week);

    week.run();
  }

  if (!next.validate(snapshot, fabric, error) || !realmCopy.validate(snapshot, error) || !economyCopy.validate(snapshot, homes, error))
  {
    log->resize(mark);
    economy.truncateEvents(economyMark);
    realm.truncateEvents(realmMark);

    return false;
  }

  *this = std::move(next);
  economy = std::move(economyCopy);
  realm = std::move(realmCopy);

  if (error)
  {
    error->clear();
  }

  return true;
}

bool CityDefenceLedger::validate(const CityHistorySnapshot &snapshot, const CityFabricLedger &fabric, std::string *error) const
{
  if (lastWeek != std::optional<uint32_t>(snapshot.week))
  {
    return fail(error, "defences validated against a different week");
  }

  for (const auto &[settlement, site] : grounds)
  {
    const auto ground = fabric.sites().find(settlement);

    if (ground == fabric.sites().end())
    {
      return fail(error, "defences without a site");
    }

    std::optional<Position> keep;

    for (const auto &[id, b] : ground->second.buildings)
    {
      if (b.use == CityBuildingUse::Keep && b.standing())
      {
        keep = b.standing()->footprint.front();
      }
    }

    for (const auto &c : site.circuits)
    {
      if (c.lifecycle == CityWallLifecycle::Standing && !c.closed())
      {
        return fail(error, "a circuit reported standing is not closed");
      }

      for (size_t i = 0; i < c.segments.size(); ++i)
      {
        const auto &s = c.segments[i], &n = c.segments[(i + 1) % c.segments.size()];

        if (flat(s.b, n.a) > 1e-6)
        {
          return fail(error, "wall segments do not chain into a circuit");
        }

        if (std::abs(s.a.y - s.b.y) > 8)
        {
          return fail(error, "a wall segment spans a cliff its foundation cannot");
        }
      }

      for (const auto &t : c.towers)
      {
        bool onEnd = false;

        for (const auto &s : c.segments)
        {
          onEnd = onEnd || (s.id == t.segment && flat(t.position, s.a) < 1e-6);
        }

        if (!onEnd)
        {
          return fail(error, "a tower stands off its wall");
        }
      }

      if (c.lifecycle != CityWallLifecycle::Standing)
      {
        continue;
      }

      const auto crossings = cityBoundaryCrossings(ground->second, c.boundary);

      for (const auto &x : crossings)
      {
        bool gated = false;

        for (const auto &g : c.gates)
        {
          gated = gated || flat(g.position, x.position) < .5;
        }

        if (!gated)
        {
          return fail(error, "a standing wall severs a way with no gate");
        }
      }

      for (const auto &g : c.gates)
      {
        bool onWall = false;

        for (const auto &s : c.segments)
        {
          onWall = onWall || (s.id == g.segment && pointToSegment(g.position, s.a, s.b) < .1);
        }

        if (!onWall)
        {
          return fail(error, "a gate stands off its wall");
        }
      }

      for (const auto &s : c.segments)
      {
        bool gateSegment = false;

        for (const auto &g : c.gates)
        {
          gateSegment = gateSegment || g.segment == s.id;
        }

        for (const auto &[id, b] : ground->second.buildings)
        {
          const auto *v = b.standing();

          if (v && segmentCrossesBox(s.a, s.b, v->footprint))
          {
            return fail(error, "a wall runs through a standing building");
          }
        }

        if (!gateSegment)
        {
          for (const auto &stair : ground->second.ground->relief.stairs)
          {
            if (meet(s.a, s.b, stair.a, stair.b))
            {
              return fail(error, "a wall cuts a stair with no gate");
            }
          }
        }
      }

      if (keep && !c.palisade && !cityInsideBoundary(c.boundary, *keep) && site.stage >= CityDefenceStage::Curtain)
      {
        return fail(error, "the keep stands outside its curtain");
      }
    }
  }

  if (error)
  {
    error->clear();
  }

  return true;
}

void CityDefenceLedger::applyTo(HistoryNode settlement, uint32_t week, CityCompletionPlan &plan) const
{
  const auto it = grounds.find(settlement);

  if (it == grounds.end())
  {
    return;
  }

  for (const auto &c : it->second.circuits)
  {
    const bool standing = c.closedWeek && *c.closedWeek <= week;
    const bool absorbed = c.absorbedWeek && *c.absorbedWeek <= week;
    const auto lifecycle = absorbed ? CityWallLifecycle::Absorbed : standing ? CityWallLifecycle::Standing : CityWallLifecycle::Planned;
    const double width = c.palisade ? rules.palisadeWidth : rules.wallWidth, height = c.palisade ? rules.palisadeHeight : rules.wallHeight;

    for (const auto &s : c.segments)
    {
      if (!s.completedWeek || *s.completedWeek > week)
      {
        continue;
      }

      CityWallStructureSegment out;

      out.id = s.id;
      out.wall = c.id;
      out.era = c.era;
      out.lifecycle = lifecycle;
      out.a = s.a;
      out.b = s.b;
      out.width = width;
      out.height = height;
      out.depth = width;
      out.built = true;
      out.faces = prismFaces(s.a, s.b, width, height);
      plan.wallSegments.push_back(out);
    }

    for (const auto &g : c.gates)
    {
      if (!g.completedWeek || *g.completedWeek > week)
      {
        continue;
      }

      CityGatehouseStructure out;

      out.id = g.id;
      out.wall = c.id;
      out.era = c.era;
      out.position = g.position;
      out.openingWidth = rules.gateWidth;
      out.depth = width + 1;
      out.height = height + 2;
      out.built = true;
      plan.gatehouses.push_back(out);
    }

    for (const auto &t : c.towers)
    {
      if (!t.completedWeek || *t.completedWeek > week)
      {
        continue;
      }

      CityWallTowerStructure out;

      out.id = t.id;
      out.wall = c.id;
      out.era = c.era;
      out.lifecycle = lifecycle;
      out.position = t.position;
      out.radius = rules.towerRadius;
      out.height = rules.towerHeight;
      out.built = true;
      plan.towers.push_back(out);
    }
  }
}

const char *cityDefenceStageName(CityDefenceStage s)
{
  static const char *const names[] = {"none", "manor", "fortified-manor", "keep", "curtain", "castle"};

  return uint32_t(s) < 6 ? names[uint32_t(s)] : "unknown";
}

const char *cityDefenceEventName(CityDefenceEventKind k)
{
  static const char *const names[] = {"stage-reached", "circuit-planned", "segment-completed", "gate-completed", "tower-completed", "circuit-closed", "circuit-abandoned", "circuit-absorbed", "works-suspended", "works-resumed", "rejected"};

  return uint32_t(k) < 11 ? names[uint32_t(k)] : "unknown";
}
} // namespace procedural::world::rewrite
