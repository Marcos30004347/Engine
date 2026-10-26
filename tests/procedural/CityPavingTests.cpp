#include "procedural/world/WorldCityPaving.hpp"
#include "procedural/world/WorldCitySurfaces.hpp"
#include <iostream>
using namespace procedural;
int main(){
  auto box=[](double x,double z,double a,double b){geom2::Polygon p;p.outer={{geom2::toUnit(x),geom2::toUnit(z)},{geom2::toUnit(a),geom2::toUnit(z)},{geom2::toUnit(a),geom2::toUnit(b)},{geom2::toUnit(x),geom2::toUnit(b)}};return p;};
  std::vector<geom2::Polygon> roads={box(-20,-5,150,5),box(59,-40,69,145),box(-20,130,150,140),box(140,-5,150,140)};
  std::vector<geom2::Operand> operands;for(const auto &p:roads)operands.push_back(geom2::toOperand(p));
  std::vector<geom2::Polygon> united,expected,actual;std::string error;
  if(!geom2::unionOf(operands,united,&error))return 1;
  geom2::OffsetOptions options;options.join=geom2::JoinStyle::Round;options.arcToleranceMetres=.06;
  for(const auto &p:united){std::vector<geom2::Polygon> pieces;if(!geom2::offsetPolygon(p,-.6,options,pieces,&error))return 1;expected.insert(expected.end(),pieces.begin(),pieces.end());}
  if(!world::rewrite::insetCityRoadTiles(roads,.6,actual,&error)){std::cerr<<error;return 1;}
  const auto contains=[](const auto &polygons,double x,double z){geom2::Point q{geom2::toUnit(x),geom2::toUnit(z)};for(const auto &p:polygons){bool inside=geom2::contains(p.outer,q);for(const auto &h:p.holes)inside &= !geom2::contains(h,q);if(inside)return true;}return false;};
  for(double x=-24.31;x<155;x+=1.13)for(double z=-44.27;z<150;z+=1.07)if(contains(actual,x,z)!=contains(expected,x,z)){std::cerr<<"Paving seam mismatch "<<x<<','<<z;return 1;}
  double a=0,b=0;for(const auto &p:expected)a+=geom2::areaMetres(p);for(const auto &p:actual){b+=geom2::areaMetres(p);if(!geom2::polygonFault(p).empty())return 1;}
  if(std::abs(a-b)>.01){std::cerr<<"Area mismatch "<<a<<' '<<b;return 1;}
  // A narrow feeder inside a wide street must add no internal sidewalk;
  // splitting ownership or excavating a corner must not add a curb either.
  std::vector<geom2::Polygon> corridors={box(0,-3,80,3),box(10,-.9,70,.9),box(36,-20,44,20)};
  std::vector<geom2::Polygon> faces={box(0,-5,40,5),box(40,-5,80,5)};
  for(const auto &face:faces){std::vector<geom2::Polygon> road,walk;
    if(!world::rewrite::splitCityCarriageway(face,corridors,road,walk,&error))return 1;
    double area=0;for(const auto &p:road)area+=geom2::areaMetres(p);for(const auto &p:walk)area+=geom2::areaMetres(p);
    if(std::abs(area-geom2::areaMetres(face))>1e-5)return 2;
    for(double x=geom2::toMetres(geom2::bounds(face).minX)+.3;x<geom2::toMetres(geom2::bounds(face).maxX);x+=.7)
      if(!contains(road,x,0)||contains(walk,x,0)){std::cerr<<"Internal curb in continuous street";return 3;}
  }
  std::vector<geom2::Polygon> road,walk;
  if(!world::rewrite::splitCityCarriageway(box(12,-2,18,2),corridors,road,walk,&error)||!walk.empty())return 4;
  // A frontage strip planned against an old block chord must be rebuilt
  // against the actual street, without diagonal fingers into the block.
  world::rewrite::CityCompletionPlan city;
  world::rewrite::CitySmoothedRoute route;route.id=8;route.halfWidth=5;route.path={{-10,0,0},{110,0,0}};city.routes.push_back(route);
  world::rewrite::CitySkeletonRegion block;block.id=21;block.kind=world::rewrite::CityRegionKind::Block;block.boundary={{0,0,5},{100,0,5},{100,0,30},{0,0,30}};city.skeleton.regions.push_back(block);
  city.skeleton.publicSpaces.push_back({21,world::rewrite::CityPublicSpace::Kind::Sidewalk,{{0,0,5},{100,0,5},{50,0,12}},0});
  if(!world::rewrite::planCitySidewalks(city,1.4,&error)){std::cerr<<error;return 5;}
  double sidewalkArea=0;
  for(const auto &walk:city.skeleton.publicSpaces){geom2::Polygon p;
    for(auto v:walk.boundary){if(v.z<5.-1e-6||v.z>6.4+1e-6)return 6;p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});}
    sidewalkArea+=std::abs(geom2::areaMetres(p));
  }
  if(std::abs(sidewalkArea-143.92)>.001){std::cerr<<"Sidewalk coverage "<<sidewalkArea;return 7;}
  // A shifted block edge must not leave a terrain sliver between the
  // sidewalk and street. This is the small wedge seen at city bends.
  city.skeleton.regions.front().boundary[0].z=5.5;
  city.skeleton.regions.front().boundary[1].z=5.5;
  if(!world::rewrite::planCitySidewalks(city,1.4,&error))return 17;
  std::vector<geom2::Polygon> gapWalks;
  for(const auto &walk:city.skeleton.publicSpaces){geom2::Polygon p;for(auto v:walk.boundary)p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});gapWalks.push_back(p);}
  if(!contains(gapWalks,50.,5.1)||contains(gapWalks,50.,4.9))return 18;
  // Bent streets and an overlapping feeder share the same sidewalk area;
  // the pedestrian band cannot cross any of the vehicle corridors.
  city.routes.front().path={{-10,0,0},{35,0,0},{55,0,15},{110,0,15}};
  auto feeder=city.routes.front();feeder.id=9;feeder.halfWidth=1.2;city.routes.push_back(feeder);
  if(!world::rewrite::planCitySidewalks(city,1.4,&error))return 10;
  std::vector<geom2::Operand> vehicle;
  for(const auto &r:city.routes){geom2::OffsetOptions o;o.join=geom2::JoinStyle::Miter;o.miterLimit=2.;o.arcToleranceMetres=.1;
    std::vector<geom2::Polygon> bands;if(!geom2::offsetPolyline(world::rewrite::cityRoadPath(r.path),r.halfWidth,o,bands,&error))return 11;
    for(const auto &p:bands)vehicle.push_back(geom2::toOperand(p));
  }
  if(city.skeleton.publicSpaces.empty())return 12;
  for(const auto &walk:city.skeleton.publicSpaces){geom2::Polygon p;for(auto v:walk.boundary)p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});
    if(!geom2::polygonFault(p).empty())return 13;
    for(const auto &road:vehicle){std::vector<geom2::Polygon> overlap;
      if(!geom2::intersectionOf({geom2::toOperand(p),road},overlap,&error))return 14;
      for(const auto &q:overlap)if(std::abs(geom2::areaMetres(q))>1e-5)return 15;
    }
  }
  city.routes.clear();
  if(!world::rewrite::planCitySidewalks(city,1.4,&error)||!city.skeleton.publicSpaces.empty())return 16;
  // Densely sampled straight paths must not grow teeth at every round join.
  std::vector<geom2::Point> path;for(int i=0;i<=30;++i)path.push_back({geom2::toUnit(i),0});
  geom2::OffsetOptions roadOptions;roadOptions.join=geom2::JoinStyle::Miter;roadOptions.miterLimit=2.;
  std::vector<geom2::Polygon> band;if(!geom2::offsetPolyline(path,2.,roadOptions,band,&error))return 8;
  for(const auto &p:band)for(auto v:p.outer)if(v.x>geom2::toUnit(2.)&&v.x<geom2::toUnit(28.)&&std::abs(geom2::toMetres(v.y))>2.000001)return 9;
  std::cout<<"PASS tiled pavement agrees with connected-network erosion across junctions, holes and tile seams\n";
}
