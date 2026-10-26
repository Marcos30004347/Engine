#include "editor/virtualgeometry/CityVoxelDwellingCompiler.hpp"
#include "editor/virtualgeometry/CityVoxelDwellingCellSampler.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>

namespace virtualgeometry {
namespace {
bool fail(std::string *error,const char *message) {
  if(error)*error=message;
  return false;
}
uint64_t mix(uint64_t hash,uint64_t value) {
  for(unsigned byte=0;byte<8;++byte) {
    hash^=uint8_t(value>>(byte*8));hash*=1099511628211ull;
  }
  return hash;
}
}

namespace {
// Overlays, glass, covered parts and cross-part ownership: what is left in
// `owned` is the dwelling's final, disjoint opaque cell set.
bool resolveDwellingCells(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    std::vector<std::vector<voxel::svraster::CityVoxelCell>> &&partCells,
    CityVoxelDwellingCompileReport &report,
    std::vector<CityVoxelAuthoredPart> &owned,std::vector<uint32_t> &sourceIndices,
    uint32_t &pitch,std::string *error,bool tolerateCovered=false) {
  using namespace voxel::svraster;
  report={};
  if(error)error->clear();
  const double micrometres=assembly.grid.voxelMetres*1'000'000.0;
  const double rounded=std::round(micrometres);
  if(!assembly.dwellingId || partCells.size()!=assembly.parts.size() ||
      !std::isfinite(micrometres) || rounded<1 || rounded>UINT32_MAX ||
      std::abs(micrometres-rounded)>0.01)
    return fail(error,"Dwelling part cells do not match a valid assembly lattice");
  pitch=uint32_t(rounded);
  using Address=std::pair<uint32_t,std::array<int32_t,3>>;
  std::map<Address,std::vector<CityVoxelCell *>> leaves;
  for(uint32_t i=0;i<assembly.parts.size();++i) {
    const auto &part=assembly.parts[i];
    if(!part.doorLeafSurface)continue;
    for(auto &cell:partCells[i])leaves[{part.segment,cell.position}].push_back(&cell);
  }
  for(uint32_t i=0;i<assembly.parts.size();++i) {
    const auto &part=assembly.parts[i];
    if(!part.appearanceOverlay)continue;
    auto &cells=partCells[i];
    cells.erase(std::remove_if(cells.begin(),cells.end(),[&](const CityVoxelCell &cell) {
      const auto found=leaves.find({part.segment,cell.position});
      if(found==leaves.end())return false;
      for(auto *leaf:found->second) {
        leaf->rgba=cell.rgba;
        leaf->materialProfile=cell.materialProfile;
      }
      ++report.overlaidCells;
      return true;
    }),cells.end());
  }
  owned.clear();
  sourceIndices.clear();
  std::unordered_set<uint64_t> stableIds;
  owned.reserve(partCells.size());
  sourceIndices.reserve(partCells.size());
  for(uint32_t i=0;i<assembly.parts.size();++i) {
    const auto &part=assembly.parts[i];
    if(part.omitFromOpaqueScene) {
      ++report.omittedGlassParts;
      continue;
    }
    if(partCells[i].empty()) {
      // A block bake keeps only final cells, so a movable part with nothing
      // visible is simply absent; per-part compiles must place it.
      if(!part.omitWhenCovered && tolerateCovered) {
        ++report.omittedCoveredMovableParts;
        continue;
      }
      if(!part.omitWhenCovered)
        return fail(error,"Movable dwelling part has no occupied 10 cm cells");
      if(part.appearanceOverlay)++report.omittedDecorativeOverlays;
      else if(part.source==procedural::world::hierarchy::DwellingVoxelPartSource::Facade)
        ++report.omittedPairedFacadeSides;
      else ++report.omittedCoveredStaticParts;
      continue;
    }
    uint64_t stable=mix(mix(14695981039346656037ull,assembly.dwellingId),i+1u);
    if(!stable)stable=1;
    if(!stableIds.insert(stable).second)
      return fail(error,"Dwelling part stable ID collision");
    owned.push_back({stable,part.occupancyPriority,std::move(partCells[i])});
    sourceIndices.push_back(i);
  }
  if(owned.empty())return fail(error,"Dwelling contains no opaque occupied parts");
  if(!resolveCityVoxelPartOwnership(owned,report.ownership,error,true)) {
    const uint32_t covered=report.ownership.fullyCoveredPartIndex;
    if(covered<sourceIndices.size())
      report.fullyCoveredSourcePartIndex=sourceIndices[covered];
    if(error) {
      std::string source=covered<sourceIndices.size()
          ? " source part "+std::to_string(sourceIndices[covered])+" type "+
              std::to_string(uint32_t(assembly.parts[sourceIndices[covered]].source))+
              " authored cells "+std::to_string(owned[covered].cells.size())+";"
          : "";
      *error="Dwelling "+std::to_string(assembly.dwellingId)+source+" "+*error;
    }
    return false;
  }
  for(uint32_t covered:report.ownership.fullyCoveredPartIndices) {
    const uint32_t sourceIndex=sourceIndices[covered];
    if(!assembly.parts[sourceIndex].omitWhenCovered && tolerateCovered) {
      ++report.omittedCoveredMovableParts;
      continue;
    }
    if(!assembly.parts[sourceIndex].omitWhenCovered) {
      report.fullyCoveredSourcePartIndex=sourceIndex;
      if(error)*error="Dwelling "+std::to_string(assembly.dwellingId)+
          " source part "+std::to_string(sourceIndex)+
          " type "+std::to_string(uint32_t(assembly.parts[sourceIndex].source))+
          " is fully covered but has no paired or fixed coverage policy";
      return false;
    }
    if(assembly.parts[sourceIndex].appearanceOverlay)
      ++report.omittedDecorativeOverlays;
    else if(assembly.parts[sourceIndex].source==procedural::world::hierarchy::DwellingVoxelPartSource::Facade)
      ++report.omittedPairedFacadeSides;
    else ++report.omittedCoveredStaticParts;
  }
  return true;
}
} // namespace

bool compileCityVoxelDwellingParts(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    std::vector<std::vector<voxel::svraster::CityVoxelCell>> &&partCells,
    voxel::svraster::CityVoxelAssetCompiler &assets,
    std::vector<CityVoxelCompiledDwellingPart> &result,
    CityVoxelDwellingCompileReport &report,std::string *error) {
  using namespace voxel::svraster;
  std::vector<CityVoxelAuthoredPart> owned;
  std::vector<uint32_t> sourceIndices;
  uint32_t pitch=0;
  if(!resolveDwellingCells(assembly,std::move(partCells),report,owned,sourceIndices,
                           pitch,error))return false;
  std::vector<CityVoxelCompiledDwellingPart> next;
  next.reserve(owned.size());
  for(uint32_t i=0;i<owned.size();++i) {
    if(owned[i].cells.empty())continue;
    const auto &source=assembly.parts[sourceIndices[i]];
    uint8_t cause=CityVoxelCanonicalShape;
    if(source.requiresUniqueOrientation)cause|=CityVoxelBakedRotation;
    if(source.requiresBakedGeometry) {
      for(double residual:source.residualMetres)
        if(std::abs(residual)>1e-6) {cause|=CityVoxelBakedSubcellOffset;break;}
    }
    CityVoxelCompiledPlacement placement;
    if(!assets.submit(owned[i].stableId,pitch,owned[i].cells,placement,error,cause))
      return false;
    next.push_back({sourceIndices[i],placement});
  }
  report.compiledParts=uint32_t(next.size());
  result.insert(result.end(),next.begin(),next.end());
  return true;
}

bool sliceCityVoxelBlocks(const std::vector<voxel::svraster::CityVoxelCell> &cells,
    int32_t across,int32_t up,std::vector<CityVoxelCanonicalBlock> &result,
    std::string *error) {
  using voxel::svraster::CityVoxelCell;
  using I=std::array<int32_t,3>;
  result.clear();
  if(across<1 || up<1)return fail(error,"City voxel block size must be positive");
  const auto floorDiv=[](int32_t value,int32_t size) {
    return value>=0?value/size:-((-value+size-1)/size);
  };
  // Cutting a disjoint cell set on a lattice partitions it exactly: blocks
  // cannot open a seam or double a face between them.
  std::map<I,std::vector<CityVoxelCell>> blocks;
  for(const auto &cell:cells) {
    const I key{floorDiv(cell.position[0],across),floorDiv(cell.position[1],up),
                floorDiv(cell.position[2],across)};
    CityVoxelCell local=cell;
    local.position={cell.position[0]-key[0]*across,cell.position[1]-key[1]*up,
                    cell.position[2]-key[2]*across};
    blocks[key].push_back(local);
  }
  const auto order=[](const CityVoxelCell &a,const CityVoxelCell &b) {
    return std::tie(a.position,a.rgba,a.materialProfile)<
           std::tie(b.position,b.rgba,b.materialProfile);
  };
  const auto less=[](const std::vector<CityVoxelCell> &a,const std::vector<CityVoxelCell> &b) {
    for(size_t i=0;i<a.size();++i)
      if(std::tie(a[i].position,a[i].rgba,a[i].materialProfile)!=
         std::tie(b[i].position,b[i].rgba,b[i].materialProfile))
        return std::tie(a[i].position,a[i].rgba,a[i].materialProfile)<
               std::tie(b[i].position,b[i].rgba,b[i].materialProfile);
    return false;
  };
  result.reserve(blocks.size());
  std::vector<CityVoxelCell> candidate;
  for(auto &[key,local]:blocks) {
    // A quarter turn of the box [c, c+1) is the cell map (x,z)->(-z-1,x).
    // Undo t turns and keep the smallest translation-free encoding, so a bay
    // facing north and the same bay facing east become one asset.
    CityVoxelCanonicalBlock block;
    block.blockCell=key;
    I bestMinimum{};
    for(uint8_t turn=0;turn<4;++turn) {
      candidate=local;
      for(auto &cell:candidate)
        for(uint8_t i=0;i<turn;++i)
          cell.position={cell.position[2],cell.position[1],-cell.position[0]-1};
      I minimum{INT32_MAX,INT32_MAX,INT32_MAX};
      for(const auto &cell:candidate)
        for(unsigned axis=0;axis<3;++axis)
          minimum[axis]=std::min(minimum[axis],cell.position[axis]);
      for(auto &cell:candidate)
        for(unsigned axis=0;axis<3;++axis)cell.position[axis]-=minimum[axis];
      std::sort(candidate.begin(),candidate.end(),order);
      if(turn==0 || less(candidate,block.cells)) {
        block.cells.swap(candidate);
        block.quarterTurn=turn;
        bestMinimum=minimum;
      }
    }
    // House cells are B + R^t(m + l) for canonical cell l, so in a frame
    // turned by t the block sits at R^-t(B) + m, with R^-1(x,z) = (z,-x).
    std::array<int64_t,3> origin{int64_t(key[0])*across,int64_t(key[1])*up,
                                 int64_t(key[2])*across};
    for(uint8_t i=0;i<block.quarterTurn;++i)origin={origin[2],origin[1],-origin[0]};
    for(unsigned axis=0;axis<3;++axis) {
      const int64_t value=origin[axis]+bestMinimum[axis];
      if(value<INT32_MIN || value>INT32_MAX)return fail(error,"City voxel block exceeds lattice");
      block.frameOrigin[axis]=int32_t(value);
    }
    result.push_back(std::move(block));
  }
  return true;
}

void downsampleCityVoxelCells(const std::vector<voxel::svraster::CityVoxelCell> &cells,
    int32_t factor,std::vector<voxel::svraster::CityVoxelCell> &result) {
  using voxel::svraster::CityVoxelCell;
  result.clear();
  if(factor<1)return;
  const auto floorDiv=[factor](int32_t value) {
    return value>=0?value/factor:-((-value+factor-1)/factor);
  };
  // Coarse cell -> (rgba, material) -> count. A coarse cell takes the most
  // common exact value of the fine cells inside it, ties to the smaller.
  std::map<std::array<int32_t,3>,std::map<std::pair<uint32_t,uint32_t>,uint32_t>> votes;
  for(const auto &cell:cells)
    ++votes[{floorDiv(cell.position[0]),floorDiv(cell.position[1]),
             floorDiv(cell.position[2])}][{cell.rgba,cell.materialProfile}];
  result.reserve(votes.size());
  for(const auto &[position,values]:votes) {
    auto best=values.begin();
    for(auto it=values.begin();it!=values.end();++it)
      if(it->second>best->second)best=it;
    result.push_back({position,best->first.first,best->first.second});
  }
}

bool resolveCityVoxelDwellingCells(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    const procedural::world::hierarchy::Skeleton &skeleton,
    std::vector<voxel::svraster::CityVoxelCell> &cells,uint32_t &pitch,
    CityVoxelDwellingCompileReport &report,std::string *error) {
  using namespace voxel::svraster;
  std::vector<std::vector<CityVoxelCell>> partCells;
  if(!sampleCityVoxelDwellingCells(assembly,skeleton,partCells,error))return false;
  std::vector<CityVoxelAuthoredPart> owned;
  std::vector<uint32_t> sourceIndices;
  if(!resolveDwellingCells(assembly,std::move(partCells),report,owned,sourceIndices,
                           pitch,error,true))return false;
  cells.clear();
  for(auto &part:owned)
    cells.insert(cells.end(),part.cells.begin(),part.cells.end());
  return true;
}

bool sliceCityVoxelDwelling(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    const procedural::world::hierarchy::Skeleton &skeleton,
    const CityVoxelDwellingBlockSettings &settings,
    std::vector<CityVoxelCanonicalBlock> &blocks,uint32_t &pitch,
    CityVoxelDwellingCompileReport &report,std::string *error) {
  std::vector<voxel::svraster::CityVoxelCell> cells;
  if(!resolveCityVoxelDwellingCells(assembly,skeleton,cells,pitch,report,error))return false;
  const int32_t across=int32_t(settings.horizontalVoxels?settings.horizontalVoxels:
                                                         assembly.grid.voxelsPerCell);
  const int32_t up=int32_t(settings.verticalVoxels?settings.verticalVoxels:
                                                   assembly.grid.voxelsPerCell);
  return sliceCityVoxelBlocks(cells,across,up,blocks,error);
}

bool submitCityVoxelDwellingBlocks(uint64_t dwellingId,uint32_t pitch,
    const std::vector<CityVoxelCanonicalBlock> &blocks,
    voxel::svraster::CityVoxelAssetCompiler &assets,
    std::vector<CityVoxelCompiledDwellingBlock> &result,std::string *error) {
  std::vector<CityVoxelCompiledDwellingBlock> next;
  next.reserve(blocks.size());
  for(const auto &block:blocks) {
    const auto &key=block.blockCell;
    uint64_t stable=mix(mix(mix(mix(0x424c4f434b000000ull,dwellingId),
        uint32_t(key[0])),uint32_t(key[1])),uint32_t(key[2]));
    if(!stable)stable=1;
    CityVoxelCompiledDwellingBlock compiled;
    if(!assets.submit(stable,pitch,block.cells,compiled.placement,error))return false;
    compiled.blockCell=key;
    compiled.quarterTurn=block.quarterTurn;
    compiled.frameOrigin=block.frameOrigin;
    next.push_back(compiled);
  }
  result.insert(result.end(),next.begin(),next.end());
  return true;
}

bool compileCityVoxelDwellingBlocks(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    const procedural::world::hierarchy::Skeleton &skeleton,
    voxel::svraster::CityVoxelAssetCompiler &assets,
    const CityVoxelDwellingBlockSettings &settings,
    std::vector<CityVoxelCompiledDwellingBlock> &result,
    CityVoxelDwellingCompileReport &report,std::string *error) {
  std::vector<CityVoxelCanonicalBlock> blocks;
  uint32_t pitch=0;
  if(!sliceCityVoxelDwelling(assembly,skeleton,settings,blocks,pitch,report,error) ||
     !submitCityVoxelDwellingBlocks(assembly.dwellingId,pitch,blocks,assets,result,error))
    return false;
  report.compiledParts=uint32_t(blocks.size());
  return true;
}

bool compileCityVoxelDwelling(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    const procedural::world::hierarchy::Skeleton &skeleton,
    voxel::svraster::CityVoxelAssetCompiler &assets,
    std::vector<CityVoxelCompiledDwellingPart> &result,
    CityVoxelDwellingCompileReport &report,std::string *error) {
  std::vector<std::vector<voxel::svraster::CityVoxelCell>> cells;
  if(!sampleCityVoxelDwellingCells(assembly,skeleton,cells,error)) return false;
  if(compileCityVoxelDwellingParts(assembly,std::move(cells),assets,result,report,error))return true;
  const uint32_t covered=report.fullyCoveredSourcePartIndex;
  if(error && covered<assembly.parts.size()) {
    const auto &part=assembly.parts[covered];
    if(part.source==procedural::world::hierarchy::DwellingVoxelPartSource::Piece &&
       part.sourceIndex<skeleton.pieces.size()) {
      const auto &piece=skeleton.pieces[part.sourceIndex];
      *error+="; piece "+std::to_string(part.sourceIndex)+" kind "+
          std::to_string(uint32_t(piece.kind))+" face kind "+
          std::to_string(uint32_t(piece.faceKind))+" size "+
          std::to_string(piece.width)+"x"+std::to_string(piece.height)+"x"+
          std::to_string(piece.depth);
    } else if(part.source==procedural::world::hierarchy::DwellingVoxelPartSource::Facade &&
              part.sourceIndex<skeleton.structure.instances.size()) {
      const auto &facade=skeleton.structure.instances[part.sourceIndex];
      *error+="; facade "+std::to_string(part.sourceIndex)+" asset "+
          std::to_string(uint32_t(facade.asset))+" size "+
          std::to_string(facade.width)+"x"+std::to_string(facade.height)+"x"+
          std::to_string(facade.depth);
    }
    std::vector<std::vector<voxel::svraster::CityVoxelCell>> authored;
    if(sampleCityVoxelDwellingCells(assembly,skeleton,authored,nullptr) &&
       covered<authored.size()) {
      std::set<std::array<int32_t,3>> target;
      for(const auto &cell:authored[covered])target.insert(cell.position);
      std::vector<std::pair<uint32_t,uint32_t>> overlaps;
      for(uint32_t i=0;i<authored.size();++i)if(i!=covered) {
        uint32_t shared=0;
        for(const auto &cell:authored[i])shared+=target.count(cell.position)!=0;
        if(shared)overlaps.push_back({shared,i});
      }
      std::sort(overlaps.begin(),overlaps.end(),std::greater<>());
      for(size_t j=0;j<std::min<size_t>(overlaps.size(),6);++j) {
        const auto [shared,i]=overlaps[j];
        *error+="; overlap "+std::to_string(i)+"/"+
            std::to_string(shared)+" cells source "+
            std::to_string(uint32_t(assembly.parts[i].source));
        if(assembly.parts[i].source==procedural::world::hierarchy::DwellingVoxelPartSource::Facade &&
           assembly.parts[i].sourceIndex<skeleton.structure.instances.size())
          *error+=" asset "+std::to_string(uint32_t(
              skeleton.structure.instances[assembly.parts[i].sourceIndex].asset));
      }
    }
  }
  return false;
}
} // namespace virtualgeometry
