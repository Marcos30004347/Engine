#include "RoofFraming.hpp"

#include <cstdio>
#include <cstdlib>
#include "FacadeAtlas.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace procedural::world::hierarchy {
using rewrite::Position;
namespace {
Position add(Position a,Position b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Position mul(Position a,double s){return {a.x*s,a.y*s,a.z*s};}
Position sub(Position a,Position b){return add(a,mul(b,-1));}
Position cross(Position a,Position b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double norm(Position p){return std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z);}
using Key=std::tuple<int64_t,int64_t,int64_t>;
Key key(Position p){return {std::llround(p.x*10000),std::llround(p.y*10000),std::llround(p.z*10000)};}
void beam(Structure &s,Position a,Position b,double size,uint32_t support,uint32_t owner=0,TimberRole role=TimberRole::Other) {
  auto u=sub(b,a);double length=norm(u);if(length<.05)return;u=mul(u,1/length);
  auto p=cross(u,{0,1,0});if(norm(p)<1e-6)p={1,0,0};p=mul(p,size*.5/norm(p));
  auto q=cross(u,p);q=mul(q,size*.5/norm(q));
  Position corners[4]={add(p,q),sub(q,p),mul(add(p,q),-1),sub(p,q)};
  const uint32_t component=uint32_t(s.faces.size());
  const auto face=[&](std::vector<Position> ring){StructureFace f;f.kind=FaceKind::Timber;f.material=Material::Timber;f.object=owner;f.component=component;f.timberRole=role;f.outline=std::move(ring);f.support=support;if(support!=UINT32_MAX)f.supports={support};s.faces.push_back(std::move(f));};
  std::vector<Position> start,end;for(auto c:corners){start.push_back(add(a,c));end.push_back(add(b,c));}
  std::reverse(start.begin(),start.end());face(start);face(end);
  for(int i=0;i<4;++i)face({add(a,corners[i]),add(a,corners[(i+1)%4]),add(b,corners[(i+1)%4]),add(b,corners[i])});
}
// Subtract opening clearance volumes from a member before building its closed
// prism. Surviving intervals get their own end caps (including partial ribs).
std::vector<std::pair<double,double>> clearSpans(const std::vector<StructureFace> &apertures,Position a,Position b,double size,uint32_t owner) {
  std::vector<std::pair<double,double>> spans{{0,1}};
  const double padding=size*.8660254038+.04;
  for(const auto &wall:apertures) {
    if(wall.object!=owner || wall.apertureBoundary.empty() || wall.outline.size()!=4)continue;
    auto origin=wall.outline[0],u=sub(wall.outline[1],origin);double length=norm(u);if(length<1e-8)continue;u=mul(u,1/length);
    const auto local=[&](Position p){auto d=sub(p,origin);return Position{d.x*u.x+d.z*u.z,d.y,-d.x*u.z+d.z*u.x};};
    auto p=local(a),q=local(b);double lo=0,hi=1;
    const double lower[]={wall.along0*length-padding,wall.low-padding,-wall.thickness-padding-.12};
    const double upper[]={wall.along1*length+padding,wall.high+padding,wall.thickness+padding+.12};
    const double start[]={p.x,p.y,p.z},end[]={q.x,q.y,q.z};bool hits=true;
    for(int axis=0;axis<3;++axis){double d=end[axis]-start[axis];if(std::abs(d)<1e-10){if(start[axis]<lower[axis] || start[axis]>upper[axis])hits=false;continue;}
      double t0=(lower[axis]-start[axis])/d,t1=(upper[axis]-start[axis])/d;if(t0>t1)std::swap(t0,t1);lo=std::max(lo,t0);hi=std::min(hi,t1);}
    if(!hits || lo>=hi)continue;
    std::vector<std::pair<double,double>> remaining;
    for(auto [x,y]:spans){if(y<=lo || x>=hi)remaining.push_back({x,y});else {if(x<lo)remaining.push_back({x,lo});if(y>hi)remaining.push_back({hi,y});}}
    spans=std::move(remaining);
  }
  return spans;
}
void clearBeam(Structure &s,const std::vector<StructureFace> &apertures,Position a,Position b,double size,uint32_t support,uint32_t owner) {
  for(auto [lo,hi]:clearSpans(apertures,a,b,size,owner))beam(s,add(a,mul(sub(b,a),lo)),add(a,mul(sub(b,a),hi)),size,support,owner);
}
void continuousRib(Structure &s,const std::vector<StructureFace> &apertures,const std::vector<Position> &path,Position normal,uint32_t support,uint32_t owner,bool vault=false,double section=.12) {
  std::vector<std::vector<Position>> chains;
  for(size_t i=1;i<path.size();++i)for(auto [lo,hi]:clearSpans(apertures,path[i-1],path[i],section,owner)) {
    auto d=sub(path[i],path[i-1]),a=add(path[i-1],mul(d,lo)),b=add(path[i-1],mul(d,hi));
    if(norm(sub(b,a))<1e-5)continue;
    if(chains.empty() || norm(sub(chains.back().back(),a))>1e-7)chains.push_back({a});
    chains.back().push_back(b);
  }
  for(const auto &chain:chains){std::vector<std::array<Position,4>> rings;
    const uint32_t component=s.faces.size();
    for(size_t i=0;i<chain.size();++i){auto tangent=sub(chain[std::min(i+1,chain.size()-1)],chain[i?i-1:0]);tangent=mul(tangent,1/norm(tangent));
      auto lateral=cross(normal,tangent);lateral=mul(lateral,section*.5/norm(lateral));auto depth=mul(normal,section*.5),p=chain[i];
      rings.push_back({add(p,add(lateral,depth)),add(p,sub(depth,lateral)),sub(p,add(lateral,depth)),add(p,sub(lateral,depth))});}
    const auto face=[&](std::vector<Position> ring){StructureFace f;f.kind=FaceKind::Pier;f.material=Material::Stone;f.engagedArch=!vault;f.masonryVault=vault;f.object=owner;f.support=support;if(support!=UINT32_MAX)f.supports={support};f.component=component;f.outline=std::move(ring);s.faces.push_back(std::move(f));};
    auto a=rings.front(),b=rings.back();face({a[3],a[2],a[1],a[0]});face({b[0],b[1],b[2],b[3]});
    for(size_t i=1;i<rings.size();++i)for(int k=0;k<4;++k)face({rings[i-1][k],rings[i][k],rings[i][(k+1)%4],rings[i-1][(k+1)%4]});
  }
}
// Constant-section masonry ribs: dimensions belong to the structure, not a
// non-uniformly scaled portal asset. Roof planes limit the complete vault.
std::set<uint32_t> vaults(Structure &s,std::vector<StructureFace> &apertures,const rewrite::DwellingPlan &plan) {
  std::set<uint32_t> owners;if(!plan.facade.stoneVaults)return owners;
  const size_t original=s.faces.size();
  for(uint32_t owner=1;owner<s.objects.size();++owner) {
    auto r=s.objects[owner].room;if(r>=plan.rooms.size())continue;
    const auto &room=plan.rooms[r];if(!room.openToRoof || room.boundary.size()!=4)continue;
    auto origin=room.boundary[0],u=sub(room.boundary[1],origin),v=sub(room.boundary[3],origin);
    double width=norm(u),length=norm(v);if(width>length){std::swap(u,v);std::swap(width,length);}
    if(width<3 || length<4)continue;u=mul(u,1/width);v=mul(v,1/length);
    const int aisles=std::max(1,int(std::ceil((width-.4)/3.0)));
    const double span=(width-.4)/aisles;
    const int bays=std::max(1,int(std::ceil((length-.4)/3.0)));
    const double bay=(length-.4)/bays;
    const auto point=[&](double x,double z,double y){auto p=add(origin,add(mul(u,x),mul(v,z)));p.y=y;return p;};
    const auto profile=[&](double x){double d=std::max(x,span-x);return std::sqrt(std::max(0.,span*span-d*d));};
    double spring=1e30;
    for(int lane=0;lane<aisles;++lane)for(int j=0;j<=bays*2;++j)for(int i=0;i<=24;++i) {
      double x=span*i/24.;auto p=point(.2+lane*span+x,.2+bay*j/2.,0);double roofY=1e30;
      for(uint32_t f=0;f<original;++f) {
        const auto &roof=s.faces[f];if(roof.kind!=FaceKind::Roof || roof.outline.size()<3)continue;
        bool pos=false,neg=false;for(size_t k=0;k<roof.outline.size();++k){auto a=roof.outline[k],b=roof.outline[(k+1)%roof.outline.size()];double t=(b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x);pos|=t>1e-7;neg|=t< -1e-7;}
        if(pos&&neg)continue;auto a=roof.outline[0],n=cross(sub(roof.outline[1],a),sub(roof.outline[2],a));if(std::abs(n.y)<1e-8)continue;
        roofY=std::min(roofY,a.y-(n.x*(p.x-a.x)+n.z*(p.z-a.z))/n.y);
      }
      if(roofY==1e30){spring=-1e30;break;}
      spring=std::min(spring,roofY-profile(x)-.25);
    }
    if(spring<origin.y+2.5 || spring>1e20)continue;
    owners.insert(owner);
    // Finalise openings before clipping transition members. Clipping against
    // the old, taller window left missing trim even after its head was lowered.
    std::set<uint32_t> closed;
    for(uint32_t f=0;f<original;++f)if(s.faces[f].object==owner && s.faces[f].kind==FaceKind::WindowedWall && s.faces[f].outline[0].y+s.faces[f].high>spring-.30)closed.insert(s.faces[f].segment);
    for(uint32_t f=0;f<original;++f)if(closed.count(s.faces[f].segment))lowerWindowHead(s,f,spring-.30);
    // Window sills can move as well as heads. Every support must use the
    // final aperture, including the interior side of the revised reveal.
    apertures.clear();
    for(uint32_t f=0;f<original;++f)if(!s.faces[f].apertureBoundary.empty() && s.faces[f].outline.size()==4)apertures.push_back(s.faces[f]);
    const auto stoneBeam=[&](Position a,Position b,double size,bool transition=false){auto start=s.faces.size();clearBeam(s,apertures,a,b,size,UINT32_MAX,owner);for(size_t f=start;f<s.faces.size();++f){s.faces[f].kind=FaceKind::Pier;s.faces[f].material=Material::Stone;s.faces[f].masonryVault=true;s.faces[f].vaultTransition=transition;}};
    const auto shell=[&](std::vector<Position> ring,double arc0,double arc1){StructureFace f;f.kind=FaceKind::Ceiling;f.material=Material::Stone;f.object=owner;f.masonryVault=true;f.outline=ring;
      f.surfaceUVs={{0,arc0},{0,arc1},{length-.4,arc1},{length-.4,arc0}};s.faces.push_back(f);
      for(auto &p:ring)p.y+=.12;std::reverse(ring.begin(),ring.end());std::reverse(f.surfaceUVs.begin(),f.surfaceUVs.end());f.outline=ring;s.faces.push_back(f);};
    for(int lane=0;lane<aisles;++lane) {
      double arc=0;
      for(int i=0;i<24;++i) {
        double x=span*i/24.,next=span*(i+1)/24.;
        auto a=point(.2+lane*span+x,.2,spring+profile(x)),b=point(.2+lane*span+next,.2,spring+profile(next));
        auto c=add(b,mul(v,length-.4)),d=add(a,mul(v,length-.4));double nextArc=arc+norm(sub(b,a));shell({a,b,c,d},arc,nextArc);arc=nextArc;
      }
      // Continuous sweeps share cross-sections instead of overlapping capped
      // beam segments. Longitudinal crowns and diagonal ribs tie each bay.
      for(int j=0;j<=bays;++j) {
        std::vector<Position> path;
        for(int i=0;i<=24;++i){double x=span*i/24.;path.push_back(point(.2+lane*span+x,.2+j*bay,spring+profile(x)-.06));}
        continuousRib(s,apertures,path,v,UINT32_MAX,owner,true);
      }
      continuousRib(s,apertures,{point(.2+(lane+.5)*span,.2,spring+profile(span*.5)-.06),point(.2+(lane+.5)*span,length-.2,spring+profile(span*.5)-.06)},u,UINT32_MAX,owner,true);
      for(int j=0;j<bays;++j)for(int direction:{-1,1}) {
        std::vector<Position> path;
        for(int i=0;i<=24;++i){double t=i/24.,x=t*span,z=.2+(j+(direction==1?t:1-t))*bay;
          path.push_back(point(.2+lane*span+x,z,spring+profile(x)-.065));}
        auto normal=cross(sub(path.back(),path.front()),{0,1,0});normal=mul(normal,1/norm(normal));
        continuousRib(s,apertures,path,normal,UINT32_MAX,owner,true);
      }
      // A small solid boss conceals the converging rib ends at each crown.
      for(int j=0;j<bays;++j)stoneBeam(
        point(.2+(lane+.5)*span,.2+(j+.5)*bay,spring+profile(span*.5)-.17),
        point(.2+(lane+.5)*span,.2+(j+.5)*bay,spring+profile(span*.5)+.02),.22);
    }
    for(int lane=0;lane<=aisles;++lane)for(int j=0;j<=bays;++j) {
      auto foot=point(.2+lane*span,.2+j*bay,origin.y+.12);
      // At doorways, bear on the existing masonry head rather than planting
      // a pier through the circulation opening.
      for(uint32_t f=0;f<original;++f){const auto &wall=s.faces[f];if(wall.object!=owner || wall.apertureBoundary.empty() || wall.kind==FaceKind::WindowedWall)continue;
        auto a=wall.outline[0],axis=sub(wall.outline[1],a);double n=norm(axis);if(n<1e-6)continue;axis=mul(axis,1/n);
        auto d=sub(foot,a);double t=d.x*axis.x+d.z*axis.z,off=std::abs(d.x*axis.z-d.z*axis.x);
        if(off<.4 && t>=wall.along0*n-.14 && t<=wall.along1*n+.14)foot.y=std::max(foot.y,a.y+wall.high+.12);
      }
      if(foot.y<spring) {
        stoneBeam(foot,point(.2+lane*span,.2+j*bay,spring),.24);
        // Stepped impost/capital spreads the narrow shaft into the rib joint.
        // Actual solid thickness avoids a coplanar decorative overlay.
        const double x=.2+lane*span,z=.2+j*bay;
        if(foot.y<spring-.32)stoneBeam(point(x,z,spring-.32),point(x,z,spring-.18),.30,true);
        if(foot.y<spring-.18)stoneBeam(point(x,z,spring-.18),point(x,z,spring+.02),.36,true);
      }
    }
    // Encase the thin shell edge at EVERY aisle boundary, including the
    // exposed seams above internal columns, not just the perimeter walls.
    for(int lane=0;lane<=aisles;++lane) {
      const double x=.2+lane*span;
      stoneBeam(point(x,.2,spring+.04),point(x,length-.2,spring+.04),.18,true);
    }
  }
  return owners;
}
}

void buildRoofFraming(Structure &s,const rewrite::DwellingPlan &plan) {
  if(plan.facade.roofCrests) {
    // Use the realised roof, including hips and clipped wings. An edge is a
    // ridge only when two sloping faces fall away from it on opposite sides.
    struct Edge { Position a,b; uint32_t support; unsigned count=0; };
    std::map<std::pair<Key,Key>,Edge> ridges;
    const size_t roofCount=s.faces.size();
    for(uint32_t f=0;f<roofCount;++f) {
      const auto &face=s.faces[f];
      if(face.kind!=FaceKind::Roof || face.outline.size()<3)continue;
      double high=-1e30,low=1e30;
      for(auto p:face.outline){high=std::max(high,p.y);low=std::min(low,p.y);}
      if(high-low<.6)continue;
      for(size_t k=0;k<face.outline.size();++k) {
        auto a=face.outline[k],b=face.outline[(k+1)%face.outline.size()];
        if(std::abs(a.y-high)>.001 || std::abs(b.y-high)>.001 || norm(sub(a,b))<1.6)continue;
        auto ka=key(a),kb=key(b);if(kb<ka){std::swap(ka,kb);std::swap(a,b);}
        auto &edge=ridges[{ka,kb}];edge.a=a;edge.b=b;edge.support=f;++edge.count;
      }
    }
    unsigned count=0;
    for(const auto &[unused,edge]:ridges) {
      if(edge.count!=2 || count++>=4)continue;
      auto a=edge.a,b=edge.b;auto direction=sub(b,a);const double length=norm(direction);direction=mul(direction,1/length);
      a=add(a,mul(direction,.3));b=add(b,mul(direction,-.3));a.y+=.06;b.y+=.06;
      beam(s,a,b,.16,edge.support,0,TimberRole::Ridge);
      for(auto foot:{a,b}) {
        beam(s,add(foot,{0,-.08,0}),add(foot,{0,.16,0}),.28,edge.support,0,TimberRole::Ridge);
        beam(s,add(foot,{0,.12,0}),add(foot,{0,.7,0}),.1,edge.support,0,TimberRole::Ridge);
        beam(s,add(foot,{0,.44,0}),add(foot,{0,.55,0}),.2,edge.support,0,TimberRole::Ridge);
      }
    }
  }
  // Clearance volumes do not change while ribs are appended. Scan only
  // those volumes, not the ever-growing decorative mesh for every rib segment.
  std::vector<StructureFace> apertures;
  for(const auto &face:s.faces)if(!face.apertureBoundary.empty() && face.outline.size()==4)apertures.push_back(face);
  const auto vaulted=vaults(s,apertures,plan);
  if(plan.facade.castleInterior) {
    // Engaged arches reinforce solid walls without making circulation holes.
    // They belong to the room-facing skin and retain its support reference.
    const size_t original=s.faces.size();
    for(uint32_t f=0;f<original;++f) {
      const auto wall=s.faces[f];
      if(wall.object==0 || wall.kind!=FaceKind::Wall || wall.segment==UINT32_MAX || wall.outline.size()!=4)continue;
      auto a=wall.outline[0],axis=sub(wall.outline[1],a);double length=norm(axis);
      double height=wall.outline[3].y-a.y;
      if(length<1.4 || height<2.7)continue;axis=mul(axis,1/length);
      if(plan.facade.corridorSupports && !vaulted.count(wall.object)) {
        const auto r=s.objects[wall.object].room;
        if(r<plan.rooms.size()) {const auto &room=plan.rooms[r];
          if(room.kind==rewrite::RoomKind::GreatHall || room.kind==rewrite::RoomKind::Gallery || room.kind==rewrite::RoomKind::Passage || room.use==rewrite::RoomUse::Corridor)
            height=(height-.28)*.5+.10; // crown meets the main pointed arch spring
        }
      }
      const int bays=std::max(1,int(std::ceil(length/2.4)));
      const double width=length/bays;
      const double span=width-.28,rise=span*std::sqrt(3.)*.5,spring=a.y+height-.16-rise;
      if(spring<a.y+.8)continue;
      s.faces[f].detailedOpening=true;
      s.faces[f].wallPanels.clear();
      const Position normal{-axis.z,0,axis.x};
      const auto stoneMember=[&](Position from,Position to,double size){
        const size_t first=s.faces.size();
        clearBeam(s,apertures,from,to,size,f,wall.object);
        for(size_t i=first;i<s.faces.size();++i){auto &face=s.faces[i];face.kind=FaceKind::Pier;
          face.material=Material::Stone;face.engagedArch=true;}
      };
      const auto panel=[&](Position p,Position q,Position r,Position t){s.faces[f].wallPanels.push_back({p,q,r,t});};
      for(int bay=0;bay<bays;++bay) {
        // The room-side skin is a sequence of real arched bays. A shallow
        // recessed infill and short stone returns meet the spandrels; the
        // supporting wall behind remains closed and load-bearing.
        const auto point=[&](double x,double y,double depth){auto p=add(a,add(mul(axis,bay*width+.14+x),mul(normal,depth)));p.y=y;return p;};
        const auto curve=[&](double x){double d=std::max(x,span-x);return spring+std::sqrt(std::max(0.,span*span-d*d));};
        panel(point(-.14,a.y,0),point(0,a.y,0),point(0,wall.outline[3].y,0),point(-.14,wall.outline[3].y,0));
        panel(point(span,a.y,0),point(span+.14,a.y,0),point(span+.14,wall.outline[3].y,0),point(span,wall.outline[3].y,0));
        for(int i=0;i<24;++i){
          const double x=span*i/24.,next=span*(i+1)/24.,y0=curve(x),y1=curve(next);
          panel(point(x,y0,0),point(next,y1,0),point(next,wall.outline[3].y,0),point(x,wall.outline[3].y,0));
          panel(point(x,a.y,-.09),point(next,a.y,-.09),point(next,y1,-.09),point(x,y0,-.09));
          panel(point(x,y0,0),point(x,y0,-.09),point(next,y1,-.09),point(next,y1,0));
        }
        auto origin=point(0,0,.04);
        const auto ribPoint=[&](double x,double y){auto p=add(origin,mul(axis,x));p.y=y;return p;};
        std::vector<Position> path;
        for(int i=0;i<=48;++i){double x=span*i/48.;
          path.push_back(ribPoint(x,curve(x)));}
        continuousRib(s,apertures,path,normal,f,wall.object,false,.20);
        for(double x:{0.,span}) {
          auto centre=ribPoint(x,0);
          stoneMember(Position{centre.x, a.y+.04,centre.z},Position{centre.x,spring+.04,centre.z},.24);
          stoneMember(Position{centre.x,a.y+.04,centre.z},Position{centre.x,a.y+.24,centre.z},.32);
          stoneMember(Position{centre.x,spring-.16,centre.z},Position{centre.x,spring+.10,centre.z},.34);
        }
      }
    }
  }
  if(plan.facade.timberFacade && plan.facade.timberFrameMembers) {
    // Framing follows a wall RUN, not a wall face.
    //
    // The cell model emits one wall face per lattice edge, and a city house is
    // planned on a one metre cell: a post on both ends of every face put a
    // post every metre, four hundred members on one cottage, and the facade
    // read as a brown palisade rather than as a frame with panels in it. The
    // faces are chained into runs first, and the run carries its sill and head
    // plate whole with posts at a real bay spacing.
    const double size=.08;
    const double bay=std::max(1.2,plan.facade.timberBayMetres);
    const auto count=s.faces.size();

    struct Member { uint32_t face; Position a,b,u; double length; };
    std::vector<Member> members;

    for(uint32_t f=0;f<count;++f) {
      const auto &wall=s.faces[f];
      if(wall.object!=0 || wall.segment==UINT32_MAX || wall.outline.size()!=4 ||
         (wall.kind!=FaceKind::Wall && wall.kind!=FaceKind::WindowedWall && wall.kind!=FaceKind::DoorWall && wall.kind!=FaceKind::DoubleDoorWall))continue;
      if(wall.outline[0].y<plan.facade.stoneBaseHeight-1e-6)continue;
      auto a=wall.outline[0],b=wall.outline[1];auto u=sub(b,a);double length=norm(u);if(length<.3)continue;u=mul(u,1/length);
      members.push_back({f,a,b,u,length});
    }

    // Chain the members into runs: same line, same storey, end to end.
    std::vector<uint8_t> used(members.size(),0);
    std::vector<std::vector<size_t>> runs;

    for(size_t i=0;i<members.size();++i) {
      if(used[i])continue;
      std::vector<size_t> run{i};used[i]=1;
      for(bool grew=true;grew;) {
        grew=false;
        for(size_t j=0;j<members.size() && !grew;++j) {
          if(used[j])continue;
          const auto &m=members[j];
          for(int end=0;end<2 && !grew;++end) {
            const size_t at=end?run.back():run.front();
            const auto &k=members[at];
            const bool aligned=std::abs(k.u.x*m.u.x+k.u.z*m.u.z)>.999 &&
              std::abs(s.faces[k.face].outline[0].y-s.faces[m.face].outline[0].y)<.01 &&
              std::abs(s.faces[k.face].outline[3].y-s.faces[m.face].outline[3].y)<.01 &&
              s.faces[k.face].segment!=UINT32_MAX;
            if(!aligned)continue;
            const bool joins=end ? norm(sub(k.b,m.a))<.01 : norm(sub(k.a,m.b))<.01;
            if(!joins)continue;
            if(end)run.push_back(j); else run.insert(run.begin(),j);
            used[j]=1;grew=true;
          }
        }
      }
      runs.push_back(std::move(run));
    }

    if(std::getenv("FRAME_DEBUG")) {
      std::fprintf(stderr,"timber frame: %zu wall faces, %zu runs:",members.size(),runs.size());
      for(const auto &run:runs)std::fprintf(stderr," %zu",run.size());
      std::fprintf(stderr,"\n");
    }

    // The gables, framed like the walls under them.
    //
    // A gable is the triangle that closes a roof end, and the facade framing
    // skipped it - it frames wall faces, and a gable is its own kind. So a
    // timber-framed house finished in a blank plaster triangle the size of
    // its roof, which is the pale wedge that reads as a hole in the
    // roofscape. It gets what a gable has: a tie across its foot, a post up
    // the middle to the apex, and a raker following each slope.
    for(uint32_t f=0;f<count;++f) {
      const auto &gable=s.faces[f];
      if(gable.object!=0 || gable.kind!=FaceKind::Gable || gable.outline.size()<3)continue;

      // Its foot is the lowest edge; its apex the highest corner.
      double lowest=1e30,highest=-1e30;
      for(const auto &p:gable.outline){lowest=std::min(lowest,p.y);highest=std::max(highest,p.y);}
      if(highest-lowest<.8)continue;

      std::vector<Position> foot;
      Position apex=gable.outline.front();
      for(const auto &p:gable.outline) {
        if(p.y<lowest+.05)foot.push_back(p);
        if(p.y>apex.y)apex=p;
      }
      if(foot.size()<2)continue;

      // The widest pair of foot corners is the span.
      Position a=foot.front(),b=foot.back();
      double span=0;
      for(size_t i=0;i<foot.size();++i)for(size_t j=i+1;j<foot.size();++j) {
        const double d=std::hypot(foot[j].x-foot[i].x,foot[j].z-foot[i].z);
        if(d>span){span=d;a=foot[i];b=foot[j];}
      }
      if(span<1.2)continue;

      // Set back into the plane, so the frame sits in the gable and not on it.
      auto n=cross(sub(gable.outline[1],gable.outline[0]),sub(gable.outline[2],gable.outline[0]));
      if(norm(n)<1e-8)continue;
      n=mul(n,1/norm(n));
      const double set=size*.5;
      const auto into=[&](Position p){return sub(p,mul(n,set));};

      const Position tieA=into(a),tieB=into(b),head=into(apex);
      const Position middle{(tieA.x+tieB.x)*.5,tieA.y,(tieA.z+tieB.z)*.5};

      beam(s,tieA,tieB,size,f,0,TimberRole::TieBeam);
      beam(s,middle,head,size,f,0,TimberRole::KingPost);

      // A raker either side, and a strut halfway up each where the gable is
      // tall enough to want one.
      for(const Position &corner:{tieA,tieB}) {
        beam(s,corner,head,size,f,0,TimberRole::Rafter);

        if(highest-lowest>2.4) {
          const Position half{(corner.x+head.x)*.5,(corner.y+head.y)*.5,(corner.z+head.z)*.5};
          const Position under{half.x,tieA.y,half.z};
          beam(s,under,half,size*.85,f,0,TimberRole::KingPost);
        }
      }
    }

    for(const auto &run:runs) {
      const auto &first=members[run.front()];
      const auto &last=members[run.back()];
      const auto u=first.u;
      Position inward{u.z,0,-u.x};
      auto a=add(first.a,mul(inward,size*.5));
      auto b=add(last.b,mul(inward,size*.5));
      const double length=norm(sub(b,a));
      if(length<.3)continue;
      const double top=s.faces[first.face].outline[3].y-size*.5;

      // The posts: both ends, a stud either side of every opening, and whole
      // bays in between.
      //
      // Spacing them evenly over the run put a post through the middle of a
      // window, because the frame was laid out without asking where the
      // openings are. A frame is not laid out that way: the studs are what
      // the window is framed BY, so the jambs are stations and nothing is
      // posted between them.
      std::vector<std::pair<double,double>> holes;

      for(size_t k=0;k<run.size();++k) {
        const auto &m=members[run[k]];
        const auto &wall=s.faces[m.face];
        if(wall.along1<=wall.along0)continue;
        const double offset=(m.a.x-first.a.x)*u.x+(m.a.z-first.a.z)*u.z;
        holes.push_back({offset+wall.along0*m.length,offset+wall.along1*m.length});
      }

      std::vector<double> stations{0.,length};
      for(const auto &[h0,h1]:holes) {
        if(h0>size && h0<length-size)stations.push_back(h0);
        if(h1>size && h1<length-size)stations.push_back(h1);
      }
      std::sort(stations.begin(),stations.end());

      std::vector<double> posts;
      for(size_t k=0;k+1<stations.size();++k) {
        if(posts.empty() || stations[k]-posts.back()>size)posts.push_back(stations[k]);
        const double gap=stations[k+1]-stations[k];
        const double middle=(stations[k]+stations[k+1])*.5;
        bool glazed=false;
        for(const auto &[h0,h1]:holes)glazed=glazed || (middle>h0 && middle<h1);
        if(glazed)continue;
        const uint32_t fill=uint32_t(std::llround(gap/bay));
        for(uint32_t j=1;j<fill;++j)posts.push_back(stations[k]+gap*double(j)/double(fill));
      }
      if(posts.empty() || length-posts.back()>size)posts.push_back(length);

      const auto at=[&](double along,double y){auto p=add(a,mul(u,std::clamp(along,size*.5,length-size*.5)));p.y=y;return p;};
      for(double station:posts) {
        auto foot=at(station,a.y+size*.5),head=at(station,top);
        beam(s,foot,head,size,first.face);
      }

      // ...and the plates, whole: a sill under the run (broken only where a
      // door stands in it) and a head plate over it.
      auto sillA=at(0,a.y+size*.5),sillB=at(length,a.y+size*.5);
      auto headA=at(0,top),headB=at(length,top);
      beam(s,headA,headB,size,first.face);
      for(size_t k=0;k<run.size();++k) {
        const auto &m=members[run[k]];
        const auto kind=s.faces[m.face].kind;
        if(kind==FaceKind::DoorWall || kind==FaceKind::DoubleDoorWall)continue;
        auto p=add(m.a,mul(inward,size*.5)),q=add(m.b,mul(inward,size*.5));
        p.y=q.y=a.y+size*.5;
        beam(s,p,q,size,m.face);
      }
      (void)sillA;(void)sillB;

      // The infill of each face in the run, clipped to the frame that is
      // actually there: the plates above and below, and any post crossing it.
      for(size_t k=0;k<run.size();++k) {
        const auto &m=members[run[k]];
        auto &wall=s.faces[m.face];
        const double offset=(m.a.x-first.a.x)*u.x+(m.a.z-first.a.z)*u.z;
        auto panels=wall.detailedOpening ? wall.wallPanels : std::vector<std::vector<Position>>{wall.outline};
        const auto along=[&](Position p){auto d=sub(p,wall.outline[0]);return d.x*u.x+d.z*u.z;};
        const auto area=[&](const auto &polygons){double result=0;for(const auto &ring:polygons){double a=0;for(size_t i=0;i<ring.size();++i){auto p=ring[i],q=ring[(i+1)%ring.size()];a+=along(p)*q.y-along(q)*p.y;}result+=std::abs(a)*.5;}return result;};
        const double beforeArea=area(panels);
        const bool door=wall.kind==FaceKind::DoorWall || wall.kind==FaceKind::DoubleDoorWall;

        // Plates first, the whole run's height.
        for(int edge=2;edge<4;++edge)for(auto &ring:panels) {
          if(ring.empty())continue;
          const auto distance=[&](Position p){return edge==2?p.y-wall.outline[0].y-(door?0.:size):wall.outline[3].y-size-p.y;};
          std::vector<Position> clipped;auto previous=ring.back();double before=distance(previous);
          for(auto current:ring){double after=distance(current);if((before>=0)!=(after>=0))clipped.push_back(add(previous,mul(sub(current,previous),before/(before-after))));if(after>=0)clipped.push_back(current);previous=current;before=after;}
          ring=std::move(clipped);
        }

        // ...then split the panel at every post that crosses this face.
        for(double station:posts) {
          const double local=station-offset;
          if(local<-size || local>m.length+size)continue;
          std::vector<std::vector<Position>> split;
          for(const auto &ring:panels) {
            if(ring.size()<3){split.push_back(ring);continue;}
            for(int side=0;side<2;++side) {
              const auto distance=[&](Position p){return side?along(p)-(local+size*.5):(local-size*.5)-along(p);};
              std::vector<Position> clipped;auto previous=ring.back();double before=distance(previous);
              for(auto current:ring){double after=distance(current);if((before>=0)!=(after>=0))clipped.push_back(add(previous,mul(sub(current,previous),before/(before-after))));if(after>=0)clipped.push_back(current);previous=current;before=after;}
              if(clipped.size()>=3)split.push_back(std::move(clipped));
            }
          }
          panels=std::move(split);
        }

        panels.erase(std::remove_if(panels.begin(),panels.end(),[](const auto &p){return p.size()<3;}),panels.end());
        wall.timberInfillArea=beforeArea-area(panels);
        wall.material=Material::Wattle;
        wall.wallPanels=std::move(panels);wall.detailedOpening=true;
      }
    }
  }
  if(!plan.facade.roofFraming && !plan.facade.corridorSupports)return;
  const size_t original=s.faces.size();
  if(plan.facade.roofFraming) {
    // No rafters, purlins or ridge pieces.
    //
    // The covering and the frame are two solutions of the same roof, built by
    // different code from the same numbers, and they agree only where the roof
    // does nothing. Every hip, valley and dormer put a stick through the tiles
    // or left one hanging in the air, and each was 2827 members per gallery -
    // six faces each, seventeen thousand polygons - to be wrong in.
    //
    // A roof plane is now the roof. What remains below is the ceiling frame,
    // which is bounded by a ceiling and cannot escape through the covering.
    // Joists under flat ceilings, and low tie beams under open roofs. Their
    // ends bear inside the wall depth, not in free space at the room centre.
    for(uint32_t owner=1;owner<s.objects.size();++owner) {
      if(vaulted.count(owner))continue;
      auto r=s.objects[owner].room;if(r>=plan.rooms.size())continue;const auto &room=plan.rooms[r];
      // Room boundaries retain shared cell nodes for watertight floors. A
      // rectangular room can therefore have more than four boundary points.
      std::vector<Position> corners;
      for(size_t i=0;i<room.boundary.size();++i) {
        const auto a=room.boundary[(i+room.boundary.size()-1)%room.boundary.size()],b=room.boundary[i],c=room.boundary[(i+1)%room.boundary.size()];
        if(std::abs((b.x-a.x)*(c.z-b.z)-(b.z-a.z)*(c.x-b.x))>1e-8)corners.push_back(b);
      }
      if(corners.size()!=4)continue;
      auto origin=corners[0],u=sub(corners[1],origin),v=sub(corners[3],origin);
      double width=norm(u),length=norm(v);if(width>length){std::swap(u,v);std::swap(width,length);}
      if(width<1 || length<1.5)continue;u=mul(u,1/width);v=mul(v,1/length);
      const double top=room.ceilingHeight>0?room.ceilingHeight:origin.y+(room.floor<plan.floors.size()?plan.floors[room.floor].height:3.);
      uint32_t support=UINT32_MAX;double underside=top;
      for(uint32_t f=0;f<original;++f)if(s.faces[f].object==owner && s.faces[f].kind==FaceKind::Ceiling) {
        if(support==UINT32_MAX)support=f;
        for(auto p:s.faces[f].outline)underside=std::min(underside,p.y);
      }
      const double beamLevel=room.openToRoof?top-.20:underside-.08;
      if(beamLevel-.08-origin.y<2.15)continue;
      const double spacing=room.openToRoof?1.5:.75;
      for(double station=spacing;station<length-.3;station+=spacing) {
        auto a=add(origin,add(mul(v,station),mul(u,.1))),b=add(a,mul(u,width-.2));a.y=b.y=beamLevel;
        // Preserve the vertical volume of stair wells: require a ceiling
        // tile under this station, unless the room is explicitly open-roof.
        bool clear=true;
        if(!room.openToRoof)for(int sample=0;sample<=12;++sample)for(double side:{-.08,.08}) {
          // Beam ends bear inside the walls; validate only the exposed span.
          auto p=add(origin,add(mul(v,station+side),mul(u,.24+(width-.48)*sample/12.)));bool covered=false;
          for(const auto &tile:plan.ceilings){if(tile.size()<3 || std::abs(tile[0].y-top)>1e-5)continue;
            bool positive=false,negative=false;for(size_t i=0;i<tile.size();++i){auto x=tile[i],z=tile[(i+1)%tile.size()];double turn=(z.x-x.x)*(p.z-x.z)-(z.z-x.z)*(p.x-x.x);positive|=turn>1e-7;negative|=turn< -1e-7;}
            if(!(positive&&negative)){covered=true;break;}}
          if(!covered){clear=false;break;}
        }
        if(clear)beam(s,a,b,.16,support,owner,room.openToRoof?TimberRole::TieBeam:TimberRole::CeilingJoist);
      }
    }
  }
  if(!plan.facade.corridorSupports)return;
  for(uint32_t owner=1;owner<s.objects.size();++owner) {
    if(vaulted.count(owner))continue;
    auto r=s.objects[owner].room;if(r>=plan.rooms.size())continue;const auto &room=plan.rooms[r];
    if(room.use!=rewrite::RoomUse::Corridor && room.kind!=rewrite::RoomKind::Gallery && room.kind!=rewrite::RoomKind::Passage &&
       room.kind!=rewrite::RoomKind::Nave && room.kind!=rewrite::RoomKind::Chancel && room.kind!=rewrite::RoomKind::GreatHall)continue;
    if(room.boundary.size()!=4)continue;
    auto origin=room.boundary[0],u=sub(room.boundary[1],origin),v=sub(room.boundary[3],origin);
    double width=norm(u),length=norm(v);if(width>length){std::swap(u,v);std::swap(width,length);}
    if(width<2.8 || length<5)continue;u=mul(u,1/width);v=mul(v,1/length);
    const double top=room.ceilingHeight>0?room.ceilingHeight:origin.y+(room.floor<plan.floors.size()?plan.floors[room.floor].height:3.);
    const double spacing=plan.facade.castleInterior?2.4:3.;
    bool shellBuilt=false;
    for(double station=spacing;station<length-1;station+=spacing) {
      bool blocked=false;
      for(uint32_t f=0;f<original;++f){const auto &wall=s.faces[f];if(wall.object!=owner || wall.apertureBoundary.empty())continue;
        double lo=1e30,hi=-1e30;for(auto p:wall.apertureBoundary){auto d=sub(p,origin);double t=d.x*v.x+d.z*v.z;lo=std::min(lo,t);hi=std::max(hi,t);}
        if(station>=lo-.16 && station<=hi+.16){blocked=true;break;}}
      if(blocked)continue;
      FacadeInstance arch;arch.asset=FacadeAsset::ArcadeArch;
      arch.shape=plan.facade.stoneVaults || plan.facade.style==rewrite::FacadeStyle::Gothic ? ApertureShape::PointedArch : ApertureShape::RoundArch;
      arch.origin=add(origin,add(mul(v,station),mul(u,.3)));arch.u=u;arch.normal=v;
      arch.width=width-.6;arch.height=top-origin.y-.28;arch.depth=.22;arch.inside=true;
      arch.origin=add(arch.origin,mul(v,-arch.depth*.5));
      for(uint32_t f=0;f<original;++f)if(s.faces[f].object==owner && s.faces[f].kind==FaceKind::Ceiling){arch.supportFace=f;break;}
      if(arch.height*.92<2.4)continue;
      s.instances.push_back(arch);
      if(plan.facade.castleInterior && !shellBuilt) {
        // A solid stone shell follows the very same contour as the ribs,
        // spanning between them rather than exposing the tiled roof inside.
        const double spring=origin.y+arch.height*(arch.shape==ApertureShape::PointedArch?.5:.55);
        bool blockedShell=false;
        for(uint32_t f=0;f<original;++f){const auto &wall=s.faces[f];if(wall.object==owner && !wall.apertureBoundary.empty() && wall.kind!=FaceKind::WindowedWall && wall.outline[0].y+wall.high>spring-.05)blockedShell=true;}
        if(!blockedShell) {
          const auto ring=apertureContour(arch.shape);
          const auto point=[&](Position p,double station,double offset){auto q=add(origin,add(mul(u,.3+p.x*arch.width),mul(v,station)));q.y=origin.y+p.y*arch.height+offset;return q;};
          const auto surface=[&](std::vector<Position> polygon){StructureFace face;face.kind=FaceKind::Ceiling;face.material=Material::Stone;face.object=owner;face.masonryVault=true;face.outline=std::move(polygon);s.faces.push_back(std::move(face));};
          for(size_t i=2;i+1<ring.size();++i) {
            auto a=ring[i],b=ring[i+1];
            auto a0=point(a,.2,.02),b0=point(b,.2,.02),a1=point(a,length-.2,.02),b1=point(b,length-.2,.02);
            auto A0=a0,B0=b0,A1=a1,B1=b1;A0.y+=.12;B0.y+=.12;A1.y+=.12;B1.y+=.12;
            surface({a0,b0,b1,a1});surface({A1,B1,B0,A0});
            surface({a0,A0,B0,b0});surface({b1,B1,A1,a1});
          }
          // Close narrow strips to the inner wall skins at both spring lines.
          for(int side=0;side<2;++side){double x=side?width-.3:.3,edge=side?width-.2:.2;
            auto a=add(origin,add(mul(u,x),mul(v,.2))),b=add(origin,add(mul(u,edge),mul(v,.2)));a.y=b.y=spring+.02;
            auto c=add(b,mul(v,length-.4)),d=add(a,mul(v,length-.4));surface({a,b,c,d});}
          for(uint32_t f=0;f<original;++f)if(s.faces[f].object==owner && s.faces[f].kind==FaceKind::WindowedWall) {
            auto segment=s.faces[f].segment;
            for(uint32_t k=0;k<original;++k)if(s.faces[k].segment==segment && s.faces[k].kind==FaceKind::WindowedWall)lowerWindowHead(s,k,spring-.15);
          }
        }
        shellBuilt=true;
      }
      for(int side=0;side<2;++side){auto post=arch;post.asset=FacadeAsset::Column;post.shape=ApertureShape::Rectangle;post.width=arch.width*.08;post.height=arch.height*(arch.shape==ApertureShape::PointedArch?.5:.55);
        post.origin=add(arch.origin,mul(u,side?arch.width-post.width:0));s.instances.push_back(post);}
      if(room.openToRoof && plan.facade.roofFraming) {
        auto crown=add(arch.origin,mul(u,arch.width*.5));crown.y+=arch.height;
        for(uint32_t f=0;f<original;++f) {
          const auto &roof=s.faces[f];if(roof.kind!=FaceKind::Roof || roof.outline.size()<3)continue;
          bool positive=false,negative=false;
          for(size_t i=0;i<roof.outline.size();++i){auto a=roof.outline[i],b=roof.outline[(i+1)%roof.outline.size()];
            double side=(b.x-a.x)*(crown.z-a.z)-(b.z-a.z)*(crown.x-a.x);positive|=side>1e-7;negative|=side< -1e-7;}
          if(positive && negative)continue;
          auto a=roof.outline[0],n=cross(sub(roof.outline[1],a),sub(roof.outline[2],a));if(std::abs(n.y)<1e-8)continue;
          auto head=crown;head.y=a.y-(n.x*(crown.x-a.x)+n.z*(crown.z-a.z))/n.y-.16;
          if(head.y>crown.y+.2){beam(s,crown,head,.16,f,owner,TimberRole::KingPost);break;}
        }
      }
    }
  }
}
}
