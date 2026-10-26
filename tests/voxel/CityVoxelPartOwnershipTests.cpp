#undef NDEBUG
#include "editor/virtualgeometry/CityVoxelPartOwnership.hpp"

#include <cassert>
#include <iostream>
#include <set>

namespace svr=voxel::svraster;
using namespace virtualgeometry;

int main() {
  auto voxel=[](int x,uint32_t color,uint32_t material) {
    return svr::CityVoxelCell{{{x,0,0}},color,material};
  };
  std::vector<CityVoxelAuthoredPart> parts={
      {10,1,{voxel(0,0xffaaaaaau,1),voxel(1,0xffaaaaaau,1),
             voxel(2,0xffaaaaaau,1)}},
      {20,3,{voxel(1,0xff332211u,2),voxel(2,0xff332211u,2)}},
      {30,2,{voxel(2,0xff665544u,3),voxel(3,0xff665544u,3)}}};
  CityVoxelPartOwnershipReport report;
  std::string error;
  assert(resolveCityVoxelPartOwnership(parts,report,&error));
  assert(report.inputCells==7 && report.uniqueCells==4 && report.clippedCells==3);
  assert(parts[0].cells.size()==1 && parts[0].cells[0].position[0]==0);
  assert(parts[1].cells.size()==2 && parts[2].cells.size()==1 &&
      parts[2].cells[0].position[0]==3);
  std::set<std::array<int32_t,3>> unionCells;
  for(const auto &part:parts)for(const auto &cell:part.cells)
    assert(unionCells.insert(cell.position).second);
  assert(unionCells.size()==4);

  svr::CityVoxelAssetCompiler compiler;
  svr::CityVoxelCompiledPlacement first{},second{};
  assert(compiler.submit(10,100000,parts[0].cells,first,&error));
  auto recolored=parts[0].cells;
  recolored[0].rgba=0xff009900u;
  assert(compiler.submit(11,100000,recolored,second,&error));
  assert(first.geometryId==second.geometryId &&
      first.appearanceId!=second.appearanceId);

  auto equal=std::vector<CityVoxelAuthoredPart>{
      {9,1,{voxel(0,0xff000000u,1),voxel(1,0xff000000u,1)}},
      {8,1,{voxel(1,0xff000000u,1),voxel(2,0xff000000u,1)}}};
  assert(resolveCityVoxelPartOwnership(equal,report,&error));
  assert(report.equalPriorityConflicts==1 && equal[0].cells.size()==1 &&
      equal[1].cells.size()==2);
  auto invalid=std::vector<CityVoxelAuthoredPart>{
      {1,1,{voxel(0,1,1)}},{2,2,{voxel(0,2,2)}}};
  assert(!resolveCityVoxelPartOwnership(invalid,report,&error));
  assert(report.fullyCoveredPartIndex==0);
  assert(invalid[0].cells.size()==1 && invalid[1].cells.size()==1);
  assert(resolveCityVoxelPartOwnership(invalid,report,&error,true));
  assert(report.fullyCoveredPartIndices==std::vector<uint32_t>{0} &&
      invalid[0].cells.empty() && invalid[1].cells.size()==1);
  std::cout<<"City voxel part ownership tests passed\n";
}
