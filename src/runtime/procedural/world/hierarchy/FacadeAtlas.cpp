#include "procedural/world/hierarchy/FacadeAtlas.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace procedural::world::hierarchy {
using rewrite::Position;

std::vector<Position> apertureContour(ApertureShape shape,double archSpring)
{
  constexpr double pi=3.14159265358979323846;
  if(shape==ApertureShape::Rectangle) return {{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
  // Low poly, and the count is load bearing.
  //
  // The contour is not only the hole's outline: `wallPanels` bands the wall
  // into one horizontal strip per distinct y ON IT, so a 24-segment arch cuts
  // the wall around it into sixteen strips a centimetre or two tall, per
  // skin, per opening. Measured over the gallery that was 4310 of 10881 wall
  // quads under 30 cm in a dimension - the small rectangles all over a
  // facade. Eight segments read as an arch at the distance a house is seen
  // from and cost a quarter of the geometry.
  constexpr int kArchSteps=8, kCircleSteps=12;
  std::vector<Position> ring;
  if(shape==ApertureShape::Circle) {
    for(int i=0;i<kCircleSteps;++i) {double a=2*pi*i/kCircleSteps;ring.push_back({.5+.5*std::cos(a),.5+.5*std::sin(a),0});}
    return ring;
  }
  ring={{0,0,0},{1,0,0}};
  for(int i=0;i<=kArchSteps;++i) {
    double t=double(i)/kArchSteps,x=1-t,y;
    if(shape==ApertureShape::RoundArch || shape==ApertureShape::SegmentalArch) {double spring=archSpring>=0?archSpring:(shape==ApertureShape::SegmentalArch?rewrite::kPassageArchSpring:.55);x=.5+.5*std::cos(pi*t);y=spring+(1-spring)*std::sin(pi*t);}
    else {double d=x>=.5 ? x : 1-x;double spring=archSpring>=0?archSpring:.5;y=spring+(1-spring)*std::sqrt(std::max(0.,1-d*d))/std::sqrt(.75);}
    ring.push_back({x,y,0});
  }
  return ring;
}

namespace {
// Clip a roof polygon against the aperture's local clearance volume. This
// operates on structure polygons, before any atlas triangles are emitted.
bool roofInOpening(const StructureFace &wall, const StructureFace &roof)
{
  if (wall.outline.size()!=4 || roof.outline.size()<3) return false;
  const auto a=wall.outline[0], b=wall.outline[1];
  const double length=std::hypot(b.x-a.x,b.z-a.z);
  if(length<1e-8)return false;
  const double ux=(b.x-a.x)/length, uz=(b.z-a.z)/length;
  std::vector<Position> polygon;
  for(auto p:roof.outline)polygon.push_back({(p.x-a.x)*ux+(p.z-a.z)*uz,p.y-a.y,-(p.x-a.x)*uz+(p.z-a.z)*ux});
  const double lower[3]={wall.along0*length-.08,wall.low-.08,-wall.thickness-.1};
  const double upper[3]={wall.along1*length+.08,wall.high+.08,wall.thickness+.1};
  const auto coord=[](Position p,int axis){return axis==0?p.x:axis==1?p.y:p.z;};
  for(int axis=0;axis<3;++axis)for(int side=0;side<2;++side) {
    if(polygon.empty())return false;
    std::vector<Position> clipped;
    auto previous=polygon.back();
    auto distance=[&](Position p){return side?upper[axis]-coord(p,axis):coord(p,axis)-lower[axis];};
    double dp=distance(previous);
    for(auto current:polygon) {
      double dc=distance(current);
      if((dp>=0)!=(dc>=0)) {
        double t=dp/(dp-dc);
        clipped.push_back({previous.x+t*(current.x-previous.x),previous.y+t*(current.y-previous.y),previous.z+t*(current.z-previous.z)});
      }
      if(dc>=0)clipped.push_back(current);
      previous=current;dp=dc;
    }
    polygon=std::move(clipped);
  }
  return !polygon.empty();
}

void aperturePanels(StructureFace &face)
{
  face.wallPanels.clear();face.openingReveals.clear();face.apertureBoundary.clear();
  const auto a=face.outline[0],b=face.outline[1];
  const double base=a.y,top=face.outline[3].y,low=base+face.low,high=base+face.high;
  const double t0=face.along0,t1=face.along1,length=std::hypot(b.x-a.x,b.z-a.z);
  const auto at=[&](double t,double y){return Position{a.x+(b.x-a.x)*t,y,a.z+(b.z-a.z)*t};};
  const auto panel=[&](double from,double to,double y0,double y1){if(y1-y0>1e-8 && to-from>1e-8)face.wallPanels.push_back({at(from,y0),at(to,y0),at(to,y1),at(from,y1)});};
  auto ring=apertureContour(face.aperture,face.archSpring);
  std::vector<double> rows{0,1};for(auto p:ring)rows.push_back(p.y);
  std::sort(rows.begin(),rows.end());rows.erase(std::unique(rows.begin(),rows.end(),[](double a,double b){return std::abs(a-b)<1e-8;}),rows.end());
  const auto bounds=[&](double y){
    double left=1,right=0;
    for(size_t i=0;i<ring.size();++i){auto p=ring[i],q=ring[(i+1)%ring.size()];
      if(y<std::min(p.y,q.y)-1e-8 || y>std::max(p.y,q.y)+1e-8)continue;
      if(std::abs(q.y-p.y)<1e-8){left=std::min(left,std::min(p.x,q.x));right=std::max(right,std::max(p.x,q.x));}
      else {double x=p.x+(q.x-p.x)*(y-p.y)/(q.y-p.y);left=std::min(left,x);right=std::max(right,x);}
    }
    return std::pair<double,double>{t0+(t1-t0)*left,t0+(t1-t0)*right};
  };
  panel(0,1,base,low);panel(0,1,high,top);
  for(size_t i=1;i<rows.size();++i){auto [l0,r0]=bounds(rows[i-1]);auto [l1,r1]=bounds(rows[i]);
    double y0=low+(high-low)*rows[i-1],y1=low+(high-low)*rows[i];
    if(l0+l1>1e-8)face.wallPanels.push_back({at(0,y0),at(l0,y0),at(l1,y1),at(0,y1)});
    if(2-r0-r1>1e-8)face.wallPanels.push_back({at(r0,y0),at(1,y0),at(1,y1),at(r1,y1)});
  }
  for(auto p:ring)face.apertureBoundary.push_back(at(t0+(t1-t0)*p.x,low+(high-low)*p.y));
  const auto through=[&](Position p){p.x+=(b.z-a.z)/length*face.thickness;p.z-=(b.x-a.x)/length*face.thickness;return p;};
  for(size_t i=0;i<ring.size();++i){auto p=face.apertureBoundary[i],q=face.apertureBoundary[(i+1)%ring.size()];
    if(face.low<1e-8 && ring[i].y<1e-8 && ring[(i+1)%ring.size()].y<1e-8)continue;
    face.openingReveals.push_back({p,q,through(q),through(p)});
  }
}

std::vector<FacadeTriangle> makeAsset(FacadeAsset asset,ApertureShape shape,double archSpring=-1)
{
  std::vector<FacadeTriangle> out;
  const auto quad=[&](Position a,Position b,Position c,Position d){out.push_back({a,b,c});out.push_back({a,c,d});};
  const auto box=[&](double x0,double y0,double x1,double y1){
    if(asset==FacadeAsset::ColumnLeftHalf) {x0*=2;x1=std::min(x1,.5)*2;}
    if(asset==FacadeAsset::ColumnRightHalf) {x0=(std::max(x0,.5)-.5)*2;x1=(x1-.5)*2;}
    Position a{x0,y0,0},b{x1,y0,0},c{x1,y1,0},d{x0,y1,0};
    Position e{x0,y0,1},f{x1,y0,1},g{x1,y1,1},h{x0,y1,1};
    quad(a,d,c,b);quad(e,f,g,h);quad(a,b,f,e);quad(d,h,g,c);quad(a,e,h,d);quad(b,c,g,f);
  };
  if(asset==FacadeAsset::Column || asset==FacadeAsset::ColumnLeftHalf || asset==FacadeAsset::ColumnRightHalf) {
    box(0,0,1,.09);box(.18,.09,.82,.85);box(0,.85,1,1);return out;
  }
  if(asset==FacadeAsset::WindowSill) {
    // Closed sill with a sloping weathering face and a recessed lower nose.
    Position a{0,0,0},b{1,0,0},c{1,.72,1},d{0,.72,1};
    Position e{0,1,0},f{1,1,0},g{1,.18,1},h{0,.18,1};
    quad(d,c,f,e);quad(b,g,h,a);quad(e,f,b,a);quad(g,c,d,h);quad(h,d,e,a);quad(f,c,g,b);
    return out;
  }
  if((asset==FacadeAsset::Mullion || asset==FacadeAsset::Shutter || asset==FacadeAsset::WallBlock)) {box(0,0,1,1);return out;}
  auto ring=apertureContour(shape,archSpring);
  if(asset==FacadeAsset::DoorLeaf || asset==FacadeAsset::DoorLeafLeft || asset==FacadeAsset::DoorLeafRight) {
    if(asset!=FacadeAsset::DoorLeaf) {
      std::vector<Position> clipped;
      const auto inside=[&](Position p){return asset==FacadeAsset::DoorLeafLeft?p.x<=.5:p.x>=.5;};
      for(size_t i=0;i<ring.size();++i){auto a=ring[i],b=ring[(i+1)%ring.size()];
        if(inside(a))clipped.push_back(a);
        if(inside(a)!=inside(b)){double t=(.5-a.x)/(b.x-a.x);clipped.push_back({.5,a.y+t*(b.y-a.y),0});}
      }
      ring.clear();for(auto p:clipped){p.x=asset==FacadeAsset::DoorLeafLeft?p.x*2:(p.x-.5)*2;
        if(ring.empty() || std::hypot(p.x-ring.back().x,p.y-ring.back().y)>1e-8)ring.push_back(p);
      }
      if(ring.size()>1 && std::hypot(ring.front().x-ring.back().x,ring.front().y-ring.back().y)<1e-8)ring.pop_back();
    }
    Position centre{};for(auto p:ring){centre.x+=p.x/ring.size();centre.y+=p.y/ring.size();}
    for(size_t i=0;i<ring.size();++i){auto a=ring[i],b=ring[(i+1)%ring.size()],aa=a,bb=b,cc=centre;aa.z=bb.z=cc.z=1;
      out.push_back({centre,b,a});out.push_back({cc,aa,bb});quad(a,b,bb,aa);
    }
    return out;
  }
  if(asset==FacadeAsset::Glass) {
    for(size_t i=0;i<ring.size();++i)out.push_back({Position{.5,.5,0},ring[i],ring[(i+1)%ring.size()]});
    return out;
  }
  // Ring occupies the aperture perimeter inward; no coplanar wall overlays.
  for(size_t i=0;i<ring.size();++i) {
    auto a=ring[i],b=ring[(i+1)%ring.size()];
    const bool arcade=asset==FacadeAsset::ArcadeArch;
    const double spring=shape==ApertureShape::PointedArch?.5:.55;
    if(arcade && (a.y<spring-1e-8 || b.y<spring-1e-8))continue;
    if(asset==FacadeAsset::PortalFrame && a.y<1e-8 && b.y<1e-8)continue;
    Position c{.5+(b.x-.5)*.84,.5+(b.y-.5)*.84,0},d{.5+(a.x-.5)*.84,.5+(a.y-.5)*.84,0};
    if(asset==FacadeAsset::PortalFrame && shape!=ApertureShape::Circle){c.y=b.y*.92;d.y=a.y*.92;}
    if(arcade){c.y=std::max(spring,b.y*.92);d.y=std::max(spring,a.y*.92);}
    // A splayed reveal, rather than a flat ribbon pasted onto the wall.
    // The outer arris stays recessed while the inner moulding catches light.
    auto aa=a,bb=b,cc=c,dd=d;
    aa.z=bb.z=.35;cc.z=dd.z=1;
    quad(a,b,c,d);quad(aa,dd,cc,bb);quad(a,aa,bb,b);quad(d,c,cc,dd);
    if(arcade) {
      if(std::abs(a.y-spring)<1e-8)quad(a,d,dd,aa);
      if(std::abs(b.y-spring)<1e-8)quad(b,bb,cc,c);
    }
    if(asset==FacadeAsset::PortalFrame && shape!=ApertureShape::Circle) {
      if(a.y<1e-8)quad(a,d,dd,aa);
      if(b.y<1e-8)quad(b,bb,cc,c);
    }
  }
  return out;
}
}

const std::vector<FacadeTriangle> &facadeAsset(FacadeAsset asset,ApertureShape shape)
{
  // Immutable after initialization: safe for parallel building generation.
  static const auto atlas=[] {
    std::map<std::pair<int,int>,std::vector<FacadeTriangle>> result;
    for(int a=0;a<=int(FacadeAsset::WallBlock);++a)for(int s=0;s<=int(ApertureShape::SegmentalArch);++s)
      result[{a,s}]=makeAsset(FacadeAsset(a),ApertureShape(s));
    return result;
  }();
  return atlas.at({int(asset),int(shape)});
}

void planFacadeInstances(Structure &structure,const rewrite::DwellingFacadePolicy &policy)
{
  if(!policy.enabled)return;
  structure.instances.clear();
  // Reserve dressed corner masonry before making either skin's aperture.
  // Moving the opening here moves its glass, reveal and frame together.
  if(!policy.boardFacade)for(size_t f=0;f<structure.faces.size();++f) {
    const auto wall=structure.faces[f];
    if(wall.object!=0 || wall.kind!=FaceKind::WindowedWall || wall.outline.size()!=4)continue;
    if(policy.timberFacade && wall.outline[0].y+wall.low>=policy.stoneBaseHeight)continue;
    const auto a=wall.outline[0],b=wall.outline[1];
    const double len=std::hypot(b.x-a.x,b.z-a.z);if(len<1e-6)continue;
    const Position u{(b.x-a.x)/len,0,(b.z-a.z)/len};
    double lo=0,hi=len;
    for(int end=0;end<2;++end) {
      const auto p=end?b:a;bool corner=false;
      for(const auto &other:structure.faces) {
        if(other.object!=0 || other.segment==wall.segment || other.outline.size()!=4 || other.thickness<=0)continue;
        const auto c=other.outline[0],d=other.outline[1];
        if(std::abs(c.y-a.y)>.01)continue;
        if((std::hypot(p.x-c.x,p.z-c.z)<.01 || std::hypot(p.x-d.x,p.z-d.z)<.01) &&
           std::abs(u.x*(d.z-c.z)-u.z*(d.x-c.x))>.1)corner=true;
      }
      if(corner){if(end)hi=len-.72;else lo=.72;}
    }
    if(wall.along0*len>=lo && wall.along1*len<=hi)continue;
    const double width=std::min((wall.along1-wall.along0)*len,hi-lo);
    const double start=width>0?std::clamp(wall.along0*len,lo,std::max(lo,hi-width)):lo;
    const Position left{a.x+u.x*start,0,a.z+u.z*start},right{left.x+u.x*width,0,left.z+u.z*width};
    for(auto &face:structure.faces)if(face.segment==wall.segment && face.kind==FaceKind::WindowedWall) {
      if(width<.6) {face.kind=FaceKind::Wall;face.low=face.high=face.along0=face.along1=0;continue;}
      const auto c=face.outline[0],d=face.outline[1];const double dx=d.x-c.x,dz=d.z-c.z,l2=dx*dx+dz*dz;
      const double t0=((left.x-c.x)*dx+(left.z-c.z)*dz)/l2,t1=((right.x-c.x)*dx+(right.z-c.z)*dz)/l2;
      face.along0=std::clamp(std::min(t0,t1),0.,1.);face.along1=std::clamp(std::max(t0,t1),0.,1.);
      if(face.along1-face.along0<1e-6){face.kind=FaceKind::Wall;face.low=face.high=face.along0=face.along1=0;}
    }
  }
  std::map<uint32_t,bool> leafDone;
  std::map<uint32_t,bool> windowAllowed;
  int windows=0;
  for(uint32_t f=0;f<structure.faces.size();++f) {
    auto &face=structure.faces[f];
    if(face.outline.size()!=4 || face.high<=face.low || face.along1<=face.along0)continue;
    if(face.kind!=FaceKind::WindowedWall && face.kind!=FaceKind::DoorWall && face.kind!=FaceKind::DoubleDoorWall && face.kind!=FaceKind::ArchWall)continue;
    face.detailedOpening=true;face.archSpring=-1;
    const bool window=face.kind==FaceKind::WindowedWall;
    const uint64_t variant=(uint64_t(face.segment)*2654435761ULL+policy.variationSeed*2246822519ULL)%11;
    const bool varied=policy.masonryVariation && !policy.timberFacade && !policy.boardFacade;
    const bool openWindow=window && varied && variant%3==0;
    if(window) {
      if(!windowAllowed.count(face.segment))windowAllowed[face.segment]=policy.maximumWindows<0 || windows++<policy.maximumWindows;
      if(!windowAllowed[face.segment]) {face.kind=FaceKind::Wall;face.low=face.high=face.along0=face.along1=0;face.detailedOpening=false;continue;}
    }
    face.aperture=policy.style==rewrite::FacadeStyle::Gothic ? ApertureShape::PointedArch :
                  policy.style==rewrite::FacadeStyle::Romanesque || face.kind==FaceKind::ArchWall ? ApertureShape::RoundArch : ApertureShape::Rectangle;
    const bool timber=policy.timberFacade && face.outline[0].y>=policy.stoneBaseHeight-1e-6;
    if(window && policy.circularWindows)face.aperture=ApertureShape::Circle;
    if(window && (timber || policy.boardFacade))face.aperture=ApertureShape::Rectangle;
    if(window && varied && !policy.circularWindows) {
      if(variant==1 || variant==7)face.aperture=ApertureShape::RoundArch;
      else if(variant==2)face.aperture=ApertureShape::Rectangle;
    }
    // A domestic-height door cannot carry the deep crown of a monumental
    // arch: its spring line would be at chest height inside the room.
    if(!window && face.kind!=FaceKind::ArchWall && face.high-face.low<2.6)
      face.aperture=ApertureShape::Rectangle;
    if(face.kind==FaceKind::ArchWall && face.high-face.low<2.7)
      face.aperture=ApertureShape::SegmentalArch;
    if(face.passagePortal)face.aperture=ApertureShape::SegmentalArch;
    if(!window && varied && variant%3==1 && face.high-face.low>=2.4)
      face.aperture=ApertureShape::SegmentalArch;
    if(!window && face.kind!=FaceKind::ArchWall) {
      if(policy.doorProfile==rewrite::DoorProfile::Rectangular)face.aperture=ApertureShape::Rectangle;
      else if(face.high-face.low>=2.6 && policy.doorProfile==rewrite::DoorProfile::RoundArch)face.aperture=ApertureShape::RoundArch;
      else if(face.high-face.low>=2.6 && policy.doorProfile==rewrite::DoorProfile::PointedArch)face.aperture=ApertureShape::PointedArch;
    }
    const auto a=face.outline[0],b=face.outline[1];
    const double length=std::hypot(b.x-a.x,b.z-a.z);
    if(length<1e-6)continue;
    // Ordinary open doorways get a width-based round crown, not the very flat
    // clearance profile reserved for through-passages. Leave masonry below
    // the ceiling and preserve 2.1 m of clear headroom inside the moulding.
    if((face.kind==FaceKind::ArchWall || (!window && (face.aperture==ApertureShape::RoundArch || face.aperture==ApertureShape::PointedArch))) && !face.passagePortal &&
       (face.kind!=FaceKind::ArchWall || face.aperture!=ApertureShape::PointedArch)) {
      const double radius=(face.along1-face.along0)*length*.5;
      const double clearSpring=2.1/.92;
      const double available=face.outline[3].y-a.y-.15-face.low;
      const double height=std::max(face.high-face.low,std::min(available,clearSpring+radius));
      const double rise=std::min(radius,std::max(0.,height-clearSpring));
      if(rise>1e-6) {
        face.high=face.low+height;
        face.archSpring=1.-rise/height;
        if(face.aperture!=ApertureShape::PointedArch)face.aperture=ApertureShape::RoundArch;
      }
    }

    if(window && policy.style==rewrite::FacadeStyle::Gothic && !policy.circularWindows)
      face.high=std::min(face.outline[3].y-a.y-.3,face.low+std::max(2.2,face.high-face.low));
    if(face.aperture==ApertureShape::Circle) {
      double width=std::min((face.along1-face.along0)*length,face.high-face.low);
      double centre=(face.along0+face.along1)*.5;
      face.along0=centre-width*.5/length;face.along1=centre+width*.5/length;face.high=face.low+width;
    }
    if(window && (timber || policy.boardFacade)) {
      const double size=std::min(policy.windowWidth,(face.along1-face.along0)*length);
      const double centre=(face.along0+face.along1)*.5;
      face.along0=centre-size*.5/length;face.along1=centre+size*.5/length;face.high=face.low+std::min(policy.windowHeight,face.high-face.low);
    }
    FacadeInstance instance;
    instance.wooden=timber || policy.boardFacade;
    if(!window)instance.asset=FacadeAsset::PortalFrame;
    instance.segment=face.segment;instance.supportFace=f;instance.shape=face.aperture;instance.archSpring=face.archSpring;
    instance.inside=face.object!=0;instance.u={(b.x-a.x)/length,0,(b.z-a.z)/length};
    instance.normal={-instance.u.z,0,instance.u.x};
    instance.origin={a.x+(b.x-a.x)*face.along0,a.y+face.low,a.z+(b.z-a.z)*face.along0};
    instance.width=(face.along1-face.along0)*length;instance.height=face.high-face.low;
    if(face.aperture==ApertureShape::Circle) {
      instance.height=std::min(instance.height,instance.width);face.high=face.low+instance.height;
    }
    instance.depth=.07;
    // Recess the frame into the wall on BOTH skins.
    //
    // The two skins are wound opposite ways, so each one's own -normal points
    // into the wall body and the same pull-back is right for both. Applied to
    // the outer skin alone - which is what `face.object==0` selects, the face
    // with no room behind it - the room side's frame box extruded its seven
    // centimetres straight out of the wall into the room.
    if(policy.inwardWalls) {
      instance.origin.x-=instance.normal.x*instance.depth;
      instance.origin.z-=instance.normal.z*instance.depth;
    }
    aperturePanels(face);
    // One frame per skin, sharing the same aperture and structural segment.
    structure.instances.push_back(instance);
    // A second recessed stone surround gives monumental entrances depth.
    // Keep it inside its own bay and below the ceiling.
    if(!window && face.object==0 && face.kind!=FaceKind::ArchWall && instance.width>1.5 && instance.height>2.6) {
      const double rim=std::min(.10,std::max(0.,(length-instance.width)*.5-.04));
      if(rim>.02 && a.y+face.low+instance.height+.12<face.outline[3].y) {
        auto surround=instance;surround.origin.x-=instance.u.x*rim;surround.origin.z-=instance.u.z*rim;
        surround.width+=2*rim;surround.height+=.12;surround.depth=.10;
        surround.origin.x-=instance.normal.x*.04;surround.origin.z-=instance.normal.z*.04;
        structure.instances.push_back(surround);
      }
    }
    // A shallow shelter is supported from the facade, above head height.
    // Keep its width within the doorway bay and its projection modest so it
    // needs no columns or additional street-level footprint.
    bool sheltered=false;
    if(!window && face.object==0)for(const auto &member:structure.faces)if(member.kind==FaceKind::Timber) {
      double x0=1e30,x1=-1e30,y0=1e30,y1=-1e30,z0=1e30,z1=-1e30;
      for(auto p:member.outline){double dx=p.x-instance.origin.x,dz=p.z-instance.origin.z;
        double x=dx*instance.u.x+dz*instance.u.z,z=dx*instance.normal.x+dz*instance.normal.z;
        x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,p.y-instance.origin.y);y1=std::max(y1,p.y-instance.origin.y);z0=std::min(z0,z);z1=std::max(z1,z);}
      sheltered |= x1>-.05 && x0<instance.width+.05 && y1>instance.height-.15 && y0<instance.height+.35 && z1>-.1 && z0<.65;
    }
    if(!window && !sheltered && face.object==0 && face.kind!=FaceKind::ArchWall &&
       instance.height>=2.1 && face.outline[3].y>instance.origin.y+instance.height+.28) {
      auto hood=instance;hood.asset=FacadeAsset::WindowSill;
      hood.origin.y+=instance.height+.01;hood.height=.12;
      hood.depth=variant%3==0?.18:.55;hood.wooden=timber || policy.boardFacade;
      structure.instances.push_back(hood);
      if(variant%3!=0)for(int side=0;side<2;++side) {
        auto corbel=hood;corbel.asset=FacadeAsset::WallBlock;
        const double along=side?instance.width-.10:0.;
        corbel.origin.x+=instance.u.x*along;corbel.origin.z+=instance.u.z*along;
        corbel.origin.y-=.10;corbel.width=.10;corbel.height=.10;corbel.depth=.22;
        structure.instances.push_back(corbel);
      }
    }
    // The masonry jambs already support adjacent arches. Do not overlay
    // paired decorative half-columns on them; these duplicate the pier and
    // introduce intersecting/coplanar surfaces between neighbouring bays.
    if(window && policy.shutters && face.object==0 && face.aperture==ApertureShape::Rectangle) {
      // Slightly ajar paired boards stay inside the recessed wall thickness.
      // They consume no unreserved street/neighbor clearance and cannot
      // coincide with the window frame or glass.
      for(unsigned side=0;side<2;++side) {
        auto shutter=instance;shutter.asset=FacadeAsset::Shutter;shutter.wooden=true;
        shutter.width=std::min(.55,(instance.width-.12)*.5);shutter.height=instance.height-.12;shutter.depth=.025;
        if(shutter.width<=.1 || shutter.height<=.2)continue;
        const double along=side ? instance.width-.06 : .06,sign=side ? -1. : 1.;
        shutter.origin.x+=instance.u.x*along-instance.normal.x*.075;
        shutter.origin.z+=instance.u.z*along-instance.normal.z*.075;
        shutter.origin.y+=.06;
        shutter.u={instance.u.x*sign*.984807753+instance.normal.x*.173648178,0,instance.u.z*sign*.984807753+instance.normal.z*.173648178};
        shutter.normal={instance.normal.x*.984807753-instance.u.x*sign*.173648178,0,instance.normal.z*.984807753-instance.u.z*sign*.173648178};
        structure.instances.push_back(shutter);
      }
    }
    if(window && policy.glazed && !openWindow) {
      auto glass=instance;glass.asset=FacadeAsset::Glass;
      glass.origin.x+=glass.normal.x*.012;glass.origin.z+=glass.normal.z*.012;
      structure.instances.push_back(glass);
    }
    if(window && instance.width>.9 && instance.height>1.1 && face.aperture!=ApertureShape::Circle) {
      auto bar=instance;bar.asset=FacadeAsset::Mullion;
      bar.origin.x+=bar.u.x*(instance.width-.065)*.5;bar.origin.z+=bar.u.z*(instance.width-.065)*.5;
      bar.width=.065;structure.instances.push_back(bar);
    }
    if(window && face.aperture==ApertureShape::Rectangle && instance.width>.9 && instance.height>1.1) {
      auto transom=instance;transom.asset=FacadeAsset::Mullion;
      transom.origin.x+=instance.u.x*instance.width*.08;
      transom.origin.z+=instance.u.z*instance.width*.08;
      transom.origin.y+=instance.height*.58;
      transom.width=instance.width*.84;transom.height=.055;
      structure.instances.push_back(transom);
    }
    if(window && face.aperture!=ApertureShape::Circle) {
      auto sill=instance;sill.asset=FacadeAsset::WindowSill;
      sill.origin.y+=.008;sill.height=.105;sill.depth=.12;
      structure.instances.push_back(sill);
    }
    // Paired lights and a small circular head are genuine stone tracery inside
    // the aperture. Selection is shared by both wall skins through segment ID.
    if(window && varied && variant>=8 && instance.width>.9 && instance.height>1.6 && face.aperture==ApertureShape::PointedArch) {
      const auto light=[&](double x,double y,double width,double height,ApertureShape shape) {
        auto detail=instance;detail.asset=FacadeAsset::Frame;detail.shape=shape;
        detail.origin.x+=instance.u.x*x;detail.origin.z+=instance.u.z*x;detail.origin.y+=y;
        detail.width=width;detail.height=height;detail.depth=.055;structure.instances.push_back(detail);
      };
      for(double x:{.09,.53})light(instance.width*x,instance.height*.08,instance.width*.38,instance.height*.57,ApertureShape::PointedArch);
      const double diameter=instance.width*.25;
      light((instance.width-diameter)*.5,instance.height*.68,diameter,diameter,ApertureShape::Circle);
    }
    if(!window && face.kind!=FaceKind::ArchWall && !leafDone[face.segment]) {
      leafDone[face.segment]=true;
      const int count=face.kind==FaceKind::DoubleDoorWall ? 2 : 1;
      for(int i=0;i<count;++i) {
        auto leaf=instance;leaf.asset=count==1?FacadeAsset::DoorLeaf:i==0?FacadeAsset::DoorLeafLeft:FacadeAsset::DoorLeafRight;
        leaf.width=instance.width*.84/count;leaf.height=instance.height*.92;
        double offset=instance.width*.08+i*instance.width*.84/count;
        leaf.origin.x+=leaf.u.x*offset-leaf.normal.x*face.thickness*.5;
        leaf.origin.z+=leaf.u.z*offset-leaf.normal.z*face.thickness*.5;
        structure.instances.push_back(leaf);
        // Reusable joinery rails and inset panels, below the arch spring.
        // All dimensions follow the real leaf rather than a stretched decal.
        for(double side:{-1.,1.}) {
          const auto detail=[&](double x,double y,double w,double h) {
            auto part=leaf;part.asset=FacadeAsset::WallBlock;part.shape=ApertureShape::Rectangle;part.archSpring=-1;part.wooden=true;
            part.width=w;part.height=h;part.depth=.025;
            const double relief=side>0?leaf.depth+.004:-.029;
            part.origin.x+=leaf.u.x*x+leaf.normal.x*relief;
            part.origin.z+=leaf.u.z*x+leaf.normal.z*relief;part.origin.y+=y;
            structure.instances.push_back(part);
          };
          for(double y:{.12,.43})detail(leaf.width*.10,leaf.height*y,leaf.width*.80,.065);
          if(leaf.width>.65)for(double x:{.10,.84})detail(leaf.width*x,leaf.height*.14,.045,leaf.height*.27);
          detail(leaf.width*(i==0?.78:.16),leaf.height*.34,.055,.15);
        }

      }
      if(count==2) {
        // A recessed meeting stile makes the two planned leaves legible even
        // when both sample the same fitted door texture.
        auto stile=instance;stile.asset=FacadeAsset::WallBlock;stile.shape=ApertureShape::Rectangle;stile.archSpring=-1;stile.wooden=true;
        stile.width=.035;stile.height=instance.height*.915;stile.depth=.025;
        const double offset=(instance.width-stile.width)*.5;
        stile.origin.x+=instance.u.x*offset-instance.normal.x*face.thickness*.5;
        stile.origin.z+=instance.u.z*offset-instance.normal.z*face.thickness*.5;
        structure.instances.push_back(stile);
      }
    }
  }
  // Alternating dressed-stone end courses distinguish actual building
  // corners from collinear cell seams. Only use clear wall margins.
  if(!policy.boardFacade)
    for(uint32_t f=0;f<structure.faces.size();++f) {
      const auto &wall=structure.faces[f];
      if(wall.object!=0 || wall.outline.size()!=4 ||
         (wall.kind!=FaceKind::Wall && wall.kind!=FaceKind::WindowedWall && wall.kind!=FaceKind::DoorWall))continue;
      const auto a=wall.outline[0],b=wall.outline[1];
      const double len=std::hypot(b.x-a.x,b.z-a.z);if(len<1.)continue;
      Position u{(b.x-a.x)/len,0,(b.z-a.z)/len};
      for(int end=0;end<2;++end) {
        auto p=end?b:a;bool corner=false,primary=false;
        for(const auto &other:structure.faces)if(&other!=&wall && other.object==0 && other.outline.size()==4 &&
          (other.kind==FaceKind::Wall || other.kind==FaceKind::WindowedWall || other.kind==FaceKind::DoorWall)) {
          auto c=other.outline[0],d=other.outline[1];
          if(std::abs(c.y-a.y)>1e-6)continue;
          if((std::hypot(p.x-c.x,p.z-c.z)<1e-5 || std::hypot(p.x-d.x,p.z-d.z)<1e-5) &&
             std::abs(u.x*(d.z-c.z)-u.z*(d.x-c.x))>.1){corner=true;primary=&wall<&other;}
        }
        if(!corner)continue;
        const double top=policy.timberFacade?std::min(wall.outline[3].y,policy.stoneBaseHeight):wall.outline[3].y;
        for(int course=0;a.y+course*.32+.30<top; ++course) {
          const double extension=primary?.065:0.;
          const double width=((course+int(primary))%2?.30:.48)+extension;
          const double along=end?len-width+extension:-extension;
          if(wall.detailedOpening && along<wall.along1*len+.04 && along+width>wall.along0*len-.04 &&
             course*.32<wall.high && course*.32+.30>wall.low)continue;
          FacadeInstance stone;stone.asset=FacadeAsset::WallBlock;stone.supportFace=f;
          stone.u=u;stone.normal={-u.z,0,u.x};stone.origin={a.x+u.x*along,a.y+course*.32,a.z+u.z*along};
          stone.width=width;stone.height=.30;stone.depth=.065;structure.instances.push_back(stone);
        }
      }
    }
  // A lower roof abutting a taller room is valid, but an aperture at that
  // junction is not. Resolve both skins together, including enlarged Gothic
  // apertures, and remove their atlas instances rather than clipping meshes.
  std::set<uint32_t> blocked;
  for(const auto &wall:structure.faces)if(wall.kind==FaceKind::WindowedWall)
    for(const auto &roof:structure.faces)
      if((roof.kind==FaceKind::Roof || roof.kind==FaceKind::DormerRoof) && roofInOpening(wall,roof)) {
        blocked.insert(wall.segment);break;
      }
  for(auto &wall:structure.faces)if(wall.kind==FaceKind::WindowedWall && blocked.count(wall.segment)) {
    wall.kind=FaceKind::Wall;wall.low=wall.high=wall.along0=wall.along1=0;
    wall.detailedOpening=false;wall.apertureBoundary.clear();wall.wallPanels.clear();wall.openingReveals.clear();
  }
  structure.instances.erase(std::remove_if(structure.instances.begin(),structure.instances.end(),
    [&](const FacadeInstance &instance){return blocked.count(instance.segment)!=0;}),structure.instances.end());
}

void lowerWindowHead(Structure &structure,uint32_t index,double absoluteHeight)
{
  auto &face=structure.faces[index];
  const double oldHeight=face.high-face.low;
  const double oldBottom=face.outline[0].y+face.low;
  if(absoluteHeight>=oldBottom+oldHeight)return;
  // Lower the sill first, preserving the window and its interior reveal,
  // instead of compressing its frame and tracery under a roof beam.
  const double newBottom=std::max(face.outline[0].y+.7,absoluteHeight-oldHeight);
  const double newHeight=absoluteHeight-newBottom;
  const double length=std::hypot(face.outline[1].x-face.outline[0].x,face.outline[1].z-face.outline[0].z);
  const double minimumHeight=std::max(.65,(face.along1-face.along0)*length*.8);
  if(newHeight<minimumHeight) {
    face.kind=FaceKind::Wall;face.detailedOpening=false;face.wallPanels.clear();face.openingReveals.clear();face.apertureBoundary.clear();face.low=face.high=face.along0=face.along1=0;
    structure.instances.erase(std::remove_if(structure.instances.begin(),structure.instances.end(),[&](const auto &i){return i.supportFace==index;}),structure.instances.end());
    return;
  }
  face.low=newBottom-face.outline[0].y;face.high=face.low+newHeight;aperturePanels(face);
  for(auto &i:structure.instances)if(i.supportFace==index) {
    i.origin.y=newBottom+(i.origin.y-oldBottom)*newHeight/oldHeight;
    i.height*=newHeight/oldHeight;
  }
}

void meshFacadeInstances(const Structure &structure,Solid &out)
{
  for(const auto &instance:structure.instances) {
    // Buried cellar walls are omitted by meshSkeleton. Their attached assets
    // must follow that visibility rule, otherwise doors float below the house.
    if(instance.origin.y+instance.height<=1e-6)continue;
    auto &group=instance.asset==FacadeAsset::Glass ? out.joinery :
                instance.wooden || instance.asset==FacadeAsset::DoorLeaf || instance.asset==FacadeAsset::DoorLeafLeft || instance.asset==FacadeAsset::DoorLeafRight ? out.timbers : out.pillars;
    const auto custom=instance.archSpring>=0?makeAsset(instance.asset,instance.shape,instance.archSpring):std::vector<FacadeTriangle>{};
    const auto &asset=instance.archSpring>=0?custom:facadeAsset(instance.asset,instance.shape);
    for(const auto &triangle:asset) {
      Solid::Face face;
      for(auto p:triangle)face.push_back({instance.origin.x+instance.u.x*p.x*instance.width+instance.v.x*p.y*instance.height+instance.normal.x*p.z*instance.depth,
        instance.origin.y+instance.u.y*p.x*instance.width+instance.v.y*p.y*instance.height+instance.normal.y*p.z*instance.depth,
        instance.origin.z+instance.u.z*p.x*instance.width+instance.v.z*p.y*instance.height+instance.normal.z*p.z*instance.depth});
      const bool leaf=instance.asset==FacadeAsset::DoorLeaf || instance.asset==FacadeAsset::DoorLeafLeft || instance.asset==FacadeAsset::DoorLeafRight;
      const bool leafSkin=leaf && std::abs(triangle[0].z-triangle[1].z)<1e-9 && std::abs(triangle[0].z-triangle[2].z)<1e-9;
      for(size_t k=0;k<face.size();++k) {
        auto &p=face[k];
        p.material=instance.asset==FacadeAsset::Glass ? Solid::SurfaceMaterial::Glass :
          leafSkin ? Solid::SurfaceMaterial::Door : instance.wooden || leaf ? Solid::SurfaceMaterial::Wood : Solid::SurfaceMaterial::CarvedStone;
        if(leafSkin) {
          // Crop the portrait (2:3) asset, preserving the proportions of its
          // hardware on narrow leaves instead of squeezing the whole image.
          const double aspect=instance.width/instance.height, sourceAspect=2./3.;
          const double x=instance.asset==FacadeAsset::DoorLeafRight?1-triangle[k].x:triangle[k].x;
          p.u=.5+(x-.5)*std::min(1.,aspect/sourceAspect);
          p.v=.5+(triangle[k].y-.5)*std::min(1.,sourceAspect/aspect);
          p.authoredUV=true;
        } else {
          // Metric asset coordinates: rotated facades retain square panes,
          // stone courses and wood grain instead of stretching a world-axis map.
          const auto a=triangle[0],b=triangle[1],c=triangle[2];
          const double nx=(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y);
          const double ny=(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z);
          const double nz=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
          const auto q=triangle[k];
          if(std::abs(nz)>=std::max(std::abs(nx),std::abs(ny))) {
            p.u=q.x*instance.width;p.v=q.y*instance.height;
          } else if(std::abs(nx)>std::abs(ny)) {
            p.u=q.z*instance.depth;p.v=q.y*instance.height;
          } else {p.u=q.x*instance.width;p.v=q.z*instance.depth;}
          p.authoredUV=true;
        }
      }
      group.push_back(std::move(face));
    }
  }
}
} // namespace procedural::world::hierarchy
