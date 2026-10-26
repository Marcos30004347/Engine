#pragma once
#include "WorldCityTerrainRoutes.hpp"
#include "WorldCityCompletion.hpp"
#include "WorldCityRelief.hpp"
namespace procedural::world::rewrite {
// Feed the accepted pre-lot solve into the existing full city model pipeline.
// No second relief solve is allowed: that would excavate an already cut street.
inline bool applyCityTerrainRoutes(const CityTerrainRoutePlan &terrain,CityCompletionPlan &city,
    CityReliefPlan &relief,std::string *error=nullptr) {
  const auto fail=[&](const char *s){if(error)*error=s;return false;};
  if(!terrain.revision||!terrain.ground||!terrain.source)return fail("missing accepted terrain route plan");
  if(std::any_of(city.lots.begin(),city.lots.end(),[](const auto &l){return l.developed;}))
    return fail("terrain routes must be applied before occupied plot allocation");
  auto next=city;CityReliefPlan levels;
  next.revision=terrain.revision;next.skeleton.roadNodes=terrain.roads.roadNodes;
  next.skeleton.roadEdges=terrain.roads.roadEdges;next.skeleton.revision=terrain.revision;
  next.routes.clear();levels.revision=terrain.revision;
  levels.constructionTerrain=std::make_shared<const CityTerrainRoutePlan>(terrain);
  levels.ground=levels.openGround=levels.accessGround=terrain.ground;
  for(auto [id,y]:terrain.profiles.junctions)levels.nodeHeight[id]=y;
  for(const auto &profile:terrain.profiles.streets) {
    auto edge=std::find_if(terrain.roads.roadEdges.begin(),terrain.roads.roadEdges.end(),[&](const auto &e){return e.id==profile.edge;});
    if(edge==terrain.roads.roadEdges.end())return fail("terrain profile has no route");
    CitySmoothedRoute route;route.id=profile.edge;route.edges={profile.edge};route.path=profile.path;route.kind=edge->kind;route.halfWidth=edge->halfWidth;
    next.routes.push_back(std::move(route));
    CityReliefPlan::EdgeProfile graded;graded.edge=profile.edge;graded.path=profile.path;graded.halfWidth=edge->halfWidth;graded.grade=profile.maximumGrade;
    levels.edges.push_back(std::move(graded));levels.maximumGrade=std::max(levels.maximumGrade,profile.maximumGrade);
  }
  for(const auto &edit:terrain.edits)if(edit.owner.kind==CityConstructionKind::Plot||edit.owner.kind==CityConstructionKind::Complex)
    levels.regionLevel[edit.owner.source]=edit.plane.constant;
  city=std::move(next);relief=std::move(levels);if(error)error->clear();return true;
}
}
