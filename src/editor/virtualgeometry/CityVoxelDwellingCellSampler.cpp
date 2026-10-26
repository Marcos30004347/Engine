#include "editor/virtualgeometry/CityVoxelDwellingCellSampler.hpp"

#include "editor/virtualgeometry/CityVoxelPartOwnership.hpp"
#include "procedural/world/hierarchy/FacadeAtlas.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace virtualgeometry {
namespace {
using namespace procedural::world::hierarchy;
using P = procedural::world::rewrite::Position;
using Cell = voxel::svraster::CityVoxelCell;
using V = std::array<double,3>;
using I = std::array<int32_t,3>;

bool fail(std::string *error, const char *reason) {
  if (error) *error = reason;
  return false;
}
V sub(V a,V b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
double dot(V a,V b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
V cross(V a,V b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
bool axisOverlap(V axis,V a,V b,V c) {
  if(dot(axis,axis)<1e-24) return true;
  const double radius=.5*(std::abs(axis[0])+std::abs(axis[1])+std::abs(axis[2]));
  return std::min({dot(axis,a),dot(axis,b),dot(axis,c)})<=radius &&
         std::max({dot(axis,a),dot(axis,b),dot(axis,c)})>=-radius;
}
bool triangleCell(V a,V b,V c,V center) {
  a=sub(a,center);b=sub(b,center);c=sub(c,center);
  const V edges[]={sub(b,a),sub(c,b),sub(a,c)};
  for(V e:edges)
    if(!axisOverlap({0,-e[2],e[1]},a,b,c) ||
       !axisOverlap({e[2],0,-e[0]},a,b,c) ||
       !axisOverlap({-e[1],e[0],0},a,b,c)) return false;
  for(unsigned d=0;d<3;++d)
    if(std::min({a[d],b[d],c[d]})>.5 || std::max({a[d],b[d],c[d]})<-.5) return false;
  const V n=cross(sub(b,a),sub(c,a));
  const double radius=.5*(std::abs(n[0])+std::abs(n[1])+std::abs(n[2]));
  return std::abs(dot(n,a))<=radius;
}
bool boundedVolume(I low,I high) {
  uint64_t count=1;
  for(unsigned d=0;d<3;++d) {
    const uint64_t width=uint64_t(int64_t(high[d])-low[d]+1);
    if(!width || width>4'000'000/count)return false;
    count*=width;
  }
  return true;
}
struct Grid {
  const DwellingVoxelGridSpec &spec;
  V point(P p) const {
    const auto &f=spec.frame;
    const double x=p.x-f.originX,z=p.z-f.originZ;
    // CellFrame V is the left perpendicular of U.
    return {(x*f.axisX+z*f.axisZ)/spec.voxelMetres,p.y/spec.voxelMetres,
            (-x*f.axisZ+z*f.axisX)/spec.voxelMetres};
  }
  P world(I p) const {
    const auto &f=spec.frame;
    const double u=(double(p[0])+.5)*spec.voxelMetres;
    const double v=(double(p[2])+.5)*spec.voxelMetres;
    return {f.originX+u*f.axisX-v*f.axisZ,(double(p[1])+.5)*spec.voxelMetres,
            f.originZ+u*f.axisZ+v*f.axisX};
  }
};
// Stable semantic profiles. Color remains per occupied cell and may be varied
// by the caller's appearance stage without changing geometry identity.
Cell makeCell(I p,Material material) {
  constexpr uint32_t colors[]={0xffa79e99u,0xff556e81u,0xff8aaeb8u,
      0xff4d5667u,0xff506b81u,0xff637477u};
  const auto index=std::min<unsigned>(unsigned(material),5);
  return {p,colors[index],index+1};
}
Cell facadePaint(Solid::SurfaceMaterial material) {
  using M=Solid::SurfaceMaterial;
  switch(material) {
    case M::Door:return {{},0xff2e4054u,7};
    case M::CarvedStone:return {{},0xffb3b3aeu,8};
    case M::Wood:
    case M::Boarding:return makeCell({},Material::Timber);
    case M::Glass:return {{},0x00ffffffu,9};
    default:return makeCell({},Material::Stone);
  }
}
class Writer {
public:
  explicit Writer(const Grid &grid):grid_(grid){}
  bool triangle(P pa,P pb,P pc,uint32_t rgba,uint32_t profile) {
    const V a=grid_.point(pa),b=grid_.point(pb),c=grid_.point(pc);
    I low{},high{};
    for(unsigned d=0;d<3;++d) {
      const double lo=std::floor(std::min({a[d],b[d],c[d]})-1);
      const double hi=std::ceil(std::max({a[d],b[d],c[d]})+1);
      if(!std::isfinite(lo)||!std::isfinite(hi)||lo<INT32_MIN||hi>=INT32_MAX) return false;
      low[d]=int32_t(lo);high[d]=int32_t(hi);
    }
    const V n=cross(sub(b,a),sub(c,a));
    unsigned d=0;
    for(unsigned k=1;k<3;++k) if(std::abs(n[k])>std::abs(n[d])) d=k;
    const double length=std::sqrt(dot(n,n));
    if(length<1e-12 || std::abs(n[d])<1e-6*length) {
      // Degenerate: no plane to walk; its box is thin, scan it.
      if(!boundedVolume(low,high)) return false;
      for(int32_t z=low[2];z<=high[2];++z)
        for(int32_t y=low[1];y<=high[1];++y)
          for(int32_t x=low[0];x<=high[0];++x)
            if(triangleCell(a,b,c,{double(x)+.5,double(y)+.5,double(z)+.5}))
              cells_[{x,y,z}]={{x,y,z},rgba,profile};
      return cells_.size()<=kCityVoxelDwellingCellBudget;
    }
    // Walk the projection on the dominant axis. Over one column's square the
    // plane spans at most two cells (|n[d]| is the largest component), so
    // the candidates grow with the triangle's area, not its bounding
    // volume: a hall roof's box was millions of empty cells, and over the
    // old 4M-cell cap it failed the house. The overlap test still decides.
    const unsigned u=(d+1)%3,w=(d+2)%3;
    if(uint64_t(int64_t(high[u])-low[u]+1)*uint64_t(int64_t(high[w])-low[w]+1)>64'000'000ull) return false;
    const auto depth=[&](double pu,double pw){return a[d]-(n[u]*(pu-a[u])+n[w]*(pw-a[w]))/n[d];};
    for(int32_t cu=low[u];cu<=high[u];++cu)
      for(int32_t cw=low[w];cw<=high[w];++cw) {
        double lo=1e300,hi=-1e300;
        for(int du=0;du<2;++du)for(int dw=0;dw<2;++dw) {
          const double z=depth(double(cu+du),double(cw+dw));
          lo=std::min(lo,z);hi=std::max(hi,z);
        }
        const int64_t k0=std::max<int64_t>(low[d],int64_t(std::floor(lo))-1);
        const int64_t k1=std::min<int64_t>(high[d],int64_t(std::floor(hi))+1);
        for(int64_t k=k0;k<=k1;++k) {
          I at{};at[d]=int32_t(k);at[u]=cu;at[w]=cw;
          if(triangleCell(a,b,c,{double(at[0])+.5,double(at[1])+.5,double(at[2])+.5}))
            cells_[at]={at,rgba,profile};
        }
      }
    return cells_.size()<=kCityVoxelDwellingCellBudget;
  }
  bool polygon(const std::vector<P> &ring,uint32_t rgba,uint32_t profile) {
    if(ring.size()<3) return true;
    if(ring.size()>4096) return false;
    // Ear clipping avoids filling the concave corners of floors and roof
    // panels. Project onto the dominant plane and reject invalid rings.
    V normal{};
    for(size_t i=0;i<ring.size();++i) {
      const P &a=ring[i],&b=ring[(i+1)%ring.size()];
      normal[0]+=(a.y-b.y)*(a.z+b.z);
      normal[1]+=(a.z-b.z)*(a.x+b.x);
      normal[2]+=(a.x-b.x)*(a.y+b.y);
    }
    unsigned drop=0;
    for(unsigned d=1;d<3;++d) if(std::abs(normal[d])>std::abs(normal[drop]))drop=d;
    if(std::abs(normal[drop])<1e-12)return true; // No area: nothing to draw.
    const auto xy=[&](const P &p) -> std::array<double,2> {
      if(drop==0)return {p.y,p.z};
      if(drop==1)return {p.x,p.z};
      return {p.x,p.y};
    };
    const auto orient=[](auto a,auto b,auto c) {
      return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
    };
    std::vector<size_t> polygon;
    polygon.reserve(ring.size());
    for(size_t i=0;i<ring.size();++i)
      if(polygon.empty() || std::hypot(xy(ring[i])[0]-xy(ring[polygon.back()])[0],
            xy(ring[i])[1]-xy(ring[polygon.back()])[1])>1e-8)polygon.push_back(i);
    if(polygon.size()>2 && std::hypot(xy(ring[polygon.front()])[0]-xy(ring[polygon.back()])[0],
          xy(ring[polygon.front()])[1]-xy(ring[polygon.back()])[1])<1e-8)polygon.pop_back();
    if(polygon.size()<3)return true;
    for(size_t i=0;i<polygon.size() && polygon.size()>3;) {
      const auto a=xy(ring[polygon[(i+polygon.size()-1)%polygon.size()]]);
      const auto b=xy(ring[polygon[i]]);
      const auto c=xy(ring[polygon[(i+1)%polygon.size()]]);
      if(std::abs(orient(a,b,c))<1e-12)polygon.erase(polygon.begin()+i);
      else ++i;
    }
    double area=0;
    for(size_t i=0;i<polygon.size();++i) {
      auto a=xy(ring[polygon[i]]),b=xy(ring[polygon[(i+1)%polygon.size()]]);
      area+=a[0]*b[1]-b[0]*a[1];
    }
    const double sign=area>=0?1.:-1.;
    size_t attempts=0;
    while(polygon.size()>3) {
      bool found=false;
      for(size_t i=0;i<polygon.size();++i) {
        const size_t prev=polygon[(i+polygon.size()-1)%polygon.size()];
        const size_t curr=polygon[i];
        const size_t next=polygon[(i+1)%polygon.size()];
        const auto a=xy(ring[prev]),b=xy(ring[curr]),c=xy(ring[next]);
        if(sign*orient(a,b,c)<=1e-12)continue;
        bool contains=false;
        for(size_t j:polygon)if(j!=prev&&j!=curr&&j!=next) {
          const auto p=xy(ring[j]);
          if(sign*orient(a,b,p)>=-1e-12 && sign*orient(b,c,p)>=-1e-12 &&
             sign*orient(c,a,p)>=-1e-12) {contains=true;break;}
        }
        if(contains)continue;
        if(!triangle(ring[prev],ring[curr],ring[next],rgba,profile))return false;
        polygon.erase(polygon.begin()+i);
        found=true;break;
      }
      if(!found || ++attempts>ring.size()) {
        // A ring ear clipping cannot split (it touches or crosses itself):
        // fan what is left rather than lose the whole house over it.
        for(size_t i=1;i+1<polygon.size();++i)
          if(!triangle(ring[polygon[0]],ring[polygon[i]],ring[polygon[i+1]],rgba,profile))return false;
        return true;
      }
    }
    if(!triangle(ring[polygon[0]],ring[polygon[1]],ring[polygon[2]],rgba,profile))return false;
    return true;
  }
  bool polygon(const std::vector<P> &ring,Material material) {
    const Cell paint=makeCell({},material);
    return polygon(ring,paint.rgba,paint.materialProfile);
  }
  bool wall(const Piece &piece,const StructureFace *face,const StructureFace *inside) {
    if(piece.width<=0||piece.height<=0||piece.depth<=0) return true;
    P corners[8];unsigned n=0;
    for(double u:{0.,piece.width}) for(double v:{0.,piece.height})
      for(double w:{0.,piece.depth}) corners[n++]=piece.world(u,v,w);
    I low{INT32_MAX,INT32_MAX,INT32_MAX}, high{INT32_MIN,INT32_MIN,INT32_MIN};
    for(P p:corners) {
      V q=grid_.point(p);
      for(unsigned d=0;d<3;++d) {
        if(!std::isfinite(q[d])||q[d]<INT32_MIN+2||q[d]>INT32_MAX-2) return false;
        low[d]=std::min(low[d],int32_t(std::floor(q[d]))-1);
        high[d]=std::max(high[d],int32_t(std::ceil(q[d]))+1);
      }
    }
    if(!boundedVolume(low,high)) return false;
    for(int32_t z=low[2];z<=high[2];++z)
      for(int32_t y=low[1];y<=high[1];++y)
        for(int32_t x=low[0];x<=high[0];++x) {
          I at{x,y,z}; double u,v,w;piece.local(grid_.world(at),u,v,w);
          if(u<0||u>=piece.width||v<0||v>=piece.height||w<0||w>=piece.depth) continue;
          if(face && face->high>face->low && face->along1>face->along0) {
            const double a=piece.width*face->along0,b=piece.width*face->along1;
            if(u>=a&&u<b&&v>=face->low&&v<face->high) {
              // The detailed aperture boundary clips arches in the wall plane.
              bool aperture=!face->detailedOpening;
              if(face->detailedOpening && face->apertureBoundary.size()>=3) {
                aperture=false;
                const auto &ring=face->apertureBoundary;
                for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
                  double ui,vi,wi,uj,vj,wj;
                  piece.local(ring[i],ui,vi,wi);piece.local(ring[j],uj,vj,wj);
                  if((vi>v)!=(vj>v) && u<(uj-ui)*(v-vi)/(vj-vi)+ui) aperture=!aperture;
                }
              }
              if(aperture) continue;
            }
          }
          Material material=piece.material;
          if(face && w<piece.depth*.5)material=face->material;
          else if(inside && w>=piece.depth*.5)material=inside->material;
          cells_[at]=makeCell(at,material);
        }
    return cells_.size()<=kCityVoxelDwellingCellBudget;
  }
  std::vector<Cell> finish() {
    std::vector<Cell> out;out.reserve(cells_.size());
    for(const auto &entry:cells_) out.push_back(entry.second);
    return out;
  }
private:
  const Grid &grid_;
  std::map<I,Cell> cells_;
};
} // namespace

bool sampleCityVoxelDwellingCells(const DwellingVoxelAssembly &assembly,
                                  const Skeleton &skeleton,
                                  std::vector<std::vector<Cell>> &result,
                                  std::string *error) {
  if(error) error->clear();
  if(!std::isfinite(assembly.grid.voxelMetres)||assembly.grid.voxelMetres<=0 ||
     !std::isfinite(assembly.grid.frame.axisX)||!std::isfinite(assembly.grid.frame.axisZ)||
     std::abs(std::hypot(assembly.grid.frame.axisX,assembly.grid.frame.axisZ)-1)>1e-5)
    return fail(error,"Invalid dwelling voxel lattice");
  const Grid grid{assembly.grid};
  std::vector<std::vector<Cell>> next;next.reserve(assembly.parts.size());
  for(const auto &part:assembly.parts) {
    Writer writer(grid);
    if(part.source==DwellingVoxelPartSource::Piece) {
      if(part.sourceIndex>=skeleton.pieces.size()) return fail(error,"Invalid dwelling piece index");
      const Piece &piece=skeleton.pieces[part.sourceIndex];
      if(piece.kind==PieceKind::Wall && piece.width>0 && piece.height>0 && piece.depth>0) {
        const StructureFace *opening=piece.outside<skeleton.structure.faces.size()
            ? &skeleton.structure.faces[piece.outside] : nullptr;
        const StructureFace *inside=piece.inside<skeleton.structure.faces.size()
            ? &skeleton.structure.faces[piece.inside] : nullptr;
        if(!writer.wall(piece,opening,inside)) return fail(error,"Wall voxel budget exceeded");
      } else {
        for(uint32_t index:piece.faces) {
          if(index>=skeleton.structure.faces.size()) return fail(error,"Invalid dwelling face index");
          const auto &face=skeleton.structure.faces[index];
          if(face.kind==FaceKind::DormerOpening||face.kind==FaceKind::DormerWindow) continue;
          if(face.detailedOpening) {
            for(const auto &panel:face.wallPanels)
              if(!writer.polygon(panel,face.material)) return fail(error,"Panel voxel budget exceeded");
          } else if(!writer.polygon(face.outline,face.material)) return fail(error,"Face voxel budget exceeded");
        }
      }
    } else if(part.source==DwellingVoxelPartSource::Facade) {
      if(part.sourceIndex>=skeleton.structure.instances.size()) return fail(error,"Invalid facade index");
      if(!part.omitFromOpaqueScene) {
        Structure one;one.instances.push_back(skeleton.structure.instances[part.sourceIndex]);
        Solid solid;meshFacadeInstances(one,solid);
        for(const auto *faces:{&solid.pillars,&solid.timbers,&solid.joinery})
          for(const auto &face:*faces) if(face.size()>=3) {
            std::vector<P> ring;ring.reserve(face.size());
            for(const auto &v:face) ring.push_back({v.x,v.y,v.z});
            const Cell paint=facadePaint(face.front().material);
            if(paint.rgba>>24u!=255u)continue;
            if(!writer.polygon(ring,paint.rgba,paint.materialProfile))
              return fail(error,"Facade voxel budget exceeded");
          }
      }
    } else if(part.source==DwellingVoxelPartSource::UnassignedFace) {
      if(part.sourceIndex>=skeleton.structure.faces.size()) return fail(error,"Invalid unassigned face index");
      const auto &face=skeleton.structure.faces[part.sourceIndex];
      if(face.kind!=FaceKind::DormerOpening&&face.kind!=FaceKind::DormerWindow &&
         !writer.polygon(face.outline,face.material)) return fail(error,"Unassigned face voxel budget exceeded");
    } else return fail(error,"Unknown dwelling part source");
    next.push_back(writer.finish());
  }
  result=std::move(next);
  return true;
}
} // namespace virtualgeometry
