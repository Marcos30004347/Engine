#include "voxel/svraster/CityVoxelAssetCompiler.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <tuple>

namespace voxel::svraster {
namespace {
constexpr uint64_t kOffset = 14695981039346656037ull;
constexpr uint64_t kPrime = 1099511628211ull;
using Position = std::array<int32_t, 3>;
using Value = std::pair<uint32_t, uint32_t>;
using BrickKey = std::array<int32_t, 3>;
struct Voxel { BrickKey brick; uint16_t bit; Value value; };

bool fail(std::string *error, const char *message) { if (error) *error = message; return false; }
void put(std::vector<uint8_t> &out, uint64_t v, unsigned width) {
  for (unsigned i = 0; i < width; ++i) out.push_back(uint8_t(v >> (i * 8)));
}
uint64_t get(const std::vector<uint8_t> &in, size_t &at, unsigned width) {
  uint64_t v = 0; for (unsigned i = 0; i < width; ++i) v |= uint64_t(in[at++]) << (i * 8); return v;
}
uint64_t hash(const uint8_t *bytes, size_t size) {
  uint64_t h = kOffset;
  for (size_t i=0;i<size;++i) { h ^= bytes[i]; h *= kPrime; }
  return h;
}
uint64_t hash(const std::vector<uint8_t> &bytes) {
  return hash(bytes.data(),bytes.size());
}
void value(std::vector<uint8_t> &out, Value v) { put(out, v.first, 4); put(out, v.second, 4); }
Value value(const std::vector<uint8_t> &in, size_t &at) {
  return {uint32_t(get(in, at, 4)), uint32_t(get(in, at, 4))};
}
uint32_t pop(uint32_t x) { return uint32_t(__builtin_popcount(x)); }
uint64_t assetId(uint64_t h, uint8_t kind) {
  std::vector<uint8_t> bytes; put(bytes, h, 8); put(bytes, kind, 1);
  const uint64_t id = hash(bytes); return id ? id : 1;
}
bool samePosition(const CityVoxelCell &a, const CityVoxelCell &b) { return a.position == b.position; }
bool lessPosition(const CityVoxelCell &a, const CityVoxelCell &b) { return a.position < b.position; }

std::vector<uint8_t> encodeAppearance(const std::vector<Value> &values, CityVoxelCodec &chosen) {
  std::map<Value, uint32_t> frequency;
  for (const auto v : values) ++frequency[v];
  Value base{}; uint32_t baseCount = 0;
  for (const auto &[v, count] : frequency)
    if (count > baseCount) { base = v; baseCount = count; }
  const uint64_t n = values.size(), palette = frequency.size();
  uint64_t best = n * 8; chosen = CityVoxelCodec::Raw;
  if (palette <= 256 && 4 + palette * 8 + n < best) {
    best = 4 + palette * 8 + n; chosen = CityVoxelCodec::Palette8;
  }
  if (palette <= 65536 && 4 + palette * 8 + n * 2 < best) {
    best = 4 + palette * 8 + n * 2; chosen = CityVoxelCodec::Palette16;
  }
  if (12 + (n - baseCount) * 12 < best) chosen = CityVoxelCodec::SparseOverride;
  std::vector<uint8_t> out;
  if (chosen == CityVoxelCodec::Raw) {
    out.reserve(size_t(n * 8)); for (auto v : values) value(out, v);
  } else if (chosen == CityVoxelCodec::SparseOverride) {
    value(out, base); put(out, n - baseCount, 4);
    for (uint32_t i = 0; i < values.size(); ++i)
      if (values[i] != base) { put(out, i, 4); value(out, values[i]); }
  } else {
    put(out, palette, 4);
    std::map<Value, uint16_t> indices;
    uint32_t i = 0;
    for (const auto &[v, count] : frequency) { value(out, v); indices[v] = uint16_t(i++); }
    for (auto v : values) put(out, indices[v], chosen == CityVoxelCodec::Palette8 ? 1 : 2);
  }
  return out;
}
} // namespace

CityVoxelAssetCompiler::CityVoxelAssetCompiler(CityVoxelCompilerLimits limits) : limits_(limits) {}

bool CityVoxelAssetCompiler::submit(uint64_t stableId, uint32_t pitch,
                                    const std::vector<CityVoxelCell> &cells,
                                    CityVoxelCompiledPlacement &placement, std::string *error,
                                    uint8_t geometryCauseFlags) {
  if (!stableId || !pitch || cells.empty() || cells.size() > limits_.maximumPartCells)
    return fail(error, "Invalid part ID, pitch or cell count");
  if (geometryCauseFlags & ~uint8_t(CityVoxelBakedSubcellOffset |
                                    CityVoxelBakedRotation | CityVoxelUniqueSourceRecipe))
    return fail(error, "Unknown geometry uniqueness cause");
  // Worst-case live storage includes source, sorted copy, brick tuples, values,
  // and encoded output. Reject before allocating the temporary vectors.
  // std::map nodes can dominate the worst case where every voxel has a
  // distinct appearance. Include allocator/tree overhead and temporary
  // identity/encoded buffers, not only the payload structs.
  const uint64_t scratch = uint64_t(cells.size()) *
      (sizeof(CityVoxelCell) * 2 + sizeof(Voxel) + sizeof(Value) + 96) + 4096;
  if (scratch > limits_.maximumScratchBytes) return fail(error, "Part exceeds scratch budget");
  report_.peakScratchBytes = std::max(report_.peakScratchBytes, scratch);
  std::vector<CityVoxelCell> sorted = cells;
  std::sort(sorted.begin(), sorted.end(), lessPosition);
  for (size_t i = 1; i < sorted.size(); ++i)
    if (samePosition(sorted[i-1], sorted[i])) return fail(error, "Part has duplicate occupied cell");
  Position minimum = sorted.front().position, maximum = minimum;
  for (const auto &cell : sorted)
    for (int axis = 0; axis < 3; ++axis) {
      minimum[axis] = std::min(minimum[axis], cell.position[axis]);
      maximum[axis] = std::max(maximum[axis], cell.position[axis]);
    }
  Position extent{};
  for (int axis = 0; axis < 3; ++axis) {
    const int64_t width = int64_t(maximum[axis]) - minimum[axis] + 1;
    if (width > INT32_MAX) return fail(error, "Part extent exceeds signed cell range");
    extent[axis] = int32_t(width);
  }
  std::vector<Voxel> voxels; voxels.reserve(sorted.size());
  for (const auto &cell : sorted) {
    Position p{}; for (int a = 0; a < 3; ++a) p[a] = int32_t(int64_t(cell.position[a]) - minimum[a]);
    voxels.push_back({{p[0]/8, p[1]/8, p[2]/8},
      uint16_t((p[0]%8) + 8*(p[1]%8) + 64*(p[2]%8)),
      {cell.rgba, cell.materialProfile}});
  }
  std::sort(voxels.begin(), voxels.end(), [](const Voxel &a, const Voxel &b) {
    return std::tie(a.brick, a.bit) < std::tie(b.brick, b.bit);
  });
  std::vector<uint8_t> geometryBytes;
  put(geometryBytes, 0x31475643u, 4); // CVG1
  put(geometryBytes, pitch, 4);
  for (int32_t e : extent) put(geometryBytes, uint32_t(e), 4);
  put(geometryBytes, cells.size(), 4);
  const size_t brickCountOffset = geometryBytes.size(); put(geometryBytes, 0, 4);
  std::vector<Value> values; values.reserve(cells.size());
  size_t at = 0; uint32_t brickCount = 0;
  while (at < voxels.size()) {
    if (++brickCount > limits_.maximumBricks) return fail(error, "Part exceeds brick limit");
    const BrickKey key = voxels[at].brick;
    std::array<uint32_t,16> bits{};
    do { bits[voxels[at].bit / 32] |= 1u << (voxels[at].bit % 32); values.push_back(voxels[at].value); ++at; }
    while (at < voxels.size() && voxels[at].brick == key);
    for (int32_t c : key) put(geometryBytes, uint32_t(c), 4);
    for (uint32_t word : bits) put(geometryBytes, word, 4);
  }
  for (unsigned i = 0; i < 4; ++i) geometryBytes[brickCountOffset+i] = uint8_t(brickCount >> (i*8));
  // Address identity excludes material and placement but includes pitch and
  // normalized shape. The canonical brick stream defines all ranks.
  const uint64_t geometryHash = hash(geometryBytes);
  CityVoxelCompiledGeometry candidate;
  candidate.contentHash = geometryHash; candidate.addressHash = geometryHash;
  candidate.id = assetId(geometryHash, 1); candidate.pitchMicrometres = pitch;
  candidate.extent = extent; candidate.occupiedCount = uint32_t(cells.size());
  candidate.bytes = std::move(geometryBytes);
  const auto geom = geometryById_.find(candidate.id);
  const bool geometryWasReused = geom != geometryById_.end();
  if (geom != geometryById_.end() && geometry_[geom->second].bytes != candidate.bytes)
    return fail(error, "Geometry hash collision");
  if (geom == geometryById_.end()) {
    report_.geometryBytes += candidate.bytes.size(); ++report_.distinctGeometry;
    ++report_.uniqueGeometryFromOccupancy;
    if (!geometryCauseFlags) ++report_.newGeometryCauses[0];
    for (unsigned bit=0;bit<3;++bit)
      if (geometryCauseFlags & (1u<<bit)) ++report_.newGeometryCauses[bit+1];
    geometryById_.emplace(candidate.id, uint32_t(geometry_.size()));
    geometry_.push_back(candidate);
  } else ++report_.geometryReuses;

  CityVoxelCodec codec;
  std::vector<uint8_t> encoded = encodeAppearance(values, codec);
  std::vector<uint8_t> identity; put(identity, candidate.addressHash, 8);
  for (auto v : values) value(identity, v);
  CityVoxelCompiledAppearance appearance;
  appearance.contentHash = hash(identity); appearance.id = assetId(appearance.contentHash, 2);
  appearance.geometryAddressHash = candidate.addressHash; appearance.geometryId = candidate.id;
  appearance.codec = codec; appearance.occupiedCount = uint32_t(values.size());
  appearance.bytes = std::move(encoded);
  const auto app = appearanceById_.find(appearance.id);
  if (app != appearanceById_.end() && (appearances_[app->second].geometryAddressHash != appearance.geometryAddressHash ||
      appearances_[app->second].codec != appearance.codec || appearances_[app->second].bytes != appearance.bytes))
    return fail(error, "Appearance hash collision");
  if (app == appearanceById_.end()) {
    report_.appearanceBytes += appearance.bytes.size(); ++report_.distinctAppearances;
    ++report_.uniqueAppearanceFromValues;
    if (geometryWasReused) ++report_.appearanceVariantsOnSharedGeometry;
    appearanceById_.emplace(appearance.id, uint32_t(appearances_.size()));
    ++report_.codecCounts[uint16_t(codec)]; appearances_.push_back(appearance);
  } else ++report_.appearanceReuses;
  ++report_.inputPlacements; report_.occupiedVoxels += cells.size();
  placement = {stableId, candidate.id, appearance.id, minimum};
  return true;
}

bool decodeCityVoxelGeometry(const CityVoxelCompiledGeometry &g, std::vector<Position> &positions,
                             std::vector<CityVoxelAddress> *addresses, std::string *error) {
  positions.clear(); if (addresses) addresses->clear();
  const auto &in = g.bytes; size_t at = 0;
  if (in.size() < 28 || get(in, at, 4) != 0x31475643u) return fail(error, "Bad geometry magic");
  const uint32_t pitch = uint32_t(get(in, at, 4)); Position extent{};
  for (auto &e : extent) e = int32_t(get(in, at, 4));
  const uint32_t count = uint32_t(get(in, at, 4)), bricks = uint32_t(get(in, at, 4));
  if (!pitch || pitch != g.pitchMicrometres || extent != g.extent || count != g.occupiedCount ||
      !count || !bricks || uint64_t(count) > uint64_t(bricks)*512 ||
      bricks > (in.size()-28)/76 || in.size() != 28ull + uint64_t(bricks)*76 ||
      hash(in) != g.contentHash || g.addressHash != g.contentHash || assetId(g.contentHash, 1) != g.id)
    return fail(error, "Invalid geometry header or checksum");
  positions.reserve(count); if (addresses) addresses->reserve(count);
  BrickKey last{};
  for (uint32_t b = 0; b < bricks; ++b) {
    BrickKey key{}; for (auto &c : key) c = int32_t(get(in, at, 4));
    if ((b && key <= last) || key[0] < 0 || key[1] < 0 || key[2] < 0) return fail(error, "Invalid brick order");
    last = key; std::array<uint32_t,16> bits{}; uint32_t localCount = 0;
    for (auto &word : bits) { word = uint32_t(get(in, at, 4)); localCount += pop(word); }
    if (!localCount) return fail(error, "Empty sparse brick");
    uint16_t rank = 0;
    for (uint32_t bit = 0; bit < 512; ++bit) if (bits[bit/32] & (1u << (bit%32))) {
      const int64_t x = int64_t(key[0])*8 + bit%8;
      const int64_t y = int64_t(key[1])*8 + (bit/8)%8;
      const int64_t z = int64_t(key[2])*8 + bit/64;
      if (x >= extent[0] || y >= extent[1] || z >= extent[2] || positions.size() >= count)
        return fail(error, "Brick cell outside part extent");
      positions.push_back({int32_t(x), int32_t(y), int32_t(z)});
      if (addresses) addresses->push_back({b, rank}); ++rank;
    }
  }
  if (positions.size() != count) return fail(error, "Geometry occupancy count mismatch");
  return true;
}

bool decodeCityVoxelAppearance(const CityVoxelCompiledAppearance &a,
                               std::vector<CityVoxelCell> &cells, std::string *error) {
  cells.clear(); const auto &in = a.bytes; const uint64_t n = a.occupiedCount;
  if (!n || n > 16'000'000 || !a.geometryAddressHash || !a.geometryId ||
      assetId(a.contentHash, 2) != a.id)
    return fail(error, "Invalid appearance identity");
  std::vector<Value> values; values.reserve(size_t(n)); size_t at = 0;
  if (a.codec == CityVoxelCodec::Raw) {
    if (in.size() != n*8) return fail(error, "Invalid raw appearance size");
    for (uint64_t i=0; i<n; ++i) values.push_back(value(in,at));
  } else if (a.codec == CityVoxelCodec::Palette8 || a.codec == CityVoxelCodec::Palette16) {
    if (in.size() < 4) return fail(error, "Truncated palette");
    const uint32_t count = uint32_t(get(in,at,4));
    const uint32_t max = a.codec == CityVoxelCodec::Palette8 ? 256 : 65536;
    const uint32_t stride = a.codec == CityVoxelCodec::Palette8 ? 1 : 2;
    if (!count || count > max || in.size() != 4ull+uint64_t(count)*8+n*stride)
      return fail(error, "Invalid palette size");
    std::vector<Value> palette; palette.reserve(count);
    for (uint32_t i=0; i<count; ++i) palette.push_back(value(in,at));
    for (uint64_t i=0; i<n; ++i) {
      const uint32_t index = uint32_t(get(in,at,stride));
      if (index >= count) return fail(error, "Palette index outside range");
      values.push_back(palette[index]);
    }
  } else if (a.codec == CityVoxelCodec::SparseOverride) {
    if (in.size() < 12) return fail(error, "Truncated sparse appearance");
    const Value base = value(in,at); const uint32_t count = uint32_t(get(in,at,4));
    if (count > n || in.size() != 12ull+uint64_t(count)*12)
      return fail(error, "Invalid override count");
    values.assign(size_t(n),base); uint32_t previous=0;
    for (uint32_t i=0; i<count; ++i) {
      const uint32_t index=uint32_t(get(in,at,4)); const Value v=value(in,at);
      if (index >= n || (i && index <= previous) || v == base)
        return fail(error, "Invalid override index");
      previous=index; values[index]=v;
    }
  } else return fail(error, "Unsupported appearance codec");
  std::vector<uint8_t> identity; put(identity,a.geometryAddressHash,8);
  for (auto v:values) value(identity,v);
  if (hash(identity) != a.contentHash) return fail(error, "Appearance checksum mismatch");
  cells.reserve(size_t(n));
  for (auto v:values) cells.push_back({{},v.first,v.second});
  return true;
}

bool writeCityVoxelCompiledAssets(const std::string &path, const CityVoxelCompiledGeometry &g,
                                  const CityVoxelCompiledAppearance &a, std::string *error) {
  if (a.geometryId != g.id || a.geometryAddressHash != g.addressHash)
    return fail(error, "Appearance and geometry do not match");
  std::vector<Position> p; std::vector<CityVoxelCell> c;
  if (!decodeCityVoxelGeometry(g,p,nullptr,error) || !decodeCityVoxelAppearance(a,c,error) || p.size()!=c.size())
    return fail(error, "Invalid compiled asset pair");
  std::vector<uint8_t> bytes;
  put(bytes,0x31415643u,4); // CVA1
  put(bytes,g.id,8); put(bytes,g.contentHash,8); put(bytes,g.addressHash,8);
  put(bytes,g.pitchMicrometres,4); for (int32_t e:g.extent) put(bytes,uint32_t(e),4);
  put(bytes,g.occupiedCount,4); put(bytes,g.bytes.size(),4);
  put(bytes,a.id,8); put(bytes,a.contentHash,8); put(bytes,a.geometryAddressHash,8);
  put(bytes,uint16_t(a.codec),2); put(bytes,0,2); put(bytes,a.occupiedCount,4); put(bytes,a.bytes.size(),4);
  bytes.insert(bytes.end(),g.bytes.begin(),g.bytes.end()); bytes.insert(bytes.end(),a.bytes.begin(),a.bytes.end());
  put(bytes,hash(bytes),8);
  std::ofstream file(path,std::ios::binary|std::ios::trunc);
  if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size())))
    return fail(error, "Cannot write compiled asset cache");
  return true;
}

bool readCityVoxelCompiledAssets(const std::string &path, const CityVoxelCompilerLimits &limits,
                                 CityVoxelCompiledGeometry &g, CityVoxelCompiledAppearance &a,
                                 std::string *error) {
  std::ifstream file(path,std::ios::binary|std::ios::ate);
  if (!file) return fail(error,"Cannot open compiled asset cache");
  const auto size=file.tellg();
  if (size < 96 || uint64_t(size) > limits.maximumScratchBytes / 5)
    return fail(error,"Compiled asset cache exceeds read budget");
  std::vector<uint8_t> in(size_t(size), uint8_t{}); file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(in.data()),size)) return fail(error,"Cannot read compiled asset cache");
  size_t at=0;
  if (get(in,at,4)!=0x31415643u) return fail(error,"Invalid compiled asset magic");
  CityVoxelCompiledGeometry ng; CityVoxelCompiledAppearance na;
  ng.id=get(in,at,8); ng.contentHash=get(in,at,8); ng.addressHash=get(in,at,8);
  ng.pitchMicrometres=uint32_t(get(in,at,4)); for(auto &e:ng.extent)e=int32_t(get(in,at,4));
  ng.occupiedCount=uint32_t(get(in,at,4)); const uint32_t gb=uint32_t(get(in,at,4));
  na.id=get(in,at,8); na.contentHash=get(in,at,8); na.geometryAddressHash=get(in,at,8);
  na.codec=CityVoxelCodec(get(in,at,2)); if(get(in,at,2))return fail(error,"Nonzero asset reserved field");
  na.occupiedCount=uint32_t(get(in,at,4)); const uint32_t ab=uint32_t(get(in,at,4));
  if (ng.occupiedCount > limits.maximumPartCells || ng.occupiedCount != na.occupiedCount ||
      uint64_t(gb)+ab+at+8 != in.size() || uint64_t(gb)+ab > limits.maximumScratchBytes)
    return fail(error,"Invalid compiled asset lengths");
  const uint64_t expected=hash(in.data(),in.size()-8);
  size_t tail=in.size()-8;
  if (get(in,tail,8)!=expected)return fail(error,"Compiled asset checksum mismatch");
  ng.bytes.assign(in.begin()+at,in.begin()+at+gb); at+=gb;
  na.bytes.assign(in.begin()+at,in.begin()+at+ab); na.geometryId=ng.id;
  std::vector<Position> positions; std::vector<CityVoxelCell> cells;
  if (!decodeCityVoxelGeometry(ng,positions,nullptr,error) ||
      !decodeCityVoxelAppearance(na,cells,error) || positions.size()!=cells.size() ||
      na.geometryAddressHash!=ng.addressHash)
    return fail(error,"Invalid compiled asset payload");
  g=std::move(ng); a=std::move(na); return true;
}
} // namespace voxel::svraster
