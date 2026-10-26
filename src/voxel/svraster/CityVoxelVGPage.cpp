#include "voxel/svraster/CityVoxelVGPage.hpp"

namespace voxel::svraster {
namespace {
bool fail(std::string *error,const char *message){if(error)*error=message;return false;}
uint64_t get(const uint8_t *data,size_t at,unsigned width){
  uint64_t value=0;for(unsigned i=0;i<width;++i)value|=uint64_t(data[at+i])<<(i*8);
  return value;
}
}
bool decodeCityVoxelVGPage(const uint8_t *data,size_t size,uint64_t geometryId,
    uint64_t geometryHash,CityVoxelVGPageView &result,std::string *error) {
  if(error)error->clear();
  if(!data || size<32 || get(data,0,4)!=0x47565643u || get(data,4,4)!=1u ||
      get(data,8,8)!=geometryId || get(data,16,8)!=geometryHash)
    return fail(error,"City VG package identity or version differs");
  const uint64_t count=get(data,24,4),blobBytes=get(data,28,4);
  if(count>10000000u || blobBytes<64u ||
      32ull+count*20ull+blobBytes!=size)
    return fail(error,"City VG package length exceeds bounds");
  CityVoxelVGPageView view;
  view.clusterSources.reserve(size_t(count));
  size_t at=32;
  for(uint64_t i=0;i<count;++i) {
    CityVoxelVGClusterSource source;
    for(unsigned axis=0;axis<3;++axis) {
      source.brickOriginCells[axis]=int32_t(uint32_t(get(data,at,4)));at+=4;
    }
    source.cellScale=uint32_t(get(data,at,4));at+=4;
    source.level=uint32_t(get(data,at,4));at+=4;
    if(!source.cellScale || source.level>27u)
      return fail(error,"City VG cluster source is invalid");
    view.clusterSources.push_back(source);
  }
  view.vgFileBytes=data+at;
  view.vgFileSize=uint32_t(blobBytes);
  result=std::move(view);
  return true;
}
}
