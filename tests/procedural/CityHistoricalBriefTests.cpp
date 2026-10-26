#include "procedural/world/WorldCityHistoricalLandBudget.hpp"
#include "procedural/world/WorldCityHistoricalBrief.hpp"
#include "procedural/world/WorldCityHistoricalPlacement.hpp"
#include "procedural/world/WorldCityScale.hpp"
#include <iostream>
using namespace procedural::world::rewrite;

int main()
{
  int failed = 0;
  auto check = [&](bool ok, const char *name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    failed += !ok;
  };
  CityScaleSettings settings;
  settings.mode = CityScaleMode::Village;
  settings.seed = 41;
  CitySocietyDriver society(cityScaleSociety(settings));
  CityChronicle chronicle(cityScalePolicies(settings), cityScaleScript(settings, society.current()));
  std::string error;
  check(chronicle.observe(society.current(), &error), "founding initializes");
  HistoricalCityBrief brief;
  check(historicalCityBrief(chronicle.state(), 100, brief, &error), "snapshot projects");
  check(brief.demand.population == brief.citizens.size() && !brief.citizens.empty(), "exact living population demand");
  std::set<HistoryNode> people;
  for (const auto &p : brief.citizens)
  {
    people.insert(p.id);
  }
  check(people.size() == brief.citizens.size(), "no duplicate residents");
  const auto hash = chronicle.stateHash();
  HistoricalCityBrief again;
  check(historicalCityBrief(chronicle.state(), 100, again, &error) && hash == chronicle.stateHash(), "read does not mutate history");
  {
    // A building with a workplace names the trade worked there, so the
    // dwelling programme can give it its room; a shop, an inn or a market
    // is its own programme and names none.
    bool named = true;
    uint32_t traded = 0;
    for (const auto &b : brief.buildings)
    {
      if (!b.workplace)
      {
        named = named && b.trade.empty();
        continue;
      }
      const bool ownProgramme = b.use == CityBuildingUse::Shop || b.use == CityBuildingUse::Inn || b.use == CityBuildingUse::Market || b.use == CityBuildingUse::Church || b.use == CityBuildingUse::Keep || b.use == CityBuildingUse::Manor;
      named = named && (ownProgramme || !b.trade.empty() || b.use == CityBuildingUse::Home || b.use == CityBuildingUse::Workshop);
      traded += b.trade.empty() ? 0 : 1;
    }
    check(named, "a building with a workplace carries its trade word");
    std::cout << "  " << traded << " of " << brief.buildings.size() << " buildings name a trade\n";
  }
  check(again.buildings.size() == brief.buildings.size() && again.demand.programmeLandArea == brief.demand.programmeLandArea, "repeated projection deterministic");
  auto midnight = historicalDailyActivities(brief, 0);
  check(midnight.size() == brief.citizens.size(), "one activity per resident");
  bool rest = true;
  for (const auto &a : midnight)
  {
    rest &= a.activity == "sleep" && !a.room;
  }
  check(rest, "sleep without invented room references");
  HistoricalCityBrief synthetic;
  HistoricalCitizenBrief worker;
  worker.id = 0;
  worker.household = 1;
  worker.home = 4;
  worker.workplace = 9;
  worker.workBuilding = 8;
  synthetic.citizens.push_back(worker);
  auto noon = historicalDailyActivities(synthetic, 720);
  check(noon[0].person == 0 && noon[0].activity == "work" && noon[0].building == 8, "node zero and workplace routing");
  synthetic.citizens[0].workBuilding.reset();
  check(!historicalDailyActivities(synthetic, 720)[0].building, "unbuilt workplace stays unresolved");
  synthetic.citizens[0].apprentice = true;
  check(historicalDailyActivities(synthetic, 720)[0].activity == "apprenticeship", "explicit apprentice activity");
  bool threw = false;
  try
  {
    historicalDailyActivities(brief, 1440);
  }
  catch (const std::invalid_argument &)
  {
    threw = true;
  }
  check(threw, "invalid time rejected");
  CityState empty;
  again.settlement = 99;
  check(!historicalCityBrief(empty, 100, again, &error) && again.settlement == 99, "invalid input leaves output unchanged");
  CityPopulationDemand large;
  check(deriveCityPopulationDemand({{42, 12, 5, false}}, {}, large, &error) && large.population == 12 && large.households == 1, "extended family stays one household");
  check(!deriveCityPopulationDemand({{42, 100000, 0, false}, {43, 1, 0, false}}, {}, large, &error), "total population budget still enforced");
  SettlementDirectorInput input;
  input.settlement = 100;
  input.catalogue = settlementProgrammeCatalogue();
  input.historicalHouseholds = std::vector<SettlementHousehold>{{42, 12, 5, false}};
  input.population = {99, 80, 80};
  SettlementState city;
  check(planSettlement(input, city, &error) && city.population == 12 && city.households.front().id == 42, "director consumes authoritative identities");
  input.historicalHouseholds = std::vector<SettlementHousehold>{};
  check(planSettlement(input, city, &error) && city.population == 0, "empty historical population never fabricates residents");
  CityCompletionPlan layout;
  for (uint64_t id = 1; id <= 3; ++id)
  {
    CitySkeletonRegion r;
    r.id = id;
    r.kind = CityRegionKind::Lot;
    const double x = double(id) * 20;
    r.boundary = {{x, 0, 0}, {x + 10, 0, 0}, {x + 10, 0, 10}, {x, 0, 10}};
    layout.skeleton.regions.push_back(r);
    CityLotDevelopment lot;
    lot.lot = id;
    layout.lots.push_back(lot);
  }
  const auto freshLayout = layout;
  std::vector<SettlementHistoricalBuilding> requests = {{9, "home", 30, 2, 6}, {7, "shop", 10, 1, 2}};
  check(allocateHistoricalCityLots(requests, layout, &error), "historical records allocate to fresh lots");
  check(
      std::count_if(
          layout.lots.begin(),
          layout.lots.end(),
          [](const auto &l) {
            return l.developed;
          }) == 2,
      "unused capacity never becomes an extra building");
  check(layout.historicalLots.front().building == 7 && layout.historicalLots.front().lot == 2, "oldest construction occupies central core first");
  auto wedge=freshLayout;
  wedge.skeleton.regions[1].boundary={{40,0,0},{50,0,0},{40,0,10}};
  check(allocateHistoricalCityLots(requests,wedge,&error) &&
        std::none_of(wedge.historicalLots.begin(),wedge.historicalLots.end(),[](const auto &b){return b.lot==2;}),
        "modular footprints take priority over a bounding-box wedge within a block");
  auto modular=freshLayout;
  modular.skeleton.regions[1].boundary={{40,0,0},{46.6,0,0},{46.6,0,6.6},{40,0,6.6}};
  modular.lots[0].developed=false;modular.lots[2].developed=false;
  check(allocateHistoricalCityLots({requests.front()},modular,&error) && modular.historicalLots[0].lot==2,
        "exact four-module plot with setbacks is used instead of discarded by a 50m2 cutoff");
  auto hillside=freshLayout;
  const auto slope=[](double x,double){return x>=40 && x<=50 ? (x-40)*.6 : 0.;};
  check(allocateHistoricalCityLots({requests.front()},hillside,&error,nullptr,slope) && hillside.historicalLots[0].lot!=2,
        "actual sampled terrain prefers level lots over the central hillside");
  auto permuted = freshLayout;
  auto reversed = requests;
  std::reverse(reversed.begin(), reversed.end());
  check(
      allocateHistoricalCityLots(reversed, permuted, &error) && permuted.historicalLots.front().building == layout.historicalLots.front().building &&
          permuted.historicalLots.front().lot == layout.historicalLots.front().lot,
      "construction input order does not change placement");
  auto compact=freshLayout;
  compact.skeleton.regions[0].parent=11;
  compact.skeleton.regions[1].parent=11;
  compact.skeleton.regions[2].parent=22;
  check(allocateHistoricalCityLots(requests,compact,&error) &&
        compact.historicalLots[0].lot==2 && compact.historicalLots[1].lot==1,
        "fill the central block before opening a second block");
  auto outward=freshLayout;
  for(size_t i=0;i<outward.skeleton.regions.size();++i)
    outward.skeleton.regions[i].parent=100+i;
  // A very expensive near block must not be skipped for a cheap satellite.
  CitySkeletonRegion nearBlock;nearBlock.id=100;nearBlock.kind=CityRegionKind::Block;
  nearBlock.boundary={{0,0,-30},{50,0,-30},{50,0,30},{0,0,30}};
  outward.skeleton.regions.push_back(nearBlock);
  const Position civic{0,0,0};
  check(allocateHistoricalCityLots(requests,outward,&error,nullptr,{},&civic) &&
        outward.historicalLots.front().lot==1 && outward.historicalLots.back().lot==2,
        "fixed civic centre grows through nearer blocks before cheaper satellite districts");
  auto spanning=freshLayout;
  spanning.skeleton.regions[0].parent=100;
  spanning.skeleton.regions[1].parent=200;
  spanning.skeleton.regions[2].parent=100;
  check(allocateHistoricalCityLots(requests,spanning,&error,nullptr,{},&civic) &&
        spanning.historicalLots[0].lot==1 && spanning.historicalLots[1].lot==2,
        "nearer infill wins over the far end of an already opened block");
  auto frontage=freshLayout;
  frontage.routes.push_back({1,CityRoadKind::Street,{},{{15,0,-2},{35,0,-2}},2});
  check(allocateHistoricalCityLots(requests,frontage,&error) && frontage.historicalLots.front().lot==1,
        "existing street frontage is occupied before central backland in the same block");
  auto reservation=freshLayout;
  for(auto &p:reservation.skeleton.regions[2].boundary)p.x-=7.;
  std::vector<SettlementHistoricalEnclosure> defence={{1,1,20,true,false,false,false,{7}}};
  auto unreserved=reservation;
  check(allocateHistoricalCityLots(requests,unreserved,&error) && unreserved.historicalLots.back().lot==3,
        "without a historical corridor the closest vacant plot is eligible");
  check(allocateHistoricalCityLots(requests,reservation,&error,&defence) && reservation.historicalLots.front().lot==2 && reservation.historicalLots.back().lot==1,
        "later houses skip an early wall corridor while retaining the older household lot");
  auto timberReservation=unreserved;
  defence.front().palisade=true;
  check(allocateHistoricalCityLots(requests,timberReservation,&error,&defence) &&
        timberReservation.historicalLots.back().lot==unreserved.historicalLots.back().lot,
        "removed timber enclosures do not reserve invisible clearance corridors");
  const auto occupied=std::find_if(layout.lots.begin(),layout.lots.end(),[](const auto &l){return l.developed;});
  check(occupied!=layout.lots.end() && occupied->preferredFootprintMetres2>40 &&
        occupied->preferredFootprintMetres2<=120 && layout.historicalLots.front().requiredFootprintArea==40,
        "urban packing fills its parcel without changing historical programme demand");
  auto farmPacking=freshLayout;
  check(allocateHistoricalCityLots({{77,"farm",0,1,2}},farmPacking,&error) &&
        std::all_of(farmPacking.lots.begin(),farmPacking.lots.end(),[](const auto &l){
          return !l.developed || (!l.rowHouse && l.preferredFootprintMetres2==40);
        }), "farm yards retain their programme instead of receiving urban infill");
  auto mixedGrowth=freshLayout;
  check(allocateHistoricalCityLots({{77,"farm",0,1,2},{78,"home",10,1,4}},mixedGrowth,&error,nullptr,{},&civic) &&
        mixedGrowth.historicalLots[0].building==77 && mixedGrowth.historicalLots[0].lot==2 &&
        mixedGrowth.historicalLots[1].building==78 && mixedGrowth.historicalLots[1].lot==1,
        "early farm uses the demand-sized outer edge while urban infill takes the civic centre");
  const auto land=historicalLandBudget({{1,"home",0,1,4},{2,"home",0,2,4},{3,"workshop",0,1,0},{4,"keep",0,1,0}});
  check(land.uses.at("home").buildings==2 && land.uses.at("workshop").footprintArea>=100 &&
        land.uses.at("keep").districtArea>=16000 && land.ordinaryDistrictArea>land.uses.at("home").footprintArea,
        "historical land budgets reserve modular buildings and circulation by actual use");
  const auto estates=historicalLandBudget({{1,"manor",0,2,8},{2,"keep",0,3,8}});
  check(estates.uses.at("manor").districtArea>=16000 && estates.uses.at("manor").districtArea<estates.uses.at("keep").districtArea,
        "manor reserves the garden compound envelope and keep reserves a larger defensive precinct");
  check(historicalLandBudget({}).ordinaryDistrictArea==0,"empty history never requests speculative housing land");
  double housing = 10000.;
  const double wealthy = takeHistoricalHousingShare(housing, .2);
  const double poor = takeHistoricalHousingShare(housing, .3);
  check(std::abs(housing + wealthy + poor - 10000.) < 1e-9 && housing == 5600.,
        "historical quarters partition housing demand instead of adding speculative land");
  double emptyHousing = 0.;
  check(takeHistoricalHousingShare(emptyHousing, .3) == 0., "no households means no speculative quarter");
  auto plazaPacking=freshLayout;
  auto &plazaPlot=plazaPacking.skeleton.regions.back();
  plazaPlot.boundary={{60,0,0},{65,0,0},{65,0,8},{60,0,8}};
  plazaPlot.attached=true;plazaPlot.courtFacing=true;
  plazaPlot.publicFrontage={plazaPlot.boundary[2],plazaPlot.boundary[3]};
  check(allocateHistoricalCityLots({{99,"home",0,2,4}},plazaPacking,&error) &&
        plazaPacking.historicalLots.front().lot==3 &&
        std::count_if(plazaPacking.lots.begin(),plazaPacking.lots.end(),[](const auto &l){return l.developed;})==1,
        "five-metre attached plaza frontage is admitted and filled without inventing households");
  auto crowded = freshLayout;
  auto many = requests;
  many.push_back({12, "home", 40, 1, 3});
  many.push_back({13, "home", 50, 1, 2});
  check(allocateHistoricalCityLots(many, crowded, &error) && crowded.historicalLots.back().lot == 0, "insufficient parcels produce explicit unmet requests");
  check(std::all_of(crowded.historicalLots.begin(),crowded.historicalLots.end(),
      [](const auto &b){return std::isfinite(b.requiredFootprintArea)&&b.requiredFootprintArea>=40.;}),
      "allocated and unmet historical buildings retain programme area independent of lot size");
  const auto before = layout.historicalLots.size();
  requests.push_back(requests.front());
  check(!allocateHistoricalCityLots(requests, layout, &error) && layout.historicalLots.size() == before, "duplicate historical identity fails transactionally");
  check(
      allocateHistoricalCityLots({}, layout, &error) && std::none_of(
                                                            layout.lots.begin(),
                                                            layout.lots.end(),
                                                            [](const auto &l) {
                                                              return l.developed;
                                                            }),
      "empty historical building set remains empty");
  return failed ? 1 : 0;
}
