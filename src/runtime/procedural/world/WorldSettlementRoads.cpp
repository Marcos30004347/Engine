#include "WorldSettlementRoads.hpp"
#include <cmath>
#include <limits>
namespace procedural::world::rewrite
{
bool buildSettlementRoads(
    const WorldSuitability &suitability,
    const SettlementProduct &settlements,
    const WorldPhysicalTerrain &physical,
    const WorldRiverChannels &channels,
    const RoadSettings &settings,
    RoadNetworkProduct &output,
    std::string *error)
{
  if (!std::isfinite(settings.halfWidthMetres) || !std::isfinite(settings.shoulderMetres) || settings.halfWidthMetres < 1 || settings.halfWidthMetres > 20 || settings.shoulderMetres <= 0 || settings.shoulderMetres > 10)
  {
    if (error)
      *error = "invalid road anchor width";
    return false;
  }
  if (!settlements.matches(suitability))
  {
    if (error)
      *error = "stale settlement road dependencies";
    return false;
  }
  if (!channels.validateReservations(physical, error))
    return false;
  double forbidden = std::numeric_limits<double>::infinity();
  auto base = [&](double x, double z)
  {
    PhysicalSample p;
    return physical.sample(x, z, p) ? p.height : forbidden;
  };
  auto terrain = [&](double x, double z)
  {
    auto index = suitability.cellAt(x, z);
    if (index == SIZE_MAX)
      return forbidden;
    if (suitability.cells()[index].reasons & (Wet | Flood))
      return forbidden;
    PhysicalSample p;
    ChannelSample water;
    if (!physical.sample(x, z, p) || p.protectedRiver || (!p.rampId && (p.region == SplineRegion::FreeFace || p.region == SplineRegion::WeatheredFace)))
      return forbidden;
    if (!channels.sample(x, z, base, water) || water.wet)
      return forbidden;
    return water.terrainHeight;
  };
  auto resistance = [&](double x, double z)
  {
    auto index = suitability.cellAt(x, z);
    if (index == SIZE_MAX)
      return forbidden;
    const auto &c = suitability.cells()[index];
    return c.slope * c.slope + (c.reasons & Talus ? 2. : 0.);
  };
  std::vector<RoadAnchor> anchors;
  for (auto &s : settlements.reservations)
  {
    Position anchor{};
    double best = forbidden;
    // A reservation seed can lie at the map edge. Choose a supported point within
    // its actual core, retaining the original seed whenever its road width fits.
    for (auto index : s.core)
    {
      if (index >= suitability.cells().size())
      {
        if (error)
          *error = "invalid settlement core cell";
        return false;
      }
      auto p = suitability.cells()[index].center;
      double distance = std::hypot(p.x - s.center.x, p.z - s.center.z);
      if (distance >= best)
        continue;
      bool supported = true;
      double radius = settings.halfWidthMetres + settings.shoulderMetres;
      int samples = int(std::ceil(radius));
      for (int x = -samples; x <= samples && supported; ++x)
        for (int z = -samples; z <= samples; ++z)
          if (x * x + z * z <= radius * radius && !std::isfinite(terrain(p.x + x, p.z + z)))
          {
            supported = false;
            break;
          }
      if (supported)
      {
        p.y = terrain(p.x, p.z);
        if (std::isfinite(p.y))
        {
          anchor = p;
          best = distance;
        }
      }
    }
    if (!std::isfinite(best))
    {
      if (error)
        *error = "settlement has no supported road anchor";
      return false;
    }
    anchors.push_back({s.id, s.parentTown, anchor, s.town});
  }
  return buildRoadNetwork(anchors, terrain, resistance, settings, suitability.fingerprint(), output, error);
}
} // namespace procedural::world::rewrite
