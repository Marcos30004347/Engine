#include "procedural/world/generator/CellPassage.hpp"
#include "procedural/geom2/CellShape.hpp"
#include <algorithm>
#include <cmath>

namespace procedural::world::generator {
bool passageGridsAlign(const geom2::CellFrame &a,double groundA,double storeyA,
                       const geom2::CellFrame &b,double groundB,double storeyB)
{
  if(!std::isfinite(a.cellMetres) || a.cellMetres<=0 || !std::isfinite(b.cellMetres) ||
     std::abs(a.cellMetres-b.cellMetres)>1e-6 || !std::isfinite(groundA) || !std::isfinite(groundB) ||
     !std::isfinite(storeyA) || !std::isfinite(storeyB) || storeyA<=0 || storeyB<=0 ||
     std::abs(groundA-groundB)>1e-6 || std::abs(storeyA-storeyB)>1e-6)return false;
  if(!std::isfinite(a.axisX) || !std::isfinite(a.axisZ) || !std::isfinite(b.axisX) || !std::isfinite(b.axisZ))return false;
  double dot=a.axisX*b.axisX+a.axisZ*b.axisZ,cross=a.axisX*b.axisZ-a.axisZ*b.axisX;
  if(std::abs(std::hypot(a.axisX,a.axisZ)-1)>1e-6 || std::abs(std::hypot(b.axisX,b.axisZ)-1)>1e-6)return false;
  if(std::min(std::abs(dot),std::abs(cross))>1e-6)return false;
  double dx=b.originX-a.originX,dz=b.originZ-a.originZ;
  double u=(dx*a.axisX+dz*a.axisZ)/a.cellMetres,v=(-dx*a.axisZ+dz*a.axisX)/a.cellMetres;
  return std::isfinite(u) && std::isfinite(v) && std::abs(u-std::round(u))<1e-6 && std::abs(v-std::round(v))<1e-6;
}

bool reserveCoveredPassage(const CoveredPassageRequest &request,CellHouse &house,std::string *error)
{
  const auto fail=[&](const char *message){if(error)*error=message;return false;};
  if(house.floors.size()<2)return fail("covered passage requires an occupied upper storey");
  if(!std::isfinite(request.clearance) || request.clearance<2.1 || request.clearance>(house.gridClass.storeyMetres-.15)*rewrite::kPassageArchSpring*.92)
    return fail("covered passage has insufficient structural headroom");
  if(request.polygon.empty() || !geom2::polygonFault(request.polygon).empty())return fail("invalid covered passage polygon");
  geom2::Board cells;
  uint32_t loI=UINT32_MAX,loJ=UINT32_MAX,hiI=0,hiJ=0;
  for(auto [i,j]:geom2::cells(house.grid.mask())) {
    double x,z;geom2::cellPoint(house.grid,house.frame,house.grid.cell(i,j),x,z);
    if(!geom2::contains(request.polygon,{geom2::toUnit(x),geom2::toUnit(z)}))continue;
    cells.set(i,j);loI=std::min(loI,i);loJ=std::min(loJ,j);hiI=std::max(hiI,i);hiJ=std::max(hiJ,j);
  }
  // What of the strip the house actually has.
  //
  // The caller names a lane; the house straddles part of it. Taking the
  // request literally refused every gateway house in the city for "an
  // unsupported side", because the lane runs the full depth of both lots and
  // the buildings on them are set back. The passage is the part of the strip
  // the house has, which is the part it can carry.
  cells=cells & house.floors.front().area;
  loI=UINT32_MAX;loJ=UINT32_MAX;hiI=0;hiJ=0;
  for(auto [i,j]:geom2::cells(cells)){loI=std::min(loI,i);loJ=std::min(loJ,j);hiI=std::max(hiI,i);hiJ=std::max(hiJ,j);}
  if(cells.empty() || cells.count()!=(hiI-loI+1)*(hiJ-loJ+1))return fail("covered passage must be a straight cell-aligned strip");
  const double span=std::min(hiI-loI+1,hiJ-loJ+1)*house.frame.cellMetres;
  if((span-.4)*.84<.9)return fail("covered passage is too narrow after jambs and frames");
  // The passage is the cells whose centres the request covers, and that is
  // the whole of the alignment asked of the caller. The request used to have
  // to coincide with those cells' outline to ten microns, which a caller
  // outside the planner cannot arrange: the frame is settled in here, from
  // the lot, and shifts by fractions of a cell from any lattice the caller
  // knows. Every gateway house the city asked for was refused on it.
  if(request.polygon.holes.size())return fail("covered passage polygon has holes");
  if(!(cells & ~house.floors.front().area).empty()) {
    if(error)*error="passage leaves the occupied building footprint ("+std::to_string((cells & ~house.floors.front().area).count())+
      " of "+std::to_string(cells.count())+" cells outside a ground floor of "+std::to_string(house.floors.front().area.count())+")";
    return false;
  }
  if(!(cells & house.courts).empty())return fail("passage crosses a court");
  if(house.stairwell && !(cells & house.stairwell->block).empty())return fail("passage conflicts with reserved stairs");
  if(house.spiral && house.grid.contains(cells,house.spiral->landing))return fail("passage conflicts with spiral landing");
  for(auto &floor:house.floors)for(auto &stair:floor.stairs)for(auto c:wellCells(house.grid,stair))
    if(house.grid.contains(cells,c))return fail("passage conflicts with stair well");
  std::vector<uint32_t> portals;
  const bool alongRows=hiI-loI>=hiJ-loJ;
  bool first=false,last=false;
  for(auto edge:house.grid.outerLoop(cells)) {
    const auto twin=house.grid.twin(edge),near=house.grid.edgeCell(twin);
    if(near!=geom2::kNoCell && house.grid.contains(house.floors.front().area,near))continue;
    double ax,az,bx,bz;geom2::edgePoint(house.grid,house.frame,edge,0,ax,az);geom2::edgePoint(house.grid,house.frame,edge,1,bx,bz);
    // Ends are perpendicular to the strip's long axis.
    const double alongU=(bx-ax)*house.frame.axisX+(bz-az)*house.frame.axisZ;
    if(alongRows ? std::abs(alongU)<house.frame.cellMetres*.9 : std::abs(alongU)>house.frame.cellMetres*.1)
      return fail("passage opens along an unsupported side");
    const auto cell=house.grid.edgeCell(edge);const auto coordinate=alongRows?house.grid.cellRow(cell):house.grid.cellColumn(cell);
    first|=coordinate==(alongRows?loI:loJ);last|=coordinate==(alongRows?hiI:hiJ);portals.push_back(edge);
  }
  if(!first || !last)return fail("passage must reach public space at both ends");
  // Keep supporting side cells, rather than permitting a floating bridge.
  for(auto [i,j]:geom2::cells(cells)) {
    for(int sign:{-1,1}) {
      auto c=alongRows?house.grid.cell(i,int(j)+sign):house.grid.cell(int(i)+sign,j);
      if(c!=geom2::kNoCell && house.grid.contains(cells,c))continue;
      if(c==geom2::kNoCell || !house.grid.contains(house.floors.front().area,c))return fail("passage requires supporting rooms on both sides");
    }
  }
  house.coveredPassage=cells;house.passagePortals=std::move(portals);house.passageClearance=request.clearance;house.passageId=request.id;
  house.floors[1].area=house.floors[1].area | cells;house.floors[1].loops=house.grid.outline(house.floors[1].area);
  return true;
}
}
