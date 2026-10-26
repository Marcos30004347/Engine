#include "WorldSettlementDirector.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>

namespace procedural::world::rewrite {
namespace {
double area(const std::vector<Position> &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  }
  return std::abs(a) * .5;
}

bool residential(ProgrammeKind k)
{
  return k == ProgrammeKind::Home || k == ProgrammeKind::Shop || k == ProgrammeKind::Collective;
}

bool finish(const SettlementDirectorInput &in, SettlementState &state, std::string *error)
{
  auto fail = [&](const std::string &s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (!in.settlement || in.settlement != state.settlement || in.nodes.size() > 8192 || in.edges.size() > 32768 || in.sites.size() > 4096 || in.catalogue.empty() || in.catalogue.size() > 128 || in.demands.size() > 128 ||
      in.maximumNewBuildings > 4096 || in.maximumFitAttempts > 65536 || !std::isfinite(in.maximumCommute) || !std::isfinite(in.maximumServiceTravel) || in.maximumCommute < 0 || in.maximumServiceTravel < 0)
  {
    return fail("invalid settlement director bounds");
  }
  auto catalogue = in.catalogue;
  std::sort(catalogue.begin(), catalogue.end(), [](auto &a, auto &b) {
    return a.id < b.id;
  });
  std::map<std::string, SettlementProgramme> programmes;
  for (auto &p : catalogue)
  {
    if (p.id.empty() || p.id.size() > 100 || !std::isfinite(p.minimumPlotArea) || p.minimumPlotArea < 20 || p.minimumPlotArea > 10000 || p.floors < 1 || p.floors > 3 || unsigned(p.kind) > 5 || unsigned(p.dwelling) > 4 ||
        p.serviceCatchmentCapacity > 100000 || !programmes.emplace(p.id, p).second)
    {
      return fail("invalid or duplicate programme");
    }
  }
  for (auto &p : catalogue)
  {
    DwellingUse expected = p.kind == ProgrammeKind::Home         ? DwellingUse::Cottage
                           : p.kind == ProgrammeKind::Shop       ? DwellingUse::ShopHouse
                           : p.kind == ProgrammeKind::Collective ? DwellingUse::Lodging
                           : p.kind == ProgrammeKind::Warehouse  ? DwellingUse::Warehouse
                                                                 : DwellingUse::Hall;
    if (p.dwelling != expected || (p.kind != ProgrammeKind::Temple && p.serviceCatchmentCapacity))
    {
      return fail("programme kind and spatial recipe disagree");
    }
  }
  std::map<uint64_t, size_t> nodeIndex;
  for (size_t i = 0; i < in.nodes.size(); ++i)
  {
    auto n = in.nodes[i];
    if (!n.id || !nodeIndex.emplace(n.id, i).second || !std::isfinite(n.position.x) || !std::isfinite(n.position.z))
    {
      return fail("invalid travel node");
    }
  }
  std::vector<std::vector<std::pair<size_t, double>>> graph(in.nodes.size());
  for (auto e : in.edges)
  {
    if (e.a == e.b || !nodeIndex.count(e.a) || !nodeIndex.count(e.b) || !std::isfinite(e.cost) || e.cost <= 0)
    {
      return fail("invalid travel edge");
    }
    auto a = nodeIndex[e.a], b = nodeIndex[e.b];
    graph[a].push_back({b, e.cost});
    graph[b].push_back({a, e.cost});
  }
  std::map<uint64_t, std::vector<double>> distanceCache;
  auto distances = [&](uint64_t node) -> const std::vector<double> & {
    auto found = distanceCache.find(node);
    if (found != distanceCache.end())
    {
      return found->second;
    }
    std::vector<double> d(in.nodes.size(), INFINITY);
    using E = std::pair<double, size_t>;
    std::priority_queue<E, std::vector<E>, std::greater<E>> q;
    d[nodeIndex[node]] = 0;
    q.push({0, nodeIndex[node]});
    while (!q.empty())
    {
      auto [c, i] = q.top();
      q.pop();
      if (c != d[i])
      {
        continue;
      }
      for (auto [j, w] : graph[i])
      {
        if (c + w < d[j])
        {
          d[j] = c + w;
          q.push({d[j], j});
        }
      }
    }
    return distanceCache.emplace(node, std::move(d)).first->second;
  };
  auto sites = in.sites;
  std::sort(sites.begin(), sites.end(), [](auto &a, auto &b) {
    return a.id < b.id;
  });
  std::set<uint64_t> siteIds;
  for (auto &s : sites)
  {
    if (!s.id || !siteIds.insert(s.id).second || !nodeIndex.count(s.node) || !std::isfinite(s.centrality) || !std::isfinite(s.elevationPreference) || !std::isfinite(s.slope) || s.centrality < 0 || s.centrality > 1 ||
        s.elevationPreference < 0 || s.elevationPreference > 1 || s.slope < 0 || s.slope > 2 || s.allowedKinds > 0x3f)
    {
      return fail("invalid site geography or identity");
    }
  }
  state.population = state.workers = state.housed = state.employed = state.unserved = 0;
  state.employment.clear();
  state.services.clear();
  state.unmetProgrammes.clear();
  state.rejections.clear();
  std::sort(state.households.begin(), state.households.end(), [](auto &a, auto &b) {
    return a.id < b.id;
  });
  std::set<uint64_t> houseIds;
  for (auto h : state.households)
  {
    if (!h.id || !houseIds.insert(h.id).second || !h.members || h.members > 100000 || state.population > 100000 - h.members || h.workers > h.members)
    {
      return fail("invalid household");
    }
    state.population += h.members;
    state.workers += h.workers;
  }
  if (state.population > 100000)
  {
    return fail("settlement population budget exceeded");
  }
  state.foodShortfall = state.population > in.foodResidentCapacity ? state.population - in.foodResidentCapacity : 0;
  state.waterShortfall = state.population > in.waterResidentCapacity ? state.population - in.waterResidentCapacity : 0;
  uint32_t supplied = std::min(in.foodResidentCapacity, in.waterResidentCapacity);
  std::set<uint64_t> usedSites;
  std::map<uint64_t, SettlementBuilding *> existing;
  for (auto &b : state.buildings)
  {
    if (!b.id || !usedSites.insert(b.site).second || !nodeIndex.count(b.node) || !programmes.count(b.programme) || !existing.emplace(b.id, &b).second)
    {
      return fail("invalid retained building or changed catalogue");
    }
    const auto &definition = programmes[b.programme];
    if (definition.kind != b.kind || definition.dwelling != b.interior.use || definition.floors != b.interior.floors.size() || definition.serviceCatchmentCapacity != b.serviceCapacity)
    {
      return fail("retained programme changed without conversion transaction");
    }
  }
  // Retain valid current homes before filling vacancies; supply deficits do not evict existing residents.
  std::vector<SettlementTenancy> retained;
  std::set<uint64_t> assigned;
  std::map<uint64_t, uint32_t> occupancy;
  for (auto t : state.tenancies)
  {
    if (houseIds.count(t.household))
    {
      auto h = std::find_if(state.households.begin(), state.households.end(), [&](auto h) {
        return h.id == t.household;
      });
      if (!existing.count(t.building) || !assigned.insert(t.household).second || t.residents != h->members)
      {
        return fail("invalid retained tenancy");
      }
      occupancy[t.building] += t.residents;
      if (occupancy[t.building] > existing[t.building]->interior.residentCapacity)
      {
        return fail("retained capacity exceeded");
      }
      retained.push_back(t);
      state.housed += t.residents;
    }
  }
  state.tenancies = retained;
  std::set<uint64_t> serviceReserves(state.serviceReserveSites.begin(), state.serviceReserveSites.end());
  if (in.demands.empty() && std::any_of(catalogue.begin(), catalogue.end(), [](auto &p) {
        return p.kind == ProgrammeKind::Temple;
      }))
  {
    std::set<uint64_t> covered;
    auto mark = [&](uint64_t node) {
      const auto &costs = distances(node);
      for (auto &s : sites)
      {
        if (costs[nodeIndex[s.node]] <= in.maximumServiceTravel)
        {
          covered.insert(s.id);
        }
      }
    };
    for (auto &b : state.buildings)
    {
      if (b.kind == ProgrammeKind::Temple)
      {
        mark(b.node);
      }
    }
    for (auto id : serviceReserves)
    {
      auto it = std::find_if(sites.begin(), sites.end(), [&](auto &s) {
        return s.id == id;
      });
      if (it == sites.end())
      {
        return fail("retained service reserve disappeared");
      }
      mark(it->node);
    }
    while (serviceReserves.size() < 128)
    {
      const SettlementSite *best = nullptr;
      uint32_t bestGain = 0;
      for (auto &s : sites)
      {
        if (!usedSites.count(s.id) && !serviceReserves.count(s.id) && s.buildable && s.water && s.slope <= .12 && (s.allowedKinds & (1u << unsigned(ProgrammeKind::Temple))) && area(s.shell.shell) >= 120)
        {
          uint32_t gain = 0, potential = 0;
          const auto &costs = distances(s.node);
          for (auto &t : sites)
          {
            if (t.buildable && costs[nodeIndex[t.node]] <= in.maximumServiceTravel)
            {
              ++potential;
              if (!covered.count(t.id))
              {
                ++gain;
              }
            }
          }
          if (potential < 2)
          {
            continue;
          }
          if (gain > bestGain || (gain == bestGain && best && s.centrality > best->centrality))
          {
            best = &s;
            bestGain = gain;
          }
        }
      }
      if (!best)
      {
        break;
      }
      serviceReserves.insert(best->id);
      mark(best->node);
    }
  }
  state.serviceReserveSites.assign(serviceReserves.begin(), serviceReserves.end());
  auto occupy = [&]() {
    std::vector<const SettlementHousehold *> waiting;
    for (auto &h : state.households)
    {
      if (!assigned.count(h.id))
      {
        waiting.push_back(&h);
      }
    }
    std::sort(waiting.begin(), waiting.end(), [](auto a, auto b) {
      return a->members == b->members ? a->id < b->id : a->members > b->members;
    });
    for (auto h : waiting)
    {
      if (state.housed + h->members > supplied)
      {
        continue;
      }
      SettlementBuilding *choice = nullptr;
      uint32_t waste = UINT32_MAX;
      for (auto &b : state.buildings)
      {
        if (!residential(b.kind) || h->collective != (b.kind == ProgrammeKind::Collective))
        {
          continue;
        }
        uint32_t used = occupancy[b.id];
        if (used && b.kind != ProgrammeKind::Collective)
        {
          continue;
        }
        uint32_t cap = std::max(1u, b.interior.residentCapacity) * 2; // Crowding accepted (see the fit loop).
        if (cap < used + h->members)
        {
          continue;
        }
        if (cap - used - h->members < waste || (cap - used - h->members == waste && choice && b.id < choice->id))
        {
          choice = &b;
          waste = cap - used - h->members;
        }
      }
      if (choice)
      {
        assigned.insert(h->id);
        occupancy[choice->id] += h->members;
        state.housed += h->members;
        state.tenancies.push_back({h->id, choice->id, h->members});
      }
    }
  };
  occupy();
  std::map<std::string, uint32_t> needed;
  if (in.demands.empty())
  {
    uint32_t families = 0, collective = 0;
    for (auto &h : state.households)
    {
      if (!assigned.count(h.id))
      {
        if (h.collective)
        {
          collective += h.members;
        }
        else
        {
          ++families;
        }
      }
    }
    auto request = [&](ProgrammeKind kind, uint32_t count) {
      auto it = std::find_if(catalogue.begin(), catalogue.end(), [&](auto &p) {
        return p.kind == kind;
      });
      if (it != catalogue.end())
      {
        needed[it->id] += count;
      }
      else if (count)
      {
        state.rejections.push_back("catalogue lacks demanded kind " + std::to_string(unsigned(kind)));
      }
    };
    request(ProgrammeKind::Home, families);
    request(ProgrammeKind::Collective, (collective + 23) / 24);
    if (state.population)
    {
      request(ProgrammeKind::Temple, std::min(uint32_t(state.households.size()), in.maximumNewBuildings));
    }
    if (families >= 4)
    {
      uint32_t shops = std::max(1u, families / 5);
      request(ProgrammeKind::Shop, shops);
      for (auto &p : catalogue)
      {
        if (p.kind == ProgrammeKind::Home)
        {
          needed[p.id] = families - shops;
        }
      }
    }
  }
  else
  {
    for (auto d : in.demands)
    {
      if (!programmes.count(d.programme) || d.count > 4096)
      {
        return fail("unknown or excessive programme demand");
      }
      uint32_t have = 0;
      for (auto &b : state.buildings)
      {
        have += b.programme == d.programme;
      }
      needed[d.programme] += d.count > have ? d.count - have : 0;
    }
  }
  std::vector<std::string> order;
  for (auto [n, count] : needed)
  {
    if (count)
    {
      order.push_back(n);
    }
  }
  std::sort(order.begin(), order.end(), [&](auto &a, auto &b) {
    bool aService = programmes[a].kind == ProgrammeKind::Temple, bService = programmes[b].kind == ProgrammeKind::Temple;
    if (aService != bService)
    {
      return !aService;
    }
    auto x = programmes[a].minimumPlotArea, y = programmes[b].minimumPlotArea;
    return x == y ? a < b : x > y;
  });

  struct Match
  {
    uint64_t household, building;
    uint32_t amount;
    double cost;
  };

  bool flowFailed = false;
  auto allocateAll = [&](bool service, std::vector<Match> &matches, std::map<uint64_t, uint32_t> &unmet) {
    std::map<uint64_t, uint64_t> homeNodes;
    for (auto t : state.tenancies)
    {
      auto home = std::find_if(state.buildings.begin(), state.buildings.end(), [&](auto &b) {
        return b.id == t.building;
      });
      homeNodes[t.household] = home->node;
    }
    std::vector<const SettlementHousehold *> rows;
    for (auto &h : state.households)
    {
      if (homeNodes.count(h.id))
      {
        rows.push_back(&h);
      }
    }
    std::vector<const SettlementBuilding *> targets;
    for (auto &b : state.buildings)
    {
      if (service ? b.serviceCapacity : b.interior.workerCapacity)
      {
        targets.push_back(&b);
      }
    }
    std::sort(targets.begin(), targets.end(), [](auto a, auto b) {
      return a->id < b->id;
    });
    if (rows.size() * targets.size() > 500000)
    {
      flowFailed = true;
      return;
    }
    struct Edge
    {
      size_t to, reverse;
      uint32_t capacity;
    };
    size_t offset = 1 + rows.size(), sink = offset + targets.size();
    std::vector<std::vector<Edge>> network(sink + 1);
    auto edge = [&](size_t a, size_t b, uint32_t capacity) {
      size_t index = network[a].size();
      network[a].push_back({b, network[b].size(), capacity});
      network[b].push_back({a, index, 0});
      return index;
    };
    struct Link
    {
      size_t row, target, edge;
      uint32_t capacity;
      double cost;
    };
    std::vector<Link> links;
    for (size_t j = 0; j < targets.size(); ++j)
    {
      edge(offset + j, sink, service ? targets[j]->serviceCapacity : targets[j]->interior.workerCapacity);
    }
    for (size_t i = 0; i < rows.size(); ++i)
    {
      uint32_t count = service ? rows[i]->members : rows[i]->workers;
      edge(0, 1 + i, count);
      unmet[rows[i]->id] = count;
      const auto &costs = distances(homeNodes[rows[i]->id]);
      std::vector<std::pair<double, size_t>> choices;
      for (size_t j = 0; j < targets.size(); ++j)
      {
        if (costs[nodeIndex[targets[j]->node]] <= (service ? in.maximumServiceTravel : in.maximumCommute))
        {
          choices.push_back({costs[nodeIndex[targets[j]->node]], j});
        }
      }
      std::sort(choices.begin(), choices.end());
      for (auto [cost, j] : choices)
      {
        links.push_back({i, j, edge(1 + i, offset + j, count), count, cost});
      }
    }
    // Maximum flow repairs the capacity traps of greedy nearest-facility
    // assignment. Stable near-first edge order is a preference, not a claim
    // of globally minimum aggregate walking distance.
    std::vector<int> level(sink + 1);
    std::vector<size_t> next(sink + 1);
    while (true)
    {
      std::fill(level.begin(), level.end(), -1);
      std::queue<size_t> queue;
      queue.push(0);
      level[0] = 0;
      while (!queue.empty())
      {
        auto i = queue.front();
        queue.pop();
        for (auto e : network[i])
        {
          if (e.capacity && level[e.to] < 0)
          {
            level[e.to] = level[i] + 1;
            queue.push(e.to);
          }
        }
      }
      if (level[sink] < 0)
      {
        break;
      }
      std::fill(next.begin(), next.end(), 0);
      std::function<uint32_t(size_t, uint32_t)> send = [&](size_t i, uint32_t amount) -> uint32_t {
        if (i == sink)
        {
          return amount;
        }
        for (auto &k = next[i]; k < network[i].size(); ++k)
        {
          auto &e = network[i][k];
          if (!e.capacity || level[e.to] != level[i] + 1)
          {
            continue;
          }
          auto pushed = send(e.to, std::min(amount, e.capacity));
          if (pushed)
          {
            e.capacity -= pushed;
            network[e.to][e.reverse].capacity += pushed;
            return pushed;
          }
        }
        return 0;
      };
      while (send(0, UINT32_MAX))
      {
      }
    }
    for (auto link : links)
    {
      auto used = link.capacity - network[1 + link.row][link.edge].capacity;
      if (used)
      {
        matches.push_back({rows[link.row]->id, targets[link.target]->id, used, link.cost});
        unmet[rows[link.row]->id] -= used;
      }
    }
  };
  auto uncovered = [&]() {
    std::vector<Match> matches;
    std::map<uint64_t, uint32_t> unmet, demand;
    allocateAll(true, matches, unmet);
    for (auto t : state.tenancies)
    {
      auto home = std::find_if(state.buildings.begin(), state.buildings.end(), [&](auto &b) {
        return b.id == t.building;
      });
      demand[home->node] += unmet[t.household];
    }
    return demand;
  };
  uint32_t built = 0;
  state.fitAttempts = 0;
  std::set<std::pair<uint64_t, std::string>> failedFits;
  std::map<std::string, uint32_t> fitReasons;
  for (auto name : order)
  {
    auto programme = programmes[name];
    uint32_t wanted = needed[name];
    for (uint32_t k = 0; k < wanted && built < in.maximumNewBuildings; ++k)
    {
      if (residential(programme.kind) && state.housed >= supplied)
      {
        break;
      }
      auto serviceDemand = uncovered();
      uint32_t unserved = 0;
      for (auto [id, n] : serviceDemand)
      {
        unserved += n;
      }
      if (programme.kind == ProgrammeKind::Temple && in.demands.empty() && !unserved)
      {
        needed[name] = 0;
        break;
      }
      std::vector<std::pair<double, const SettlementSite *>> options;
      for (auto &s : sites)
      {
        if (!usedSites.count(s.id) && (programme.kind == ProgrammeKind::Temple || !serviceReserves.count(s.id)) && !failedFits.count({s.id, name}) && s.buildable && s.water && s.slope <= .12 &&
            (s.allowedKinds & (1u << unsigned(programme.kind))) && area(s.shell.shell) >= programme.minimumPlotArea)
        {
          double score = area(s.shell.shell) - programme.minimumPlotArea - (programme.kind == ProgrammeKind::Temple || programme.kind == ProgrammeKind::Civic ? 80 * s.centrality : 0);
          if (programme.kind == ProgrammeKind::Temple)
          {
            uint32_t gain = 0;
            const auto &costs = distances(s.node);
            for (auto [node, count] : serviceDemand)
            {
              if (costs[nodeIndex[node]] <= in.maximumServiceTravel)
              {
                gain += count;
              }
            }
            gain = std::min(gain, programme.serviceCatchmentCapacity);
            if (!gain && in.demands.empty())
            {
              continue;
            }
            score -= gain * 100000.;
          }
          options.push_back({score, &s});
        }
      }
      std::sort(options.begin(), options.end(), [](auto &a, auto &b) {
        return a.first == b.first ? a.second->id < b.second->id : a.first < b.first;
      });
      bool success = false;
      for (auto [cost, site] : options)
      {
        if (state.fitAttempts >= in.maximumFitAttempts)
        {
          break;
        }
        ++state.fitAttempts;
        auto brief = site->shell;
        brief.id = site->id;
        brief.seed = featureSeed(in.seed, 1, in.settlement, site->id);
        brief.use = programme.dwelling;
        brief.floors = programme.floors;
        brief.jettyMetres = site->shell.jettyMetres; // Accepted street-facing projections (A1).
        DwellingPlan dwelling;
        std::string why;
        if (!generateDwellingPlan(brief, dwelling, &why))
        {
          failedFits.insert({site->id, name});
          ++fitReasons[name + ": " + why];
          continue;
        }
        if (in.demands.empty() && residential(programme.kind))
        {
          // Crowding: a household may take a dwelling at up to twice its measured
          // capacity (three square metres of bedroom per person), as poor
          // quarters were lived in; population is a scale, not a target.
          bool fitsHousehold = false;
          for (auto h : state.households)
          {
            if (!assigned.count(h.id) && h.collective == (programme.kind == ProgrammeKind::Collective) && h.members <= std::max(1u, dwelling.residentCapacity) * 2 && state.housed + h.members <= supplied)
            {
              fitsHousehold = true;
            }
          }
          if (!fitsHousehold)
          {
            failedFits.insert({site->id, name});
            ++fitReasons[name + ": no pending household fits measured capacity"];
            continue;
          }
        }
        SettlementBuilding building;
        building.id = site->id;
        building.site = site->id;
        building.node = site->node;
        building.programme = name;
        building.kind = programme.kind;
        building.serviceCapacity = programme.serviceCatchmentCapacity;
        building.interior = std::move(dwelling);
        building.builtYear = state.year;
        state.buildings.push_back(std::move(building));
        usedSites.insert(site->id);
        ++built;
        --needed[name];
        success = true;
        occupy();
        break;
      }
      if (!success)
      {
        break;
      }
    }
    if (programme.kind == ProgrammeKind::Temple && in.demands.empty())
    {
      auto missing = uncovered();
      bool any = false;
      for (auto [id, n] : missing)
      {
        any |= n > 0;
      }
      needed[name] = any ? 1 : 0;
    }
    if (in.demands.empty() && programme.kind == ProgrammeKind::Shop && needed[name])
    {
      for (auto &p : catalogue)
      {
        if (p.kind == ProgrammeKind::Home)
        {
          needed[p.id] += needed[name];
          break;
        }
      }
    }
    if (needed[name])
    {
      state.unmetProgrammes.push_back({name, needed[name]});
    }
  }
  std::sort(state.buildings.begin(), state.buildings.end(), [](auto &a, auto &b) {
    return a.id < b.id;
  });
  existing.clear();
  for (auto &b : state.buildings)
  {
    existing[b.id] = &b;
  }
  std::vector<Match> workMatches, serviceMatches;
  std::map<uint64_t, uint32_t> unemployed, unserved;
  allocateAll(false, workMatches, unemployed);
  allocateAll(true, serviceMatches, unserved);
  if (flowFailed)
  {
    return fail("household/facility matching exceeds bounded network budget");
  }
  for (auto m : workMatches)
  {
    state.employment.push_back({m.household, m.building, m.amount, m.cost});
    state.employed += m.amount;
  }
  for (auto m : serviceMatches)
  {
    state.services.push_back({m.household, m.building, m.amount, m.cost});
  }
  state.unserved = state.population - state.housed;
  for (auto [id, n] : unserved)
  {
    state.unserved += n;
  }
  state.unhoused = state.population - state.housed;
  for (auto [reason, count] : fitReasons)
  {
    state.rejections.push_back(std::to_string(count) + " rejected sites: " + reason);
  }
  if (state.fitAttempts >= in.maximumFitAttempts)
  {
    state.rejections.push_back("interior fit attempt budget exhausted");
  }
  std::sort(state.tenancies.begin(), state.tenancies.end(), [](auto &a, auto &b) {
    return a.household < b.household;
  });
  const uint32_t thresholds[] = {100, 500, 2000, 10000};
  unsigned size = unsigned(state.sizeClass);
  if (size > 4)
  {
    return fail("invalid retained size class");
  }
  if (state.revision == 1)
  {
    size = 0;
    while (size < 4 && state.population >= thresholds[size])
    {
      ++size;
    }
  }
  else
  {
    while (size > 0 && state.population < thresholds[size - 1] * .85)
    {
      --size;
    }
    if (!state.unhoused && !state.unserved && !state.foodShortfall && !state.waterShortfall)
    {
      while (size < 4 && state.population >= thresholds[size] * 1.05)
      {
        ++size;
      }
    }
  }
  state.sizeClass = SettlementSizeClass(size);
  if (error)
  {
    error->clear();
  }
  return true;
}
} // namespace

bool generateSettlementPopulation(const PopulationBrief &b, std::vector<SettlementHousehold> &out, std::string *error)
{
  if (b.minimum > b.maximum || b.maximum > 100000)
  {
    if (error)
    {
      *error = "invalid population range";
    }
    return false;
  }
  uint32_t remaining = b.minimum + featureSeed(b.seed, 1, 1, 1) % (uint64_t(b.maximum) - b.minimum + 1);
  std::vector<SettlementHousehold> h;
  uint64_t next = 1;
  while (remaining)
  {
    auto draw = featureSeed(b.seed, 1, next, 2);
    bool collective = draw % 5 == 0;
    uint32_t members = std::min(remaining, collective ? 1u : 2u + uint32_t(draw % 5));
    uint64_t id = featureSeed(b.seed, 1, next++, 3);
    if (!id)
    {
      id = next;
    }
    h.push_back({id, members, std::min(members, collective ? 1u : std::max(1u, members / 2)), collective});
    remaining -= members;
  }
  std::sort(h.begin(), h.end(), [](auto &a, auto &b) {
    return a.id < b.id;
  });
  for (size_t i = 1; i < h.size(); ++i)
  {
    if (h[i].id == h[i - 1].id)
    {
      if (error)
      {
        *error = "household identity collision";
      }
      return false;
    }
  }
  if (error)
  {
    error->clear();
  }
  out = std::move(h);
  return true;
}

std::vector<SettlementProgramme> settlementProgrammeCatalogue()
{
  return {
    {"ordinary_house", ProgrammeKind::Home, DwellingUse::Cottage, 1, 50, 0},
    {"general_shop_home", ProgrammeKind::Shop, DwellingUse::ShopHouse, 2, 100, 0},
    {"worker_lodging_hall", ProgrammeKind::Collective, DwellingUse::Lodging, 1, 240, 0},
    {"community_temple", ProgrammeKind::Temple, DwellingUse::Hall, 1, 120, 300},
    {"town_hall", ProgrammeKind::Civic, DwellingUse::Hall, 1, 120, 0},
    {"warehouse", ProgrammeKind::Warehouse, DwellingUse::Warehouse, 1, 100, 0}};
}

bool planSettlement(const SettlementDirectorInput &in, SettlementState &out, std::string *error)
{
  SettlementState s;
  s.settlement = in.settlement;
  s.revision = 1;
  s.year = in.year;
  auto populationBrief = in.population;
  populationBrief.seed = featureSeed(in.population.seed, 1, in.settlement, 1);
  if (in.historicalHouseholds) s.households = *in.historicalHouseholds;
  else if (!generateSettlementPopulation(populationBrief, s.households, error)) return false;
  if (!finish(in, s, error))
  {
    return false;
  }
  s.history.push_back({1, s.year, s.population, 0, uint32_t(s.buildings.size())});
  out = std::move(s);
  return true;
}

bool advanceSettlement(const SettlementDirectorInput &in, const SettlementState &old, const SettlementGrowth &g, SettlementState &out, std::string *error)
{
  auto fail = [&](const char *s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (!g.event || old.settlement != in.settlement || old.revision != g.expectedRevision || g.year < old.year || old.history.size() >= 1024)
  {
    return fail("invalid growth revision or date");
  }
  for (auto e : old.history)
  {
    if (e.id == g.event)
    {
      return fail("growth event already applied");
    }
  }
  SettlementState s = old;
  std::set<uint64_t> depart(g.departingHouseholds.begin(), g.departingHouseholds.end());
  if (depart.size() != g.departingHouseholds.size())
  {
    return fail("duplicate departing household");
  }
  uint32_t departures = 0;
  for (auto h : s.households)
  {
    if (depart.erase(h.id))
    {
      departures += h.members;
    }
  }
  if (!depart.empty())
  {
    return fail("unknown departing household");
  }
  std::set<uint64_t> remove(g.departingHouseholds.begin(), g.departingHouseholds.end());
  s.households.erase(
      std::remove_if(
          s.households.begin(),
          s.households.end(),
          [&](auto h) {
            return remove.count(h.id);
          }),
      s.households.end());
  auto arrivalBrief = g.arrivals;
  arrivalBrief.seed = featureSeed(arrivalBrief.seed, 1, in.settlement, g.event);
  std::vector<SettlementHousehold> arrivals;
  if (!generateSettlementPopulation(arrivalBrief, arrivals, error))
  {
    return false;
  }
  uint32_t arrived = 0;
  for (auto h : arrivals)
  {
    arrived += h.members;
    s.households.push_back(h);
  }
  size_t before = s.buildings.size();
  s.year = g.year;
  ++s.revision;
  if (!finish(in, s, error))
  {
    return false;
  }
  s.history.push_back({g.event, g.year, arrived, departures, uint32_t(s.buildings.size() - before)});
  out = std::move(s);
  return true;
}
} // namespace procedural::world::rewrite
