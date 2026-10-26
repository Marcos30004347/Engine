#include "voxel/svraster/CityVoxelSceneFormat.hpp"
#include <vector>

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  voxel::svraster::CityVoxelSceneFileReader reader;
  if (!reader.open(argv[1], {})) return 2;
  if (reader.index().sourceHash != 123 || reader.index().settingsHash != 456 ||
      reader.index().geometry.size() != 1 || reader.index().instances.size() != 1) return 3;
  for (uint32_t id=0; id<reader.index().pages.size(); ++id) {
    std::vector<uint8_t> bytes;
    if (!reader.readPage(id,bytes) || bytes.empty()) return 4;
  }
  return 0;
}
