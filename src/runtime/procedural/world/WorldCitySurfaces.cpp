#include "WorldCitySurfaces.hpp"
#include "WorldCityPaving.hpp"
#include "procedural/geom3/Prism.hpp"
#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Offset.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace procedural::world::rewrite {
namespace {
geom2::Polygon polygon(const std::vector<Position> &points)
{
  geom2::Polygon p;
  for (auto v:points) p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});
  if (!geom2::counterClockwise(p.outer)) geom2::reverse(p.outer);
  return p;
}
}
void nodeCitySurfaceBoundaries(CitySurfaceStructure &surfaces)
{
  // Boolean output may simplify one side of a shared edge independently.
  // Restore its neighbour's vertices before adaptive terrain subdivision so
  // both sides sample exactly the same terrain points, including on slopes.
  using Bucket=std::pair<int64_t,int64_t>;
  constexpr double cell=8.;
  // A millimetre, in the micrometre units the polygons are quantised to.
  constexpr long double kNodeToleranceUnits=1000.L;
  const auto bucket=[](geom2::Point p) {return Bucket{int64_t(std::floor(geom2::toMetres(p.x)/cell)),int64_t(std::floor(geom2::toMetres(p.y)/cell))};};
  std::map<Bucket,std::set<geom2::Point>> points;
  const auto collect=[&](const geom2::Ring &ring){for(auto p:ring)points[bucket(p)].insert(p);};
  for(const auto &piece:surfaces.pieces) {collect(piece.polygon.outer);for(const auto &h:piece.polygon.holes)collect(h);}
  const auto node=[&](geom2::Ring &ring) {
    geom2::Ring result;
    const std::set<geom2::Point> original(ring.begin(),ring.end());
    std::set<geom2::Point> inserted;
    for(size_t i=0;i<ring.size();++i) {
      const auto a=ring[i],b=ring[(i+1)%ring.size()];
      const long double dx=b.x-a.x,dy=b.y-a.y,l2=dx*dx+dy*dy;
      result.push_back(a);if(l2==0)continue;
      std::set<Bucket> visited;std::vector<std::pair<long double,geom2::Point>> interior;
      const unsigned steps=std::max(1u,unsigned(std::ceil(std::sqrt(l2)*geom2::kMetresPerUnit/(cell*.5))));
      for(unsigned j=0;j<=steps;++j) {
        auto key=bucket({a.x+int64_t(dx*j/steps),a.y+int64_t(dy*j/steps)});
        for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y) {
          Bucket k{key.first+x,key.second+y};if(!visited.insert(k).second)continue;
          auto found=points.find(k);if(found==points.end())continue;
          for(auto p:found->second) {
            const long double px=p.x-a.x,py=p.y-a.y,t=(px*dx+py*dy)/l2;
            if(t<=0 || t>=1 || original.count(p))continue;

            // Near-collinear counts, and what gets inserted is the foot of
            // the perpendicular, not the neighbour's vertex.
            //
            // Exact collinearity in micrometres is a test almost nothing
            // passes: the booleans leave the two sides of a shared edge
            // agreeing to a micron, not to the unit, so the edge kept its
            // straight chord while the neighbour followed the ground, and on
            // the terrace skirt the two parted by up to a metre. Inserting
            // the neighbour's own point would move this ring sideways by that
            // micron, which is what could turn a road face a few centimetres
            // wide inside out; the foot of the perpendicular lies on the edge
            // by construction, so the ring keeps its shape and simply gains a
            // vertex where its neighbour has one.
            if(std::abs(px*dy-py*dx)>kNodeToleranceUnits*std::sqrt(l2))continue;

            const geom2::Point foot{a.x+int64_t(std::llround(double(dx*t))),a.y+int64_t(std::llround(double(dy*t)))};
            if(foot==a || foot==b || original.count(foot))continue;
            interior.push_back({t,foot});
          }
        }
      }
      std::sort(interior.begin(),interior.end());
      for(auto [t,p]:interior)if(inserted.insert(p).second)result.push_back(p);
    }
    ring=std::move(result);
  };
  for(auto &piece:surfaces.pieces){node(piece.polygon.outer);for(auto &h:piece.polygon.holes)node(h);}
}

bool planCitySidewalks(CityCompletionPlan &city,double width,std::string *error) {
  if(!std::isfinite(width)||width<0){if(error)*error="Invalid sidewalk width";return false;}
  std::set<uint64_t> owners;
  for(const auto &p:city.skeleton.publicSpaces)if(p.kind==CityPublicSpace::Kind::Sidewalk)owners.insert(p.owner);
  if(owners.empty())return true;
  std::vector<geom2::Polygon> roads,expanded;
  for(const auto &r:city.routes){auto path=cityRoadPath(r.path);if(path.size()<2||r.halfWidth<=0)continue;
    geom2::OffsetOptions options;options.join=geom2::JoinStyle::Miter;options.miterLimit=2.;options.arcToleranceMetres=.1;
    std::vector<geom2::Polygon> inner,outer;
    if(!geom2::offsetPolyline(path,r.halfWidth,options,inner,error)||!geom2::offsetPolyline(path,r.halfWidth+width,options,outer,error))return false;
    roads.insert(roads.end(),inner.begin(),inner.end());expanded.insert(expanded.end(),outer.begin(),outer.end());
  }
  std::vector<CityPublicSpace> planned;
  for(const auto &block:city.skeleton.regions)if(block.kind==CityRegionKind::Block&&owners.count(block.id)) {
    const auto p=polygon(block.boundary);
    // Curving a street leaves small unowned gaps beside the original block.
    // The public shoulder may occupy those gaps, but never another block.
    geom2::OffsetOptions marginOptions;marginOptions.join=geom2::JoinStyle::Miter;marginOptions.miterLimit=2.;
    std::vector<geom2::Polygon> margins;
    if(!geom2::offsetPolygon(p,width,marginOptions,margins,error))return false;
    geom2::Operand domain;
    for(const auto &margin:margins){std::vector<geom2::Operand> neighbours;
      for(const auto &other:city.skeleton.regions)if(other.kind==CityRegionKind::Block&&other.id!=block.id){auto q=polygon(other.boundary);if(geom2::bounds(margin).overlaps(geom2::bounds(q)))neighbours.push_back(geom2::toOperand(q));}
      std::vector<geom2::Polygon> available;
      if(neighbours.empty())available.push_back(margin);
      else if(!geom2::differenceOf(geom2::toOperand(margin),neighbours,available,error))return false;
      for(const auto &q:available){auto op=geom2::toOperand(q);domain.rings.insert(domain.rings.end(),op.rings.begin(),op.rings.end());}
    }
    auto bounds=geom2::bounds(p);const auto margin=geom2::toUnit(width);bounds.minX-=margin;bounds.minY-=margin;bounds.maxX+=margin;bounds.maxY+=margin;
    geom2::Operand outer;std::vector<geom2::Operand> cuts;
    for(const auto &q:expanded)if(bounds.overlaps(geom2::bounds(q))){auto op=geom2::toOperand(q);outer.rings.insert(outer.rings.end(),op.rings.begin(),op.rings.end());}
    if(outer.rings.empty())continue;
    for(const auto &q:roads)if(bounds.overlaps(geom2::bounds(q)))cuts.push_back(geom2::toOperand(q));
    std::vector<geom2::Polygon> joined;
    if(!geom2::intersectionOf({domain,outer},joined,error))return false;
    for(const auto &q:joined){std::vector<geom2::Polygon> walks;if(!geom2::differenceOf(geom2::toOperand(q),cuts,walks,error))return false;
      for(const auto &walk:walks){
        const auto add=[&](const geom2::Ring &ring){std::vector<Position> boundary;for(auto v:ring)boundary.push_back({geom2::toMetres(v.x),0,geom2::toMetres(v.y)});planned.push_back({block.id,CityPublicSpace::Kind::Sidewalk,std::move(boundary),0});};
        if(walk.holes.empty())add(walk.outer);
        else{std::vector<std::array<uint32_t,3>> triangles;std::vector<geom2::Point> vertices;if(!geom3::triangulate(walk,triangles,vertices,error))return false;for(auto t:triangles)add({vertices[t[0]],vertices[t[1]],vertices[t[2]]});}
      }
    }
  }
  auto &spaces=city.skeleton.publicSpaces;
  spaces.erase(std::remove_if(spaces.begin(),spaces.end(),[](const auto &p){return p.kind==CityPublicSpace::Kind::Sidewalk;}),spaces.end());
  spaces.insert(spaces.end(),std::make_move_iterator(planned.begin()),std::make_move_iterator(planned.end()));
  return true;
}

bool buildCitySurfaces(const CityCompletionPlan &city, CitySurfaceStructure &out, std::string *error)
{
  out={};
  struct Segment { uint64_t owner; uint32_t index; Position a,b; double width; };
  std::vector<Segment> segments;
  std::vector<geom2::Polygon> bands;
  std::map<uint32_t,std::vector<geom2::Operand>> roadMaterials;
  std::vector<geom2::InputRing> rings;
  for (const auto &r:city.routes)
  {
    auto path=cityRoadPath(r.path);
    if(path.size()<2 || r.halfWidth<=0)continue;
    geom2::OffsetOptions options;options.join=geom2::JoinStyle::Miter;options.miterLimit=2.;options.arcToleranceMetres=.1;
    std::vector<geom2::Polygon> routeBands;
    if(!geom2::offsetPolyline(path,r.halfWidth,options,routeBands,error)) {if(error)*error="road band "+std::to_string(r.id)+": "+*error;return false;}
    for(auto &band:routeBands) {
      const uint32_t channel=r.groundCover==CityGroundCover::Stone ? 2 : r.groundCover==CityGroundCover::Grass ? 0 :
        r.groundCover==CityGroundCover::Dirt ? 1 : r.kind==CityRoadKind::FieldTrack || r.halfWidth<2.2 ? 1 : 2;
      roadMaterials[channel].push_back(geom2::toOperand(band));
      rings.push_back({band.outer,0,false});for(const auto &hole:band.holes)rings.push_back({hole,0,false});
      bands.push_back(std::move(band));
    }
    // Shared cross-sections divide the already unified surface into segment
    // pieces. They carry no area/winding, so they cannot duplicate a junction.
    for(size_t k=1;k+1<path.size();++k) {
      const auto position=[](geom2::Point q){return Position{geom2::toMetres(q.x),0,geom2::toMetres(q.y)};};
      const auto a=position(path[k-1]),p=position(path[k]),b=position(path[k+1]);
      const double dx=b.x-a.x,dz=b.z-a.z,len=std::hypot(dx,dz);
      if(len<1e-6)continue;
      const double nx=-dz/len*r.halfWidth*1.1,nz=dx/len*r.halfWidth*1.1;
      rings.push_back({{{geom2::toUnit(p.x+nx),geom2::toUnit(p.z+nz)},
                        {geom2::toUnit(p.x-nx),geom2::toUnit(p.z-nz)}},0,true});
    }
    for (size_t k=1;k<r.path.size();++k) {
    auto a=r.path[k-1],b=r.path[k]; const double len=std::hypot(b.x-a.x,b.z-a.z);
    if(len<1e-6 || r.halfWidth<=0) continue;
    segments.push_back({r.id,uint32_t(k-1),a,b,r.halfWidth});
    }
  }
  geom2::Arrangement network;
  if(!rings.empty() && !geom2::buildArrangement(rings,{},network,error)) {if(error)*error="road network: "+*error;return false;}
  for(const auto &face:network.faces)
  {
    if(face.unbounded || face.winding.empty() || face.winding[0]==0 || face.cycles.empty()) continue;
    geom2::Polygon p;
    for(size_t i=0;i<face.cycles.size();++i)
    {
      geom2::Ring ring; auto start=face.cycles[i],e=start;
      do {ring.push_back(network.vertices[network.halfEdges[e].origin]);e=network.halfEdges[e].next;} while(e!=start);
      // Open separators can contribute a two-edge, zero-area spur cycle.
      // It has no surface to subtract and is not a polygon hole.
      if(ring.size()<3 || geom2::doubleSignedArea(ring)==0)continue;
      if(geom2::counterClockwise(ring))p.outer=std::move(ring);else p.holes.push_back(std::move(ring));
    }
    if(p.outer.empty())continue;
    // Ownership only: the arrangement, not this nearest-segment choice,
    // determines geometry. Junction faces have one deterministic owner.
    double x=0,z=0;for(auto v:p.outer){x+=geom2::toMetres(v.x);z+=geom2::toMetres(v.y);}x/=p.outer.size();z/=p.outer.size();
    const Segment *best=nullptr;double distance=1e100;
    for(const auto &s:segments)
    {
      const double dx=s.b.x-s.a.x,dz=s.b.z-s.a.z;
      const double t=std::clamp(((x-s.a.x)*dx+(z-s.a.z)*dz)/(dx*dx+dz*dz),0.,1.);
      const double d=std::hypot(x-s.a.x-t*dx,z-s.a.z-t*dz)/s.width;
      if(d<distance){distance=d;best=&s;}
    }
    // An open separator may terminate in a junction and leave a zero-area
    // spur in the face walk. Canonicalise that walk before triangulation;
    // preserve all positive-area pieces, never fill holes or drop constraints.
    // Every face with nonzero road winding belongs to the carriageway.
    // Thin overlap partitions are still road: dropping them creates grass
    // slots that acquire their own curb when the network is inset below.
    if(geom2::polygonFault(p).empty())
      out.pieces.push_back({CitySurfaceKind::Road,best->owner,best->index,std::move(p),.035,0,true});
    else {
      std::vector<geom2::Polygon> canonical;
      if(!geom2::unionOf({geom2::toOperand(p)},canonical,error)) return false;
      for(auto &part:canonical) out.pieces.push_back({CitySurfaceKind::Road,best->owner,best->index,std::move(part),.035,0,true});
    }
  }
  // Material unions exclude artificial road segment boundaries from feathering.
  for(const auto &[channel,operands]:roadMaterials) {
    std::vector<geom2::Polygon> merged;if(!geom2::unionOf(operands,merged,error))return false;
    for(auto &p:merged)out.influences.push_back({std::move(p),channel,1,.9,true});
  }
  const auto subtract=[&](const geom2::Polygon &p,const std::vector<geom2::Polygon> &reserved,std::vector<geom2::Polygon> &result) {
    std::vector<geom2::Operand> clips;const auto box=geom2::bounds(p);
    for(const auto &q:reserved)if(box.overlaps(geom2::bounds(q)))clips.push_back(geom2::toOperand(q));
    if(clips.empty()){result={p};return true;}
    return geom2::differenceOf(geom2::toOperand(p),clips,result,error);
  };
  // Canonical ownership order, independent of input region iteration order.
  std::set<uint64_t> developedLots,inhabitedBlocks;
  for(const auto &lot:city.lots)if(lot.developed)developedLots.insert(lot.lot);
  for(const auto &r:city.skeleton.regions)if(developedLots.count(r.id))inhabitedBlocks.insert(r.parent);
  const bool occupancyDriven=!city.historicalLots.empty();
  std::map<uint64_t,const CitySkeletonRegion*> blocks,lots;
  for(const auto &r:city.skeleton.regions)if(r.boundary.size()>=3) {
    if(r.kind==CityRegionKind::Block)blocks[r.id]=&r;
    if(r.kind==CityRegionKind::Lot && (!occupancyDriven || developedLots.count(r.id)))lots[r.id]=&r;
  }
  // The corridors inside a block, before anything is allocated out of it.
  //
  // An alley is reserved like a street band is: it is taken out of the block
  // before the lots are, so a lot can never be allocated over the only way to
  // reach the lot behind it. It is paved, because a walked gap between houses
  // is a paved gap, and it joins the walkable network like any other ground.
  auto bandsWithAlleys=bands;
  // Public space has ownership before lots. Every tread is a disjoint polygon;
  // the central deck and step rings replace ground, never overlay it.
  for(const auto &space:city.skeleton.publicSpaces) {
    if(space.boundary.size()<3) continue;
    if(occupancyDriven && blocks.count(space.owner) && !inhabitedBlocks.count(space.owner))continue;
    if(!std::isfinite(space.rise) || space.rise<0 || space.rise>3.) {
      if(error)*error="public-space rise must be between zero and three metres";
      return false;
    }
    std::vector<geom2::Polygon> clear;
    auto reservedPublic=bandsWithAlleys;
    if(space.kind==CityPublicSpace::Kind::Garden || space.kind==CityPublicSpace::Kind::Yard)
      for(const auto &alley:city.skeleton.alleys)if(alley.size()>=3)reservedPublic.push_back(polygon(alley));
    // A market square can contain authored shambles and a hall.
    if(space.kind==CityPublicSpace::Kind::Court && space.rise==0)
      for(const auto &[id,lot]:lots) if(lot->parent==space.owner)reservedPublic.push_back(polygon(lot->boundary));
    if(!subtract(polygon(space.boundary),reservedPublic,clear))return false;
    for(const auto &p:clear) {
      bandsWithAlleys.push_back(p);
      const bool garden=space.kind==CityPublicSpace::Kind::Garden;
      const bool yard=space.kind==CityPublicSpace::Kind::Yard;
      out.influences.push_back({p,garden?0u:yard?1u:2u,1,garden || yard?.7:1.25,false,0,0,!garden && !yard,!garden && !yard});
      if(garden || yard) {
        out.pieces.push_back({CitySurfaceKind::Block,space.owner,0,p,.035,0,true});
        continue;
      }
      if(space.kind==CityPublicSpace::Kind::Sidewalk || space.rise==0) {
        out.pieces.push_back({space.kind==CityPublicSpace::Kind::Sidewalk?CitySurfaceKind::Sidewalk:CitySurfaceKind::Court,
                              space.owner,0,p,.035,0,true});
        continue;
      }
      const unsigned steps=unsigned(std::ceil(space.rise/.18));
      std::vector<geom2::Polygon> remaining{p};
      for(unsigned step=1;step<=steps;++step) {
        std::vector<geom2::Polygon> next;
        for(const auto &part:remaining) {
          std::vector<geom2::Polygon> inner;
          if(step<steps && !geom2::offsetPolygon(part,-.38,{},inner,error))return false;
          std::vector<geom2::Operand> clips;
          for(const auto &q:inner)clips.push_back(geom2::toOperand(q));
          std::vector<geom2::Polygon> ring;
          if(clips.empty())ring={part};
          else if(!geom2::differenceOf(geom2::toOperand(part),clips,ring,error))return false;
          for(auto &q:ring) out.pieces.push_back({step==steps?CitySurfaceKind::Court:CitySurfaceKind::StairTread,
                space.owner,step,std::move(q),.035+space.rise*step/steps,.035,true});
          next.insert(next.end(),inner.begin(),inner.end());
        }
        remaining=std::move(next);
      }
    }
  }
  uint32_t alleyPieces=0;
  {
    uint64_t alleyId=1;
    for(const auto &alley:city.skeleton.alleys) {
      if(alley.size()<3)continue;
      if(occupancyDriven) {
        bool serves=false;
        for(auto id:inhabitedBlocks)if(blocks.count(id)) {
          const auto block=polygon(blocks.at(id)->boundary);
          if(!geom2::bounds(block).overlaps(geom2::bounds(polygon(alley))))continue;
          std::vector<geom2::Polygon> overlap;
          if(!geom2::intersectionOf({geom2::toOperand(block),geom2::toOperand(polygon(alley))},overlap,error))return false;
          if(!overlap.empty()){serves=true;break;}
        }
        if(!serves)continue;
      }
      auto p=polygon(alley);
      std::vector<geom2::Polygon> clear;
      if(!subtract(p,bandsWithAlleys,clear))return false;
      for(auto &q:clear) {
        if(geom2::areaMetres(q)<.6)continue;
        out.influences.push_back({q,2,.9,.5,true});
        bandsWithAlleys.push_back(q);
        // Block alleys are pedestrian paving. Carving them as vehicle
        // roads creates two tiny sidewalks inside an already paved court.
        out.pieces.push_back({CitySurfaceKind::Sidewalk,alleyId,0,std::move(q),.035,0,true});
        ++alleyPieces;
      }
      ++alleyId;
    }
    if(alleyPieces)std::fprintf(stderr,"city surfaces: %u alley corridors paved inside blocks\n",alleyPieces);
  }

  std::map<uint64_t,std::vector<geom2::Polygon>> blockGround;
  auto reserved=bandsWithAlleys;
  for(const auto &[id,r]:blocks) {
    if(!subtract(polygon(r->boundary),reserved,blockGround[id]))return false;
    for(const auto &p:blockGround[id])reserved.push_back(p);
  }
  std::vector<geom2::Polygon> lotGround;
  for(const auto &[id,r]:lots) {
    std::vector<geom2::Polygon> candidates;
    const auto p=polygon(r->boundary);
    if(blocks.count(r->parent)) {
      // A lot may never expand outside its already allocated parent block.
      geom2::Operand parent;
      for(const auto &q:blockGround[r->parent]) {auto op=geom2::toOperand(q);parent.rings.insert(parent.rings.end(),op.rings.begin(),op.rings.end());}
      if(parent.rings.empty())continue;
      if(!geom2::intersectionOf({geom2::toOperand(p),parent},candidates,error))return false;
    } else if(!subtract(p,bandsWithAlleys,candidates))return false; // Standalone suburban lot.
    for(const auto &candidate:candidates) {
      std::vector<geom2::Polygon> accepted;if(!subtract(candidate,lotGround,accepted))return false;
      CityGroundCover cover=CityGroundCover::Automatic;bool developed=false,merchant=false;
      for(const auto &a:city.lots)if(a.lot==id){cover=a.groundCover;developed=a.developed;merchant=a.merchantHouse || a.residence!=CityResidenceKind::House;break;}
      const bool stone=cover==CityGroundCover::Stone || (cover==CityGroundCover::Automatic && developed && (merchant || r->districtUse==DistrictUse::WealthyResidential));
      const bool dirt=cover==CityGroundCover::Dirt || (cover==CityGroundCover::Automatic && developed);
      for(auto &q:accepted) {
        lotGround.push_back(q);
        if(cover!=CityGroundCover::Grass && (stone || dirt))out.influences.push_back({q,stone?2u:1u,stone?.85:.8,1.2,false});
        // Paving is owned by the allocated lot. Reserve a street-side strip
        // before meshing houses; clipping keeps it out of roads and neighbours.
        // This is a material influence, not another pavement mesh. Extend
        // the public walk naturally across the private doorstep as well.
        if(developed && !segments.empty()) {
          size_t edge=0;double nearest=1e100;
          for(size_t k=0;k<q.outer.size();++k) {
            const auto a=q.outer[k],b=q.outer[(k+1)%q.outer.size()];
            const double x=geom2::toMetres(a.x+b.x)*.5,z=geom2::toMetres(a.y+b.y)*.5;
            for(const auto &s:segments) {
              const double dx=s.b.x-s.a.x,dz=s.b.z-s.a.z;
              const double t=std::clamp(((x-s.a.x)*dx+(z-s.a.z)*dz)/(dx*dx+dz*dz),0.,1.);
              const double d=std::hypot(x-s.a.x-t*dx,z-s.a.z-t*dz);
              if(d<nearest){nearest=d;edge=k;}
            }
          }
          std::vector<geom2::Polygon> strip;
          if(!geom2::offsetPolyline({q.outer[edge],q.outer[(edge+1)%q.outer.size()]},1.8,{},strip,error))return false;
          for(const auto &band:strip) {
            std::vector<geom2::Polygon> paving;
            if(!geom2::intersectionOf({geom2::toOperand(band),geom2::toOperand(q)},paving,error))return false;
            for(auto &p:paving)out.influences.push_back({std::move(p),2,1,1.25,false,0,0,true,true});
          }
        }
        out.pieces.push_back({CitySurfaceKind::Lot,id,0,std::move(q),.035,0,true});
      }
    }
  }
  std::set<uint64_t> occupiedBlocks;
  for(const auto &a:city.lots)if(a.developed && lots.count(a.lot))occupiedBlocks.insert(lots.at(a.lot)->parent);
  for(const auto &[id,polygons]:blockGround)for(const auto &p:polygons) {
    std::vector<geom2::Polygon> remainder;if(!subtract(p,lotGround,remainder))return false;
    for(auto &q:remainder) {
      // Unbuilt interiors are deliberate ground, not the default grass layer.
      const auto use=blocks.at(id)->districtUse;
      const bool rural=use==DistrictUse::Suburb || use==DistrictUse::Manor || !city.historicalLots.empty();
      out.influences.push_back({q,rural?0u:2u,rural?1.:.82,.5,false});
      out.pieces.push_back({CitySurfaceKind::Block,id,0,std::move(q),.035,0,true});
    }
  }
  // Rural parcels use the same disjoint terrain ownership as city lots.
  // Fine fields take precedence over their coarse parent rural patches.
  std::map<uint64_t,const CitySkeletonRegion*> fields;
  for(const auto &r:city.skeleton.regions)
    if((r.kind==CityRegionKind::Field || r.kind==CityRegionKind::Farmyard) && r.boundary.size()>=3)fields[r.id]=&r;
  auto fieldReserved=reserved;
  fieldReserved.insert(fieldReserved.end(),lotGround.begin(),lotGround.end());
  for(const auto &[id,r]:fields) {
    std::vector<geom2::Polygon> ground;
    if(!subtract(polygon(r->boundary),fieldReserved,ground))return false;
    double dx=1,dz=0,longest=0;
    for(size_t k=0;k<r->boundary.size();++k){const auto a=r->boundary[k],b=r->boundary[(k+1)%r->boundary.size()];const double length=std::hypot(b.x-a.x,b.z-a.z);if(length>longest){longest=length;dx=(b.x-a.x)/length;dz=(b.z-a.z)/length;}}
    for(auto &p:ground) {
      fieldReserved.push_back(p);
      if(r->landUse==CityLandUse::Arable || r->landUse==CityLandUse::Vineyard || r->landUse==CityLandUse::Orchard)
        out.influences.push_back({p,1,.95,.7,false,-dz,dx});
      else if(r->kind==CityRegionKind::Farmyard)out.influences.push_back({p,1,.9,.7,false});
      out.pieces.push_back({CitySurfaceKind::Field,id,0,std::move(p),.035,0,true});
    }
  }
  return true;
}

CityGroundWeightIndex buildCityGroundWeightIndex(const CitySurfaceStructure &s)
{
  CityGroundWeightIndex index;
  index.surfaces=&s;
  double minX=1e30,minZ=1e30,maxX=-1e30,maxZ=-1e30;

  for(uint32_t k=0;k<uint32_t(s.influences.size());++k) {
    const auto &influence=s.influences[k];
    const auto bounds=geom2::bounds(influence.polygon);
    const double feather=influence.featherMetres;
    CityGroundWeightIndex::Entry entry{k,
      geom2::toMetres(bounds.minX)-feather,geom2::toMetres(bounds.minY)-feather,
      geom2::toMetres(bounds.maxX)+feather,geom2::toMetres(bounds.maxY)+feather};
    minX=std::min(minX,entry.minX);minZ=std::min(minZ,entry.minZ);
    maxX=std::max(maxX,entry.maxX);maxZ=std::max(maxZ,entry.maxZ);
    index.entries.push_back(entry);
  }
  index.edgeStart.push_back(0u);
  for(const auto &influence:s.influences) {
    const auto addRing=[&](const geom2::Ring &ring){for(size_t k=0;k<ring.size();++k) {
      const auto a=ring[k],b=ring[(k+1)%ring.size()];
      const double ax=geom2::toMetres(a.x),az=geom2::toMetres(a.y),bx=geom2::toMetres(b.x),bz=geom2::toMetres(b.y);
      const double dx=bx-ax,dz=bz-az;
      index.edges.push_back({ax,az,dx,dz,1./std::max(1e-20,dx*dx+dz*dz),
                             std::min(ax,bx),std::min(az,bz),std::max(ax,bx),std::max(az,bz)});
    }};
    addRing(influence.polygon.outer);
    for(const auto &hole:influence.polygon.holes)addRing(hole);
    index.edgeStart.push_back(uint32_t(index.edges.size()));
  }

  if(index.entries.empty() || minX>maxX)return index;
  index.originX=minX;index.originZ=minZ;
  index.columns=uint32_t(std::floor((maxX-minX)/index.cell))+1;
  index.rows=uint32_t(std::floor((maxZ-minZ)/index.cell))+1;
  index.cells.assign(size_t(index.columns)*index.rows,{});

  for(uint32_t k=0;k<uint32_t(index.entries.size());++k) {
    const auto &entry=index.entries[k];
    const uint32_t x0=uint32_t(std::floor((entry.minX-minX)/index.cell)),x1=uint32_t(std::floor((entry.maxX-minX)/index.cell));
    const uint32_t z0=uint32_t(std::floor((entry.minZ-minZ)/index.cell)),z1=uint32_t(std::floor((entry.maxZ-minZ)/index.cell));
    for(uint32_t cx=x0;cx<=x1 && cx<index.columns;++cx)
      for(uint32_t cz=z0;cz<=z1 && cz<index.rows;++cz)
        index.cells[size_t(cz)*index.columns+cx].push_back(k);
  }

  return index;
}

std::array<double,3> cityGroundWeights(const CityGroundWeightIndex &index,double x,double z)
{
  std::array<double,3> result{1,0,0};

  if(index.surfaces==nullptr || index.cells.empty())return result;

  const double fx=(x-index.originX)/index.cell,fz=(z-index.originZ)/index.cell;
  if(fx<0 || fz<0)return result;
  const uint32_t cx=uint32_t(fx),cz=uint32_t(fz);
  if(cx>=index.columns || cz>=index.rows)return result;

  const auto &bucket=index.cells[size_t(cz)*index.columns+cx];
  if(bucket.empty())return result;

  const geom2::Point point{geom2::toUnit(x),geom2::toUnit(z)};

  for(unsigned layer=0;layer<3;++layer) {
    std::array<double,3> coverage{};

    for(const uint32_t which:bucket) {
      const auto &entry=index.entries[which];
      const auto &i=index.surfaces->influences[entry.influence];
      if((i.road?2u:i.frontage?1u:0u)!=layer)continue;
      if(x<entry.minX || x>entry.maxX || z<entry.minZ || z>entry.maxZ)continue;
      const double feather=i.featherMetres;
      // Only distances inside the feather change t: beyond it t clamps to
      // the same end either side. So the search starts at the feather and
      // skips every edge whose box is already farther than the best.
      const double cap=std::max(1e-6,feather);
      double best2=cap*cap;
      for(uint32_t e=index.edgeStart[entry.influence];e<index.edgeStart[entry.influence+1];++e) {
        const auto &edge=index.edges[e];
        const double bx=x<edge.minX?edge.minX-x:x>edge.maxX?x-edge.maxX:0.;
        const double bz=z<edge.minZ?edge.minZ-z:z>edge.maxZ?z-edge.maxZ:0.;
        if(bx*bx+bz*bz>=best2)continue;
        const double t=std::clamp(((x-edge.ax)*edge.dx+(z-edge.az)*edge.dz)*edge.inverseLength2,0.,1.);
        const double ex=x-edge.ax-t*edge.dx,ez=z-edge.az-t*edge.dz;
        best2=std::min(best2,ex*ex+ez*ez);
      }
      double distance=std::sqrt(best2);
      if(!geom2::contains(i.polygon,point))distance=-distance;
      const double t=std::clamp(i.paving ? 1.+distance/std::max(1e-6,feather) : .5+distance/(2*std::max(1e-6,feather)),0.,1.);
      const double rows=(i.furrowX!=0 || i.furrowZ!=0) ? .65+.35*std::pow(std::sin((x*i.furrowX+z*i.furrowZ)*3.141592653589793/1.2),2) : 1.;
      const double wear=i.paving ? .97+.03*std::sin(x*.37+std::sin(z*.19))*std::sin(z*.31+x*.11) : 1.;
      coverage[i.channel]=std::max(coverage[i.channel],i.strength*wear*rows*t*t*(3-2*t));
    }

    const double sum=coverage[0]+coverage[1]+coverage[2],alpha=std::max({coverage[0],coverage[1],coverage[2]});
    if(sum>0)for(size_t k=0;k<3;++k)result[k]=result[k]*(1-alpha)+alpha*coverage[k]/sum;
  }

  return result;
}

std::array<double,3> cityGroundWeights(const CitySurfaceStructure &s,double x,double z)
{
  std::array<double,3> result{1,0,0};
  const geom2::Point point{geom2::toUnit(x),geom2::toUnit(z)};
  for(unsigned layer=0;layer<3;++layer) {
    std::array<double,3> coverage{};
    for(const auto &i:s.influences) {
      if((i.road?2u:i.frontage?1u:0u)!=layer)continue;
      const auto bounds=geom2::bounds(i.polygon);const double feather=i.featherMetres;
      if(x<geom2::toMetres(bounds.minX)-feather || x>geom2::toMetres(bounds.maxX)+feather ||
         z<geom2::toMetres(bounds.minY)-feather || z>geom2::toMetres(bounds.maxY)+feather)continue;
      double distance=1e30;
      const auto ringDistance=[&](const geom2::Ring &ring) {for(size_t k=0;k<ring.size();++k) {
        const auto a=ring[k],b=ring[(k+1)%ring.size()];const double ax=geom2::toMetres(a.x),az=geom2::toMetres(a.y),dx=geom2::toMetres(b.x-a.x),dz=geom2::toMetres(b.y-a.y);
        const double t=std::clamp(((x-ax)*dx+(z-az)*dz)/std::max(1e-20,dx*dx+dz*dz),0.,1.);
        distance=std::min(distance,std::hypot(x-ax-t*dx,z-az-t*dz));
      }};
      ringDistance(i.polygon.outer);for(const auto &hole:i.polygon.holes)ringDistance(hole);
      if(!geom2::contains(i.polygon,point))distance=-distance;
      const double t=std::clamp(i.paving ? 1.+distance/std::max(1e-6,feather) : .5+distance/(2*std::max(1e-6,feather)),0.,1.);
      const double rows=(i.furrowX!=0 || i.furrowZ!=0) ? .65+.35*std::pow(std::sin((x*i.furrowX+z*i.furrowZ)*3.141592653589793/1.2),2) : 1.;
      const double wear=i.paving ? .97+.03*std::sin(x*.37+std::sin(z*.19))*std::sin(z*.31+x*.11) : 1.;
      coverage[i.channel]=std::max(coverage[i.channel],i.strength*wear*rows*t*t*(3-2*t));
    }
    const double sum=coverage[0]+coverage[1]+coverage[2],alpha=std::max({coverage[0],coverage[1],coverage[2]});
    if(sum>0)for(size_t k=0;k<3;++k)result[k]=result[k]*(1-alpha)+alpha*coverage[k]/sum;
  }
  return result;
}
}
