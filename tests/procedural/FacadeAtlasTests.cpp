#include "procedural/world/hierarchy/FacadeAtlas.hpp"
#include "procedural/world/hierarchy/CellSkeleton.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"
#include "procedural/world/generator/CellBlock.hpp"
#include "procedural/world/generator/CellPassage.hpp"
#include "procedural/geom3/Prism.hpp"
#include <stdexcept>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

using namespace procedural::world;
using namespace procedural::world::hierarchy;
using rewrite::Position;

rewrite::DwellingPlan pavilion(rewrite::FacadeStyle style,bool arcade,bool circle=false)
{
  rewrite::DwellingPlan p;
  p.facade.enabled=true;p.facade.style=style;p.facade.circularWindows=circle;
  p.facade.bayMetres=3;p.facade.glazed=!arcade;
  const double h=style==rewrite::FacadeStyle::Gothic ? 5.5 : 3.8;
  std::vector<Position> ring{{0,0,0},{12,0,0},{12,0,7},{0,0,7}};
  rewrite::DwellingRoom room;room.id=1;room.boundary=ring;room.ceilingHeight=h;p.rooms.push_back(room);
  rewrite::DwellingFloor floor;floor.height=h;floor.slabs={ring};p.floors.push_back(floor);
  for(size_t i=0;i<4;++i) {
    rewrite::DwellingWall wall;wall.id=i+1;wall.a=ring[i];wall.b=ring[(i+1)%4];wall.thickness=.2;wall.exterior=true;wall.height=h;p.walls.push_back(wall);
    if(arcade && i!=2)p.facade.arcadeWalls.push_back(wall.id);
    else {
      rewrite::DwellingOpening cut;cut.wall=wall.id;cut.a=wall.a;cut.b=wall.b;cut.base=1;cut.top=h-.7;cut.kind=rewrite::OpeningKind::Window;cut.exterior=true;p.openings.push_back(cut);
    }
  }
  if(!arcade) {
    rewrite::DwellingOpening door;door.wall=1;door.a={4.8,0,0};door.b={7.2,0,0};door.base=0;door.top=style==rewrite::FacadeStyle::Gothic?3.6:2.3;door.kind=rewrite::OpeningKind::Door;door.exterior=true;p.openings.push_back(door);
    rewrite::DwellingDoor leaf;leaf.wall=1;leaf.privateAccess=true;leaf.exterior=true;p.doors.push_back(leaf);
  }
  const double ridge=h+2.6;
  p.roofs.push_back({{{0,h,0},{12,h,0},{12,ridge,3.5},{0,ridge,3.5}},0,false,false});
  p.roofs.push_back({{{12,h,7},{0,h,7},{0,ridge,3.5},{12,ridge,3.5}},0,false,false});
  p.roofs.push_back({{{0,h,7},{0,h,0},{0,ridge,3.5}},0,true,false});
  p.roofs.push_back({{{12,h,0},{12,h,7},{12,ridge,3.5}},0,true,false});
  return p;
}

void writeSolid(const Solid &solid,const std::filesystem::path &path,double elevation=0,const Structure *structure=nullptr,const rewrite::DwellingPlan *plan=nullptr)
{
  std::vector<Solid::Vertex> vertices;std::vector<std::array<size_t,3>> triangles;std::vector<int> materials;
  const auto add=[&](const auto &faces,int material){for(const auto &face:faces) {
    size_t first=vertices.size();vertices.insert(vertices.end(),face.begin(),face.end());
    // Quad strips are emitted in their authored order; larger outlines may
    // contain re-entrant foundation corners and must use polygon triangulation.
    if(face.size()<=4) {
      for(size_t i=2;i<face.size();++i){triangles.push_back({first,first+i-1,first+i});materials.push_back(material);}
      continue;
    }
    std::vector<procedural::geom3::Vertex> polygon;
    for(auto p:face)polygon.push_back({p.x,p.y,p.z});
    std::vector<std::array<uint32_t,3>> indices;std::string error;
    if(!procedural::geom3::triangulateFace(polygon,indices,&error))throw std::runtime_error("preview face: "+error);
    for(auto t:indices){triangles.push_back({first+t[0],first+t[1],first+t[2]});materials.push_back(material);}
  }};
  add(solid.walls,12);add(solid.innerWalls,12);add(solid.gables,12);add(solid.slopes,14);
  add(solid.pillars,40);add(solid.joinery,16);add(solid.timbers,17);add(solid.reveals,12);add(solid.innerReveals,12);add(solid.decking,15);
  add(solid.dormerWalls,12);add(solid.dormerRoofs,14);add(solid.dormerWindows,16);add(solid.dormerCeilings,15);
  add(solid.soffits,12);add(solid.stairs,15);add(solid.foundations,53);add(solid.yard,52);
  add(solid.furniture,17);
  std::ofstream out(path);out<<"{\"vertices\":[";
  for(size_t i=0;i<vertices.size();++i){auto p=vertices[i];if(i)out<<",";out<<"["<<p.x<<","<<p.y+elevation<<","<<p.z<<"]";}
  out<<"],\"triangles\":[";for(size_t i=0;i<triangles.size();++i){auto t=triangles[i];if(i)out<<",";out<<"["<<t[0]<<","<<t[1]<<","<<t[2]<<"]";}
  out<<"],\"uvs\":[";for(size_t i=0;i<vertices.size();++i){if(i)out<<",";out<<"["<<vertices[i].u<<","<<vertices[i].v<<"]";}
  out<<"],\"uvIslands\":[";for(size_t i=0;i<vertices.size();++i){if(i)out<<",";out<<vertices[i].uvIsland;}
  out<<"],\"surfaceMaterials\":[";for(size_t i=0;i<triangles.size();++i){if(i)out<<",";out<<uint32_t(vertices[triangles[i][0]].material);}
  out<<"],\"textureManifest\":\"assets/procedural/textures/medieval-pixel/materials.json\",\"materials\":[";for(size_t i=0;i<materials.size();++i){if(i)out<<",";out<<materials[i];}out<<"],\"buildings\":[],\"sectionWalls\":[";
  bool first=true;std::set<uint32_t> emitted;
  if(structure)for(const auto &face:structure->faces) {
    if(face.segment==UINT32_MAX || face.outline.size()!=4 || face.thickness<=0 || !emitted.insert(face.segment).second)continue;
    auto a=face.outline[0],b=face.outline[1];double length=std::hypot(b.x-a.x,b.z-a.z);if(length<1e-8)continue;
    auto panels=face.detailedOpening?face.wallPanels:std::vector<std::vector<Position>>{face.outline};
    for(const auto &panel:panels) {
      if(!first)out<<",";first=false;
      out<<"{\"through\":["<<(b.z-a.z)/length*face.thickness<<",0,"<<-(b.x-a.x)/length*face.thickness<<"],\"polygon\":[";
      for(size_t i=0;i<panel.size();++i){auto p=panel[i];if(i)out<<",";out<<"["<<p.x<<","<<p.y+elevation<<","<<p.z<<"]";}
      out<<"]}";
    }
  }
  out<<"],\"rooms\":[";
  if(plan)for(size_t r=0;r<plan->rooms.size();++r) {
    if(r)out<<",";const auto &room=plan->rooms[r];
    out<<"{\"id\":"<<room.id<<",\"floor\":"<<room.floor<<",\"boundary\":[";
    for(size_t i=0;i<room.boundary.size();++i){auto p=room.boundary[i];if(i)out<<",";out<<"["<<p.x<<","<<p.y+elevation<<","<<p.z<<"]";}
    out<<"]}";
  }
  out<<"]}";
}

int main(int argc,char **argv)
{
  int failures=0;
  const auto check=[&](bool ok,const char *message){std::cout<<(ok?"PASS ":"FAIL ")<<message<<"\n";failures+=!ok;};
  {
    rewrite::DwellingPlan plan;
    plan.timbers.push_back({0,rewrite::TimberKind::Beam,{0,2,0},{2,2,0},.2,.3});
    auto structure=buildStructure(plan);size_t faces=0;bool outward=true;
    for(const auto &f:structure.faces)if(f.kind==FaceKind::Timber) {
      ++faces;auto a=f.outline[0],b=f.outline[1],c=f.outline[2];
      Position centre{};for(auto p:f.outline){centre.x+=p.x/4;centre.y+=p.y/4;centre.z+=p.z/4;}
      Position normal{(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),
        (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
      outward&=normal.x*(centre.x-1)+normal.y*(centre.y-2)+normal.z*centre.z>0;
    }
    check(faces==6 && outward,"jetty members have both end caps and six outward-facing surfaces");
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Gothic,false);
    plan.facade.roofCrests=true;
    const auto structure=buildStructure(plan);
    double roofTop=-1e9;for(const auto &r:plan.roofs)for(auto p:r.polygon)roofTop=std::max(roofTop,p.y);
    unsigned crestFaces=0;bool supported=true;
    for(const auto &face:structure.faces)if(face.kind==FaceKind::Timber && face.object==0) {
      bool high=false;for(auto p:face.outline)high|=p.y>roofTop+.2;
      if(high){++crestFaces;supported &= face.support<structure.faces.size() && structure.faces[face.support].kind==FaceKind::Roof;}
    }
    check(crestFaces>0 && crestFaces<=42 && supported,"fantasy ridge ornaments are bounded and anchored to realised roof faces");
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Domestic,false);
    auto ceiling=plan.rooms.front().boundary;ceiling.insert(ceiling.begin()+1,{6,0,0});for(auto &p:ceiling)p.y=3.8;plan.ceilings={ceiling};
    const auto structure=buildStructure(plan);
    size_t sills=0,transoms=0,joists=0;bool below=true;
    for(const auto &i:structure.instances) {
      sills+=i.asset==FacadeAsset::WindowSill;
      transoms+=i.asset==FacadeAsset::Mullion && i.width>i.height;
    }
    for(const auto &f:structure.faces)if(f.kind==FaceKind::Timber && f.object!=0) {
      ++joists;for(auto p:f.outline)below &= p.y<=3.68+1e-8 && p.y>=3.52-1e-8;
    }
    check(sills>0 && transoms>0,"domestic windows have solid weathered sills and crossbars");
    check(joists>0 && below,"ceiling joists sit directly below the slab within safe headroom");
    plan.rooms.front().openToRoof=true;
    const auto open=buildStructure(plan);bool clear=true;
    for(const auto &f:open.faces)clear &= !(f.kind==FaceKind::Timber && f.object!=0);
    check(clear,"open-to-roof rooms do not acquire false ceiling joists");
    double volume=0;
    for(const auto &t:facadeAsset(FacadeAsset::WindowSill,ApertureShape::Rectangle)) {
      auto a=t[0],b=t[1],c=t[2];
      volume+=a.x*(b.y*c.z-b.z*c.y)+a.y*(b.z*c.x-b.x*c.z)+a.z*(b.x*c.y-b.y*c.x);
    }
    check(volume>0,"weathered sill faces wind outward as a closed solid");
    check(facadeAsset(FacadeAsset::WindowSill,ApertureShape::Rectangle).size()==12,
          "window sill atlas emits six solid faces");
  }
  for(auto shape:{ApertureShape::PointedArch,ApertureShape::RoundArch}) {
    bool above=true;const double spring=shape==ApertureShape::PointedArch?.5:.55;
    const auto &arch=facadeAsset(FacadeAsset::ArcadeArch,shape);
    for(const auto &tri:arch)for(auto p:tri)above&=p.y>=spring-1e-9;
    check(!arch.empty() && above,"arcade arch omits portal legs and meets columns at its spring line");
  }
  {
    Structure structure;
    FacadeInstance glass;glass.asset=FacadeAsset::Glass;glass.shape=ApertureShape::Rectangle;
    glass.origin={10,2,7};glass.u={.6,0,.8};glass.normal={-.8,0,.6};glass.width=1.4;glass.height=1.2;
    structure.instances.push_back(glass);Solid solid;meshFacadeInstances(structure,solid);
    bool metric=!solid.joinery.empty();
    for(const auto &face:solid.joinery)for(const auto &p:face) {
      metric &= p.authoredUV && std::abs(p.u-((p.x-10)*.6+(p.z-7)*.8))<1e-8 && std::abs(p.v-(p.y-2))<1e-8;
    }
    check(metric,"rotated windows retain metric facade-aligned glazing coordinates");
  }
  bool valid=true;
  for(int a=0;a<=int(FacadeAsset::Shutter);++a)for(int s=0;s<=int(ApertureShape::SegmentalArch);++s) {
    const auto &mesh=facadeAsset(FacadeAsset(a),ApertureShape(s));valid&=!mesh.empty();
    for(auto t:mesh) {
      auto p=t[0],q=t[1],r=t[2];double ux=q.x-p.x,uy=q.y-p.y,uz=q.z-p.z,vx=r.x-p.x,vy=r.y-p.y,vz=r.z-p.z;
      double cross=std::hypot(uy*vz-uz*vy,std::hypot(uz*vx-ux*vz,ux*vy-uy*vx));valid&=std::isfinite(cross)&&cross>1e-10;
    }
    if(argc>1) {
      std::filesystem::create_directories(argv[1]);
      std::ofstream obj(std::filesystem::path(argv[1])/("asset-"+std::to_string(a)+"-"+std::to_string(s)+".obj"));
      obj<<"# normalized facade atlas; metres after instance scaling\n";
      for(auto t:mesh)for(auto p:t)obj<<"v "<<p.x<<" "<<p.y<<" "<<p.z<<"\n";
      for(size_t i=0;i<mesh.size();++i)obj<<"f "<<i*3+1<<" "<<i*3+2<<" "<<i*3+3<<"\n";
    }
  }
  check(valid,"all atlas entries have finite, non-degenerate triangles");
  check(std::abs(thicknessFor(Material::Stone)-.20)<1e-8 && std::abs(thicknessFor(Material::Timber)-.10)<1e-8,
        "stone and timber wall defaults are 20 cm and 10 cm");
  for(unsigned type=0;type<4;++type) {
    auto plan=pavilion(rewrite::FacadeStyle::Domestic,false);
    plan.facade.inwardWalls=true;plan.facade.timberFacade=type==1 || type==3;plan.facade.boardFacade=type==2;plan.facade.shutters=type==3;
    auto structure=buildStructure(plan);
    std::map<uint32_t,std::set<Material>> skins;
    for(const auto &face:structure.faces)if(face.segment!=UINT32_MAX &&
      (face.kind==FaceKind::Wall || face.kind==FaceKind::WindowedWall || face.kind==FaceKind::DoorWall))skins[face.segment].insert(face.material);
    bool matched=!skins.empty();for(const auto &[segment,materials]:skins)matched&=materials.size()==1;
    check(matched,"interior and exterior wall skins share their construction material");
    if(type==3) {
      bool contained=true;unsigned count=0;
      for(const auto &i:structure.instances)if(i.asset==FacadeAsset::Shutter){++count;for(const auto &triangle:facadeAsset(i.asset,i.shape))for(auto p:triangle){
        const double x=i.origin.x+i.u.x*p.x*i.width+i.normal.x*p.z*i.depth,z=i.origin.z+i.u.z*p.x*i.width+i.normal.z*p.z*i.depth;
        contained&=x>=-1e-6 && x<=12+1e-6 && z>=-1e-6 && z<=7+1e-6;
      }}
      check(count>0 && contained,"paired recessed shutters remain within the building envelope");
    }
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Romanesque,false);
    plan.facade.inwardWalls=true;plan.facade.roofFraming=true;plan.facade.corridorSupports=true;plan.facade.glazed=false;
    plan.rooms.front().use=rewrite::RoomUse::Corridor;plan.rooms.front().openToRoof=true;
    auto structure=buildStructure(plan);Solid solid;meshSkeleton(skeletonOf(structure),solid);
    bool contained=true;size_t arches=0,timbers=0;
    for(const auto &f:structure.faces){if(f.kind==FaceKind::ArchWall)++arches;
      if(f.segment!=UINT32_MAX)for(auto p:f.outline)contained&=p.x>=-1e-7 && p.x<=12+1e-7 && p.z>=-1e-7 && p.z<=7+1e-7;
      if(f.kind==FaceKind::Timber && f.support<structure.faces.size())++timbers;}
    for(const auto *group:{&solid.walls,&solid.innerWalls,&solid.reveals,&solid.innerReveals,&solid.pillars})
      for(const auto &face:*group)for(auto p:face)contained&=p.x>=-1e-7 && p.x<=12+1e-7 && p.z>=-1e-7 && p.z<=7+1e-7;
    check(contained,"inward wall skins, frames, pillars and mitred corners stay inside the planned footprint");
    size_t ribs=0;for(const auto &i:structure.instances)ribs+=i.asset==FacadeAsset::ArcadeArch && i.segment==UINT32_MAX;
    check(ribs>0,"castle gallery retains internal supporting arch ribs between exterior openings");
    check(arches>4 && solid.joinery.empty(),"ground-level castle galleries open outside through unglazed arches");
    bool extraColumns=false;for(const auto &i:structure.instances)extraColumns|=i.asset==FacadeAsset::ColumnLeftHalf || i.asset==FacadeAsset::ColumnRightHalf;
    check(!extraColumns,"arcade masonry piers do not carry duplicate decorative half-columns");
    check(timbers>12 && !solid.timbers.empty(),"roof rafters and ridge members retain structural roof support references");
    bool belowRoof=true;
    for(const auto &f:structure.faces)if(f.kind==FaceKind::Timber && f.support<structure.faces.size()) {
      const auto &roof=structure.faces[f.support];if(roof.kind!=FaceKind::Roof || roof.outline.size()<3)continue;
      auto a=roof.outline[0],b=roof.outline[1],c=roof.outline[2];
      double ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z,vx=c.x-a.x,vy=c.y-a.y,vz=c.z-a.z;
      double nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;if(std::abs(ny)<1e-8)continue;
      for(auto p:f.outline)belowRoof&=p.y<=a.y-(nx*(p.x-a.x)+nz*(p.z-a.z))/ny+1e-7;
    }
    check(belowRoof,"complete roof timber cross-sections remain below their roof covering planes");
    std::set<TimberRole> roles;for(const auto &f:structure.faces)if(f.kind==FaceKind::Timber)roles.insert(f.timberRole);
    check(!roles.count(TimberRole::RoofEdge),"roof framing does not add an exposed timber perimeter border");
    size_t caps=0;for(const auto &f:structure.faces)caps+=f.roofJointCap;
    check(caps==0,"a roof is its planes: no half-cylinder laid along every ridge and hip");
    check(!roles.count(TimberRole::Ridge) && !roles.count(TimberRole::Rafter) && !roles.count(TimberRole::Purlin),
          "no rafters, purlins or ridge pieces: the covering is the roof");
    check(roles.count(TimberRole::TieBeam) || roles.count(TimberRole::CeilingJoist),
          "the ceiling frame remains - it is bounded by a ceiling and cannot escape through the covering");
    if(argc>1)writeSolid(solid,std::filesystem::path(argv[1])/"castle-gallery.json",0,&structure,&plan);
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Gothic,false);
    plan.openings.clear();plan.doors.clear();plan.facade.castleInterior=true;
    plan.facade.corridorSupports=true;plan.facade.inwardWalls=true;
    plan.rooms.front().kind=rewrite::RoomKind::GreatHall;
    auto structure=buildStructure(plan);Solid solid;meshSkeleton(skeletonOf(structure),solid);
    size_t engaged=0;bool intact=true;
    for(const auto &i:structure.faces)if(i.engagedArch && i.support<structure.faces.size()) {
      const auto &wall=structure.faces[i.support];if(wall.kind!=FaceKind::Wall)continue;
      ++engaged;intact&=wall.apertureBoundary.empty() && wall.detailedOpening &&
        wall.wallPanels.size()>12 && wall.object!=0;
    }
    check(engaged>=8 && intact,"castle wall skins form recessed arched bays without cutting openings");
    std::map<uint32_t,size_t> ribComponents;
    for(const auto &face:structure.faces)if(face.engagedArch)++ribComponents[face.component];
    size_t sweeps=0,piers=0;for(auto [id,count]:ribComponents){sweeps+=count>100;piers+=count==6;}
    check(sweeps>=4 && piers>=8,"each arched wall bay has a continuous sweep borne by masonry piers");
    size_t shell=0;bool lowRibs=true;
    for(const auto &face:structure.faces){if(face.masonryVault && face.kind==FaceKind::Ceiling)++shell;
      if(face.engagedArch)for(auto p:face.outline)lowRibs&=p.y<=(5.5-.28)*.5+.12;}
    // Counted against the contour, not a number typed in once.
    //
    // The shell is built by walking `apertureContour`, four faces per span, so
    // its face count is the arch's resolution and nothing else. Written as a
    // constant it said "the vault exists" only for as long as nobody changed
    // how finely an arch is drawn - and lowering the arch from 24 segments to
    // 8, for the small rectangles it was cutting into every wall around it,
    // took the shell from about 94 faces to about 30 and failed a check the
    // vault had not stopped satisfying.
    const size_t arc=apertureContour(ApertureShape::PointedArch).size();
    check(shell>=4*(arc-3) && lowRibs,"castle ceiling follows main arch contour and wall ribs stop at its spring line");
    if(argc>1)writeSolid(solid,std::filesystem::path(argv[1])/"castle-corridor.json",0,&structure,&plan);
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Gothic,false);
    plan.facade.inwardWalls=true;plan.facade.roofFraming=true;plan.facade.stoneVaults=true;
    plan.rooms.front().openToRoof=true;
    auto structure=buildStructure(plan);Solid solid;meshSkeleton(skeletonOf(structure),solid);
    size_t masonry=0;bool contained=true;
    for(const auto &face:structure.faces)if(face.masonryVault){++masonry;for(auto p:face.outline)contained&=p.x>=0 && p.x<=12 && p.z>=0 && p.z<=7 && p.y>=0;}
    check(masonry>100 && contained,"stone vault bays and constant-section ribs remain inside the room footprint");
    size_t transitions=0;bool solidTransitions=true;
    for(const auto &face:structure.faces)if(face.vaultTransition) {
      ++transitions;solidTransitions&=face.masonryVault && face.kind==FaceKind::Pier && face.outline.size()==4;
    }
    check(transitions>=24 && solidTransitions,"vault junctions contain solid stepped capitals and perimeter spring courses");
    size_t internalEdgeFaces=0;
    for(const auto &face:structure.faces)if(face.vaultTransition) {
      double minX=1e30,maxX=-1e30,minZ=1e30,maxZ=-1e30;
      for(auto p:face.outline){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minZ=std::min(minZ,p.z);maxZ=std::max(maxZ,p.z);}
      internalEdgeFaces+=maxX-minX>10 && minZ>1 && maxZ<6;
    }
    check(internalEdgeFaces>=8,"thin vault-edge blocks continue along both internal aisle seams");
    bool roofTimbers=false;for(const auto &face:structure.faces)roofTimbers|=face.kind==FaceKind::Timber;
    check(!roofTimbers,"vaulted hall replaces exposed roof timbers with masonry");
    size_t mappedVault=0;bool metricUV=true;
    for(const auto &face:solid.soffits)for(const auto &p:face)if(p.material==Solid::SurfaceMaterial::VaultMasonry) {
      ++mappedVault;metricUV&=p.authoredUV && std::isfinite(p.u) && std::isfinite(p.v);
    }
    check(mappedVault>0 && metricUV,"vault infill has dedicated small-course masonry and authored metric UVs");
    std::map<uint32_t,size_t> ribComponents;
    for(const auto &face:structure.faces)if(face.masonryVault && face.kind==FaceKind::Pier)++ribComponents[face.component];
    size_t swept=0;for(auto [id,count]:ribComponents)swept+=count>=98;
    check(swept>10,"vault bays contain continuous transverse and diagonal rib sweeps");
    size_t windows=0;for(const auto &face:structure.faces)windows+=face.kind==FaceKind::WindowedWall;
    check(windows>0,"vault spring preserves shorter windows instead of blanking the hall facade");
    bool clear=true;
    for(const auto &rib:structure.faces)if(rib.masonryVault && rib.kind==FaceKind::Pier)
      for(const auto &wall:structure.faces)if(wall.object==rib.object && !wall.apertureBoundary.empty()) {
        auto a=wall.outline[0],b=wall.outline[1];double length=std::hypot(b.x-a.x,b.z-a.z);if(length<1e-8)continue;
        double ux=(b.x-a.x)/length,uz=(b.z-a.z)/length;
        for(auto p:rib.outline){double x=(p.x-a.x)*ux+(p.z-a.z)*uz,z=-(p.x-a.x)*uz+(p.z-a.z)*ux,y=p.y-a.y;
          if(x>wall.along0*length && x<wall.along1*length && y>wall.low && y<wall.high && std::abs(z)<wall.thickness+.12)clear=false;}
      }
    check(clear,"trimmed masonry supports leave door and window clearance volumes empty");
    if(argc>1)writeSolid(solid,std::filesystem::path(argv[1])/"stone-vault.json",0,&structure,&plan);
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Domestic,false);plan.facade.inwardWalls=true;plan.facade.timberFacade=true;
    auto structure=buildStructure(plan);Solid solid;meshSkeleton(skeletonOf(structure),solid);
    size_t windows=0;bool square=true;double widestWindow=0;
    for(const auto &i:structure.instances)if(i.asset==FacadeAsset::Frame){++windows;widestWindow=std::max(widestWindow,i.width);square&=i.wooden && i.shape==ApertureShape::Rectangle && std::abs(i.width-i.height)<1e-7 && i.width<=1.000001;}
    check(windows>0 && square && !solid.timbers.empty(),"middle-class facade has timber members and square wooden window frames");
    check(widestWindow>.8,"domestic windows retain human scale instead of a half-metre cap");
    size_t doorSkins=0;bool fitted=true,plaster=false;
    for(const auto &face:solid.timbers)for(const auto &p:face)if(p.material==Solid::SurfaceMaterial::Door) {
      ++doorSkins;fitted&=p.authoredUV && p.u>=0 && p.u<=1 && p.v>=0 && p.v<=1 && p.uvIsland>0;
    }
    for(const auto &face:solid.walls)for(const auto &p:face)plaster|=p.material==Solid::SurfaceMaterial::HalfTimber;
    check(doorSkins>0 && fitted,"door hardware uses fitted normalized UVs preserved by the mesh mapper");
    bool undistorted=true;
    for(const auto &face:solid.timbers)if(face.size()>=3 && face[0].material==Solid::SurfaceMaterial::Door) {
      double first=0;
      for(size_t k=0;k<face.size();++k) {
        const auto &a=face[k],&b=face[(k+1)%face.size()];
        const double distance=std::hypot(a.x-b.x,std::hypot(a.y-b.y,a.z-b.z));
        const double texture=std::hypot((a.u-b.u)*2./3.,a.v-b.v);
        if(texture<1e-9)continue;
        const double scale=distance/texture;
        if(first==0)first=scale;else undistorted &= std::abs(first-scale)<1e-6;
      }
    }
    check(undistorted,"door portrait hardware retains its aspect ratio on physical leaf geometry");
    check(plaster,"timber facade uses the dedicated half-timber infill material");
    if(argc>1)writeSolid(solid,std::filesystem::path(argv[1])/"timber-house.json",0,&structure,&plan);
  }
  {
    rewrite::DwellingPlan plan;rewrite::DwellingRoom room;room.id=1;room.ceilingHeight=3;
    room.boundary={{0,0,0},{4,0,0},{4,0,2},{0,0,2}};plan.rooms.push_back(room);
    plan.ceilings={{{0,3,0},{2,3,0},{2,3,2},{0,3,2}},{{2,3,0},{4,3,0},{4,3,2},{2,3,2}}};
    auto structure=buildStructure(plan);size_t caps=0,rims=0;bool thickness=true,seam=false;
    for(const auto &f:structure.faces)if(f.kind==FaceKind::Ceiling) {
      double low=10,high=-10;bool atSeam=true;
      for(auto p:f.outline){low=std::min(low,p.y);high=std::max(high,p.y);atSeam&=std::abs(p.x-2)<1e-8;}
      if(high-low>1e-8){++rims;thickness&=std::abs(high-low-.12)<1e-8;seam|=atSeam;}
      else ++caps;
    }
    check(caps==4 && rims==6 && thickness && !seam,"ceiling slab has two skins, 12 cm boundary rims, and no internal cell seam");
    plan.rooms.front().openToRoof=true;
    check(buildStructure(plan).faces.empty(),"open-to-roof rooms do not receive a flat ceiling");
    plan.rooms.front().openToRoof=false;
    plan.rooms.front().boundary={{0,0,0},{6,0,0},{6,0,6},{0,0,6}};
    plan.ceilings.clear();
    for(int x=0;x<3;++x)for(int z=0;z<3;++z)if(x!=1 || z!=1)
      plan.ceilings.push_back({{2.*x,3,2.*z},{2.*x+2,3,2.*z},{2.*x+2,3,2.*z+2},{2.*x,3,2.*z+2}});
    structure=buildStructure(plan);size_t holeRims=0;bool holeOpen=true;
    for(const auto &f:structure.faces)if(f.kind==FaceKind::Ceiling) {
      double minx=100,maxx=-100,minz=100,maxz=-100,low=10,high=-10;
      for(auto p:f.outline){minx=std::min(minx,p.x);maxx=std::max(maxx,p.x);minz=std::min(minz,p.z);maxz=std::max(maxz,p.z);low=std::min(low,p.y);high=std::max(high,p.y);}
      if(high-low<1e-8)holeOpen&=!(minx<3 && maxx>3 && minz<3 && maxz>3);
      else if(minx>=2 && maxx<=4 && minz>=2 && maxz<=4)++holeRims;
    }
    check(holeOpen && holeRims==4,"stair opening remains empty and has four finished ceiling reveals");
    plan.facade.roofFraming=true;structure=buildStructure(plan);
    std::set<uint32_t> joists;bool wellClear=true;
    for(const auto &f:structure.faces)if(f.timberRole==TimberRole::CeilingJoist) {
      joists.insert(f.component);for(auto p:f.outline)wellClear&=p.z<2 || p.z>4;
    }
    check(joists.size()==4 && wellClear,"closely spaced ceiling joists support both sides without bridging the stair opening");
    bool soffitsOutward=true;size_t soffits=0;
    for(const auto &f:structure.faces)if(f.timberRole==TimberRole::CeilingJoist && f.outline.size()>=3) {
      auto a=f.outline[0],b=f.outline[1],c=f.outline[2];
      if(std::abs(a.y-2.72)<1e-8 && std::abs(b.y-a.y)<1e-8 && std::abs(c.y-a.y)<1e-8) {
        ++soffits;soffitsOutward&=(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z)<0;
      }
    }
    check(soffits==joists.size() && soffitsOutward,"joist bottom faces point into the room and survive backface culling");
    plan.ceilings={{{.2,3,.2},{5.8,3,.2},{5.8,3,5.8},{.2,3,5.8}}};
    structure=buildStructure(plan);size_t insetJoists=0;
    for(const auto &f:structure.faces)insetJoists+=f.timberRole==TimberRole::CeilingJoist;
    check(insetJoists>0,"ceiling joists fit the wall inset used by generated dwellings");
  }
  {
    namespace gen=procedural::world::generator;
    gen::CellDwellingBrief brief;brief.id=901;brief.setbackMetres=4;brief.frontage={0};
    for(auto p:std::vector<Position>{{0,0,0},{22,0,0},{22,0,22},{0,0,22}})
      brief.lot.outer.push_back({procedural::geom2::toUnit(p.x),procedural::geom2::toUnit(p.z)});
    brief.capacity=gen::CellDwellingBrief::Capacity{48,3,15,false,false};
    gen::CellDwellingSettings settings;settings.maximumFloors=1;settings.entranceFlightCells=2;
    gen::CellHouse house;gen::Ledger ledger;rewrite::DwellingPlan plan;std::string error;bool built=false;
    for(uint64_t seed=1;seed<=30 && !built;++seed) {
      brief.seed=seed;
      built=gen::planCellDwelling(brief,settings,house,ledger,&error) && house.siteAccess.rise>.54 &&
            gen::realiseCellDwelling(house,settings,plan,&error);
    }
    check(built,"raised-platform fixture builds with more than 54 cm of visible elevation");
    if(built) {
      // A generated roof must have an actual oversail, with downward-facing
      // closed wood soffits, rather than relying on a two-sided tile plane.
      auto eaveSettings=settings;eaveSettings.eavesOverhangMetres=.5;
      rewrite::DwellingPlan eavePlan;
      bool roofBuilt=gen::realiseCellDwelling(house,eaveSettings,eavePlan,&error);
      size_t underside=0;bool timber=true;
      if(roofBuilt)for(const auto &f:buildStructure(eavePlan).faces)if(f.kind==FaceKind::Soffit) {
        timber &= f.material==Material::Timber;
        if(f.outline.size()>=3) {
          auto a=f.outline[0],b=f.outline[1],c=f.outline[2];
          if(std::abs(a.y-b.y)<1e-8 && std::abs(a.y-c.y)<1e-8 &&
             (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z)<-1e-8)++underside;
        }
      }
      check(roofBuilt && underside>0 && timber,"generated overhang has downward-facing timber soffits");
      auto structure=buildStructure(plan);Solid solid;meshSkeleton(skeletonOf(structure),solid);
      size_t faces=0,landings=0;for(const auto &f:structure.faces)faces+=f.siteSupport;
      for(const auto &s:plan.siteAccess.slabs)landings+=s.landing;
      check(faces>0 && solid.foundations.size()==faces,"all platform and stair faces survive the structure-to-mesh pass");
      size_t paved=0,dark=0;
      for(const auto &face:solid.foundations)if(!face.empty()) {
        paved+=face.front().material==Solid::SurfaceMaterial::Paving;
        dark+=face.front().material==Solid::SurfaceMaterial::DarkStone;
      }
      check(paved>0 && dark>0,"platform walking surfaces are paved while support sides remain dark stone");
      check(landings>=2,"free lot cells widen the level entrance platform");
      if(argc>1)writeSolid(solid,std::filesystem::path(argv[1])/"raised-platform.json",plan.siteAccess.rise);
    }
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Domestic,false);
    // A neighboring lower roof crosses the back wall at window height.
    plan.roofs.push_back({{{-1,1.7,6.5},{13,1.7,6.5},{13,2.2,8},{-1,2.2,8}},0,false,false});
    auto structure=buildStructure(plan);
    bool clear=true;size_t retained=0;
    for(const auto &face:structure.faces)if(face.kind==FaceKind::WindowedWall) {
      ++retained;
      if(std::abs(face.outline[0].z-7)<.3 && std::abs(face.outline[1].z-7)<.3)clear=false;
    }
    check(clear && retained>0,"roof clearance suppresses both conflicting window skins without removing unrelated windows");
  }
  for(int variant=0;variant<5;++variant) {
    auto plan=pavilion(variant==1?rewrite::FacadeStyle::Romanesque:variant==2?rewrite::FacadeStyle::Gothic:rewrite::FacadeStyle::Domestic,variant==1,variant==3);
    if(variant==4){plan.facade.maximumWindows=1;plan.facade.glazed=false;}
    auto structure=buildStructure(plan);auto skeleton=skeletonOf(structure);Solid solid;meshSkeleton(skeleton,solid);
    std::map<uint32_t,int> frames,leaves;std::set<uint32_t> windowSegments;bool halfPillars=false;
    for(auto face:structure.faces){if(face.kind==FaceKind::WindowedWall)windowSegments.insert(face.segment);halfPillars|=face.leftEdge!=FacadeEdge::Plain;}
    for(auto instance:structure.instances) {
      if(instance.asset==FacadeAsset::Frame || instance.asset==FacadeAsset::PortalFrame)++frames[instance.segment];
      if(instance.asset==FacadeAsset::DoorLeaf || instance.asset==FacadeAsset::DoorLeafLeft || instance.asset==FacadeAsset::DoorLeafRight)++leaves[instance.segment];
    }
    bool paired=!frames.empty();for(auto [id,count]:frames)paired&=count==2 || (count==3 && !windowSegments.count(id));
    check(paired,"openings retain paired frames with an optional exterior monumental surround");
    if(variant==1)check(halfPillars && leaves.empty() && solid.joinery.empty(),"open arcade has half-pillar ownership, no door leaves and no glass");
    else {bool doubleLeaf=leaves.size()==1 && leaves.begin()->second==2;check(doubleLeaf,"double doorway has two leaves total, not duplicated per skin");}
    if(variant==4)check(windowSegments.size()==1 && solid.joinery.empty(),"poor facade budget is one physical unglazed opening");
    if(argc>1)writeSolid(solid,std::filesystem::path(argv[1])/("building-"+std::to_string(variant)+".json"));
  }
  size_t elevatedTurrets=0;
  // Material gallery uses real planner output, not a separately modeled facade.
  for(int variant=0;variant<2;++variant) {
    namespace gen=procedural::world::generator;
    gen::CellDwellingBrief brief;brief.id=170+variant;brief.seed=81+variant;brief.frontage={0};
    brief.building=variant==0?gen::BuildingClass::Hut:gen::BuildingClass::Inn;
    brief.standing=variant==0?gen::PlotStanding::Slum:gen::PlotStanding::Comfortable;
    const double size=variant==0?10:24;
    for(auto p:std::vector<Position>{{0,0,0},{size,0,0},{size,0,size},{0,0,size}})
      brief.lot.outer.push_back({procedural::geom2::toUnit(p.x),procedural::geom2::toUnit(p.z)});
    gen::CellDwellingSettings settings;if(variant==0)settings.maximumFloors=1;
    gen::CellHouse house;gen::Ledger ledger;rewrite::DwellingPlan plan;std::string error;
    const bool built=gen::planCellDwelling(brief,settings,house,ledger,&error) && gen::realiseCellDwelling(house,settings,plan,&error);
    check(built,"wooden hut and inn gallery fixtures realize through the planner");
    if(built) {
      auto skeleton=buildSkeleton(plan);Solid solid;meshSkeleton(skeleton,solid);
      bool expectedWall=false;for(const auto &face:solid.walls)for(const auto &p:face)
        expectedWall|=p.material==(variant==0?Solid::SurfaceMaterial::Boarding:Solid::SurfaceMaterial::HalfTimber);
      check(expectedWall,"hut boarding and inn half-timber infill have distinct surface materials");
      bool noBuriedDoors=true;for(const auto &face:solid.timbers)for(const auto &p:face)
        if(p.material==Solid::SurfaceMaterial::Door)noBuriedDoors&=p.y>=-1e-6;
      check(noBuriedDoors,"buried cellar doors are not rendered without their omitted cellar walls");
      bool noBuriedTimbers=true;for(const auto &face:solid.timbers) {
        double top=-1e30;for(const auto &p:face)top=std::max(top,p.y);
        noBuriedTimbers&=top>=-1e-6; // Ground-level door bottom caps are valid.
      }
      check(noBuriedTimbers,"cellar timber facade is hidden consistently with buried walls");
      if(argc>1)writeSolid(solid,std::filesystem::path(argv[1])/(variant==0?"wood-hut.json":"inn.json"),0,&skeleton.structure,&plan);
    }
  }
  for(int variant=0;variant<7;++variant) {
    namespace gen=procedural::world::generator;
    gen::CellDwellingBrief brief;brief.seed=71;brief.id=71;
    brief.building=variant==0?gen::BuildingClass::Church:gen::BuildingClass::Dwelling;
    const double w=variant==0?24:10,d=variant==0?30:10;
    const double lotWidth=variant>=5?24:variant>=2?40+8*(variant-2):w,lotDepth=variant>=5?24:variant>=2?44+4*(variant-2):d;
    if(variant>=2){brief.building=gen::BuildingClass::Castle;brief.standing=gen::PlotStanding::Grand;brief.seed=variant==2?1:variant==3?7:19;brief.id=900+variant;}
    if(variant>=5) {brief.seed=variant==5?31:43;brief.setbackMetres=3.;}
    for(auto p:std::vector<Position>{{0,0,0},{lotWidth,0,0},{lotWidth,0,lotDepth},{0,0,lotDepth}})
      brief.lot.outer.push_back({procedural::geom2::toUnit(p.x),procedural::geom2::toUnit(p.z)});
    brief.frontage={0};gen::CellDwellingSettings settings;
    if(variant==1){brief.standing=gen::PlotStanding::Slum;settings.maximumFloors=1;settings.maximumWindows=1;
      brief.capacity=gen::CellDwellingBrief::Capacity{32,2,15,false,false};brief.capacity->minimumSpanCells=4;
    }
    gen::CellHouse house;gen::Ledger ledger;rewrite::DwellingPlan plan;std::string error;
    const bool built=gen::planCellDwelling(brief,settings,house,ledger,&error) && gen::realiseCellDwelling(house,settings,plan,&error);
    check(built,"cell blueprint realizes into the architectural facade pipeline");
    if(!built)std::cerr<<error<<"\n";
    if(built){auto skeleton=buildSkeleton(plan);Solid solid;meshSkeleton(skeleton,solid);
      if(plan.facade.stoneVaults) {
        bool stonePartitions=true;
        for(const auto &face:skeleton.structure.faces)
          if(face.kind==FaceKind::Wall || face.kind==FaceKind::WindowedWall || face.kind==FaceKind::DoorWall || face.kind==FaceKind::ArchWall)
            stonePartitions&=face.material==Material::Stone;
        check(stonePartitions,"stone buildings retain masonry on internal and exposed partition walls");
      }
      bool uvValid=true;size_t mapped=0;
      static const char *kGroupNames[]={"walls","innerWalls","gables","slopes","soffits","pillars","joinery","reveals",
        "innerReveals","decking","stairs","timbers","furniture","dormerWalls","dormerWindows","dormerRoofs",
        "dormerCeilings","yard","foundations"};
      size_t groupIndex=0;
      for(const auto *group:{&solid.walls,&solid.innerWalls,&solid.gables,&solid.slopes,&solid.soffits,&solid.pillars,
                            &solid.joinery,&solid.reveals,&solid.innerReveals,&solid.decking,&solid.stairs,&solid.timbers,
                            &solid.furniture,&solid.dormerWalls,&solid.dormerWindows,&solid.dormerRoofs,&solid.dormerCeilings,
                            &solid.yard,&solid.foundations}){const char *groupName=kGroupNames[groupIndex++];(void)groupName;for(const auto &face:*group) {
        const bool before=uvValid;
        if(face.size()<3)continue;
        for(auto p:face)uvValid&=std::isfinite(p.u)&&std::isfinite(p.v)&&p.uvIsland>0&&p.material!=Solid::SurfaceMaterial::Unassigned;
        for(size_t i=2;i<face.size();++i) {
          auto a=face[0],b=face[i-1],c=face[i];
          double ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z,vx=c.x-a.x,vy=c.y-a.y,vz=c.z-a.z;
          const double area=std::hypot(uy*vz-uz*vy,std::hypot(uz*vx-ux*vz,ux*vy-uy*vx));
          const double uvArea=std::abs((b.u-a.u)*(c.v-a.v)-(b.v-a.v)*(c.u-a.u));
          if(area>1e-9){uvValid&=uvArea>1e-10;++mapped;}
        }
        if(before && !uvValid && std::getenv("UV_DEBUG"))
          std::cerr<<"UVFAIL group="<<groupName<<" corners="<<face.size()<<" island="<<face[0].uvIsland
                   <<" material="<<int(face[0].material)<<" p0=("<<face[0].x<<","<<face[0].y<<","<<face[0].z
                   <<") uv=("<<face[0].u<<","<<face[0].v<<")\n";
      }}
      check(uvValid && mapped>0,"every nondegenerate building surface has finite noncollapsed UVs and a material");
      bool furnitureClear=true;
      for(const auto &item:plan.furniture)for(auto p:item.footprint)for(const auto &wall:plan.walls) {
        if(wall.floor!=item.floor || wall.kind!=rewrite::WallKind::Solid)continue;
        if(wall.a.y>=p.y+1.15 || wall.a.y+wall.height<=p.y)continue;
        const double dx=wall.b.x-wall.a.x,dz=wall.b.z-wall.a.z,length2=dx*dx+dz*dz;
        if(length2<1e-10)continue;
        const double t=std::clamp(((p.x-wall.a.x)*dx+(p.z-wall.a.z)*dz)/length2,0.,1.);
        const double distance=std::hypot(p.x-wall.a.x-dx*t,p.z-wall.a.z-dz*t);
        if(furnitureClear && distance<(wall.exterior?wall.thickness:wall.thickness*.5)+.015)
          std::cerr<<"Furniture clearance variant="<<variant<<" room="<<item.room<<" wall="<<wall.id<<" exterior="<<wall.exterior<<" distance="<<distance<<" thickness="<<wall.thickness<<" point="<<p.x<<","<<p.z<<" wall="<<wall.a.x<<","<<wall.a.z<<" to "<<wall.b.x<<","<<wall.b.z<<"\n";
        furnitureClear&=distance>=(wall.exterior?wall.thickness:wall.thickness*.5)+.015;
      }
      check(furnitureClear,"furniture footprints clear inner wall surfaces with a non-coplanar margin");
      if(variant>=5) check(house.courts.empty() && house.plannedFloors>=4,"compact keep preserves its footprint and grows vertically");
      for(const auto &t:plan.wallTurrets) {
        ++elevatedTurrets;
        check(t.footprint.size()==(t.round?6u:4u),"tower chambers use hexagonal or square footprints");
        bool access=false,room=false;
        for(const auto &d:plan.doors)access|=d.wall==t.hostWall && d.room==t.room && d.fromRoom!=0;
        for(const auto &r:plan.rooms)room|=r.id==t.room && r.floor==t.floor && r.usableArea>0;
        check(t.base>0 && t.corbelBase<t.base && t.top>t.base && access && room,
              "elevated turret has a supported hollow chamber and same-floor access");
      }
      bool singleSurface=true;
      for(const auto &floor:plan.floors)for(const auto &paving:floor.paving)for(const auto &slab:floor.slabs) {
        if(paving.size()!=slab.size() || paving.empty())continue;
        double px=0,py=0,pz=0,sx=0,sy=0,sz=0;
        for(auto p:paving){px+=p.x;py+=p.y;pz+=p.z;}for(auto p:slab){sx+=p.x;sy+=p.y;sz+=p.z;}
        if(std::abs(px-sx)<1e-7 && std::abs(py-sy)<1e-7 && std::abs(pz-sz)<1e-7)singleSurface=false;
      }
      check(singleSurface,"outdoor paving cells do not also receive coincident floor slabs");
      size_t ceilings=0;bool owned=true;
      for(const auto &face:skeleton.structure.faces)if(face.kind==FaceKind::Ceiling){++ceilings;owned&=face.object!=0;}
      check(!plan.ceilings.empty() && ceilings>=2*plan.ceilings.size() && owned,
            "room ceilings survive blueprint and structure with interior ownership");
      if(variant==0) {
        check(!house.bellTowers.empty(),"church fixture exercises a bell chamber");
        bool roofUp=true;
        for(const auto &roof:plan.roofs)if(roof.wing==house.floors.size() && roof.polygon.size()>=3){
          const auto a=roof.polygon[0],b=roof.polygon[1],c=roof.polygon[2];
          roofUp&=(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z)>0;
        }
        check(roofUp,"bell deck and cap face outward for back-face culling");
        auto turned=house;
        const double cs=std::cos(.63),sn=std::sin(.63);
        const auto rotate=[&](double &x,double &z){const double old=x;x=cs*x-sn*z;z=sn*old+cs*z;};
        rotate(turned.frame.originX,turned.frame.originZ);
        rotate(turned.frame.axisX,turned.frame.axisZ);
        for(auto &tower:turned.bellTowers)rotate(tower.x,tower.z);
        for(auto &slab:turned.siteAccess.slabs)for(auto &p:slab.polygon)rotate(p.x,p.z);
        rewrite::DwellingPlan rotated;
        check(gen::realiseCellDwelling(turned,settings,rotated,&error),"rotated church blueprint realizes");
        std::vector<Position> originalTower,rotatedTower;
        for(const auto &roof:plan.roofs)if(roof.wing==house.floors.size())
          originalTower.insert(originalTower.end(),roof.polygon.begin(),roof.polygon.end());
        for(const auto &roof:rotated.roofs)if(roof.wing==house.floors.size())
          rotatedTower.insert(rotatedTower.end(),roof.polygon.begin(),roof.polygon.end());
        bool equivariant=!originalTower.empty() && originalTower.size()==rotatedTower.size();
        if(equivariant)for(size_t i=0;i<originalTower.size();++i){auto p=originalTower[i],q=rotatedTower[i];rotate(p.x,p.z);
          equivariant&=std::hypot(p.x-q.x,p.z-q.z)<1e-5 && std::abs(p.y-q.y)<1e-5;}
        check(equivariant,"tower deck, stair aperture and cap rotate with their shaft without enlarging");
        if(argc>1){auto rs=buildSkeleton(rotated);Solid mesh;meshSkeleton(rs,mesh);
          writeSolid(mesh,std::filesystem::path(argv[1])/"rotated-church.json",0,&rs.structure,&rotated);}

        for(const auto &tower:house.bellTowers) {
          const rewrite::DwellingRoom *chamber=nullptr;
          for(const auto &room:plan.rooms)if(room.kind==rewrite::RoomKind::Belfry && !room.boundary.empty() &&
            std::abs(room.boundary.front().y-tower.deckMetres)<1e-6)chamber=&room;
          check(chamber!=nullptr,"bell chamber is registered as an interior volume");
          if(!chamber)continue;
          bool ceiling=false,skinInside=true;size_t skins=0;
          for(const auto &ring:plan.ceilings)if(ring.size()==chamber->boundary.size() &&
            std::abs(ring.front().y-chamber->ceilingHeight)<1e-6)ceiling=true;
          for(const auto &face:skeleton.structure.faces)if(face.segment!=UINT32_MAX && !face.outline.empty() &&
            face.outline.front().y>=tower.deckMetres-1e-6 && face.outline.front().y<chamber->ceilingHeight) {
            ++skins;
            for(const auto &p:face.outline)for(size_t k=0;k<chamber->boundary.size();++k) {
              auto a=chamber->boundary[k],b=chamber->boundary[(k+1)%chamber->boundary.size()];
              skinInside&=(b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x)>=-1e-5;
            }
          }
          check(ceiling,"bell chamber has a ceiling closing its roof underside");
          check(skins>0 && skinInside,"bell chamber wall skins stay within the cap footprint");
          bool duplicate=false;
          for(const auto &ring:plan.ceilings)if(!ring.empty() && std::abs(ring.front().y-tower.deckMetres)<1e-6)duplicate=true;
          check(!duplicate,"bell deck replaces the original ceiling and preserves stair access");
        }
      }
      if(variant==1)check(plan.dormers.empty() && solid.joinery.empty(),"poor home cannot bypass the unglazed budget through dormers");
      if(variant==1){bool joists=false;for(const auto &f:skeleton.structure.faces)joists|=f.timberRole==TimberRole::CeilingJoist;
        check(joists,"flat room ceilings have structural supporting joists");}
      check(!skeleton.structure.instances.empty(),"generated dwelling retains atlas instances in its structural skeleton");
      if(variant>=2){size_t arches=0,windows=0;for(const auto &f:skeleton.structure.faces){arches+=f.kind==FaceKind::ArchWall;windows+=f.kind==FaceKind::WindowedWall;}
        std::cout<<"CASTLE "<<brief.seed<<" rooms="<<plan.rooms.size()<<" arches="<<arches<<" windows="<<windows<<" turrets="<<plan.wallTurrets.size()<<" floors="<<house.floors.size()<<"\n";
        check(!plan.facade.glazed && arches>0 && windows>0,"castle sample has arched circulation and unglazed window openings");}
      if(argc>1)writeSolid(solid,std::filesystem::path(argv[1])/(variant>=2?"generated-castle-"+std::to_string(brief.seed)+".json":variant==0?"generated-church.json":"generated-cottage.json"),0,&skeleton.structure,&plan);
    }
  }
  check(elevatedTurrets>0,"castle fixtures exercise elevated wall turrets");
  {
    namespace gen=procedural::world::generator;namespace g=procedural::geom2;
    const auto rect=[](double x,double z,double w,double d){g::Polygon p;for(auto q:std::vector<Position>{{x,0,z},{x+w,0,z},{x+w,0,z+d},{x,0,z+d}})p.outer.push_back({g::toUnit(q.x),g::toUnit(q.z)});return p;};
    gen::CellBlockBrief block;block.id=80;block.seed=80;block.cellMetres=2;block.block=rect(0,0,26,26);
    gen::CellBlockLotBrief lot;lot.id=81;lot.storeys=2;lot.polygon=rect(4,4,16,16);block.lots.push_back(lot);
    gen::CellBlockPassage passage;passage.upperLot=0;passage.request={82,rect(10,4,4,16),2.4};block.passages.push_back(passage);
    gen::CellBlockPlan planned;gen::Ledger ledger;std::string error;
    bool ok=gen::planCellBlock(block,planned,ledger,&error);check(ok,"block allocates public passage cells independently of upper ownership");
    size_t publicCells=0;bool connected=true;
    for(size_t i=0;i<planned.cells.size();++i)if(planned.cells[i].kind==gen::BlockCellKind::CoveredPassage){++publicCells;connected&=planned.cells[i].lot==UINT32_MAX && planned.cells[i].upperLot==0 && !planned.pavedGraph[i].empty();}
    check(publicCells==16 && connected,"covered cells retain public paving graph and upper lot owner");
    gen::CellHouse house;rewrite::DwellingPlan dwelling;gen::CellDwellingSettings settings;settings.maximumFloors=2;
    bool built=false;
    if(ok)for(uint64_t seed=1;seed<=30 && !built;++seed) {
      auto brief=planned.buildings.front().dwelling;brief.seed=seed;
      built=gen::planCellDwelling(brief,settings,house,ledger,&error) && gen::realiseCellDwelling(house,settings,dwelling,&error);
    }
    check(built,"block passage builds through the cell blueprint, structure and mesh pipeline");
    if(!built)std::cerr<<error<<"\n";
    if(built) {
      check(dwelling.coveredPassages.size()==1 && dwelling.coveredPassages.front().cells.size()==16 && !dwelling.coveredPassages.front().portalWalls.empty(),"dwelling retains passage cells, room ownership and portal walls");
      check((house.coveredPassage & ~house.floors[1].area).empty(),"occupied upper floor covers the entire passage");
      bool room=false;for(size_t r=0;r<house.floors[0].roomAreas.size();++r)if(house.floors[0].roomAreas[r]==house.coveredPassage)room=house.floors[0].roomKinds[r]==rewrite::RoomKind::Passage;
      check(room,"public passage cannot be allocated as a bedroom or shop");
      auto skeleton=buildSkeleton(dwelling);Solid solid;meshSkeleton(skeleton,solid);
      size_t arches=0;bool height=true;
      for(auto &face:skeleton.structure.faces)if(face.passagePortal){++arches;height&=face.aperture==ApertureShape::SegmentalArch && face.high*rewrite::kPassageArchSpring*.92>=passage.request.clearance-1e-6;}
      check(arches==4 && height,"two passage mouths have paired arched skins and required clear headroom");
      size_t exterior=0;for(auto &connection:skeleton.roomConnections)exterior+=connection.kind==RoomConnectionKind::Exterior;
      check(exterior>=2,"passage portals are present in the room circulation graph");
      auto rejected=passage.request;rejected.clearance=9;auto before=house.coveredPassage;
      check(!gen::reserveCoveredPassage(rejected,house,&error) && house.coveredPassage==before,"invalid clearance is rejected without changing passage cells");
      if(argc>1)writeSolid(solid,std::filesystem::path(argv[1])/"covered-passage.json",0,&skeleton.structure,&dwelling);
    }
    g::CellFrame a,b;a.cellMetres=b.cellMetres=2;b.originX=8;
    check(gen::passageGridsAlign(a,0,3,b,0,3),"whole-cell neighboring lattices can be joined");
    b.originX=8.5;check(!gen::passageGridsAlign(a,0,3,b,0,3),"half-cell offset cannot be joined");
    b.originX=8;check(!gen::passageGridsAlign(a,0,3,b,1,3),"mismatched ground levels cannot be joined");
    b.axisX=.8;b.axisZ=.6;check(!gen::passageGridsAlign(a,0,3,b,0,3),"oblique neighboring grids cannot be joined");
  }
  {
    std::set<ApertureShape> shapes;bool hasOpen=false,hasGlazed=false,paired=true,hasTracery=false;
    for(uint64_t seed=0;seed<4;++seed) {
      auto p=pavilion(rewrite::FacadeStyle::Gothic,false);p.facade.masonryVariation=true;p.facade.variationSeed=seed;
      auto skeleton=buildSkeleton(p);const auto &structure=skeleton.structure;
      std::map<uint32_t,std::set<ApertureShape>> perSegment;std::set<uint32_t> windows,glass;std::map<uint32_t,unsigned> frames;
      for(const auto &face:structure.faces)if(face.kind==FaceKind::WindowedWall) {windows.insert(face.segment);shapes.insert(face.aperture);perSegment[face.segment].insert(face.aperture);}
      for(const auto &instance:structure.instances) {if(instance.asset==FacadeAsset::Glass)glass.insert(instance.segment);if(instance.asset==FacadeAsset::Frame)++frames[instance.segment];}
      for(auto segment:windows){hasOpen|=!glass.count(segment);hasGlazed|=glass.count(segment);hasTracery|=frames[segment]>2;}
      for(const auto &[segment,forms]:perSegment)paired&=forms.size()==1;
      if(argc>1 && seed==0){Solid solid;meshSkeleton(skeleton,solid);writeSolid(solid,std::filesystem::path(argv[1])/"varied-masonry.json",0,&structure,&p);}
    }
    check(hasOpen && hasGlazed,"masonry variation contains genuinely open apertures alongside glazing");
    check(shapes.size()>=3 && hasTracery,"masonry facades mix arch profiles and paired-light tracery");
    check(paired,"both wall skins agree on each varied opening profile");
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Gothic,false);
    for(auto &o:plan.openings)if(o.kind==rewrite::OpeningKind::Door)o.top=o.head=2.3;
    auto structure=buildStructure(plan);bool clear=true;unsigned doors=0;
    for(const auto &f:structure.faces)if(f.kind==FaceKind::DoorWall || f.kind==FaceKind::DoubleDoorWall) {
      ++doors;clear &= f.aperture==ApertureShape::Rectangle && (f.high-f.low)*.92>=2.1;
    }
    check(doors>0 && clear,"domestic-height doors retain full-height lintels on both interior and exterior skins");
  }
  {
    Structure structure;StructureFace f;f.kind=FaceKind::WindowedWall;f.segment=1;
    f.outline={{0,0,0},{3,0,0},{3,3.5,0},{0,3.5,0}};f.low=1.2;f.high=2.5;f.along0=.35;f.along1=.65;f.thickness=.2;
    structure.faces.push_back(f);rewrite::DwellingFacadePolicy policy;policy.enabled=true;
    planFacadeInstances(structure,policy);
    double height=structure.faces[0].high-structure.faces[0].low;
    lowerWindowHead(structure,0,2.2);
    check(std::abs(structure.faces[0].high-structure.faces[0].low-height)<1e-8 && structure.faces[0].low>=.7,
      "low ceiling moves window sill before compressing the window model");
    bool matched=true;for(const auto &i:structure.instances)matched &= i.origin.y>=structure.faces[0].low-1e-8 && i.origin.y+i.height<=2.2+1e-8;
    check(matched && !structure.faces[0].openingReveals.empty(),"lowered windows keep detailed frames and interior reveals within their apertures");
  }
  {
    Structure s;StructureFace f;f.kind=FaceKind::ArchWall;f.segment=1;f.high=2.5;f.along0=.3;f.along1=.7;
    f.outline={{0,0,0},{3,0,0},{3,3,0},{0,3,0}};s.faces.push_back(f);
    rewrite::DwellingFacadePolicy p;p.enabled=true;p.style=rewrite::FacadeStyle::Gothic;planFacadeInstances(s,p);
    check(s.faces[0].aperture==ApertureShape::RoundArch && s.faces[0].high*s.faces[0].archSpring*.92>=2.1-1e-8,
      "domestic interior arches retain over two metres of clear spring-line headroom");
    const auto &arch=s.faces[0];
    const double rise=arch.high*(1-arch.archSpring);
    check(rise>.5 && arch.high<=2.85+1e-8,"domestic arch uses ceiling space for a rounder crown");
    bool matched=true;for(const auto &i:s.instances)if(i.asset==FacadeAsset::PortalFrame)
      matched &= std::abs(i.archSpring-arch.archSpring)<1e-8 && std::abs(i.height-arch.high)<1e-8;
    check(matched,"doorway frame and carved wall share the same spring and height");
    s.faces[0].outline[2].y=s.faces[0].outline[3].y=3.4;
    p.style=rewrite::FacadeStyle::Romanesque;
    planFacadeInstances(s,p);
    const auto &round=s.faces[0];
    const double radius=(round.along1-round.along0)*3*.5;
    const double springY=round.high*round.archSpring;
    bool circular=true;for(auto point:apertureContour(round.aperture,round.archSpring))if(point.y>0)
      circular &= std::abs(std::hypot((point.x-.5)*radius*2,point.y*round.high-springY)-radius)<1e-8;
    check(circular,"roomy doorways have a semicircular crown in physical metres");
  }
  {
    const auto capArea=[](double covered,double elevation) {
      rewrite::DwellingPlan plan;
      rewrite::DwellingWall wall;wall.id=1;wall.a={0,0,0};wall.b={4,0,0};wall.height=3;wall.thickness=.2;wall.exterior=true;plan.walls.push_back(wall);
      if(covered>0){rewrite::DwellingFloor floor;floor.elevation=elevation;floor.slabs={{{-1,elevation,-1},{covered,elevation,-1},{covered,elevation,1},{-1,elevation,1}}};plan.floors.push_back(floor);}
      Solid mesh;meshSkeleton(buildSkeleton(plan),mesh);double area=0;
      for(const auto &face:mesh.reveals){bool horizontal=face.size()>=3;for(auto p:face)horizontal &= std::abs(p.y-3)<1e-8;if(!horizontal)continue;
        double a=0;for(size_t i=0;i<face.size();++i){auto p=face[i],q=face[(i+1)%face.size()];a+=p.x*q.z-q.x*p.z;}area+=std::abs(a)*.5;}
      return area;
    };
    const double full=capArea(0,3);
    check(full>.5 && std::abs(capArea(5,3))<1e-8,"upper floor owns the surface over a fully covered wall cap");
    check(std::abs(capArea(2,3)-full*.5)<1e-6,"partial floor coverage trims the cap and preserves its exposed shoulder");
    check(std::abs(capArea(5,3.1)-full)<1e-6,"floors at different elevations do not remove exposed wall caps");
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Gothic,false);
    const auto structure=buildStructure(plan);
    size_t bars=0;bool complete=true;
    for(const auto &bar:structure.instances)if(bar.asset==FacadeAsset::Mullion){
      ++bars;bool matches=false;
      for(const auto &frame:structure.instances)if(frame.asset==FacadeAsset::Frame)
        matches |= std::abs(bar.origin.y-frame.origin.y)<1e-8 && std::abs(bar.height-frame.height)<1e-8;
      complete &= matches;
    }
    check(bars>0 && complete,"Gothic window mullions reach the full aperture height");
  }
  {
    rewrite::DwellingPlan plan;plan.floors.emplace_back();plan.floors.back().height=3;
    rewrite::DwellingStair stair;stair.lowerFloor=0;stair.startRise=1.5;stair.rise=1.5;stair.risers=10;
    stair.opening={{0,1.5,0},{1.5,1.5,0},{1.5,3,3},{0,3,3}};
    stair.upperLanding={{0,3,3},{1.5,3,3},{1.5,3,4},{0,3,4}};plan.stairs.push_back(stair);
    Solid mesh;meshSkeleton(buildSkeleton(plan),mesh);
    size_t treads=0;bool supported=false,arrival=false;
    for(const auto &f:mesh.stairs){if(f.size()<3)continue;bool level=true;for(auto p:f)level &= std::abs(p.y-f.front().y)<1e-8;
      if(level && f.front().y>1.5 && f.front().y<3)treads++;
      if(level && std::abs(f.front().y)<1e-8)supported=true;
      if(level && std::abs(f.front().y-3)<1e-8)arrival=true;
    }
    check(treads==9,"return flight emits discrete level treads rather than an inclined sheet");
    check(supported && arrival,"return flight is supported to its storey floor and meets its arrival landing");
  }
  {
    rewrite::DwellingPlan plan;plan.cellMetres=2;plan.facade.enabled=true;plan.facade.arcadeWalls={1};
    rewrite::DwellingWall wall;wall.id=1;wall.a={0,0,0};wall.b={8,0,0};wall.height=3;wall.thickness=.2;wall.exterior=true;plan.walls.push_back(wall);
    plan.floors.emplace_back();plan.floors.back().height=3;
    const auto arches=[](const Structure &s){size_t n=0;for(const auto &f:s.faces)n+=f.kind==FaceKind::ArchWall;return n;};
    const auto before=arches(buildStructure(plan));
    rewrite::DwellingStair stair;stair.lowerFloor=0;stair.rise=3;stair.risers=16;
    stair.opening={{0,0,.2},{2,0,.2},{2,3,3},{0,3,3}};plan.stairs.push_back(stair);
    const auto after=arches(buildStructure(plan));
    check(before>after && after>0,"decorative arcades close obstructed stair bays and preserve clear bays");
  }
  {
    Structure s;
    StructureFace wall;wall.object=0;wall.segment=1;wall.kind=FaceKind::WindowedWall;wall.thickness=.2;
    wall.outline={{0,0,0},{4,0,0},{4,3,0},{0,3,0}};wall.low=1;wall.high=2.2;wall.along0=.05;wall.along1=.30;
    s.faces.push_back(wall);
    auto back=wall;back.object=1;back.outline={{4,0,.2},{0,0,.2},{0,3,.2},{4,3,.2}};back.along0=.70;back.along1=.95;s.faces.push_back(back);
    auto side=wall;side.segment=2;side.kind=FaceKind::Wall;side.high=side.low=0;
    side.outline={{0,0,-3},{0,0,0},{0,3,0},{0,3,-3}};s.faces.push_back(side);
    rewrite::DwellingFacadePolicy policy;policy.enabled=true;
    planFacadeInstances(s,policy);
    check(s.faces[0].kind==FaceKind::WindowedWall && s.faces[0].along0*4>=.719,
          "corner window moves clear of alternating quoins and its frame margin");
    check(std::abs(s.faces[0].along0-(1-s.faces[1].along1))<1e-8 &&
          std::abs(s.faces[0].along1-(1-s.faces[1].along0))<1e-8,
          "moved corner window keeps its interior and exterior apertures aligned");
    bool blocks=false,frame=false;
    for(const auto &i:s.instances){blocks |= i.asset==FacadeAsset::WallBlock;frame |= i.asset==FacadeAsset::Frame;}
    check(blocks && frame,"corner clearance preserves both masonry quoins and usable glazing");
    Structure narrow;wall.outline={{0,0,0},{1.2,0,0},{1.2,3,0},{0,3,0}};wall.along0=.1;wall.along1=.9;
    narrow.faces={wall,side};auto rightSide=side;rightSide.segment=3;
    for(auto &p:rightSide.outline)p.x+=1.2;narrow.faces.push_back(rightSide);
    planFacadeInstances(narrow,policy);
    bool glass=false;for(const auto &i:narrow.instances)glass |= i.asset==FacadeAsset::Frame;
    check(narrow.faces[0].kind==FaceKind::Wall && !glass,
          "a wall too narrow for quoins and a useful window closes the opening before meshing");
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Domestic,false);
    plan.facade.inwardWalls=true;plan.openings.clear();
    rewrite::DwellingWall partition;partition.id=99;partition.a={0,0,0};partition.b={3,0,3};partition.height=3.8;partition.thickness=.15;
    plan.walls.push_back(partition);
    auto skeleton=buildSkeleton(plan);unsigned joined=0;
    for(const auto &face:skeleton.structure.faces)if(face.object!=0 && face.kind==FaceKind::Wall)
      for(auto p:face.outline)if(std::abs(p.x-.2)<1e-6 && std::abs(p.z-.2)<1e-6 && p.y<1e-6)++joined;
    check(joined>=2,"partition at exterior corner preserves the closed inward wall miter");
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Gothic,false);plan.openings.clear();plan.doors.clear();
    plan.facade.inwardWalls=true;plan.floors.front().height=3.;plan.rooms.front().ceilingHeight=6.;
    for(auto &wall:plan.walls)wall.height=6.;
    rewrite::DwellingOpening opening;opening.wall=1;opening.a={4.7,0,0};opening.b={7.3,0,0};opening.top=opening.head=3.6;opening.kind=rewrite::OpeningKind::Door;opening.exterior=true;
    plan.openings.push_back(opening);
    rewrite::DwellingDoor door;door.wall=1;door.center={6,0,0};door.width=2.6;door.privateAccess=true;door.exterior=true;door.leaves=2;plan.doors.push_back(door);
    auto structure=buildStructure(plan);unsigned skins=0;bool full=true;
    for(const auto &face:structure.faces)if(face.kind==FaceKind::DoubleDoorWall){++skins;full &= face.high>=3.6-1e-6 && face.low==0;}
    check(skins==2 && full,"monumental door remains one full-height aperture across nominal storey bands");
  }
  for(auto profile:{rewrite::DoorProfile::Rectangular,rewrite::DoorProfile::RoundArch,rewrite::DoorProfile::PointedArch}) {
    Structure structure;StructureFace door;door.kind=FaceKind::DoubleDoorWall;door.material=Material::Stone;
    door.outline={{0,0,0},{4,0,0},{4,5,0},{0,5,0}};door.low=0;door.high=3.6;door.along0=.175;door.along1=.825;door.thickness=.4;
    structure.faces.push_back(door);rewrite::DwellingFacadePolicy policy;policy.enabled=true;policy.doorProfile=profile;
    planFacadeInstances(structure,policy);
    const auto expected=profile==rewrite::DoorProfile::Rectangular?ApertureShape::Rectangle:profile==rewrite::DoorProfile::RoundArch?ApertureShape::RoundArch:ApertureShape::PointedArch;
    unsigned leaves=0,details=0;for(const auto &i:structure.instances){leaves+=i.asset==FacadeAsset::DoorLeafLeft || i.asset==FacadeAsset::DoorLeafRight;details+=i.wooden && i.asset==FacadeAsset::WallBlock;}
    check(structure.faces.front().aperture==expected && leaves==2 && details>=8,"shared entrance profile retains two detailed leaves and matching wall aperture");
  }
  {
    auto plan=pavilion(rewrite::FacadeStyle::Domestic,false);plan.openings.clear();plan.doors.clear();
    plan.facade.inwardWalls=true;plan.floors.front().height=3.;plan.rooms.front().ceilingHeight=9.;
    plan.walls[0].height=plan.walls[3].height=9.;
    plan.walls[3].exterior=false;plan.walls[3].thickness=.15;
    auto structure=buildStructure(plan);unsigned joined=0;
    for(const auto &face:structure.faces)if(face.kind==FaceKind::Wall)
      for(auto p:face.outline)if(std::abs(p.x-.075)<1e-6 && std::abs(p.z-.2)<1e-6 && p.y>=6.)++joined;
    check(joined>=4,"tower corner joins unequal interior and exterior skins above the lower roof");
  }
  {
    rewrite::DwellingPlan plan;plan.floors.emplace_back();plan.floors.back().height=20.;
    rewrite::DwellingStair stair;stair.opening={{0,0,0},{1,0,0},{1,0,3},{0,0,3}};
    stair.startRise=10.;stair.rise=2.;stair.risers=10;stair.wallSupported=true;
    stair.upperLanding={{0,0,3},{1,0,3},{1,0,4},{0,0,4}};plan.stairs.push_back(stair);
    auto structure=buildStructure(plan);Solid solid;meshSkeleton(skeletonOf(structure),solid);
    bool shallow=!solid.stairs.empty();for(const auto &f:solid.stairs)for(const auto &v:f)shallow &= v.y>=9.8;
    for(const auto &f:solid.timbers)for(const auto &v:f)shallow &= v.y>=9.8;
    check(shallow,"wall-supported stair and landing never extrude to the ground");
  }
  return failures?1:0;
}
