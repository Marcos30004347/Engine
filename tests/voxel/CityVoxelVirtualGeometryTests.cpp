#undef NDEBUG
#include "editor/virtualgeometry/CityVoxelVirtualGeometryCompiler.hpp"
#include "voxel/svraster/CityVoxelExactAppearanceLookup.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <map>

using namespace voxel::svraster;

int main() {
  CityVoxelAssetCompiler first, second;
  std::vector<CityVoxelCell> brickA, brickB;
  // Two connected wall runs cross a brick and an LOD block boundary.
  for(int x=0;x<18;++x) for(int y=0;y<9;++y) {
    brickA.push_back({{{x,y,0}},uint32_t(0xff000000u | (x*7u) | (y*3u<<8)),uint32_t(10+x+y)});
    brickB.push_back({{{x+100,y-40,0}},uint32_t(0xff000000u | (x*11u) | (y*13u<<16)),uint32_t(100000+x*9+y)});
  }
  CityVoxelCompiledPlacement a,b;
  assert(first.submit(1,100000,brickA,a));
  assert(second.submit(2,100000,brickB,b));
  assert(a.geometryId==b.geometryId && a.appearanceId!=b.appearanceId);
  assert(first.geometry()[0].bytes==second.geometry()[0].bytes);
  std::vector<std::array<int32_t,3>> occupied;
  std::vector<CityVoxelAddress> addresses;
  assert(decodeCityVoxelGeometry(first.geometry()[0],occupied,&addresses));
  std::vector<CityVoxelCell> colorA,colorB;
  assert(decodeCityVoxelAppearance(first.appearances()[0],colorA));
  assert(decodeCityVoxelAppearance(second.appearances()[0],colorB));
  CityVoxelExactAppearanceLookup lookupA, lookupB;
  assert(lookupA.bind(first.geometry()[0], first.appearances()[0]));
  assert(lookupB.bind(second.geometry()[0], second.appearances()[0]));
  assert(lookupA.scratchBytes() < first.geometry()[0].bytes.size());
  std::map<std::array<int32_t,3>,size_t> addressOf;
  SparseVoxelBrickOccupancyGPU firstBrick{};
  for(size_t i=0;i<occupied.size();++i) {
    addressOf.emplace(occupied[i],i);
    const auto &p=occupied[i];
    if(p[0]<8 && p[1]<8 && p[2]<8) {
      const uint32_t bit=uint32_t(p[0]+8*p[1]+64*p[2]);
      firstBrick.cells[bit/32].bits|=1u<<(bit%32);
    }
  }
  SparseVoxelBrickFaceMasks exposed{};
  sparseVoxelBrickExposedFaces(firstBrick,{},exposed);
  std::vector<SparseVoxelBrickSurfaceQuad> quads;
  sparseVoxelBrickGreedyGeometryQuads(exposed,8,quads);
  const auto top=std::find_if(quads.begin(),quads.end(),[](const auto &q) {
    return q.direction==5 && q.width==8 && q.height==8;
  });
  assert(top!=quads.end());
  for(int y=0;y<8;++y)for(int x=0;x<8;++x) {
    const auto source=sparseVoxelBrickSourceCell(*top,float(x)+0.5f,float(y)+0.5f);
    std::array<int32_t,3> fromHit{};
    assert(cityVoxelSourceCellFromHit({{0,0,0},1,0},
      {float(x)*0.1f+0.05f,float(y)*0.1f+0.05f,0.1f},
      {0.0f,0.0f,1.0f},{float(x)+0.5f,float(y)+0.5f},100000,fromHit));
    const std::array<int32_t,3> expectedCell={
      int32_t(source[0]),int32_t(source[1]),int32_t(source[2])};
    assert(fromHit==expectedCell);
    const auto found=addressOf.find({int32_t(source[0]),int32_t(source[1]),int32_t(source[2])});
    assert(found!=addressOf.end());
    const size_t index=found->second;
    assert(colorA[index].rgba==brickA[size_t(x*9+y)].rgba);
    assert(colorB[index].materialProfile==brickB[size_t(x*9+y)].materialProfile);
    uint32_t rgbaA=0, profileA=0, rgbaB=0, profileB=0;
    assert(lookupA.lookupHit({{0,0,0},1,0},
      {float(x)*0.1f+0.05f,float(y)*0.1f+0.05f,0.1f},
      {0.0f,0.0f,1.0f},{float(x)+0.5f,float(y)+0.5f},rgbaA,profileA));
    assert(lookupB.lookupCell(fromHit,rgbaB,profileB));
    assert(rgbaA==brickA[size_t(x*9+y)].rgba &&
           profileA==brickA[size_t(x*9+y)].materialProfile &&
           rgbaB==brickB[size_t(x*9+y)].rgba &&
           profileB==brickB[size_t(x*9+y)].materialProfile);
  }
  CityVoxelVGSettings settings;
  settings.maximumLevels=8;
  CityVoxelVGAsset vgA,vgB;
  std::string error;
  assert(buildCityVoxelVirtualGeometry(first.geometry()[0],settings,vgA,&error));
  CityVoxelVGSettings parallel=settings;
  parallel.encodeThreads=4;
  assert(buildCityVoxelVirtualGeometry(second.geometry()[0],parallel,vgB,&error));
  assert(vgA.encodedPageHash==vgB.encodedPageHash);
  assert(vgA.pages.size()==vgB.pages.size() && !vgA.pages.empty());
  assert(vgA.pageRanges.size()==vgA.pages.size());
  assert(vgA.buildStats.levelCount>1 && vgA.buildStats.quadCount>0);
  assert(vgA.buildStats.exposedFacesPerLevel[0]==2u*18u*9u+2u*18u+2u*9u);
  assert(vgA.requiresFinestInterfaceCut);
  uint64_t meshlets=0;
  for(size_t page=0;page<vgA.pages.size();++page) {
    assert(vgA.pages[page].data==vgB.pages[page].data);
    assert(vgA.pageRanges[page].checksum==vgB.pageRanges[page].checksum);
    virtualgeometry::VirtualGeometryPage decoded;
    virtualgeometry::PageBuffer::decode(vgA.pages[page],decoded,settings.quantization);
    assert(decoded.meshlets.size()==vgA.encoded.pages[page].meshlets.size());
    meshlets+=decoded.meshlets.size();
  }
  assert(meshlets==vgA.clusterSources.size());
  bool hasFine=false,hasCoarse=false;
  for(const auto &source:vgA.clusterSources) {
    hasFine|=source.level==0 && source.cellScale==1;
    hasCoarse|=source.level>0 && source.cellScale>1;
  }
  assert(hasFine && hasCoarse);
  CityVoxelVGSettings owned=settings;
  owned.suppressFace=[](uint32_t, const std::array<int32_t,3>&, uint32_t direction) {
    return direction==5;
  };
  CityVoxelVGAsset clipped;
  assert(buildCityVoxelVirtualGeometry(first.geometry()[0],owned,clipped,&error));
  assert(clipped.faceOwnershipHash!=vgA.faceOwnershipHash);
  assert(clipped.buildStats.exposedFaceCount<vgA.buildStats.exposedFaceCount);
  assert(clipped.encodedPageHash!=vgA.encodedPageHash);
  // Page framing survives a real file round trip byte for byte. Phase 9 will
  // install these page ranges into the complete .cvscene container.
  const auto path=std::filesystem::temp_directory_path()/"city-voxel-vg-phase5.pages";
  { std::ofstream out(path,std::ios::binary|std::ios::trunc);
    for(const auto &page:vgA.pages)
      out.write(reinterpret_cast<const char*>(page.data.data()),
                std::streamsize(page.data.size()*sizeof(uint32_t)));
    assert(out.good()); }
  { std::ifstream in(path,std::ios::binary);
    for(size_t pageIndex=0;pageIndex<vgA.pages.size();++pageIndex) {
      const auto &page=vgA.pages[pageIndex];
      const auto &range=vgA.pageRanges[pageIndex];
      assert(range.bytes==page.data.size()*sizeof(uint32_t));
      in.seekg(std::streamoff(range.offset));
      std::vector<uint32_t> words(page.data.size());
      in.read(reinterpret_cast<char*>(words.data()),std::streamsize(words.size()*sizeof(uint32_t)));
      assert(in.good() && words==page.data);
    } }
  std::filesystem::remove(path);
  CityVoxelVGSettings small=settings; small.maximumNodes=1;
  CityVoxelVGAsset rejected;
  assert(!buildCityVoxelVirtualGeometry(first.geometry()[0],small,rejected,&error));
}
