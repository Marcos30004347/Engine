#include "voxel/svraster/CityVoxelFallback.hpp"

namespace voxel::svraster {
namespace {
bool fail(std::string *error,const char *message){if(error)*error=message;return false;}
}
bool encodeCityVoxelFallback(const CityVoxelFallbackProxy &proxy,
    std::vector<uint8_t> &result,std::string *error) {
  if(error)error->clear();
  for(unsigned axis=0;axis<3;++axis)
    if(proxy.minimumCell[axis]>=proxy.maximumCell[axis])
      return fail(error,"Invalid city fallback bounds");
  std::vector<uint8_t> bytes;
  bytes.reserve(40);
  auto put=[&](uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(value>>(i*8)));};
  put(0x42465643u);put(1u);
  for(int32_t x:proxy.minimumCell)put(uint32_t(x));
  for(int32_t x:proxy.maximumCell)put(uint32_t(x));
  put(proxy.instanceCount);put(0u);
  result=std::move(bytes);
  return true;
}
bool decodeCityVoxelFallback(const uint8_t *data,size_t size,
    CityVoxelFallbackProxy &result,std::string *error) {
  if(error)error->clear();
  if(!data || size!=40)return fail(error,"City fallback payload length differs");
  auto get=[&](size_t at){return uint32_t(data[at])|(uint32_t(data[at+1])<<8u)|
      (uint32_t(data[at+2])<<16u)|(uint32_t(data[at+3])<<24u);};
  if(get(0)!=0x42465643u || get(4)!=1u || get(36)!=0u)
    return fail(error,"City fallback magic, version or reserved field differs");
  CityVoxelFallbackProxy parsed;
  for(unsigned axis=0;axis<3;++axis) {
    parsed.minimumCell[axis]=int32_t(get(8+axis*4));
    parsed.maximumCell[axis]=int32_t(get(20+axis*4));
    if(parsed.minimumCell[axis]>=parsed.maximumCell[axis])
      return fail(error,"Invalid city fallback bounds");
  }
  parsed.instanceCount=get(32);
  result=parsed;
  return true;
}
}
