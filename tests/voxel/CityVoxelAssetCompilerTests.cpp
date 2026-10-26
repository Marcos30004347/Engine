#undef NDEBUG
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include "voxel/svraster/CityVoxelExactAppearanceLookup.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <future>
#include <vector>

using namespace voxel::svraster;

int main() {
  CityVoxelAssetCompiler compiler;
  std::vector<CityVoxelCell> red, blue;
  for (int z=0;z<10;++z) for(int y=0;y<3;++y) for(int x=0;x<17;++x) {
    if ((x+y+z)%3==0) continue;
    red.push_back({{{x-21,y+4,z-7}},uint32_t(0xff000000u|x|(y<<8)|(z<<16)),uint32_t(100+y)});
    blue.push_back({{{x+79,y-12,z+14}},uint32_t(0xff000000u|z|(x<<8)|(y<<16)),uint32_t(900+x)});
  }
  std::reverse(blue.begin(),blue.end());
  CityVoxelCompiledPlacement p1,p2,p3;
  assert(compiler.submit(1,100000,red,p1));
  assert(compiler.submit(2,100000,blue,p2));
  assert(compiler.submit(3,100000,red,p3));
  assert(p1.geometryId==p2.geometryId && p1.geometryId==p3.geometryId);
  assert(p1.appearanceId!=p2.appearanceId && p1.appearanceId==p3.appearanceId);
  assert(compiler.report().distinctGeometry==1 && compiler.report().distinctAppearances==2);
  assert(compiler.report().geometryReuses==2 && compiler.report().appearanceReuses==1);
  assert(compiler.report().newGeometryCauses[0]==1);
  assert(compiler.report().appearanceVariantsOnSharedGeometry==1);
  CityVoxelAssetCompiler reordered;
  auto shuffled=red; std::reverse(shuffled.begin(),shuffled.end());
  CityVoxelCompiledPlacement other;
  assert(reordered.submit(1,100000,shuffled,other));
  assert(other.geometryId==p1.geometryId && other.appearanceId==p1.appearanceId);
  assert(reordered.geometry()[0].bytes==compiler.geometry()[0].bytes);
  assert(reordered.appearances()[0].bytes==compiler.appearances()[0].bytes);
  std::vector<std::future<std::pair<CityVoxelCompiledGeometry,CityVoxelCompiledAppearance>>> workers;
  for(int worker=0;worker<4;++worker)
    workers.push_back(std::async(std::launch::async,[shuffled,worker] {
      auto input=shuffled;
      if(worker&1)std::reverse(input.begin(),input.end());
      CityVoxelAssetCompiler local; CityVoxelCompiledPlacement placed;
      assert(local.submit(1,100000,input,placed));
      return std::make_pair(local.geometry()[0],local.appearances()[0]);
    }));
  for(auto &worker:workers) {
    const auto [wg,wa]=worker.get();
    assert(wg.id==p1.geometryId && wa.id==p1.appearanceId);
    assert(wg.bytes==compiler.geometry()[0].bytes && wa.bytes==compiler.appearances()[0].bytes);
  }
  const auto &g=compiler.geometry()[0];
  std::vector<std::array<int32_t,3>> positions;
  std::vector<CityVoxelAddress> addresses;
  assert(decodeCityVoxelGeometry(g,positions,&addresses));
  assert(positions.size()==red.size() && addresses.size()==red.size());
  std::vector<CityVoxelCell> decoded;
  for(const auto &a:compiler.appearances()) {
    assert(decodeCityVoxelAppearance(a,decoded));
    assert(decoded.size()==red.size());
    for(size_t i=0;i<decoded.size();++i) {
      const auto source=a.id==p1.appearanceId?red:blue;
      const std::array<int32_t,3> world={positions[i][0]+(a.id==p1.appearanceId?p1.originCell[0]:p2.originCell[0]),
        positions[i][1]+(a.id==p1.appearanceId?p1.originCell[1]:p2.originCell[1]),
        positions[i][2]+(a.id==p1.appearanceId?p1.originCell[2]:p2.originCell[2])};
      const auto found=std::find_if(source.begin(),source.end(),[&](const auto &c){return c.position==world;});
      assert(found!=source.end() && found->rgba==decoded[i].rgba && found->materialProfile==decoded[i].materialProfile);
    }
    const auto path=std::filesystem::temp_directory_path()/"city-voxel-phase4-assets.cva";
    assert(writeCityVoxelCompiledAssets(path.string(),g,a));
    CityVoxelCompiledGeometry diskG; CityVoxelCompiledAppearance diskA;
    assert(readCityVoxelCompiledAssets(path.string(),{},diskG,diskA));
    assert(diskG.bytes==g.bytes && diskA.bytes==a.bytes && diskA.codec==a.codec);
    { std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out); file.seekg(90); char c=0; file.read(&c,1); c^=1; file.seekp(90); file.write(&c,1); }
    assert(!readCityVoxelCompiledAssets(path.string(),{},diskG,diskA));
    std::filesystem::remove(path);
  }
  // Palette, sparse override and raw must all preserve 32-bit profiles.
  CityVoxelAssetCompiler codecs;
  auto checkExactLookup=[&](const std::vector<CityVoxelCell> &input,
                            const CityVoxelCompiledPlacement &placement) {
    const auto &all=codecs.geometry();
    const auto geometryIt=std::find_if(all.begin(),all.end(),[&](const auto &g){return g.id==placement.geometryId;});
    assert(geometryIt!=all.end());
    CityVoxelExactAppearanceLookup lookup;
    assert(lookup.bind(*geometryIt,codecs.appearances().back()));
    for(const auto &cell:input) {
      const std::array<int32_t,3> local={cell.position[0]-placement.originCell[0],
          cell.position[1]-placement.originCell[1],cell.position[2]-placement.originCell[2]};
      uint32_t rgba=0,profile=0;
      assert(lookup.lookupCell(local,rgba,profile));
      assert(rgba==cell.rgba && profile==cell.materialProfile);
    }
  };
  auto testCodec=[&](int count, auto make, CityVoxelCodec codec) {
    std::vector<CityVoxelCell> input;
    for(int i=0;i<count;++i) input.push_back({{{i,0,0}},uint32_t(make(i)),uint32_t(0xf0000000u+i)});
    CityVoxelCompiledPlacement p; assert(codecs.submit(uint64_t(count),100000,input,p));
    const auto &a=codecs.appearances().back(); assert(a.codec==codec);
    std::vector<CityVoxelCell> output; assert(decodeCityVoxelAppearance(a,output));
    assert(output.size()==input.size());
    for(size_t i=0;i<input.size();++i) assert(input[i].materialProfile==output[i].materialProfile);
    checkExactLookup(input,p);
  };
  testCodec(2,[](int i){return i;},CityVoxelCodec::Raw);
  // Repeated full value pairs select palette or sparse codec.
  std::vector<CityVoxelCell> palette;
  for(int i=0;i<1000;++i)palette.push_back({{{i,0,0}},uint32_t(i%4),uint32_t(i%4+70000)});
  CityVoxelCompiledPlacement pp; assert(codecs.submit(5000,100000,palette,pp));
  assert(codecs.appearances().back().codec==CityVoxelCodec::Palette8);
  checkExactLookup(palette,pp);
  std::vector<CityVoxelCell> wide;
  for(int i=0;i<12000;++i) wide.push_back({{{i,0,0}},uint32_t(i%300),uint32_t((i%300)+100000)});
  assert(codecs.submit(5002,100000,wide,pp));
  assert(codecs.appearances().back().codec==CityVoxelCodec::Palette16);
  std::vector<CityVoxelCell> wideDecoded;
  assert(decodeCityVoxelAppearance(codecs.appearances().back(),wideDecoded));
  for(size_t i=0;i<wide.size();++i)
    assert(wideDecoded[i].rgba==wide[i].rgba && wideDecoded[i].materialProfile==wide[i].materialProfile);
  checkExactLookup(wide,pp);
  for(int i=0;i<1000;++i){palette[i].rgba=7;palette[i].materialProfile=70000;}
  palette[200].rgba=9;palette[200].materialProfile=0xffffffffu;
  assert(codecs.submit(5001,100000,palette,pp));
  assert(codecs.appearances().back().codec==CityVoxelCodec::SparseOverride);
  checkExactLookup(palette,pp);
  CityVoxelCompilerLimits tiny; tiny.maximumScratchBytes=4096;
  CityVoxelAssetCompiler bounded(tiny); assert(!bounded.submit(1,100000,palette,pp));
  CityVoxelAssetCompiler causes;
  std::vector<CityVoxelCell> offset={{{{0,0,0}},0xff123456u,99}};
  assert(causes.submit(42,100000,offset,pp,nullptr,
    CityVoxelBakedSubcellOffset|CityVoxelUniqueSourceRecipe));
  assert(causes.report().newGeometryCauses[1]==1 && causes.report().newGeometryCauses[3]==1);
}
