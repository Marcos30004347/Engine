#include "WorldDomainGeneratorAdapters.hpp"
#include "WorldNativeGenerator.hpp"
namespace procedural::world::rewrite
{
GenerationTask cityMasterPlanGenerator(uint64_t d)
{
  return nativeGenerator<CityPlanGenerationBrief, CityMasterPlan>(
      d,
      "city.master-plan",
      "city.plan-brief",
      "city.master-plan",
      GenerationScope::Neighbourhood,
      [](auto &b, auto &o, auto *e)
      {
        return generateCityMasterPlan(b.cells, b.districts, b.gates, b.settings, o, e);
      },
      true);
}
GenerationTask cityLayoutGenerator(uint64_t d)
{
  return nativeGenerator<CityLayoutGenerationBrief, CityLayout>(
      d,
      "city.layout",
      "city.layout-brief",
      "city.layout",
      GenerationScope::Neighbourhood,
      [](auto &b, auto &o, auto *e)
      {
        return generateCityLayout(b.polygon, b.reservations, b.arteries, b.gates, b.settings, b.terrain, o, e);
      });
}
GenerationTask ruralPatchesGenerator(uint64_t d)
{
  return nativeGenerator<RuralGenerationBrief, RuralPatchProduct>(
      d,
      "rural.patches",
      "rural.brief",
      "rural.patches",
      GenerationScope::Neighbourhood,
      [](auto &b, auto &o, auto *e)
      {
        return growRuralPatches(b.suitability, b.reservations, b.sources, b.settings, o, e);
      });
}
GenerationTask cityTerritoryGenerator(uint64_t d)
{
  return nativeGenerator<TerritoryGenerationBrief, CityTerritory>(
      d,
      "city.territory",
      "city.territory-brief",
      "city.territory",
      GenerationScope::Neighbourhood,
      [](auto &b, auto &o, auto *e)
      {
        return generateCityTerritory(b.input, b.suitability, b.settings, b.terrain, o, e);
      });
}
GenerationTask settlementProgrammeGenerator(uint64_t d)
{
  return nativeGenerator<SettlementDirectorInput, SettlementState>(d, "settlement.programmes", "settlement.brief", "settlement.state", GenerationScope::Network, planSettlement, true);
}
GenerationTask runtimeWorldGenerator(uint64_t d)
{
  return nativeGenerator<RuntimeBuildInput, RuntimeBuildResult>(
      d,
      "runtime.world",
      "runtime.brief",
      "runtime.candidate",
      GenerationScope::World,
      [](auto &b, auto &o, auto *e)
      {
        return buildRuntimeWorld(b, nullptr, o, e);
      });
}

GenerationTask landPartitionGenerator(uint64_t d)
{
  return nativeGenerator<LandGenerationBrief, LandProduct>(
      d,
      "land.partition",
      "land.brief",
      "land.partition",
      GenerationScope::World,
      [](auto &b, auto &o, auto *e)
      {
        return generateLand(b.spec, b.spacing, b.cores, b.landFraction, o, e, b.oceanMargin);
      },
      true);
}
GenerationTask coastDetailGenerator(uint64_t d)
{
  return nativeGenerator<CoastGenerationBrief, LandProduct>(
      d,
      "coast.detail",
      "coast.brief",
      "coast.detail",
      GenerationScope::Neighbourhood,
      [](auto &b, auto &o, auto *e)
      {
        return detailLandCoasts(b.land, b.settings, o, e);
      },
      true);
}
GenerationTask riverNetworkGenerator(uint64_t d)
{
  return nativeGenerator<RiverGenerationBrief, RiverGrowthProduct>(
      d,
      "river.network",
      "river.brief",
      "river.network",
      GenerationScope::Watershed,
      [](auto &b, auto &o, auto *e)
      {
        return growRiverGraph(b.land, b.relief, b.mouths, b.settings, o, e);
      },
      true);
}
GenerationTask watershedGenerator(uint64_t d)
{
  return nativeGenerator<WatershedGenerationBrief, WatershedProduct>(
      d,
      "river.watersheds",
      "watershed.brief",
      "river.watersheds",
      GenerationScope::Watershed,
      [](auto &b, auto &o, auto *e)
      {
        return buildWatersheds(b.land, b.rivers, b.relief, b.allowTruncated, o, e);
      },
      true);
}
GenerationTask channelGenerator(uint64_t d)
{
  return nativeGenerator<ChannelGenerationBrief, WorldRiverChannels>(
      d,
      "river.channels",
      "channel.brief",
      "river.channels",
      GenerationScope::Watershed,
      [](auto &b, auto &o, auto *e)
      {
        return o.buildFromWatersheds(b.rivers, b.watersheds, b.rainfall, b.runoff, b.settings, b.allowTruncated, e);
      },
      true);
}
GenerationTask roadNetworkGenerator(uint64_t d)
{
  return nativeGenerator<RoadGenerationBrief, RoadNetworkProduct>(
      d,
      "road.network",
      "road.brief",
      "road.network",
      GenerationScope::Network,
      [](auto &b, auto &o, auto *e)
      {
        return buildRoadNetwork(b.anchors, b.terrain, b.resistance, b.settings, b.revision, o, e);
      });
}
GenerationTask roadEarthworkGenerator(uint64_t d)
{
  return nativeGenerator<EarthworkGenerationBrief, RoadEarthworkProduct>(
      d,
      "road.earthworks",
      "earthwork.brief",
      "road.earthworks",
      GenerationScope::Network,
      [](auto &b, auto &o, auto *e)
      {
        return generateRoadEarthworks(b.roads, b.terrain, b.water, b.contours, b.bridges, b.settings, o, e);
      });
}
GenerationTask cityDemandGenerator(uint64_t d)
{
  return nativeGenerator<CityDemandGenerationBrief, CityPopulationDemand>(
      d,
      "city.demand",
      "city.demand-brief",
      "city.demand",
      GenerationScope::Neighbourhood,
      [](auto &b, auto &o, auto *e)
      {
        return deriveCityPopulationDemand(b.households, b.settings, o, e);
      },
      true);
}
} // namespace procedural::world::rewrite
