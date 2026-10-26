#include "WorldCityBase.hpp"
#include <algorithm>
#include <cmath>

namespace procedural::world::rewrite {
// Internal implementation; settings are validated by planCityBase.
bool planCityDistrictBase(const CityBaseSettings &, CityBasePlan &, std::string *);
namespace {
using Ring=std::vector<Position>;
Ring rect(double x,double z,double w,double d) {return {{x,0,z},{x+w,0,z},{x+w,0,z+d},{x,0,z+d}};}
double random(uint64_t seed,uint64_t k) {
  uint64_t x=seed+k*0x9e3779b97f4a7c15ULL;x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
  x=(x^(x>>27))*0x94d049bb133111ebULL;return double((x^(x>>31))>>11)/9007199254740992.;
}
Ring voronoiCell(Ring ring,Position site,const std::vector<Position> &sites) {
  for(auto other:sites) {
    const double nx=other.x-site.x,nz=other.z-site.z;
    if(std::abs(nx)+std::abs(nz)<1e-8)continue;
    const double c=(other.x*other.x+other.z*other.z-site.x*site.x-site.z*site.z)*.5;
    Ring clipped;
    for(size_t i=0;i<ring.size();++i) {
      auto a=ring[i],b=ring[(i+1)%ring.size()];
      const double da=nx*a.x+nz*a.z-c,db=nx*b.x+nz*b.z-c;
      if(da<=0)clipped.push_back(a);
      if((da<0 && db>0)||(da>0 && db<0)) {
        const double t=da/(da-db);clipped.push_back({a.x+(b.x-a.x)*t,0,a.z+(b.z-a.z)*t});
      }
    }
    ring=std::move(clipped);
  }
  return ring;
}
}
bool planCityStreetQuarter(uint64_t seed,CityCompletionPlan &result,std::string *error) {
  CityCompletionPlan city;city.revision=city.skeleton.revision=seed?seed:1;
  constexpr unsigned bays=6;constexpr double frontage=7.,depth=10.,streetHalf=3.2;
  const auto centre=[](double x){return 2.8*std::sin(x*3.141592653589793/42.);};
  CitySmoothedRoute route;route.id=1;route.halfWidth=streetHalf;route.groundCover=CityGroundCover::Stone;
  for(int k=-4;k<=46;++k)route.path.push_back({double(k),0,centre(k)});
  city.routes.push_back(route);
  for(unsigned side=0;side<2;++side) {
    const double sign=side?1.:-1.;Ring boundary;
    for(unsigned k=0;k<=bays;++k)boundary.push_back({k*frontage,0,centre(k*frontage)+sign*streetHalf});
    for(int k=bays;k>=0;--k)boundary.push_back({k*frontage,0,centre(k*frontage)+sign*(streetHalf+depth+.8)});
    city.skeleton.regions.push_back({100+side,100+side,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Merchant,boundary,false,453.6});
    for(unsigned k=0;k<bays;++k) {
      const double a=k*frontage,b=(k+1)*frontage;
      // Reserve a continuous pedestrian apron outside the dwelling lots.
      // Entry treads stay in the lot; the apron connects them around bends.
      const Position frontA{a,0,centre(a)+sign*(streetHalf+.8)},frontB{b,0,centre(b)+sign*(streetHalf+.8)};
      Ring lot={frontA,frontB,{b,0,frontB.z+sign*depth},{a,0,frontA.z+sign*depth}};
      if(!side)std::reverse(lot.begin(),lot.end());
      const uint64_t id=200+side*bays+k;
      city.skeleton.regions.push_back({id,id,100+side,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Merchant,lot,false,frontage*depth});
      auto &region=city.skeleton.regions.back();region.courtFacing=true;region.publicFrontage={frontA,frontB};
      CityLotDevelopment house;house.lot=id;house.rowHouse=true;house.merchantHouse=k%3!=0;
      house.minimumFloors=house.maximumFloors=2+(k+side)%2;house.desiredRooms=4;
      house.preferredFootprintMetres2=frontage*depth*.88;house.groundCover=CityGroundCover::Stone;
      city.lots.push_back(house);
    }
  }
  // The lane arrives in a civic pocket, with its approach reserved by the
  // same route used to keep monuments and planting out of circulation.
  const Ring square=rect(44,-6,14,16);
  city.skeleton.regions.push_back({300,300,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Merchant,square,false,224});
  city.skeleton.publicSpaces.push_back({300,CityPublicSpace::Kind::Court,square,0});
  result=std::move(city);if(error)error->clear();return true;
}
bool planCityBase(const CityBaseSettings &s,CityBasePlan &result,std::string *error) {
  const auto fail=[&](const char *why){if(error)*error=why;return false;};
  if(s.tiers<2 || s.tiers>8 || !std::isfinite(s.width) || s.width<60 || s.width>240 ||
     !std::isfinite(s.tierDepth) || s.tierDepth<34 || s.tierDepth>60 ||
     !std::isfinite(s.tierRise) || s.tierRise<3.6 || s.tierRise>5.4 ||
     !std::isfinite(s.lotFrontage) || s.lotFrontage<7 || s.lotFrontage>16 ||
     !std::isfinite(s.alleyWidth) || s.alleyWidth<2.5 || s.alleyWidth>4 ||
     !std::isfinite(s.laneMeanderMetres) || s.laneMeanderMetres<0 || s.laneMeanderMetres>1.5)
    return fail("city base requires 2-8 tiers, 60-240 m width and walkable terrace dimensions");
  if(s.layout==CityBaseLayout::Districts)return planCityDistrictBase(s,result,error);
  CityBasePlan base;base.settings=s;base.circulation.revision=s.seed;base.circulation.skeleton.revision=s.seed;
  const auto terrain=[s](double x,double z) {
    // A continuous hillside beneath independently meshed terrace cliffs.
    // Its slope clears the back of each deck, so houses never bury into it.
    const double rise=s.tierRise/s.tierDepth*std::max(0.,z)-s.tierRise;
    const double hill=.28*std::sin(x*.065+s.seed*.01)*std::sin(z*.04);
    const double shape=s.terrain==CityBaseTerrain::Ridge?1.2*(std::cos(x*.045)-1.):
      s.terrain==CityBaseTerrain::Valley?-2.2*std::exp(-x*x/500.):0.;
    return rise+hill+shape;
  };
  base.relief.ground=base.relief.openGround=terrain;
  base.relief.revision=s.seed;
  if(s.layout==CityBaseLayout::Voronoi) {
    // One shared diagram: contour-aligned inhabited sites preserve connected
    // stair approaches, while jittered exterior sites carve irregular cliffs.
    for(uint32_t tier=0;tier<s.tiers;++tier)base.landSites.push_back({0,0,(tier+.5)*s.tierDepth});
    for(uint32_t tier=0;tier<s.tiers;++tier)for(int side:{-1,1}) {
      const uint64_t key=700+tier*4+(side>0?2:0);
      base.landSites.push_back({side*(s.width+16+36*random(s.seed,key)),0,(tier+.1+.8*random(s.seed,key+1))*s.tierDepth});
    }
  }
  std::vector<CityReliefPlan::RaisedBlock> decks;
  uint64_t routeId=1,envelopeId=10000;
  const auto route=[&](Ring path,double width) {
    CitySmoothedRoute r;r.id=routeId++;r.path=std::move(path);r.halfWidth=width*.5;r.groundCover=CityGroundCover::Stone;
    base.circulation.routes.push_back(std::move(r));
  };
  for(uint32_t tier=0;tier<s.tiers;++tier) {
    const double z=tier*s.tierDepth,level=1+tier*s.tierRise;
    const double shift=s.layout==CityBaseLayout::Staggered?(tier%2?2.5:-2.5):0.;
    const double taper=s.layout==CityBaseLayout::Broad?.4:1.;
    double left=-s.width*.5+tier*1.7*taper+shift,right=s.width*.5-tier*1.3*taper+shift;
    const double notchWidth=14,flightPitch=2.2;
    const double notchDepth=tier==0?12.:2*flightPitch+1.2;
    const double notch=left+14+random(s.seed,tier)*std::max(1.,right-left-44);
    CityReliefPlan::RaisedBlock deck;deck.block=100+tier;deck.level=level;deck.rockCut=true;deck.stairHalfWidth=1.05;
    deck.boundary={{left+2,0,z},{notch,0,z},{notch,0,z+notchDepth},{notch+notchWidth,0,z+notchDepth},
      {notch+notchWidth,0,z},{right-2,0,z},{right,0,z+2},
      {right+1.8,0,z+s.tierDepth*.3},{right-.7,0,z+s.tierDepth*.65},{right,0,z+s.tierDepth-2},
      {right-2,0,z+s.tierDepth},{left+2,0,z+s.tierDepth},{left,0,z+s.tierDepth-2},
      {left-1.4,0,z+s.tierDepth*.7},{left+.6,0,z+s.tierDepth*.35},{left,0,z+2}};
    if(s.layout==CityBaseLayout::Voronoi) {
      auto cell=voronoiCell(rect(-s.width*.5-40,z,s.width+80,s.tierDepth),base.landSites[tier],base.landSites);
      if(cell.size()<4)return fail("Voronoi terrace lost its usable core");
      base.landCells.push_back(cell);deck.boundary.clear();bool cut=false;
      for(size_t i=0;i<cell.size();++i) {
        auto a=cell[i],b=cell[(i+1)%cell.size()];deck.boundary.push_back(a);
        if(std::abs(a.z-z)<1e-6 && std::abs(b.z-z)<1e-6 && a.x<notch && b.x>notch+notchWidth) {
          deck.boundary.insert(deck.boundary.end(),{{notch,0,z},{notch,0,z+notchDepth},
            {notch+notchWidth,0,z+notchDepth},{notch+notchWidth,0,z}});cut=true;
        }
      }
      if(!cut)return fail("Voronoi terrace cannot reserve its stair court");
      // Fit the inhabited core to the narrowest part of this convex cell,
      // keeping the reserved stair position within the shared original core.
      // Wider cells gain frontage instead of becoming empty paved margins.
      left=-1e30;right=1e30;
      for(auto p:cell) {
        if(p.x<0)left=std::max(left,p.x+1.5);
        else right=std::min(right,p.x-1.5);
      }
    }
    if(tier==0) {
      // The initial approach climbs from the hillside into the same reserved
      // court used between terraces; no road is silently converted to stairs.
      const CityReliefSettings stairs;
      double approach=12;
      for(;approach<40;approach+=2) {
        const double rise=level-terrain(notch+7,z-approach);
        const unsigned steps=unsigned(std::ceil(rise/stairs.stepRiseMetres));
        const unsigned breaks=(steps-1)/stairs.stepsPerFlight;
        const double required=steps*stairs.stepGoingMetres+(breaks+2)*std::max(stairs.landingMetres,1.55);
        if(approach+notchDepth>=required+.1)break;
      }
      deck.accessFrom={notch+7,terrain(notch+7,z-approach),z-approach};
      deck.accessTo={notch+7,level,z+notchDepth};
      deck.stairHalfWidth=1.4;
    } else {
      const double a=notch+1.2,b=notch+notchWidth-1.2,lower=level-s.tierRise;
      deck.accessPath={{a,lower,z},{b,lower+s.tierRise/3,z},{b,lower+s.tierRise/3,z+flightPitch},
        {a,lower+2*s.tierRise/3,z+flightPitch},{a,lower+2*s.tierRise/3,z+2*flightPitch},
        {b,level,z+2*flightPitch},{b,level,z+notchDepth}};
    }
    base.circulation.skeleton.regions.push_back({deck.block,deck.block,0,CityRegionKind::Block,CityLandUse::UrbanBlock,
      tier==s.tiers-1?DistrictUse::WealthyResidential:DistrictUse::Merchant,deck.boundary,false,0});
    // Public lanes run along the contour, with cross alleys reaching the
    // central lane and the stair court. Every future plot fronts one of them.
    const double middle=z+s.tierDepth*.5,back=z+s.tierDepth-2;
    route({{left+2,level,middle},{right-2,level,middle}},s.alleyWidth);
    route({{left+3,level,z+3},{left+3,level,back},{right-3,level,back},{right-3,level,z+3}},s.alleyWidth);
    route({{notch+7,level,z+notchDepth},{notch+7,level,middle}},s.alleyWidth);
    route({{left+3,level,z+3},{notch,level,z+3}},s.alleyWidth);
    route({{notch+notchWidth,level,z+3},{right-3,level,z+3}},s.alleyWidth);
    const double margin=5,frontDepth=middle-s.alleyWidth*.5-(z+5)-.2;
    const auto envelope=[&](double x,double zz,double w,double depth){if(w<s.lotFrontage || depth<8)return;
      base.buildable.push_back({envelopeId++,0,deck.block,CityRegionKind::Reservation,CityLandUse::UrbanBlock,
        DistrictUse::Merchant,rect(x,zz,w,depth),false,w*depth});};
    envelope(left+margin,z+5,notch-1.5-(left+margin),frontDepth);
    envelope(notch+notchWidth+1.5,z+5,right-margin-(notch+notchWidth+1.5),frontDepth);
    // A rear service alley splits the long frontage, allowing courtyards and
    // pedestrian circulation through the block instead of inaccessible infill.
    const double rearZ=middle+s.alleyWidth*.5+.2,rearDepth=back-s.alleyWidth*.5-rearZ-.2;
    const double crossX=left+(right-left)*(.42+.12*random(s.seed,100+tier));
    route({{crossX,level,middle},{crossX,level,back}},s.alleyWidth);
    envelope(left+margin,rearZ,crossX-s.alleyWidth*.5-left-margin,rearDepth);
    envelope(crossX+s.alleyWidth*.5,rearZ,right-margin-crossX-s.alleyWidth*.5,rearDepth);
    decks.push_back(deck);
    for(int side:{-1,1}) {
      double edge=side<0?left:right;
      if(s.layout==CityBaseLayout::Voronoi)for(auto p:deck.boundary)edge=side<0?std::min(edge,p.x):std::max(edge,p.x);
      const double x=edge+side*5,zz=z+12+8*random(s.seed,200+tier*2+(side>0));
      base.relief.rocks.push_back({{x,terrain(x,zz)-.5,zz},3.8+random(s.seed,300+tier*2+(side>0))*2,4+random(s.seed,400+tier*2+(side>0))*9,s.seed+tier*2+(side>0)});
    }
  }
  CityReliefSettings settings;
  if(!planRaisedCityBlocks(base.circulation.skeleton,decks,settings,base.relief,error))return false;
  base.relief.minimumLevel=1;base.relief.maximumLevel=1+(s.tiers-1)*s.tierRise;
  base.relief.fallMetres=base.relief.maximumLevel-base.relief.minimumLevel;
  result=std::move(base);if(error)error->clear();return true;
}
bool populateCityBase(const CityBasePlan &base,CityCompletionPlan &result,CityReliefPlan &relief,std::string *error) {
  if(base.relief.platforms.empty() || !base.relief.openGround){if(error)*error="city base must be planned before allocating lots";return false;}
  auto city=base.circulation;auto levels=base.relief;uint64_t id=1000;
  for(const auto &envelope:base.buildable) {
    const auto a=envelope.boundary.front(),b=envelope.boundary[2];
    const double width=b.x-a.x,depth=b.z-a.z;
    const unsigned count=std::max(1u,unsigned(std::floor(width/base.settings.lotFrontage)));
    const double span=width/count;
    for(unsigned k=0;k<count;++k) {
      const double gap=.18;CitySkeletonRegion lot=envelope;lot.id=id++;lot.kind=CityRegionKind::Lot;lot.landUse=CityLandUse::UrbanLot;
      lot.boundary=rect(a.x+k*span+gap,a.z,span-2*gap,depth);lot.area=(span-2*gap)*depth;city.skeleton.regions.push_back(lot);
      CityLotDevelopment development;development.lot=lot.id;development.rowHouse=true;development.merchantHouse=lot.id%3==0;
      development.minimumFloors=2+unsigned(random(base.settings.seed,lot.id)*2);development.maximumFloors=development.minimumFloors;
      development.desiredRooms=4;development.preferredFootprintMetres2=lot.area*.8;development.groundCover=CityGroundCover::Stone;
      development.residence=lot.id%17==0?CityResidenceKind::Inn:lot.id%13==0?CityResidenceKind::Pub:CityResidenceKind::House;
      city.lots.push_back(development);levels.regionLevel[lot.id]=levels.regionLevel.at(lot.parent);
      for(auto &block:levels.landBlocks)if(block.block==lot.parent)block.lots.push_back(lot.id);
    }
  }
  result=std::move(city);relief=std::move(levels);if(error)error->clear();return true;
}
}
