#include "procedural/geometry/TerrainSettlements.hpp"
#include <cmath>
#include <iostream>
using namespace procedural;
using namespace procedural::geometry;
int main()
{
  int failures = 0;
  const auto check = [&](bool value, const char *label)
  { std::cout << (value ? "PASS " : "FAIL ") << label << '\n'; failures += !value; };

  // A synthetic world: a coastal plain rising to a ridge in the east, with one
  // river running west across it. Everything the siting likes is arranged so we
  // know where the answer should be.
  const double extent = 60000.0;
  const double ridgeX = 46000.0, riverY = 30000.0;
  const auto elevation = [&](Point2 p)
  {
    // Gentle coastal plain, then a genuinely steep wall so the slope gate is
    // actually exercised - a quadratic over 34 km never exceeds 0.06.
    const double plain = std::max(0.0, p.x - 12000.0) / 34000.0;
    const double wall = std::max(0.0, p.x - 38000.0) / 1200.0;
    return -40.0 + 260.0 * plain * plain + 700.0 * wall * wall;
  };
  SettlementFields fields;
  fields.elevationMetres = elevation;
  fields.slope = [&](Point2 p)
  { const double d = 40.0; return std::abs(elevation({p.x + d, p.y}) - elevation({p.x - d, p.y})) / (2 * d); };
  fields.waterType = [&](Point2 p) -> uint8_t { return elevation(p) < 0.0 ? 1u : 0u; };
  fields.waterLevelMetres = [](Point2) { return 0.0; };
  fields.riverDistanceMetres = [&](Point2 p) { return std::abs(p.y - riverY); };
  fields.riverDischarge = [&](Point2 p) { return std::abs(p.y - riverY) < 6000.0 ? 40.0 : 0.0; };
  fields.coastDistanceMetres = [&](Point2 p) { return p.x - 12000.0; };
  fields.treeCover = [&](Point2) { return .6; };
  fields.soilQuality = [&](Point2) { return .6; };
  fields.temperatureCelsius = [](Point2) { return 12.0; };
  fields.moisture = [](Point2) { return .6; };

  SettlementSettings settings;
  settings.resolution = 192u;
  settings.seed = 5;
  settings.tiers[0] = {24000.0, 2u, .30};
  settings.tiers[1] = {9000.0, 10u, .22};
  settings.tiers[2] = {3000.0, 60u, .14};
  const SettlementNetwork network = planSettlements(extent, fields, settings);

  check(!network.sites.empty(), "places settlements");
  bool onLand = true, insideWorld = true, aboveWater = true;
  for (const auto &s : network.sites)
  {
    onLand = onLand && fields.waterType(s.position) == 0u;
    insideWorld = insideWorld && s.position.x >= 0 && s.position.y >= 0 &&
        s.position.x <= extent && s.position.y <= extent;
    aboveWater = aboveWater && elevation(s.position) >= settings.minimumFreeboardMetres;
  }
  check(onLand, "never sites a settlement in water");
  check(insideWorld, "every site is inside the world");
  check(aboveWater, "every site clears the flood freeboard");

  bool gentle = true;
  for (const auto &s : network.sites) gentle = gentle && fields.slope(s.position) <= settings.maximumSlope;
  check(gentle, "never sites a settlement on ground steeper than the gate");

  // Exclusion: a site clears its own tier's radius against everything placed
  // before it, which is what lets villages be satellites of a city rather than
  // being pushed a city-radius away from it. Sites are stored in placement
  // order and tiers run largest first, so for a < b the binding radius is b's.
  bool spaced = true;
  for (size_t a = 0; a < network.sites.size(); ++a)
    for (size_t b = a + 1; b < network.sites.size(); ++b)
    {
      const double radius = settings.tiers[uint32_t(network.sites[b].tier)].exclusionRadiusMetres;
      spaced = spaced && distance(network.sites[a].position, network.sites[b].position) >= radius - 1e-6;
    }
  check(spaced, "each site clears its own tier radius against earlier ones");

  // And the point of the rule: a village is allowed to be a satellite.
  bool satellite = false;
  for (const auto &v : network.sites)
  {
    if (v.tier == SettlementTier::City) continue;
    for (const auto &c : network.sites)
      if (c.tier == SettlementTier::City &&
          distance(v.position, c.position) < settings.tiers[0].exclusionRadiusMetres)
        satellite = true;
  }
  check(satellite, "smaller settlements may sit inside a city's exclusion radius");

  // The river and the coast are the only water, so sites should hug one.
  bool watered = true;
  for (const auto &s : network.sites)
    watered = watered && (std::abs(s.position.y - riverY) < 6000.0 || (s.position.x - 12000.0) < 6000.0);
  check(watered, "sites gather on the river or the coast");

  // Nothing should climb the ridge: it is steep and far from water.
  bool offRidge = true;
  for (const auto &s : network.sites) offRidge = offRidge && s.position.x < ridgeX;
  check(offRidge, "nothing is sited up the steep ridge");
  (void)ridgeX;

  check(!network.roads.empty(), "connects the settlements with roads");
  bool joined = true;
  for (const auto &r : network.roads)
    joined = joined && r.path.size() > 1u && r.lengthMetres > 0 &&
        distance(r.path.front(), network.sites[r.from].position) < 2000.0 &&
        distance(r.path.back(), network.sites[r.to].position) < 2000.0;
  check(joined, "each road runs between the two sites it names");

  // Determinism.
  const SettlementNetwork again = planSettlements(extent, fields, settings);
  bool identical = again.sites.size() == network.sites.size();
  for (size_t i = 0; identical && i < network.sites.size(); ++i)
    identical = again.sites[i].position.x == network.sites[i].position.x &&
        again.sites[i].position.y == network.sites[i].position.y &&
        again.sites[i].tier == network.sites[i].tier;
  check(identical, "two runs of the same seed agree exactly");

  SettlementSettings off = settings; off.enabled = false;
  check(planSettlements(extent, fields, off).sites.empty(), "disabled places nothing");

  uint32_t cities = 0, towns = 0, villages = 0;
  for (const auto &s : network.sites)
  { cities += s.tier == SettlementTier::City; towns += s.tier == SettlementTier::Town; villages += s.tier == SettlementTier::Village; }
  std::cout << "  " << cities << " cities, " << towns << " towns, " << villages
            << " villages, " << network.roads.size() << " roads\n";
  check(cities <= settings.tiers[0].maximumCount && towns <= settings.tiers[1].maximumCount, "tier caps hold");
  check(villages > towns && towns >= cities, "the settlement hierarchy widens downward");

  std::cout << (failures ? "FAILED" : "OK") << " settlement siting\n";
  return failures ? 1 : 0;
}
