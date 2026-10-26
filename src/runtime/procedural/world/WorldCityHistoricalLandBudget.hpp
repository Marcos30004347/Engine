#pragma once
#include "WorldSettlementDirector.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
namespace procedural::world::rewrite {
struct HistoricalLandBudget {
  struct Use { uint32_t buildings=0; double footprintArea=0, districtArea=0; };
  std::map<std::string,Use> uses;
  double ordinaryDistrictArea=0;
};
// Divide an existing housing budget, without inventing extra residential land.
inline double takeHistoricalHousingShare(double &remaining, double fraction)
{
  const double share = remaining * std::clamp(fraction, 0., 1.);
  remaining -= share;
  return share;
}
// A spatial projection of completed history, independent of former lot shapes.
// Keep modular minimum envelopes; circulation is reserved before subdivision.
inline HistoricalLandBudget historicalLandBudget(const std::vector<SettlementHistoricalBuilding> &records)
{
  HistoricalLandBudget budget;
  for(const auto &b:records) {
    auto &use=budget.uses[b.use];++use.buildings;
    double area=0;
    if(b.use=="keep")area=28000;
    // Reserve the estate, not only its roof: the garden layout needs a
    // 78 x 70 m clear rectangle plus wings, planting and street margins.
    else if(b.use=="manor")area=16000;
    else if(b.use=="church")area=900;
    else {
      const double floorArea=std::max(64.,double(b.residents)*12./std::max(1u,b.floors));
      area=std::ceil(std::sqrt(floorArea)/1.2);area=area*area*1.44;
      if(b.use=="workshop" || b.use=="market")area=std::max(area,100.);
      if(b.use=="inn")area=std::max(area,144.);
      // 65% buildable plots; remaining land is circulation and shared space.
      budget.ordinaryDistrictArea+=area/.65;
    }
    use.footprintArea+=area;
    use.districtArea+=area/((b.use=="keep" || b.use=="manor" || b.use=="church")?1.:.65);
  }
  return budget;
}
}
