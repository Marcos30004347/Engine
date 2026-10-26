#include "procedural/world/generator/CityBlockPlanner.hpp"
#include "procedural/world/WorldCityModel.hpp"
#include "procedural/geom2/Boolean.hpp"
#include <cmath>
#include <iostream>
using namespace procedural;
using namespace procedural::world;
using namespace procedural::world::rewrite;
geom2::Polygon polygon(const std::vector<Position> &ring) {
  geom2::Polygon p;for(auto q:ring)p.outer.push_back({geom2::toUnit(q.x),geom2::toUnit(q.z)});return p;
}
int main() {
  int failures=0;
  auto check=[&](bool ok,const char *message){std::cout<<(ok?"PASS ":"FAIL ")<<message<<'\n';failures+=!ok;};
  for(double skew:{0.,8.,-6.}) {
    generator::CityBlockPlanInput in;
    in.boundary={{0,0,0},{46,0,0},{46+skew,0,38},{skew*.4,0,38}};
    in.streetEdges={true,true,true,true};
    generator::CityBlockPlan plan,again;std::string error;
    check(generator::planCityBlock(in,plan,&error),"skewed block plans");
    check(!plan.paving.empty() && plan.corridors.size()>=2,"court has two reserved entrances");
    check(generator::planCityBlock(in,again,&error) && again.lots.size()==plan.lots.size(),"deterministic allocation");
    std::vector<geom2::Operand> publicGround;
    for(const auto &ring:plan.paving)publicGround.push_back(geom2::toOperand(polygon(ring)));
    for(const auto &ring:plan.corridors)publicGround.push_back(geom2::toOperand(polygon(ring)));
    for(const auto &ring:plan.sidewalks)publicGround.push_back(geom2::toOperand(polygon(ring)));
    double overlaps=0,lotArea=0;
    for(size_t i=0;i<plan.lots.size();++i) {
      const auto p=polygon(plan.lots[i].boundary);lotArea+=geom2::areaMetres(p);
      std::vector<geom2::Polygon> outside;
      check(geom2::differenceOf(geom2::toOperand(p),{geom2::toOperand(polygon(in.boundary))},outside,&error) && outside.empty(),"lot stays within skewed block");
      for(const auto &q:publicGround) {
        std::vector<geom2::Polygon> overlap;
        check(geom2::intersectionOf({geom2::toOperand(p),q},overlap,&error),"public/private intersection computes");
        for(const auto &r:overlap)overlaps+=std::abs(geom2::areaMetres(r));
      }
      for(size_t j=i+1;j<plan.lots.size();++j) {
        std::vector<geom2::Polygon> overlap;
        check(geom2::intersectionOf({geom2::toOperand(p),geom2::toOperand(polygon(plan.lots[j].boundary))},overlap,&error),"lot intersection computes");
        for(const auto &r:overlap)overlaps+=std::abs(geom2::areaMetres(r));
      }
    }
    check(overlaps<.001,"lots do not overlap public space or neighbours");
    check(lotArea/geom2::areaMetres(polygon(in.boundary))>.48,"frontages retain useful block coverage");
    CityCompletionPlan city;city.revision=1;
    city.skeleton.regions.push_back({100,100,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Residential,in.boundary,false,46*38});
    city.skeleton.alleys=plan.corridors;
    for(const auto &ring:plan.sidewalks)city.skeleton.publicSpaces.push_back({100,CityPublicSpace::Kind::Sidewalk,ring,0});
    for(const auto &ring:plan.paving)city.skeleton.publicSpaces.push_back({100,CityPublicSpace::Kind::Court,ring,.54});
    CitySurfaceStructure surfaces;
    check(buildCitySurfaces(city,surfaces,&error),"public ground meshes before dwellings");
    double surfaceArea=0;bool tread=false,deck=false;
    for(const auto &piece:surfaces.pieces) {
      surfaceArea+=geom2::areaMetres(piece.polygon);
      tread|=piece.kind==CitySurfaceKind::StairTread;
      deck|=piece.kind==CitySurfaceKind::Court && std::abs(piece.height-.575)<1e-6;
    }
    check(std::abs(surfaceArea-geom2::areaMetres(polygon(in.boundary)))<.01,"public ground conserves block area");
    check(tread && deck,"raised court has real tread and deck geometry");
    CityModelPlan model;CityModelSettings settings;settings.groundOnly=true;settings.groundVertexSpacing=3;
    SettlementFabric fabric;
    check(buildCityModel(city,fabric,settings,model,&error) && !model.mesh.triangles.empty(),"infrastructure-only model triangulates stair rings");
    check(std::find(model.surfaceMaterials.begin(),model.surfaceMaterials.end(),18)!=model.surfaceMaterials.end() &&
          std::find(model.surfaceMaterials.begin(),model.surfaceMaterials.end(),19)!=model.surfaceMaterials.end(),
          "sidewalks and court have distinct physical texture materials");
    bool discrete=true;
    for(size_t t=0;t<model.surfaceMaterials.size();++t)if(model.surfaceMaterials[t]==18 || model.surfaceMaterials[t]==19)
      for(const auto &w:model.groundWeights[t])discrete&=std::abs(w[0])+std::abs(w[1])+std::abs(w[2])<1e-9;
    check(discrete,"explicit public paving materials do not blend with terrain textures");
    if(skew==0) {
      uint64_t id=200;
      for(const auto &lot:plan.lots) {
        city.skeleton.regions.push_back({id,id,100,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Residential,lot.boundary,false,lot.area});
        CityLotDevelopment home;home.lot=id++;home.rowHouse=true;home.minimumFloors=1;home.maximumFloors=2;
        home.preferredFootprintMetres2=lot.area*.94;home.desiredRooms=3;city.lots.push_back(home);
      }
      for(size_t i=0;i<4;++i) {
        auto a=in.boundary[i],b=in.boundary[(i+1)%4];
        const double len=std::hypot(b.x-a.x,b.z-a.z),nx=(b.z-a.z)/len*2.6,nz=-(b.x-a.x)/len*2.6;
        CitySmoothedRoute way;way.id=i+1;way.halfWidth=2.4;way.groundCover=CityGroundCover::Stone;
        way.path={{a.x+nx,0,a.z+nz},{b.x+nx,0,b.z+nz}};city.routes.push_back(way);
      }
      settings.groundOnly=false;
      check(buildCityModel(city,fabric,settings,model,&error),"houses realise on planned infrastructure");
      check(model.gatewayHouses>=1,"neighbouring houses connect above an open public passage");
      bool clearPassage=true;
      // A horizontal eye-height ray must travel through both courtyard
      // entrances. This catches a portal that is only painted onto a wall.
      for(const auto &t:model.mesh.triangles) {
        const auto a=model.mesh.vertices[t[0]],b=model.mesh.vertices[t[1]],c=model.mesh.vertices[t[2]];
        const double den=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
        if(std::abs(den)<1e-9)continue;
        const double u=((b.y-c.y)*(23-c.x)+(c.x-b.x)*(1.7-c.y))/den;
        const double v=((c.y-a.y)*(23-c.x)+(a.x-c.x)*(1.7-c.y))/den;
        if(u<0 || v<0 || u+v>1)continue;
        const double z=u*a.z+v*b.z+(1-u-v)*c.z;
        if(z>1.4 && z<36.6) {
          if(clearPassage)std::cout<<"  passage hit at z="<<z<<" material "<<model.materials[&t-&model.mesh.triangles[0]]<<" surface "<<model.surfaceMaterials[&t-&model.mesh.triangles[0]]
            <<" tri ("<<a.x<<","<<a.y<<","<<a.z<<") ("<<b.x<<","<<b.y<<","<<b.z<<") ("<<c.x<<","<<c.y<<","<<c.z<<")\n";
          clearPassage=false;
        }
      }
      if(!clearPassage) {
        double lo[3]={1e9,1e9,1e9},hi[3]={-1e9,-1e9,-1e9};size_t count=0;
        for(size_t i=0;i<model.mesh.triangles.size();++i)if(model.materials[i]==17) {
          for(auto v:model.mesh.triangles[i]){const auto q=model.mesh.vertices[v];if(std::hypot(q.x-23,q.z-18.25)<3){++count;
            lo[0]=std::min(lo[0],q.x);hi[0]=std::max(hi[0],q.x);lo[1]=std::min(lo[1],q.y);hi[1]=std::max(hi[1],q.y);lo[2]=std::min(lo[2],q.z);hi[2]=std::max(hi[2],q.z);}}
        }
        std::cout<<"  material-17 object near the centre: "<<count<<" corners, x "<<lo[0]<<".."<<hi[0]<<" y "<<lo[1]<<".."<<hi[1]<<" z "<<lo[2]<<".."<<hi[2]<<"\n";
        for(const auto &b:model.buildings)for(auto d:b.exteriorDoors)if(std::hypot(d.x-23,d.z-18.25)<8)std::cout<<"  door at ("<<d.x<<","<<d.z<<") of "<<b.id<<"\n";
        std::cout<<"  functional props "<<model.functionalProps.size()<<"\n";
      }
      check(clearPassage,"public passages remain physically open through the final triangle mesh");
      check(model.buildings.size()+model.gatewayHouses>=plan.lots.size()*.8,"connected housing uses at least eighty percent of the lots");
    }
    if(!error.empty())std::cerr<<error<<'\n';
  }
  {
    CitySurfaceStructure paving;
    paving.influences.push_back({polygon({{0,0,0},{2,0,0},{2,0,20},{0,0,20}}),2,1,1.25,false,0,0,true,true});
    const auto index=buildCityGroundWeightIndex(paving);
    const auto left=cityGroundWeights(paving,1.999,10),right=cityGroundWeights(paving,2.001,10);
    const auto outside=cityGroundWeights(paving,3.5,10);
    check(left[2]>.93 && std::abs(left[2]-right[2])<.001 && outside[0]>.999,
          "sidewalk paving reaches its edge and fades continuously into adjoining ground");
    bool same=true;
    for(double x=-2;x<5;x+=.13) {
      const auto a=cityGroundWeights(paving,x,10),b=cityGroundWeights(index,x,10);
      for(size_t k=0;k<3;++k)same&=std::abs(a[k]-b[k])<1e-12;
    }
    check(same,"indexed and reference paving fields agree through the transition");
  }
  {
    generator::CityBlockPlanInput in;
    in.boundary={{0,0,0},{100,0,0},{100,0,80},{0,0,80}};
    in.streetEdges={true,true,true,true};
    generator::CityBlockPlan plan;std::string error;
    check(generator::planCityBlock(in,plan,&error),"deep urban block plans with interior housing");
    double privateArea=0,maxCourt=0;bool admitted=true,clear=true;
    std::vector<geom2::Operand> publicGround;
    for(const auto *rings:{&plan.sidewalks,&plan.corridors,&plan.paving,&plan.gardens})
      for(const auto &ring:*rings)publicGround.push_back(geom2::toOperand(polygon(ring)));
    for(const auto &lot:plan.lots) {
      privateArea+=lot.area;admitted&=lot.area>=in.minimumLotMetres2;
      std::vector<geom2::Polygon> overlap;
      for(const auto &ground:publicGround) {
        clear&=geom2::intersectionOf({geom2::toOperand(polygon(lot.boundary)),ground},overlap,&error);
        for(const auto &piece:overlap)clear&=std::abs(geom2::areaMetres(piece))<.001;
      }
    }
    for(const auto &court:plan.paving)maxCourt=std::max(maxCourt,geom2::areaMetres(polygon(court)));
    check(privateArea>8000*.6 && maxCourt<500,"deep block uses its interior without an oversized central void");
    check(admitted && clear,"interior lots meet admission size and leave all access lanes clear");
  }
  {
    CityCompletionPlan city;
    const std::vector<Position> boundary={{0,0,0},{20,0,0},{20,0,20},{0,0,20}};
    city.skeleton.regions.push_back({100,100,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Residential,boundary,false,400});
    city.skeleton.publicSpaces.push_back({100,CityPublicSpace::Kind::Garden,boundary,0});
    city.skeleton.alleys.push_back({{9,0,0},{11,0,0},{11,0,20},{9,0,20}});
    CitySurfaceStructure surfaces;std::string error;
    check(buildCitySurfaces(city,surfaces,&error),"common garden with a pedestrian route meshes");
    double roadArea=0,total=0;
    for(const auto &piece:surfaces.pieces) {
      const double area=geom2::areaMetres(piece.polygon);total+=area;
      if(piece.kind==CitySurfaceKind::Sidewalk)roadArea+=area;
    }
    check(std::abs(roadArea-40)<.001 && std::abs(total-400)<.001,
          "common garden preserves its pedestrian alley without carving a second road");
  }
  {
    generator::CityBlockPlanInput in;
    in.boundary={{0,0,0},{80,0,0},{80,0,15},{0,0,15}};
    in.streetEdges={true,false,false,false};in.commonCourt=false;
    in.lotFrontageMetres=8.4;in.minimumLotWidthMetres=6.6;
    generator::CityBlockPlan plan;std::string error;
    check(generator::planCityBlock(in,plan,&error) && plan.lots.size()>=7,
          "native modular frontage produces a row of houses");
    bool fits=!plan.lots.empty();
    for(const auto &lot:plan.lots) {
      double lo=1e30,hi=-1e30;
      for(auto p:lot.boundary){lo=std::min(lo,p.x);hi=std::max(hi,p.x);}
      fits &= hi-lo>=6.6;
    }
    check(fits,"frontage plots fit four dwelling modules and both setbacks");
    in.minimumLotWidthMetres=0;
    check(!generator::planCityBlock(in,plan,&error),"invalid module width rejected");
  }
  {
    generator::CityBlockPlanInput in;
    in.id=911;in.inwardCourt=true;
    in.boundary={{0,0,0},{46,0,0},{49,0,38},{-2,0,38}};
    in.streetEdges={true,true,true,true};in.lotFrontageMetres=8.4;
    in.minimumLotWidthMetres=7.4;in.lotDepthMetres=10.;in.corridorMetres=2.4;
    generator::CityBlockPlan plan;std::string why;
    check(generator::planCityBlock(in,plan,&why),"inward urban block plans");
    unsigned facing=0;for(const auto &lot:plan.lots)facing+=lot.courtFacing;
    double courtArea=0;for(const auto &ring:plan.paving)courtArea+=geom2::areaMetres(polygon(ring));
    check(courtArea>100 && facing>=4,"organic court retains useful paving and inward-facing lots");
    check(plan.corridors.size()>=2,"inward court has two public passages from the streets");
  }
  {
    generator::CityBlockPlanInput in;
    in.inwardCourt=true;in.boundary={{0,0,0},{90,0,0},{90,0,80},{0,0,80}};
    in.streetEdges={true,true,true,true};
    generator::CityBlockPlan plan;std::string why;
    check(generator::planCityBlock(in,plan,&why),"large inward court supports lane-fronted infill");
    bool infill=false;for(const auto &note:plan.notes)infill |= note.find("deep interior subdivided")!=std::string::npos;
    check(infill && plan.lots.size()>30 && !plan.corridors.empty(),"oversized inward court gains connected housing instead of a vacant centre");
  }
  generator::CityBlockPlanInput narrow;
  narrow.boundary={{0,0,0},{30,0,0},{30,0,12},{0,0,12}};narrow.streetEdges={true,true,true,true};
  generator::CityBlockPlan result;std::string error;
  check(generator::planCityBlock(narrow,result,&error) && result.paving.empty(),"shallow block does not force a courtyard");
  narrow.lotFrontageMetres=0;
  check(!generator::planCityBlock(narrow,result,&error),"zero lot frontage is rejected");
  std::cout<<failures<<" failures\n";return failures?1:0;
}
