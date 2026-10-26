#include "editor/virtualgeometry/CityVoxelVirtualGeometryCompiler.hpp"

#include "editor/virtualgeometry/VirtualGeometryPacker.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace voxel::svraster {
namespace {
using Key = std::array<int32_t,3>;
using Bits = std::array<uint32_t,16>;
using Level = std::map<Key,Bits>;
bool fail(std::string *error, const char *message) { if(error)*error=message; return false; }
uint64_t hashWords(uint64_t hash, const std::vector<uint32_t> &words) {
  for(uint32_t word:words) for(unsigned byte=0;byte<4;++byte) {
    hash ^= uint8_t(word>>(byte*8)); hash*=1099511628211ull;
  }
  return hash;
}
}

bool buildCityVoxelVirtualGeometry(
    const CityVoxelCompiledGeometry &geometry, const CityVoxelVGSettings &settings,
    CityVoxelVGAsset &asset, std::string *error) {
  asset={}; if(error)error->clear();
  if(!settings.maximumLevels || settings.maximumLevels>27 ||
     !settings.maximumNodes || !settings.maximumEncodedBytes)
    return fail(error,"Invalid city VG build limits");
  std::vector<Key> cells;
  if(!decodeCityVoxelGeometry(geometry,cells,nullptr,error))return false;
  if(cells.empty())return fail(error,"City VG geometry has no cells");
  const double pitch=double(geometry.pitchMicrometres)*1e-6;
  if(!std::isfinite(pitch) || pitch<=0)return fail(error,"Invalid city VG pitch");

  std::vector<Level> levels;
  uint64_t totalNodes=0;
  for(uint32_t level=0;level<settings.maximumLevels;++level) {
    const int64_t scale=int64_t(1)<<level;
    Level bricks;
    for(const Key &cell:cells) {
      const Key coarse={int32_t(cell[0]/scale),int32_t(cell[1]/scale),int32_t(cell[2]/scale)};
      const Key brick={coarse[0]/8,coarse[1]/8,coarse[2]/8};
      const uint32_t bit=uint32_t(coarse[0]%8)+8u*uint32_t(coarse[1]%8)+64u*uint32_t(coarse[2]%8);
      bricks[brick][bit/32]|=1u<<(bit%32);
      if(bricks.size()+totalNodes>settings.maximumNodes)
        return fail(error,"City VG hierarchy exceeds node budget");
    }
    totalNodes+=bricks.size(); levels.push_back(std::move(bricks));
    if(levels.back().size()==1)break;
  }

  SparseVoxelBrickHierarchy hierarchy;
  hierarchy.nodes.reserve(size_t(totalNodes));
  hierarchy.occupancy.reserve(size_t(totalNodes));
  std::vector<std::map<Key,uint32_t>> nodeIndices(levels.size());
  for(uint32_t level=0;level<levels.size();++level) {
    const int64_t scale=int64_t(1)<<level;
    for(const auto &[key,bits]:levels[level]) {
      SparseVoxelBrickNodeGPU node{};
      node.level=level;
      node.occupancyIndex=uint32_t(hierarchy.occupancy.size());
      node.canonicalGridOriginScale[3]=int32_t(scale);
      for(unsigned axis=0;axis<3;++axis) {
        const int64_t origin=int64_t(key[axis])*8*scale;
        if(origin>INT32_MAX-7*scale || origin<INT32_MIN)
          return fail(error,"City VG canonical origin exceeds signed range");
        node.canonicalGridOriginScale[axis]=int32_t(origin);
        node.minimumVoxelSize[axis]=float(double(origin)*pitch);
      }
      node.minimumVoxelSize[3]=float(double(scale)*pitch);
      node.selfError=level==0?0.0f:float(double(scale)*pitch);
      SparseVoxelBrickOccupancyGPU occupancy{};
      uint32_t count=0;
      for(unsigned word=0;word<16;++word) {
        occupancy.cells[word].bits=bits[word];
        occupancy.cells[word].prefix=count;
        count+=uint32_t(__builtin_popcount(bits[word]));
      }
      node.occupiedCount=count;
      nodeIndices[level].emplace(key,uint32_t(hierarchy.nodes.size()));
      hierarchy.nodes.push_back(node);
      hierarchy.occupancy.push_back(occupancy);
    }
  }
  for(uint32_t level=0;level+1<levels.size();++level)
    for(const auto &[key,index]:nodeIndices[level]) {
      const Key parent={key[0]/2,key[1]/2,key[2]/2};
      const auto found=nodeIndices[level+1].find(parent);
      if(found==nodeIndices[level+1].end())return fail(error,"Missing city VG parent brick");
      hierarchy.nodes[index].parentIndex=found->second;
      ++hierarchy.nodes[found->second].childCount;
    }
  for(const auto &[key,index]:nodeIndices.back())hierarchy.rootIndices.push_back(index);
  hierarchy.rootCount=uint32_t(hierarchy.rootIndices.size());
  hierarchy.levelCount=uint32_t(levels.size());
  hierarchy.finestVoxelSize=float(pitch);

  std::vector<SparseVoxelBrickFaceMasks> suppressed;
  uint64_t ownershipHash=14695981039346656037ull;
  bool anySuppression=false;
  if(settings.suppressFace) {
    suppressed.resize(hierarchy.nodes.size());
    for(size_t nodeIndex=0;nodeIndex<hierarchy.nodes.size();++nodeIndex) {
      const auto &node=hierarchy.nodes[nodeIndex];
      const auto &occupancy=hierarchy.occupancy[node.occupancyIndex];
      const int64_t scale=node.canonicalGridOriginScale[3];
      for(uint32_t bit=0;bit<512;++bit) {
        if(!(occupancy.cells[bit/32].bits&(1u<<(bit%32))))continue;
        const std::array<int32_t,3> cell={
          int32_t(int64_t(node.canonicalGridOriginScale[0])+(bit%8)*scale),
          int32_t(int64_t(node.canonicalGridOriginScale[1])+((bit/8)%8)*scale),
          int32_t(int64_t(node.canonicalGridOriginScale[2])+(bit/64)*scale)};
        const Key coarse={int32_t(cell[0]/scale),int32_t(cell[1]/scale),
                          int32_t(cell[2]/scale)};
        for(uint32_t direction=0;direction<6;++direction)
        {
          Key across=coarse;
          across[direction/2]+=direction&1u?1:-1;
          bool internal=false;
          if(across[0]>=0 && across[1]>=0 && across[2]>=0) {
            const Key neighbourBrick={across[0]/8,across[1]/8,across[2]/8};
            const auto found=levels[node.level].find(neighbourBrick);
            if(found!=levels[node.level].end()) {
              const uint32_t neighbourBit=uint32_t(across[0]%8)+
                  8u*uint32_t(across[1]%8)+64u*uint32_t(across[2]%8);
              internal=(found->second[neighbourBit/32]&(1u<<(neighbourBit%32)))!=0;
            }
          }
          if(!internal && settings.suppressFace(node.level,cell,direction)) {
            suppressed[nodeIndex][direction][bit/64]|=1ull<<(bit%64);
            anySuppression=true;
          }
        }
      }
      for(const auto &direction:suppressed[nodeIndex])
        for(uint64_t plane:direction)
          for(unsigned byte=0;byte<8;++byte) {
            ownershipHash^=uint8_t(plane>>(byte*8));
            ownershipHash*=1099511628211ull;
          }
    }
  }
  if(!anySuppression) { suppressed.clear(); ownershipHash=0; }

  SparseVoxelOctreeVirtualGeometrySettings octree{};
  octree.geometryOnly=true;
  octree.canonicalWorldGrid={0.0,0.0,0.0,pitch};
  octree.originOffset={0.0,0.0,0.0};
  if(!suppressed.empty())octree.suppressedFaces=&suppressed;
  virtualgeometry::VirtualGeometryBuildData build;
  SparseVoxelOctreeVirtualGeometryStats stats;
  if(!buildSparseVoxelOctreeVirtualGeometry(hierarchy,0,uint32_t(hierarchy.nodes.size()),
       octree,settings.pageSettings,build,&stats,error))return false;
  if(build.clusters.empty() || build.pages.empty())
    return fail(error,"City VG build emitted no clusters or pages");

  asset.geometryId=geometry.id;
  asset.geometryAddressHash=geometry.addressHash;
  asset.faceOwnershipHash=ownershipHash;
  asset.requiresFinestInterfaceCut=levels.size()>1;
  asset.buildStats=std::move(stats);
  asset.clusterSources.reserve(asset.buildStats.clusterSources.size());
  for(const auto &source:asset.buildStats.clusterSources) {
    if(source.brickNodeIndex>=hierarchy.nodes.size())
      return fail(error,"City VG cluster source is invalid");
    const auto &node=hierarchy.nodes[source.brickNodeIndex];
    asset.clusterSources.push_back({{
      node.canonicalGridOriginScale[0],node.canonicalGridOriginScale[1],
      node.canonicalGridOriginScale[2]},
      uint32_t(node.canonicalGridOriginScale[3]),source.level});
  }
  asset.buildPages=build.pages;
  asset.encoded=virtualgeometry::VirtualGeometryPacker::encode(
      build,settings.quantization,nullptr,settings.encodeThreads);
  asset.pages.reserve(asset.encoded.pages.size());
  asset.pageRanges.reserve(asset.encoded.pages.size());
  uint64_t pageHash=14695981039346656037ull;
  for(const auto &page:asset.encoded.pages) {
    auto buffer=virtualgeometry::PageBuffer::encode(page);
    const uint64_t bytes=uint64_t(buffer.data.size())*4;
    if(bytes>UINT32_MAX)return fail(error,"City VG page exceeds 32-bit range size");
    const uint64_t checksum=hashWords(14695981039346656037ull,buffer.data);
    asset.pageRanges.push_back({asset.encodedBytes,uint32_t(bytes),checksum});
    asset.encodedBytes+=bytes;
    if(asset.encodedBytes>settings.maximumEncodedBytes)
      return fail(error,"City VG encoded pages exceed byte budget");
    pageHash=hashWords(pageHash,buffer.data);
    asset.pages.push_back(std::move(buffer));
  }
  asset.encodedPageHash=pageHash;
  return true;
}
} // namespace voxel::svraster
