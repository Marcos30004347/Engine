#include "WorldCityBase.hpp"
#include <algorithm>
#include <cmath>

namespace procedural::world::rewrite {
namespace {
using Ring=std::vector<Position>;
double roll(uint64_t seed,uint64_t key) {
  uint64_t n=seed+key*0x9e3779b97f4a7c15ULL;n=(n^(n>>30))*0xbf58476d1ce4e5b9ULL;n=(n^(n>>27))*0x94d049bb133111ebULL;
  return double((n^(n>>31))>>11)/9007199254740992.;
}
Ring rectangle(double x,double z,double w,double d){return {{x,0,z},{x+w,0,z},{x+w,0,z+d},{x,0,z+d}};}
struct Patch {double left,right,front,back;};
bool inRing(const Ring &ring,Position p) {
  bool in=false;for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
    auto a=ring[j],b=ring[i];if((a.z>p.z)!=(b.z>p.z) && p.x<(b.x-a.x)*(p.z-a.z)/(b.z-a.z)+a.x)in=!in;
  }return in;
}
}
bool planCityDistrictBase(const CityBaseSettings &s,CityBasePlan &result,std::string *error) {
  CityBasePlan base;base.settings=s;base.circulation.revision=base.circulation.skeleton.revision=base.relief.revision=s.seed;
  const unsigned columns=s.width>=105?3:2;
  std::vector<double> xs(columns+1);
  for(unsigned c=0;c<=columns;++c)xs[c]=-s.width*.5+s.width*c/columns+(c && c<columns?(roll(s.seed,800+c)-.5)*4:0);
  std::vector<std::vector<double>> zs(columns,std::vector<double>(s.tiers+1));
  for(unsigned c=0;c<columns;++c)for(unsigned row=0;row<=s.tiers;++row)
    zs[c][row]=row*s.tierDepth+(row && row<s.tiers?(roll(s.seed,900+c*20+row)-.5)*8:0);
  std::vector<Patch> patches;
  for(unsigned row=0;row<s.tiers;++row)for(unsigned k=0;k<columns;++k) {
    const unsigned c=row%2?columns-1-k:k;
    patches.push_back({xs[c],xs[c+1],zs[c][row],zs[c][row+1]});
  }
  std::vector<double> shelfLevels(patches.size(),1.);
  for(size_t i=1;i<shelfLevels.size();++i)
    shelfLevels[i]=shelfLevels[i-1]+s.tierRise/columns*(.85+.3*roll(s.seed,1000+i));
  const auto terrain=[s,patches,shelfLevels](double x,double z) {
    const double shape=s.terrain==CityBaseTerrain::Ridge?-.7*(1-std::cos(x*.055)):
      s.terrain==CityBaseTerrain::Valley?-1.3*std::exp(-x*x/500.):0.;
    double height=-1.6+std::max(0.,z)*s.tierRise/s.tierDepth+
      .2*std::sin(x*.05+s.seed*.01)*std::sin(z*.04)+shape;
    // The hillside supports the neighbourhood. Cut only where a planned
    // shelf needs clearance, with a graded bank beyond its retained edge.
    for(size_t i=0;i<patches.size();++i) {
      const auto p=patches[i];
      const double dx=std::max({p.left-x,0.,x-p.right}),dz=std::max({p.front-z,0.,z-p.back});
      height=std::min(height,shelfLevels[i]-.8+.3*std::hypot(dx,dz));
    }
    return height;
  };
  base.relief.openGround=base.relief.ground=terrain;
  std::vector<CityReliefPlan::RaisedBlock> decks;
  double level=1;
  for(size_t i=0;i<patches.size();++i) {
    const auto p=patches[i];
    level=shelfLevels[i];
    CityReliefPlan::RaisedBlock deck;deck.block=100+i;deck.level=level;deck.rockCut=true;deck.stairHalfWidth=1.4;
    const double cut=roll(s.seed,1100+i)>.5?2.:0.;
    deck.boundary=cut?Ring{{p.left+cut,0,p.front},{p.right-cut,0,p.front},{p.right,0,p.front+cut},{p.right,0,p.back-cut},
      {p.right-cut,0,p.back},{p.left+cut,0,p.back},{p.left,0,p.back-cut},{p.left,0,p.front+cut}}:rectangle(p.left,p.front,p.right-p.left,p.back-p.front);
    Position mouth{(p.left+p.right)*.5,0,p.front};double nx=0,nz=1;
    if(i) {
      const auto previous=patches[i-1];
      if(std::abs(previous.right-p.left)<.01){mouth={p.left,0,(std::max(p.front,previous.front)+std::min(p.back,previous.back))*.5};nx=1;nz=0;}
      else if(std::abs(previous.left-p.right)<.01){mouth={p.right,0,(std::max(p.front,previous.front)+std::min(p.back,previous.back))*.5};nx=-1;nz=0;}
    }
    const double approach=i?7.:18.,depth=10.,half=1.4;
    deck.accessFrom={mouth.x-nx*approach,i?decks.back().level:terrain(mouth.x-nx*approach,mouth.z-nz*approach),mouth.z-nz*approach};
    deck.accessTo={mouth.x+nx*depth,level,mouth.z+nz*depth};
    Ring boundary;bool inserted=false;
    for(size_t k=0;k<deck.boundary.size();++k) {
      auto a=deck.boundary[k],b=deck.boundary[(k+1)%deck.boundary.size()];boundary.push_back(a);
      double dx=b.x-a.x,dz=b.z-a.z,len=std::hypot(dx,dz),ux=dx/len,uz=dz/len;
      double along=(mouth.x-a.x)*ux+(mouth.z-a.z)*uz,side=std::abs((mouth.x-a.x)*uz-(mouth.z-a.z)*ux);
      if(side<1e-6 && along>half && along<len-half) {
        Position first{mouth.x-ux*half,0,mouth.z-uz*half},last{mouth.x+ux*half,0,mouth.z+uz*half};
        boundary.insert(boundary.end(),{first,{first.x+nx*depth,0,first.z+nz*depth},{last.x+nx*depth,0,last.z+nz*depth},last});inserted=true;
      }
    }
    if(!inserted){if(error)*error="district edge cannot fit its stair court";return false;}
    deck.boundary=std::move(boundary);decks.push_back(deck);
    base.circulation.skeleton.regions.push_back({deck.block,deck.block,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Merchant,deck.boundary,false,0});
  }
  uint64_t routeId=1,envelopeId=10000;
  for(size_t i=0;i<patches.size();++i) {
    const auto p=patches[i];const auto &deck=decks[i];
    const double bend=s.laneMeanderMetres;
    const double margin=3+bend,half=s.alleyWidth*.5;
    const double x0=p.left+margin,x1=p.right-margin,z0=p.front+margin,z1=p.back-margin;
    const unsigned countX=std::max(1u,unsigned((x1-x0)/(s.lotFrontage+s.alleyWidth+2*bend))),countZ=std::max(1u,unsigned((z1-z0)/(13.+2*bend)));
    const double dx=(x1-x0)/countX,dz=(z1-z0)/countZ;
    const auto route=[&](Position a,Position b) {
      // Split public lanes at stair courts, retaining curved stations for meshing.
      double len=std::hypot(b.x-a.x,b.z-a.z);unsigned n=unsigned(std::ceil(len/1.5));Ring path;
      for(unsigned k=0;k<=n;++k) {
        double t=double(k)/n;Position point{a.x+(b.x-a.x)*t,deck.level,a.z+(b.z-a.z)*t};
        // A shared displacement field preserves lane intersections. It vanishes
        // at the district perimeter; plots reserve its full swept envelope.
        const double u=(point.x-x0)/(x1-x0),v=(point.z-z0)/(z1-z0);
        const double envelope=std::sin(3.141592653589793*u)*std::sin(3.141592653589793*v);
        point.x+=bend*envelope*std::sin(6.283185307179586*v);
        point.z+=bend*envelope*std::sin(6.283185307179586*u);
        bool clear=inRing(deck.boundary,point);
        if(clear)path.push_back(point);
        else {if(path.size()>1){CitySmoothedRoute r;r.id=routeId++;r.path=path;r.halfWidth=half;r.groundCover=CityGroundCover::Stone;base.circulation.routes.push_back(r);}path.clear();}
      }
      if(path.size()>1){CitySmoothedRoute r;r.id=routeId++;r.path=path;r.halfWidth=half;r.groundCover=CityGroundCover::Stone;base.circulation.routes.push_back(r);}
    };
    for(unsigned x=0;x<=countX;++x)route({x0+x*dx,0,z0},{x0+x*dx,0,z1});
    for(unsigned z=0;z<=countZ;++z)route({x0,0,z0+z*dz},{x1,0,z0+z*dz});
    for(unsigned x=0;x<countX;++x)for(unsigned z=0;z<countZ;++z) {
      const double left=x0+x*dx+half+.25+bend,right=x0+(x+1)*dx-half-.25-bend;
      const double front=z0+z*dz+half+.25+bend,back=z0+(z+1)*dz-half-.25-bend;
      if(right-left<7 || back-front<7)continue;
      // Subtract reserved stair rectangles instead of discarding a whole
      // frontage cell when only one corner overlaps its approach or landing.
      std::vector<Patch> available{{left,right,front,back}};
      for(const auto &access:decks) {
        const double reserve=access.stairHalfWidth+1.6;
        const double minX=std::min(access.accessFrom.x,access.accessTo.x)-reserve,maxX=std::max(access.accessFrom.x,access.accessTo.x)+reserve;
        const double minZ=std::min(access.accessFrom.z,access.accessTo.z)-reserve,maxZ=std::max(access.accessFrom.z,access.accessTo.z)+reserve;
        std::vector<Patch> remaining;
        for(auto cell:available) {
          double l=std::max(cell.left,minX),r=std::min(cell.right,maxX),f=std::max(cell.front,minZ),b=std::min(cell.back,maxZ);
          if(l>=r || f>=b){remaining.push_back(cell);continue;}
          remaining.insert(remaining.end(),{{cell.left,l,cell.front,cell.back},{r,cell.right,cell.front,cell.back},
            {l,r,cell.front,f},{l,r,b,cell.back}});
        }
        available=std::move(remaining);
      }
      for(auto cell:available) {
        if(cell.right-cell.left<7 || cell.back-cell.front<7)continue;
        Ring ring=rectangle(cell.left,cell.front,cell.right-cell.left,cell.back-cell.front);bool clear=true;
        for(auto point:ring)clear &= inRing(deck.boundary,point);
        if(clear)base.buildable.push_back({envelopeId++,0,deck.block,CityRegionKind::Reservation,CityLandUse::UrbanBlock,DistrictUse::Merchant,ring,false,(cell.right-cell.left)*(cell.back-cell.front)});
      }
    }
  }
  for(unsigned row=0;row<s.tiers;++row)for(int side:{-1,1}) {
    double x=side*(s.width*.5+5),z=(row+.5)*s.tierDepth;
    base.relief.rocks.push_back({{x,terrain(x,z)-.5,z},3.2,4+6*roll(s.seed,1200+row*2+(side>0)),s.seed+row*2+(side>0)});
  }
  if(!planRaisedCityBlocks(base.circulation.skeleton,decks,{},base.relief,error))return false;
  base.relief.minimumLevel=1;base.relief.maximumLevel=level;base.relief.fallMetres=level-1;
  result=std::move(base);if(error)error->clear();return true;
}
}
