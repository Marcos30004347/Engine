#undef NDEBUG
#include "voxel/svraster/CityVoxelSceneFormat.hpp"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace voxel::svraster;

namespace
{
uint64_t read(const std::vector<uint8_t> &bytes, size_t offset, unsigned width)
{
  uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i) value |= uint64_t(bytes[offset + i]) << (i * 8);
  return value;
}

void write(std::vector<uint8_t> &bytes, size_t offset, uint64_t value, unsigned width)
{
  for (unsigned i = 0; i < width; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

uint64_t hash(const uint8_t *data, size_t count)
{
  uint64_t value = 14695981039346656037ull;
  for (size_t i = 0; i < count; ++i) { value ^= data[i]; value *= 1099511628211ull; }
  return value;
}

void refresh(std::vector<uint8_t> &bytes, unsigned section)
{
  const size_t descriptor = 96 + section * 24;
  const size_t start = read(bytes, descriptor, 8);
  const size_t count = read(bytes, descriptor + 8, 4);
  const size_t stride = read(bytes, descriptor + 12, 4);
  write(bytes, descriptor + 16, hash(bytes.data() + start, count * stride), 8);
  write(bytes, 80, hash(bytes.data() + 96, bytes.size() - 96), 8);
}

void refreshHeader(std::vector<uint8_t> &bytes)
{
  write(bytes, 80, hash(bytes.data() + 96, bytes.size() - 96), 8);
}

CityVoxelSceneIndex fixture()
{
  CityVoxelSceneIndex scene;
  scene.sourceHash = 123;
  scene.settingsHash = 456;
  scene.cityOriginMillimetres = {-3000000, 7000, 2000000};
  CityVoxelGeometryAsset geometry;
  geometry.id = 11; geometry.contentHash = 101; geometry.voxelAddressHash = 202;
  geometry.minimumCell = {-4, 0, 0}; geometry.maximumCell = {12, 24, 8};
  geometry.rootPage = 0; geometry.firstPage = 0; geometry.pageCount = 1;
  scene.geometry.push_back(geometry);
  CityVoxelAppearanceAsset appearance;
  appearance.id = 21; appearance.geometryId = 11; appearance.geometryAddressHash = 202;
  appearance.contentHash = 303; appearance.rootPage = 1; appearance.firstPage = 1;
  appearance.pageCount = 1; appearance.codec = CityVoxelCodec::Palette8;
  scene.appearances.push_back(appearance);
  scene.instances.push_back({31, 11, 21, 41, 51, {-2, 4, 10}, 3, 0, 0});
  scene.spatialCells.push_back({51, {-3000000, 7000, 2000000}, 0, 1, 2, 0});
  scene.pages.push_back({0, CityVoxelPageKind::Geometry, CityVoxelCodec::Raw, 11});
  scene.pages.push_back({1, CityVoxelPageKind::Appearance, CityVoxelCodec::Palette8, 21, 0, 0, 0, 0, 0, 1});
  scene.pages.push_back({2, CityVoxelPageKind::Fallback, CityVoxelCodec::Raw, 11, 0, 0, 0, 0, 1, 1});
  scene.dependencies = {0, 0};
  scene.materials.push_back({7, 0, 707});
  return scene;
}

bool rejected(const std::vector<uint8_t> &bytes, const CityVoxelSceneLimits &limits = {})
{
  CityVoxelSceneIndex loaded;
  std::string error;
  return !deserializeCityVoxelSceneIndex(bytes.data(), bytes.size(), limits, loaded, &error) && !error.empty();
}
} // namespace

int main()
{
  const auto scene = fixture();
  const CityVoxelSceneLimits limits;
  std::string error;
  std::vector<uint8_t> bytes;
  assert(serializeCityVoxelSceneIndex(scene, limits, bytes, &error));
  CityVoxelSceneIndex loaded;
  assert(deserializeCityVoxelSceneIndex(bytes.data(), bytes.size(), limits, loaded, &error));
  assert(loaded.geometry[0].voxelAddressHash == 202 && loaded.instances[0].gridPosition[0] == -2);
  assert(loaded.cityOriginMillimetres[0] == -3000000 && loaded.pages.size() == 3);
  auto missingOwner=scene;
  missingOwner.instances[0].ownerId=0;
  std::vector<uint8_t> rejectedOwner;
  assert(!serializeCityVoxelSceneIndex(missingOwner,limits,rejectedOwner,&error));
  std::vector<uint8_t> roundTrip;
  assert(serializeCityVoxelSceneIndex(loaded, limits, roundTrip, &error));
  assert(roundTrip == bytes);

  const auto path = std::filesystem::temp_directory_path() / "city-voxel-phase1-index.cvscene";
  assert(writeCityVoxelSceneIndex(path.string(), scene, limits, &error));
  CityVoxelSceneIndex fromDisk;
  assert(readCityVoxelSceneIndex(path.string(), limits, fromDisk, &error));
  assert(fromDisk.appearances[0].geometryId == 11);
  std::filesystem::remove(path);

  // A bad directory is rejected from the fixed header before the file reader
  // allocates storage for any claimed section.
  auto badFile = bytes;
  write(badFile, 96 + 2 * 24 + 8, UINT32_MAX, 4);
  refreshHeader(badFile);
  { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char *>(badFile.data()), badFile.size()); }
  assert(!readCityVoxelSceneIndex(path.string(), limits, fromDisk, &error));
  assert(fromDisk.appearances[0].geometryId == 11);
  std::filesystem::remove(path);

  auto bad = bytes;
  bad.pop_back();
  assert(rejected(bad));
  bad = bytes; write(bad, 8, 2, 2); assert(rejected(bad));
  bad = bytes; write(bad, 96, UINT64_MAX, 8); refreshHeader(bad); assert(rejected(bad));
  bad = bytes; bad[read(bad, 96, 8) + 16] ^= 1; assert(rejected(bad));
  bad = bytes;
  const size_t appearance = read(bad, 96 + 24, 8);
  write(bad, appearance + 16, 999, 8); refresh(bad, 1);
  assert(rejected(bad));
  bad = bytes;
  const size_t page = read(bad, 96 + 4 * 24, 8);
  write(bad, page + 40, 0, 4); write(bad, page + 44, 1, 4);
  const size_t dependencies = read(bad, 96 + 5 * 24, 8);
  write(bad, dependencies, 1, 4); refresh(bad, 4); refresh(bad, 5);
  assert(rejected(bad)); // Page 0 -> page 1 -> page 0.
  CityVoxelSceneLimits small = limits; small.maximumInstances = 0;
  assert(rejected(bytes, small));
  small = limits; small.maximumIndexBytes = 128;
  assert(rejected(bytes, small));

  auto cyclicAppearance = scene;
  cyclicAppearance.appearances[0].baseAppearanceId = 21;
  assert(!serializeCityVoxelSceneIndex(cyclicAppearance, limits, bad, &error));
  auto wrongOwner = scene;
  wrongOwner.instances[0].spatialCellId = 52;
  assert(!serializeCityVoxelSceneIndex(wrongOwner, limits, bad, &error));

  const auto scenePath = std::filesystem::temp_directory_path() / "city-voxel-phase9-pages.cvscene";
  const std::vector<std::vector<uint8_t>> payloads = {{1, 2, 3, 4}, {5, 6, 7}, {8, 9}, {10, 11, 12}};
  auto producer = [&](uint32_t id, std::vector<uint8_t> &out, std::string *) {
    out = payloads.at(id); return true;
  };
  auto paged = scene;
  paged.requiredFeatures = kCityVoxelFeatureSharedGeometry | kCityVoxelFeatureTracePages;
  paged.pages[1].decodedBytes = uint32_t(payloads[1].size());
  paged.pages.push_back({3, CityVoxelPageKind::Trace, CityVoxelCodec::Raw, 11});
  assert(writeCityVoxelSceneFile(scenePath.string(), paged, limits, producer, &error));
  CityVoxelSceneFileReader reader;
  assert(reader.open(scenePath.string(), limits, &error));
  assert(reader.index().sourceHash == 123 && reader.index().settingsHash == 456);
  for (uint32_t id : {3u, 2u, 0u, 1u}) {
    std::vector<uint8_t> page;
    assert(reader.readPage(id, page, &error) && page == payloads[id]);
  }
  std::vector<uint8_t> pageData = {99};
  assert(!reader.readPage(100, pageData, &error) && pageData == std::vector<uint8_t>{99});
  assert(std::system((std::string("\"") + CITY_VOXEL_SCENE_PROBE + "\" \"" +
      scenePath.string() + "\"").c_str()) == 0);
  // A producer failure never replaces the published file.
  assert(!writeCityVoxelSceneFile(scenePath.string(), paged, limits,
      [](uint32_t, std::vector<uint8_t> &, std::string *e) { if (e) *e = "producer failed"; return false; }, &error));
  assert(reader.readPage(0, pageData, &error) && pageData == payloads[0]);

  const auto corruptPath = std::filesystem::temp_directory_path() / "city-voxel-phase9-corrupt.cvscene";
  std::filesystem::copy_file(scenePath, corruptPath, std::filesystem::copy_options::overwrite_existing);
  { std::fstream file(corruptPath, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(reader.index().pages[1].fileOffset));
    const char broken = 42; file.write(&broken, 1); }
  CityVoxelSceneFileReader corrupted;
  assert(corrupted.open(corruptPath.string(), limits, &error));
  pageData = {99};
  assert(!corrupted.readPage(1, pageData, &error) && pageData == std::vector<uint8_t>{99});
  std::filesystem::remove(corruptPath);
  std::filesystem::copy_file(scenePath, corruptPath, std::filesystem::copy_options::overwrite_existing);
  { std::fstream file(corruptPath, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(12); const char wrongEndian = 0; file.write(&wrongEndian, 1); }
  assert(!corrupted.open(corruptPath.string(), limits, &error));
  std::filesystem::remove(corruptPath);
  auto unknownCodec = paged;
  unknownCodec.pages[1].codec = static_cast<CityVoxelCodec>(99);
  assert(!writeCityVoxelSceneFile(corruptPath.string(), unknownCodec, limits, producer, &error));
  std::filesystem::remove(scenePath);

  // Compressed pages: a dictionary per kind is trained from the pages and
  // every page that shrinks is stored as LZ4; the reader returns the same
  // bytes either way.
  {
    auto many = paged;
    std::vector<std::vector<uint8_t>> manyPayloads = payloads;
    for (uint32_t i = 0; i < 64u; ++i) {
      many.pages.push_back({uint32_t(many.pages.size()), CityVoxelPageKind::Trace, CityVoxelCodec::Raw, 11});
      std::vector<uint8_t> page(3000u);
      for (size_t b = 0; b < page.size(); ++b)  // Shared structure, a little per-page noise.
        page[b] = uint8_t((b * 7u) ^ (b / 64u) ^ (b % 97u == 0u ? i : 0u));
      manyPayloads.push_back(std::move(page));
    }
    auto manyProducer = [&](uint32_t id, std::vector<uint8_t> &out, std::string *) {
      out = manyPayloads.at(id); return true;
    };
    const auto rawPath = std::filesystem::temp_directory_path() / "city-voxel-raw-pages.cvscene";
    const auto packedPath = std::filesystem::temp_directory_path() / "city-voxel-packed-pages.cvscene";
    CityVoxelSceneCompression compression;
    compression.enabled = true;
    compression.workers = 3u;
    assert(writeCityVoxelSceneFile(rawPath.string(), many, limits, manyProducer, &error));
    const bool packedWritten = writeCityVoxelSceneFile(packedPath.string(), many, limits, manyProducer, &error, compression);
    if (!packedWritten) std::fprintf(stderr, "compressed write: %s\n", error.c_str());
    assert(packedWritten);
    CityVoxelSceneFileReader packed;
    assert(packed.open(packedPath.string(), limits, &error));
    assert(packed.index().pages.size() == many.pages.size() + 1u);
    assert(packed.index().pages.back().kind == CityVoxelPageKind::Dictionary &&
           packed.index().pages.back().assetId == uint64_t(CityVoxelPageKind::Trace));
    uint32_t lz4 = 0;
    for (uint32_t id = 0; id < many.pages.size(); ++id) {
      std::vector<uint8_t> page;
      assert(packed.readPage(id, page, &error) && page == manyPayloads[id]);
      lz4 += packed.index().pages[id].codec == CityVoxelCodec::LZ4;
    }
    assert(lz4 >= 64u);
    assert(std::filesystem::file_size(packedPath) * 4u < std::filesystem::file_size(rawPath));
    std::filesystem::remove(rawPath);
    std::filesystem::remove(packedPath);
  }

  std::puts("City voxel scene format and phase 9 page IO tests passed");
}
