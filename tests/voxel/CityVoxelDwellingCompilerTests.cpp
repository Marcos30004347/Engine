#undef NDEBUG
#include "editor/virtualgeometry/CityVoxelDwellingCompiler.hpp"
#include "editor/virtualgeometry/CityVoxelDwellingCellSampler.hpp"
#include "editor/virtualgeometry/CityVoxelTerrainTileSampler.hpp"

#include <cassert>
#include <iostream>
#include <algorithm>
#include <cmath>

namespace svr=voxel::svraster;
using namespace virtualgeometry;
namespace hierarchy=procedural::world::hierarchy;

int main() {
  hierarchy::DwellingVoxelAssembly assembly;
  assembly.dwellingId=42;
  assembly.grid.voxelMetres=0.1;
  assembly.parts.resize(3);
  assembly.parts[0].occupancyPriority=10; // Wall.
  assembly.parts[1].occupancyPriority=50; // Independent frame.
  assembly.parts[2].occupancyPriority=50;
  assembly.parts[2].omitFromOpaqueScene=true; // Glass stays disabled.
  auto authored=[](uint32_t wallColor) {
    return std::vector<std::vector<svr::CityVoxelCell>>{
        {{{{0,0,0}},wallColor,1},{{{1,0,0}},wallColor,1},
          {{{2,0,0}},wallColor,1}},
        {{{{1,0,0}},0xff554433u,2},{{{2,0,0}},0xff554433u,2}},
        {{{{1,0,0}},0x80ffffffu,3}}};
  };
  svr::CityVoxelAssetCompiler assets;
  std::vector<CityVoxelCompiledDwellingPart> result;
  CityVoxelDwellingCompileReport report;
  std::string error;
  assert(compileCityVoxelDwellingParts(assembly,authored(0xffaaaaaau),assets,
      result,report,&error));
  assert(report.compiledParts==2 && report.omittedGlassParts==1 &&
      report.ownership.clippedCells==2 && report.ownership.uniqueCells==3);
  assert(result.size()==2 && result[0].sourcePartIndex==0 &&
      result[1].sourcePartIndex==1 &&
      result[0].placement.stableId!=result[1].placement.stableId);
  assert(assets.geometry().size()==2 && assets.appearances().size()==2);
  const auto firstWall=result[0].placement;
  assembly.dwellingId=43;
  assert(compileCityVoxelDwellingParts(assembly,authored(0xff00aa00u),assets,
      result,report,&error));
  assert(result.size()==4 && assets.geometry().size()==2 &&
      assets.appearances().size()==3 &&
      result[2].placement.geometryId==firstWall.geometryId &&
      result[2].placement.appearanceId!=firstWall.appearanceId &&
      result[3].placement.geometryId==result[1].placement.geometryId &&
      result[3].placement.appearanceId==result[1].placement.appearanceId);
  {
    hierarchy::DwellingVoxelAssembly thin;
    thin.dwellingId=91;thin.grid.voxelMetres=.1;thin.parts.resize(2);
    thin.parts[0].occupancyPriority=30;thin.parts[0].omitWhenCovered=true;
    thin.parts[1].occupancyPriority=40;
    auto cells=[] {return std::vector<std::vector<svr::CityVoxelCell>>{
      {{{{0,0,0}},0xffaaaaaau,1}},{{{{0,0,0}},0xffbbbbbb,2}}};};
    svr::CityVoxelAssetCompiler thinAssets;
    std::vector<CityVoxelCompiledDwellingPart> thinParts;
    CityVoxelDwellingCompileReport thinReport;
    assert(compileCityVoxelDwellingParts(thin,cells(),thinAssets,thinParts,thinReport,&error));
    assert(thinReport.omittedCoveredStaticParts==1 && thinParts.size()==1 &&
      thinParts[0].sourcePartIndex==1);
    thin.parts[0].omitWhenCovered=false;thinParts.clear();
    assert(!compileCityVoxelDwellingParts(thin,cells(),thinAssets,thinParts,thinReport,&error));
    assert(thinReport.fullyCoveredSourcePartIndex==0);
    thin.parts[0].omitWhenCovered=true;
    auto empty=cells();empty[0].clear();thinParts.clear();
    assert(compileCityVoxelDwellingParts(thin,std::move(empty),thinAssets,
      thinParts,thinReport,&error));
    assert(thinReport.omittedCoveredStaticParts==1 && thinParts.size()==1);
    thin.parts[0].source=hierarchy::DwellingVoxelPartSource::Facade;
    thinParts.clear();
    assert(compileCityVoxelDwellingParts(thin,cells(),thinAssets,
      thinParts,thinReport,&error));
    assert(thinReport.omittedPairedFacadeSides==1 && thinParts.size()==1);
  }
  {
    hierarchy::DwellingVoxelAssembly door;
    door.dwellingId=92;door.grid.voxelMetres=.1;door.parts.resize(2);
    door.parts[0].source=hierarchy::DwellingVoxelPartSource::Facade;
    door.parts[0].segment=7;door.parts[0].doorLeafSurface=true;
    door.parts[1].source=hierarchy::DwellingVoxelPartSource::Facade;
    door.parts[1].segment=7;door.parts[1].appearanceOverlay=true;
    door.parts[1].omitWhenCovered=true;
    std::vector<std::vector<svr::CityVoxelCell>> source={
      {{{{0,0,0}},0xff54402eu,7}},{{{{0,0,0}},0xff816e55u,2}}};
    svr::CityVoxelAssetCompiler doorAssets;
    std::vector<CityVoxelCompiledDwellingPart> doorParts;
    CityVoxelDwellingCompileReport doorReport;
    assert(compileCityVoxelDwellingParts(door,std::move(source),doorAssets,
      doorParts,doorReport,&error));
    assert(doorReport.overlaidCells==1 && doorReport.omittedDecorativeOverlays==1 &&
      doorParts.size()==1 && doorParts[0].sourcePartIndex==0);
    std::vector<svr::CityVoxelCell> decoded;
    assert(svr::decodeCityVoxelAppearance(doorAssets.appearances().front(),decoded,&error));
    assert(decoded.size()==1 && decoded[0].rgba==0xff816e55u && decoded[0].materialProfile==2);
    const auto firstDoor=doorParts.front().placement;
    door.dwellingId=93;doorParts.clear();
    std::vector<std::vector<svr::CityVoxelCell>> variant={
      {{{{0,0,0}},0xff54402eu,7}},{{{{0,0,0}},0xff665544u,2}}};
    assert(compileCityVoxelDwellingParts(door,std::move(variant),doorAssets,
      doorParts,doorReport,&error));
    assert(doorParts.front().placement.geometryId==firstDoor.geometryId &&
      doorParts.front().placement.appearanceId!=firstDoor.appearanceId);
  }
  {
    hierarchy::DwellingVoxelAssembly authored;
    authored.dwellingId=100;
    authored.grid.voxelMetres=.1;
    authored.parts.resize(2);
    authored.parts[0].source=hierarchy::DwellingVoxelPartSource::Piece;
    authored.parts[0].sourceIndex=0;
    authored.parts[0].occupancyPriority=10;
    authored.parts[1].source=hierarchy::DwellingVoxelPartSource::Facade;
    authored.parts[1].sourceIndex=0;
    authored.parts[1].occupancyPriority=50;
    hierarchy::Skeleton skeleton;
    hierarchy::Piece wall;
    wall.kind=hierarchy::PieceKind::Wall;
    wall.origin={0,0,0};wall.width=1;wall.height=1;wall.depth=.2;
    wall.segment=0;wall.outside=0;wall.faces={0};
    skeleton.pieces.push_back(wall);
    hierarchy::StructureFace opening;
    opening.kind=hierarchy::FaceKind::DoorWall;
    opening.material=hierarchy::Material::Stone;
    opening.along0=.4;opening.along1=.6;opening.low=0;opening.high=.8;
    skeleton.structure.faces.push_back(opening);
    hierarchy::FacadeInstance frame;
    frame.asset=hierarchy::FacadeAsset::Frame;
    frame.origin={.4,0,0};frame.width=.2;frame.height=.8;frame.depth=.1;
    skeleton.structure.instances.push_back(frame);
    std::vector<std::vector<svr::CityVoxelCell>> sampled;
    assert(sampleCityVoxelDwellingCells(authored,skeleton,sampled,&error));
    assert(sampled.size()==2 && !sampled[0].empty() && !sampled[1].empty());
    const auto occupied=[](const auto &cells,int x,int y,int z) {
      return std::any_of(cells.begin(),cells.end(),[&](const auto &c) {
        return c.position==std::array<int32_t,3>{x,y,z};
      });
    };
    assert(occupied(sampled[0],1,4,0));
    assert(!occupied(sampled[0],4,4,0)); // Open threshold is carved through the wall.
    assert(occupied(sampled[0],4,9,0));
    assert(occupied(sampled[1],4,4,0)); // Frame is a separate movable part.
    assert(std::any_of(sampled[1].begin(),sampled[1].end(),[](const auto &cell) {
      return cell.materialProfile==8 && cell.rgba==0xffb3b3aeu;
    }));
    svr::CityVoxelAssetCompiler nativeAssets;
    std::vector<CityVoxelCompiledDwellingPart> nativePlacements;
    CityVoxelDwellingCompileReport nativeReport;
    assert(compileCityVoxelDwelling(authored,skeleton,nativeAssets,nativePlacements,
      nativeReport,&error));
    assert(nativePlacements.size()==2 && nativeReport.compiledParts==2 &&
      nativePlacements[0].sourcePartIndex==0 && nativePlacements[1].sourcePartIndex==1);
  }
  {
    CityVoxelTerrainTileSpec tile;
    tile.cellsPerSide=2;
    auto terrain=[](double x,double) { return x<.2?.0:.2; };
    std::vector<svr::CityVoxelCell> left,right;
    assert(sampleCityVoxelTerrainTile(tile,terrain,left,&error));
    tile.tileX=1;
    assert(sampleCityVoxelTerrainTile(tile,terrain,right,&error));
    assert(left.size()==4 && right.size()==6);
    assert((left[0].position==std::array<int32_t,3>{0,0,0}));
    assert((right[0].position==std::array<int32_t,3>{2,1,0}));
    tile.tileX=0;tile.overlapHaloCells=1;
    std::vector<svr::CityVoxelCell> haloLeft,haloRight;
    assert(sampleCityVoxelTerrainTile(tile,terrain,haloLeft,&error));
    tile.tileX=1;
    assert(sampleCityVoxelTerrainTile(tile,terrain,haloRight,&error));
    const auto boundaryCell=[](const auto &values,int32_t x,int32_t z) {
      return std::find_if(values.begin(),values.end(),[&](const auto &cell) {
        return cell.position[0]==x && cell.position[2]==z;
      });
    };
    for(int32_t x:{1,2})for(int32_t z:{0,1}) {
      const auto a=boundaryCell(haloLeft,x,z),b=boundaryCell(haloRight,x,z);
      assert(a!=haloLeft.end() && b!=haloRight.end() &&
          a->position==b->position && a->rgba==b->rgba &&
          a->materialProfile==b->materialProfile);
    }
    tile.topAppearance=[](double x,double) {
      return std::pair<uint32_t,uint32_t>{x<.2?0xff486a3eu:0xff536b86u,
          x<.2?11u:12u};
    };
    assert(sampleCityVoxelTerrainTile(tile,terrain,haloRight,&error));
    const auto painted=std::find_if(haloRight.begin(),haloRight.end(),[](const auto &cell) {
      return cell.position==(std::array<int32_t,3>{2,2,0});
    });
    assert(painted!=haloRight.end() && painted->materialProfile==12u &&
        painted->rgba==0xff536b86u);
    tile.topAppearance={};
    tile.excludeColumn=[](double x,double) {return x<.2;};
    tile.tileX=0;
    tile.maximumCells=100;
    assert(sampleCityVoxelTerrainTile(tile,terrain,right,&error));
    assert(std::none_of(right.begin(),right.end(),[](const auto &cell) {
      return cell.position[0]==1;
    }));
    assert(std::any_of(right.begin(),right.end(),[](const auto &cell) {
      return cell.position[0]==2;
    }));
    tile.excludeColumn={};
    tile.overlapHaloCells=0;
    tile.maximumCells=3;
    assert(!sampleCityVoxelTerrainTile(tile,terrain,right,&error));
  }
  {
    CityVoxelTerrainTileSpec tile;
    tile.cellsPerSide=2;tile.maximumCells=100;
    CityVoxelBasePrism deck;
    deck.footprint.outer={{procedural::geom2::toUnit(0),procedural::geom2::toUnit(0)},
        {procedural::geom2::toUnit(.4),procedural::geom2::toUnit(0)},
        {procedural::geom2::toUnit(.4),procedural::geom2::toUnit(.4)},
        {procedural::geom2::toUnit(0),procedural::geom2::toUnit(.4)}};
    deck.bottomMetres=0;deck.topMetres=.35;
    std::vector<svr::CityVoxelCell> a,b;
    assert(sampleCityVoxelBaseTile(tile,{deck},a,&error));
    tile.tileX=1;
    assert(sampleCityVoxelBaseTile(tile,{deck},b,&error));
    const auto has=[](const auto &values,int x,int y,int z) {
      return std::any_of(values.begin(),values.end(),[&](const auto &cell) {
        return cell.position==(std::array<int32_t,3>{x,y,z});
      });
    };
    assert(has(a,1,3,1)&&has(b,2,3,1));
    assert(!has(a,2,3,1)&&!has(b,1,3,1)); // One owner at the tile seam.
    assert(!has(a,1,0,1)&&!has(b,2,0,1)); // No buried fill at an internal seam.
    assert(has(b,3,0,1)); // Exposed exterior side reaches the foundation.
    deck.terrainFollowing=true;deck.bottomMetres=0;deck.topMetres=.25;
    tile.tileX=0;
    assert(sampleCityVoxelBaseTile(tile,{deck},a,&error,
        [](double,double){return 1.0;}));
    assert(has(a,1,12,1));
    assert(!has(a,1,3,1));
  }
  {
    hierarchy::DwellingVoxelAssembly authored;
    authored.grid.voxelMetres=.1;
    authored.parts.resize(1);
    authored.parts[0].source=hierarchy::DwellingVoxelPartSource::UnassignedFace;
    hierarchy::Skeleton skeleton;
    hierarchy::StructureFace face;
    face.kind=hierarchy::FaceKind::Paving;
    face.outline={{0,0,0},{.3,0,0},{.3,0,.1},{.1,0,.1},{.1,0,.3},{0,0,.3}};
    skeleton.structure.faces.push_back(face);
    std::vector<std::vector<svr::CityVoxelCell>> cells;
    assert(sampleCityVoxelDwellingCells(authored,skeleton,cells,&error));
    const auto has=[&](int x,int z) {return std::any_of(cells[0].begin(),cells[0].end(),
      [&](const auto &cell){return cell.position[0]==x&&cell.position[2]==z;});};
    assert(has(0,2) && has(2,0) && !has(2,2));
  }
  {
    hierarchy::DwellingVoxelAssembly rotated;
    rotated.grid.voxelMetres=.1;
    rotated.grid.frame.originX=10;rotated.grid.frame.originZ=20;
    rotated.grid.frame.axisX=0;rotated.grid.frame.axisZ=1;
    rotated.parts.resize(1);
    rotated.parts[0].source=hierarchy::DwellingVoxelPartSource::UnassignedFace;
    hierarchy::Skeleton skeleton;
    hierarchy::StructureFace square;
    square.kind=hierarchy::FaceKind::Paving;
    square.outline={{10,0,20},{10,0,20.3},{9.7,0,20.3},{9.7,0,20}};
    skeleton.structure.faces.push_back(square);
    std::vector<std::vector<svr::CityVoxelCell>> cells;
    assert(sampleCityVoxelDwellingCells(rotated,skeleton,cells,&error));
    assert(std::any_of(cells[0].begin(),cells[0].end(),[](const auto &cell) {
      return cell.position[0]==1 && cell.position[2]==1;
    }));
  }
  {
    // A square house ring: four walls on cell edges, a door in each wall at a
    // different quarter turn, plus an off-axis marker cell. Blocks must put
    // every cell back exactly and share the four door bays as one asset.
    constexpr int32_t cell=10,size=40;
    std::vector<svr::CityVoxelCell> house;
    const auto add=[&](int32_t x,int32_t y,int32_t z,uint32_t profile) {
      house.push_back({{x,y,z},0xff000000u|profile,profile});
    };
    for(int32_t y=-3;y<12;++y)
      for(int32_t a=0;a<size;++a)
        for(int32_t t=0;t<2;++t) {
          const bool door=a>=13 && a<17 && y>=0 && y<8;
          if(door)continue;
          const uint32_t material=y<0?3u:1u+uint32_t(t);
          add(a,y,t,material);             // South wall, outside face t=0.
          add(size-1-t,y,a,material);      // East.
          add(size-1-a,y,size-1-t,material); // North.
          add(t,y,size-1-a,material);      // West.
        }
    std::sort(house.begin(),house.end(),[](const auto &a,const auto &b){return a.position<b.position;});
    house.erase(std::unique(house.begin(),house.end(),[](const auto &a,const auto &b){
      return a.position==b.position;}),house.end());
    add(23,5,21,9); // Breaks the symmetry of one interior block.
    std::vector<CityVoxelCanonicalBlock> blocks;
    assert(sliceCityVoxelBlocks(house,cell,cell,blocks,&error));
    std::vector<std::pair<std::array<int32_t,3>,uint32_t>> rebuilt,expected;
    for(const auto &c:house)expected.push_back({c.position,c.materialProfile});
    for(const auto &block:blocks) {
      assert(block.quarterTurn<4 && !block.cells.empty());
      for(const auto &c:block.cells) {
        assert(c.position[0]>=0 && c.position[1]>=0 && c.position[2]>=0);
        // Centre of l in the turned frame, rotated back by R^t.
        double x=block.frameOrigin[0]+c.position[0]+.5,z=block.frameOrigin[2]+c.position[2]+.5;
        for(uint8_t i=0;i<block.quarterTurn;++i) {const double nx=-z;z=x;x=nx;}
        rebuilt.push_back({{int32_t(std::floor(x)),block.frameOrigin[1]+c.position[1],
                            int32_t(std::floor(z))},c.materialProfile});
      }
    }
    std::sort(rebuilt.begin(),rebuilt.end());
    std::sort(expected.begin(),expected.end());
    assert(rebuilt==expected);
    svr::CityVoxelAssetCompiler blockAssets;
    std::vector<uint64_t> doorGeometry;
    uint64_t stable=1;
    for(const auto &block:blocks) {
      svr::CityVoxelCompiledPlacement placement;
      assert(blockAssets.submit(stable++,100000u,block.cells,placement,&error));
      const auto &b=block.blockCell;
      const bool doorBay=b[1]==0 &&
          ((b[2]==0 && b[0]==1) || (b[0]==3 && b[2]==1) ||
           (b[2]==3 && b[0]==2) || (b[0]==0 && b[2]==2));
      if(doorBay)doorGeometry.push_back(placement.geometryId);
    }
    assert(doorGeometry.size()==4);
    assert(std::all_of(doorGeometry.begin(),doorGeometry.end(),
        [&](uint64_t id){return id==doorGeometry.front();}));
    // 37 blocks would be unique; corners, plain bays, door bays, footings
    // and the marker collapse to a handful.
    assert(blockAssets.geometry().size()<=12);
    std::cout<<"block test: "<<blocks.size()<<" blocks, "<<blockAssets.geometry().size()<<" geometries\n";
    assert(blocks.size()==37); // 12 perimeter columns x 3 layers + the marker.
    std::vector<svr::CityVoxelCell> far;
    downsampleCityVoxelCells(house,4,far);
    // A 40-cell ring is a 10x10 coarse grid: 36 perimeter columns in coarse
    // layers -1..2 (doors only thin a coarse cell), plus the marker's cell.
    assert(far.size()==36*4+1);
    assert(std::all_of(far.begin(),far.end(),[](const auto &c){
      return c.position[1]>=-1 && c.position[1]<=2 && c.materialProfile!=0;}));
  }
  std::cout<<"Native dwelling part compilation and instancing passed\n";
}
