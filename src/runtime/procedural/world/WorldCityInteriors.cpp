#include "WorldCityInteriors.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace procedural::world::rewrite {
bool compileCityInteriors(const CityLayout &city, const std::vector<CityInteriorRequest> &requests, uint64_t seed, CityInteriors &out, std::string *error)
{
  auto fail = [&](const char *s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (!city.revision || requests.size() > 10000)
  {
    return fail("invalid city interior dependency/budget");
  }
  std::map<uint64_t, const CityObject *> objects;
  for (auto &o : city.objects)
  {
    if (!o.id || !objects.emplace(o.id, &o).second)
    {
      return fail("invalid city object identity");
    }
  }
  auto sorted = requests;
  std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) {
    return a.object < b.object;
  });
  CityInteriors result;
  result.layoutRevision = city.revision;
  uint64_t previous = 0;
  for (auto request : sorted)
  {
    if (!request.object || request.object == previous || !objects.count(request.object) || unsigned(request.use) > unsigned(DwellingUse::Tavern) || request.floors < 1 || request.floors > 2)
    {
      return fail("invalid city interior request");
    }
    previous = request.object;
    auto &o = *objects[request.object];
    if (o.parts.size() != 1 || !o.courtyard.empty())
    {
      result.unresolved.push_back({o.id, "multi-wing/courtyard connectors require compound solver"});
      continue;
    }
    if (o.kind != CityObjectKind::House && o.kind != CityObjectKind::Workshop)
    {
      result.unresolved.push_back({o.id, "object is not an occupied building shell"});
      continue;
    }
    DwellingBrief brief;
    brief.id = o.id;
    brief.seed = featureSeed(seed, 1, o.id, 1);
    brief.use = request.use;
    brief.floors = request.floors;
    brief.shell = o.parts.front().footprint;
    brief.foundation = o.foundation;
    brief.availableHeight = o.parts.front().height;
    brief.floorRise = std::min(2.8, brief.availableHeight / request.floors);
    brief.entrance = o.entrance;
    DwellingPlan plan;
    std::string why;
    if (!generateDwellingPlan(brief, plan, &why))
    {
      result.unresolved.push_back({o.id, why});
      continue;
    }
    result.dwellings.push_back(std::move(plan));
  }
  if (error)
  {
    error->clear();
  }
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
