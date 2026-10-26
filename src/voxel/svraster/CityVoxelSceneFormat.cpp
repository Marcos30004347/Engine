#include "voxel/svraster/CityVoxelSceneFormat.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace voxel::svraster
{
namespace
{
constexpr uint64_t kHeaderBytes = 272;
constexpr uint32_t kSectionCount = 7;
constexpr uint32_t kEndian = 0x01020304u;
constexpr uint8_t kMagic[8] = {'C', 'V', 'S', 'C', 'E', 'N', 'E', 0};
constexpr uint32_t kStrides[kSectionCount] = {80, 56, 56, 48, 56, 4, 16};

bool fail(std::string *error, const char *message)
{
  if (error) *error = message;
  return false;
}

bool range(uint64_t first, uint64_t count, uint64_t size)
{
  return first <= size && count <= size - first;
}

uint64_t hashBytes(const uint8_t *data, uint64_t count)
{
  uint64_t hash = 14695981039346656037ull;
  for (uint64_t i = 0; i < count; ++i)
  {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

void put(std::vector<uint8_t> &out, uint64_t value, unsigned width)
{
  for (unsigned i = 0; i < width; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void set(std::vector<uint8_t> &out, uint64_t offset, uint64_t value, unsigned width)
{
  for (unsigned i = 0; i < width; ++i) out[static_cast<size_t>(offset + i)] = static_cast<uint8_t>(value >> (i * 8));
}

uint64_t get(const uint8_t *data, uint64_t offset, unsigned width)
{
  uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i) value |= uint64_t(data[offset + i]) << (i * 8);
  return value;
}

template <class T> void putSigned(std::vector<uint8_t> &out, T value)
{
  put(out, static_cast<typename std::make_unsigned<T>::type>(value), sizeof(T));
}

template <class T> T getSigned(const uint8_t *data, uint64_t offset)
{
  using U = typename std::make_unsigned<T>::type;
  const U value = static_cast<U>(get(data, offset, sizeof(T)));
  if (value <= static_cast<U>(std::numeric_limits<T>::max())) return static_cast<T>(value);
  const U magnitude = static_cast<U>(~value) + 1u;
  if (magnitude == (U{1} << (sizeof(T) * 8 - 1))) return std::numeric_limits<T>::min();
  return static_cast<T>(-static_cast<T>(magnitude));
}

bool validCodec(CityVoxelCodec codec)
{
  return codec >= CityVoxelCodec::Raw && codec <= CityVoxelCodec::SparseOverride;
}

bool validPageKind(CityVoxelPageKind kind)
{
  return kind >= CityVoxelPageKind::Geometry && kind <= CityVoxelPageKind::Dictionary;
}

struct Section
{
  uint64_t offset = 0;
  uint32_t count = 0;
  uint32_t stride = 0;
  uint64_t checksum = 0;
};

} // namespace

bool validateCityVoxelSceneIndex(const CityVoxelSceneIndex &index, const CityVoxelSceneLimits &limits, std::string *error)
{
  if (!index.sourceHash || !index.settingsHash) return fail(error, "City voxel source/settings hash is missing");
  if (index.requiredFeatures & ~kCityVoxelSupportedFeatures)
    return fail(error, "Unsupported city voxel scene feature bits");
  if (index.nominalPitchMicrometres == 0) return fail(error, "Zero city voxel pitch");
  if (index.geometry.size() > limits.maximumGeometry || index.appearances.size() > limits.maximumAppearances ||
      index.instances.size() > limits.maximumInstances || index.spatialCells.size() > limits.maximumSpatialCells ||
      index.pages.size() > limits.maximumPages || index.dependencies.size() > limits.maximumDependencies ||
      index.materials.size() > limits.maximumMaterials)
    return fail(error, "City voxel scene record limit exceeded");

  std::unordered_map<uint64_t, const CityVoxelGeometryAsset *> geometry;
  std::unordered_map<uint64_t, const CityVoxelAppearanceAsset *> appearances;
  std::unordered_set<uint64_t> stableIds, cellIds;
  std::unordered_set<uint32_t> materialIds;
  for (const auto &g : index.geometry)
  {
    if (!g.id || !g.contentHash || !g.voxelAddressHash || !g.pitchMicrometres || !geometry.emplace(g.id, &g).second ||
        !range(g.firstPage, g.pageCount, index.pages.size()))
      return fail(error, "Invalid or duplicate city geometry asset");
    for (unsigned axis = 0; axis < 3; ++axis)
      if (g.maximumCell[axis] <= g.minimumCell[axis]) return fail(error, "Invalid city geometry bounds");
    if ((g.pageCount == 0 && g.rootPage != kCityVoxelInvalidPage) ||
        (g.pageCount && (g.rootPage < g.firstPage || g.rootPage >= g.firstPage + g.pageCount ||
                         index.pages[g.rootPage].kind != CityVoxelPageKind::Geometry)))
      return fail(error, "Geometry root outside its page range");
  }
  for (const auto &a : index.appearances)
  {
    const auto owner = geometry.find(a.geometryId);
    if (!a.id || !a.contentHash || owner == geometry.end() || a.geometryAddressHash != owner->second->voxelAddressHash ||
        !validCodec(a.codec) || !appearances.emplace(a.id, &a).second ||
        !range(a.firstPage, a.pageCount, index.pages.size()))
      return fail(error, "Invalid, duplicate or geometry-mismatched appearance");
    if ((a.pageCount == 0 && a.rootPage != kCityVoxelInvalidPage) ||
        (a.pageCount && (a.rootPage < a.firstPage || a.rootPage >= a.firstPage + a.pageCount ||
                         index.pages[a.rootPage].kind != CityVoxelPageKind::Appearance)))
      return fail(error, "Appearance root outside its page range");
  }
  std::unordered_map<uint64_t, uint8_t> appearanceColor;
  for (const auto &a : index.appearances)
  {
    if (appearanceColor[a.id] == 2) continue;
    std::vector<uint64_t> chain;
    auto current = &a;
    while (current && appearanceColor[current->id] != 2)
    {
      if (appearanceColor[current->id] == 1) return fail(error, "Cyclic appearance base chain");
      appearanceColor[current->id] = 1;
      chain.push_back(current->id);
      if (!current->baseAppearanceId) break;
      const auto base = appearances.find(current->baseAppearanceId);
      if (base == appearances.end() || base->second->geometryId != current->geometryId)
        return fail(error, "Appearance base is missing or uses different geometry");
      current = base->second;
    }
    for (uint64_t id : chain) appearanceColor[id] = 2;
  }
  for (const auto &instance : index.instances)
  {
    const auto a = appearances.find(instance.appearanceId);
    const bool localFrame=(instance.flags & kCityVoxelInstanceLocalFrame)!=0;
    if (!instance.stableId || !instance.ownerId || !stableIds.insert(instance.stableId).second || geometry.count(instance.geometryId) == 0 ||
        a == appearances.end() || a->second->geometryId != instance.geometryId || instance.quarterTurn > 3 ||
        (instance.flags & ~kCityVoxelInstanceLocalFrame) ||
        (localFrame ? instance.quarterTurn!=0 : instance.frameId!=0))
      return fail(error, "Invalid city voxel instance");
  }
  std::vector<uint8_t> assigned(index.instances.size(), 0);
  for (const auto &cell : index.spatialCells)
  {
    if (!cell.id || cell.id > std::numeric_limits<uint32_t>::max() || !cellIds.insert(cell.id).second ||
        !range(cell.firstInstance, cell.instanceCount, index.instances.size()) ||
        (cell.fallbackPage != kCityVoxelInvalidPage &&
         (cell.fallbackPage >= index.pages.size() || index.pages[cell.fallbackPage].kind != CityVoxelPageKind::Fallback)))
      return fail(error, "Invalid city spatial cell");
    for (uint64_t i = cell.firstInstance; i < uint64_t(cell.firstInstance) + cell.instanceCount; ++i)
      if (assigned[i]++ || index.instances[i].spatialCellId != cell.id)
        return fail(error, "City instance belongs to multiple or wrong spatial cells");
  }
  if (std::find(assigned.begin(), assigned.end(), 0) != assigned.end())
    return fail(error, "City instance has no spatial cell");
  for (size_t i = 0; i < index.pages.size(); ++i)
  {
    const auto &p = index.pages[i];
    if (p.id != i || !validPageKind(p.kind) || !validCodec(p.codec) ||
        !range(p.firstDependency, p.dependencyCount, index.dependencies.size()) ||
        (p.kind == CityVoxelPageKind::Geometry && geometry.count(p.assetId) == 0) ||
        (p.kind == CityVoxelPageKind::Appearance && appearances.count(p.assetId) == 0) ||
        ((p.kind == CityVoxelPageKind::Trace || p.kind == CityVoxelPageKind::Fallback ||
          p.kind == CityVoxelPageKind::VirtualGeometry) && p.assetId && geometry.count(p.assetId) == 0) ||
        (p.kind == CityVoxelPageKind::Dictionary &&
         (p.codec != CityVoxelCodec::Raw || p.assetId < uint64_t(CityVoxelPageKind::Geometry) ||
          p.assetId >= uint64_t(CityVoxelPageKind::Dictionary))) ||
        p.storedBytes > limits.maximumStoredPageBytes || p.decodedBytes > limits.maximumDecodedPageBytes ||
        (p.storedBytes == 0 && (p.fileOffset || p.checksum)) ||
        (p.storedBytes != 0 && (!p.fileOffset || !p.decodedBytes || !p.checksum)) ||
        (p.storedBytes != 0 && p.codec == CityVoxelCodec::Raw && p.storedBytes != p.decodedBytes))
      return fail(error, "Invalid city voxel page record");
    for (uint64_t d = p.firstDependency; d < uint64_t(p.firstDependency) + p.dependencyCount; ++d)
      if (index.dependencies[d] >= index.pages.size() || index.dependencies[d] == i)
        return fail(error, "Invalid city page dependency");
  }
  // Iterative DFS avoids stack overflow on malformed files with deep chains.
  std::vector<uint8_t> color(index.pages.size(), 0);
  std::vector<std::pair<uint32_t, uint32_t>> stack;
  for (uint32_t root = 0; root < index.pages.size(); ++root)
  {
    if (color[root]) continue;
    color[root] = 1;
    stack.emplace_back(root, 0);
    while (!stack.empty())
    {
      auto &top = stack.back();
      const auto &page = index.pages[top.first];
      if (top.second == page.dependencyCount)
      {
        color[top.first] = 2;
        stack.pop_back();
        continue;
      }
      const uint32_t dependency = index.dependencies[page.firstDependency + top.second++];
      if (color[dependency] == 1) return fail(error, "Cyclic city page dependencies");
      if (!color[dependency])
      {
        color[dependency] = 1;
        stack.emplace_back(dependency, 0);
      }
    }
  }
  for (const auto &g : index.geometry)
    for (uint64_t i = g.firstPage; i < uint64_t(g.firstPage) + g.pageCount; ++i)
      if (index.pages[i].kind != CityVoxelPageKind::Geometry || index.pages[i].assetId != g.id)
        return fail(error, "Geometry page range has wrong owner");
  for (const auto &a : index.appearances)
    for (uint64_t i = a.firstPage; i < uint64_t(a.firstPage) + a.pageCount; ++i)
      if (index.pages[i].kind != CityVoxelPageKind::Appearance || index.pages[i].assetId != a.id)
        return fail(error, "Appearance page range has wrong owner");
  for (const auto &m : index.materials)
    if (!materialIds.insert(m.profileId).second || !m.assetHash)
      return fail(error, "Invalid or duplicate material profile reference");
  return true;
}

bool serializeCityVoxelSceneIndex(const CityVoxelSceneIndex &index, const CityVoxelSceneLimits &limits, std::vector<uint8_t> &bytes, std::string *error)
{
  if (!validateCityVoxelSceneIndex(index, limits, error)) return false;
  const uint64_t counts[] = {index.geometry.size(), index.appearances.size(), index.instances.size(),
                             index.spatialCells.size(), index.pages.size(), index.dependencies.size(), index.materials.size()};
  uint64_t size = kHeaderBytes;
  for (unsigned section = 0; section < kSectionCount; ++section)
  {
    if (counts[section] > std::numeric_limits<uint32_t>::max() ||
        counts[section] > (limits.maximumIndexBytes - std::min(size, limits.maximumIndexBytes)) / kStrides[section])
      return fail(error, "City scene index byte limit exceeded");
    size += counts[section] * kStrides[section];
  }
  if (size > limits.maximumIndexBytes) return fail(error, "City scene index byte limit exceeded");
  std::vector<uint8_t> out(static_cast<size_t>(kHeaderBytes), 0);
  out.reserve(static_cast<size_t>(size));
  for (unsigned i = 0; i < 8; ++i) out[i] = kMagic[i];
  set(out, 8, kCityVoxelSceneMajor, 2); set(out, 10, kCityVoxelSceneMinor, 2);
  set(out, 12, kEndian, 4); set(out, 16, kHeaderBytes, 4); set(out, 20, kSectionCount, 4);
  set(out, 24, size, 8); set(out, 32, index.sourceHash, 8); set(out, 40, index.settingsHash, 8);
  for (unsigned i = 0; i < 3; ++i) set(out, 48 + i * 8, static_cast<uint64_t>(index.cityOriginMillimetres[i]), 8);
  set(out, 72, index.nominalPitchMicrometres, 4); set(out, 76, index.requiredFeatures, 4);
  auto emit = [&](unsigned section, const auto &records, auto writer)
  {
    const uint64_t start = out.size();
    for (const auto &record : records) writer(record);
    const uint64_t count = records.size();
    const uint64_t offset = 96 + section * 24;
    set(out, offset, start, 8); set(out, offset + 8, count, 4);
    set(out, offset + 12, kStrides[section], 4);
    set(out, offset + 16, hashBytes(out.data() + start, count * kStrides[section]), 8);
  };
  emit(0, index.geometry, [&](const auto &g) {
    put(out,g.id,8);put(out,g.contentHash,8);put(out,g.voxelAddressHash,8);put(out,g.connectorHash,8);
    put(out,g.rootPage,4);put(out,g.firstPage,4);put(out,g.pageCount,4);put(out,g.pitchMicrometres,4);
    for (auto x:g.minimumCell) putSigned(out,x); for (auto x:g.maximumCell) putSigned(out,x);
    put(out,g.flags,4);put(out,0,4);
  });
  emit(1, index.appearances, [&](const auto &a) {
    put(out,a.id,8);put(out,a.contentHash,8);put(out,a.geometryAddressHash,8);put(out,a.geometryId,8);put(out,a.baseAppearanceId,8);
    put(out,a.rootPage,4);put(out,a.firstPage,4);put(out,a.pageCount,4);put(out,static_cast<uint16_t>(a.codec),2);put(out,a.flags,2);
  });
  emit(2, index.instances, [&](const auto &i) {
    put(out,i.stableId,8);put(out,i.geometryId,8);put(out,i.appearanceId,8);put(out,i.ownerId,8);put(out,i.spatialCellId,4);
    for (auto x:i.gridPosition) putSigned(out,x);
    put(out,i.quarterTurn,1);put(out,i.flags,1);put(out,i.frameId,2);put(out,0,4);
  });
  emit(3, index.spatialCells, [&](const auto &c) {
    put(out,c.id,8);for (auto x:c.originMillimetres) putSigned(out,x);
    put(out,c.firstInstance,4);put(out,c.instanceCount,4);put(out,c.fallbackPage,4);put(out,c.flags,4);
  });
  emit(4, index.pages, [&](const auto &p) {
    put(out,p.id,4);put(out,static_cast<uint16_t>(p.kind),2);put(out,static_cast<uint16_t>(p.codec),2);
    put(out,p.assetId,8);put(out,p.fileOffset,8);put(out,p.storedBytes,4);put(out,p.decodedBytes,4);
    put(out,p.checksum,8);put(out,p.firstDependency,4);put(out,p.dependencyCount,4);put(out,p.flags,4);put(out,0,4);
  });
  emit(5, index.dependencies, [&](uint32_t id) { put(out,id,4); });
  emit(6, index.materials, [&](const auto &m) {put(out,m.profileId,4);put(out,m.flags,4);put(out,m.assetHash,8);});
  if (out.size() != size) return fail(error, "Internal city scene index size mismatch");
  set(out, 80, hashBytes(out.data() + 96, size - 96), 8);
  bytes = std::move(out);
  return true;
}

bool deserializeCityVoxelSceneIndex(const uint8_t *data, uint64_t byteCount, const CityVoxelSceneLimits &limits, CityVoxelSceneIndex &result, std::string *error)
{
  if (!data || byteCount < kHeaderBytes || byteCount > limits.maximumIndexBytes)
    return fail(error, "City scene index is truncated or oversized");
  for (unsigned i = 0; i < 8; ++i) if (data[i] != kMagic[i]) return fail(error, "Wrong city scene magic");
  if (get(data,8,2) != kCityVoxelSceneMajor || get(data,10,2) > kCityVoxelSceneMinor)
    return fail(error, "Unsupported city scene version");
  if (get(data,12,4) != kEndian || get(data,16,4) != kHeaderBytes || get(data,20,4) != kSectionCount ||
      get(data,24,8) != byteCount || get(data,88,8) != 0 || get(data,264,8) != 0 ||
      hashBytes(data + 96, byteCount - 96) != get(data,80,8))
    return fail(error, "Invalid city scene header or checksum");
  const uint32_t caps[] = {limits.maximumGeometry,limits.maximumAppearances,limits.maximumInstances,
                           limits.maximumSpatialCells,limits.maximumPages,limits.maximumDependencies,limits.maximumMaterials};
  Section sections[kSectionCount];
  uint64_t next = kHeaderBytes;
  for (unsigned s = 0; s < kSectionCount; ++s)
  {
    const uint64_t at = 96 + s * 24;
    sections[s] = {get(data,at,8),static_cast<uint32_t>(get(data,at+8,4)),static_cast<uint32_t>(get(data,at+12,4)),get(data,at+16,8)};
    const auto &v = sections[s];
    if (v.offset != next || v.stride != kStrides[s] || v.count > caps[s] ||
        uint64_t(v.count) * v.stride > byteCount - next ||
        hashBytes(data + next, uint64_t(v.count) * v.stride) != v.checksum)
      return fail(error, "Invalid city scene section bounds or checksum");
    next += uint64_t(v.count) * v.stride;
  }
  if (next != byteCount) return fail(error, "Unexpected city scene trailing bytes");
  CityVoxelSceneIndex parsed;
  parsed.geometry.reserve(sections[0].count);
  parsed.appearances.reserve(sections[1].count);
  parsed.instances.reserve(sections[2].count);
  parsed.spatialCells.reserve(sections[3].count);
  parsed.pages.reserve(sections[4].count);
  parsed.dependencies.reserve(sections[5].count);
  parsed.materials.reserve(sections[6].count);
  parsed.sourceHash=get(data,32,8);parsed.settingsHash=get(data,40,8);
  for (unsigned i=0;i<3;++i) parsed.cityOriginMillimetres[i]=getSigned<int64_t>(data,48+i*8);
  parsed.nominalPitchMicrometres=static_cast<uint32_t>(get(data,72,4));
  parsed.requiredFeatures=static_cast<uint32_t>(get(data,76,4));
  for (uint32_t j=0;j<sections[0].count;++j) {
    const auto at=sections[0].offset+uint64_t(j)*80;CityVoxelGeometryAsset g;
    g.id=get(data,at,8);g.contentHash=get(data,at+8,8);g.voxelAddressHash=get(data,at+16,8);g.connectorHash=get(data,at+24,8);
    g.rootPage=get(data,at+32,4);g.firstPage=get(data,at+36,4);g.pageCount=get(data,at+40,4);g.pitchMicrometres=get(data,at+44,4);
    for(unsigned a=0;a<3;++a){g.minimumCell[a]=getSigned<int32_t>(data,at+48+a*4);g.maximumCell[a]=getSigned<int32_t>(data,at+60+a*4);}
    g.flags=get(data,at+72,4);if(get(data,at+76,4))return fail(error,"Nonzero geometry reserved field");parsed.geometry.push_back(g);
  }
  for(uint32_t j=0;j<sections[1].count;++j){const auto at=sections[1].offset+uint64_t(j)*56;CityVoxelAppearanceAsset a;
    a.id=get(data,at,8);a.contentHash=get(data,at+8,8);a.geometryAddressHash=get(data,at+16,8);a.geometryId=get(data,at+24,8);a.baseAppearanceId=get(data,at+32,8);
    a.rootPage=get(data,at+40,4);a.firstPage=get(data,at+44,4);a.pageCount=get(data,at+48,4);a.codec=static_cast<CityVoxelCodec>(get(data,at+52,2));a.flags=get(data,at+54,2);parsed.appearances.push_back(a);
  }
  for(uint32_t j=0;j<sections[2].count;++j){const auto at=sections[2].offset+uint64_t(j)*56;CityVoxelInstance i;
    i.stableId=get(data,at,8);i.geometryId=get(data,at+8,8);i.appearanceId=get(data,at+16,8);i.ownerId=get(data,at+24,8);i.spatialCellId=get(data,at+32,4);
    for(unsigned a=0;a<3;++a)i.gridPosition[a]=getSigned<int32_t>(data,at+36+a*4);
    i.quarterTurn=get(data,at+48,1);i.flags=get(data,at+49,1);i.frameId=get(data,at+50,2);if(get(data,at+52,4))return fail(error,"Nonzero instance reserved field");parsed.instances.push_back(i);
  }
  for(uint32_t j=0;j<sections[3].count;++j){const auto at=sections[3].offset+uint64_t(j)*48;CityVoxelSpatialCell c;
    c.id=get(data,at,8);for(unsigned a=0;a<3;++a)c.originMillimetres[a]=getSigned<int64_t>(data,at+8+a*8);
    c.firstInstance=get(data,at+32,4);c.instanceCount=get(data,at+36,4);c.fallbackPage=get(data,at+40,4);c.flags=get(data,at+44,4);parsed.spatialCells.push_back(c);
  }
  for(uint32_t j=0;j<sections[4].count;++j){const auto at=sections[4].offset+uint64_t(j)*56;CityVoxelPageRecord p;
    p.id=get(data,at,4);p.kind=static_cast<CityVoxelPageKind>(get(data,at+4,2));p.codec=static_cast<CityVoxelCodec>(get(data,at+6,2));p.assetId=get(data,at+8,8);
    p.fileOffset=get(data,at+16,8);p.storedBytes=get(data,at+24,4);p.decodedBytes=get(data,at+28,4);p.checksum=get(data,at+32,8);
    p.firstDependency=get(data,at+40,4);p.dependencyCount=get(data,at+44,4);p.flags=get(data,at+48,4);if(get(data,at+52,4))return fail(error,"Nonzero page reserved field");parsed.pages.push_back(p);
  }
  for(uint32_t j=0;j<sections[5].count;++j)parsed.dependencies.push_back(get(data,sections[5].offset+uint64_t(j)*4,4));
  for(uint32_t j=0;j<sections[6].count;++j){const auto at=sections[6].offset+uint64_t(j)*16;
    parsed.materials.push_back({static_cast<uint32_t>(get(data,at,4)),static_cast<uint32_t>(get(data,at+4,4)),get(data,at+8,8)});
  }
  if (!validateCityVoxelSceneIndex(parsed,limits,error)) return false;
  result=std::move(parsed);return true;
}

bool writeCityVoxelSceneIndex(const std::string &path, const CityVoxelSceneIndex &index, const CityVoxelSceneLimits &limits, std::string *error)
{
  std::vector<uint8_t> bytes;
  if (!serializeCityVoxelSceneIndex(index,limits,bytes,error)) return false;
  const std::string temporary=path+".tmp";
  {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
   if(!file)return fail(error,"Cannot open temporary city scene index");
   file.write(reinterpret_cast<const char *>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
   file.flush();if(!file)return fail(error,"Cannot write temporary city scene index");}
  CityVoxelSceneIndex verified;
  if(!readCityVoxelSceneIndex(temporary,limits,verified,error)){std::remove(temporary.c_str());return false;}
  if(std::rename(temporary.c_str(),path.c_str())!=0){std::remove(temporary.c_str());return fail(error,"Cannot publish city scene index");}
  return true;
}

bool readCityVoxelSceneIndex(const std::string &path, const CityVoxelSceneLimits &limits, CityVoxelSceneIndex &index, std::string *error)
{
  std::ifstream file(path,std::ios::binary|std::ios::ate);
  if(!file)return fail(error,"Cannot open city scene index");
  const auto end=file.tellg();
  if(end<0 || uint64_t(end)<kHeaderBytes || uint64_t(end)>limits.maximumIndexBytes)
    return fail(error,"City scene index file is truncated or oversized");
  // Preflight fixed-size metadata before allocating the section buffer. This
  // matters for malformed files claiming millions of records or huge offsets.
  std::array<uint8_t, kHeaderBytes> header{};
  file.seekg(0);
  file.read(reinterpret_cast<char *>(header.data()),header.size());
  if(!file)return fail(error,"Cannot read city scene header");
  for(unsigned i=0;i<8;++i)if(header[i]!=kMagic[i])return fail(error,"Wrong city scene magic");
  if(get(header.data(),8,2)!=kCityVoxelSceneMajor || get(header.data(),10,2)>kCityVoxelSceneMinor ||
     get(header.data(),12,4)!=kEndian || get(header.data(),16,4)!=kHeaderBytes ||
     get(header.data(),20,4)!=kSectionCount || get(header.data(),24,8)!=uint64_t(end))
    return fail(error,"Invalid or unsupported city scene header");
  const uint32_t caps[] = {limits.maximumGeometry,limits.maximumAppearances,limits.maximumInstances,
                           limits.maximumSpatialCells,limits.maximumPages,limits.maximumDependencies,limits.maximumMaterials};
  uint64_t next=kHeaderBytes;
  for(unsigned s=0;s<kSectionCount;++s)
  {
    const auto at=96+s*24;
    const uint64_t offset=get(header.data(),at,8);
    const uint64_t count=get(header.data(),at+8,4);
    const uint64_t stride=get(header.data(),at+12,4);
    if(offset!=next || stride!=kStrides[s] || count>caps[s] ||
       count>(uint64_t(end)-next)/stride)
      return fail(error,"Invalid city scene section directory");
    next+=count*stride;
  }
  if(next!=uint64_t(end))return fail(error,"Invalid city scene index length");
  std::vector<uint8_t> bytes(static_cast<size_t>(end));
  file.seekg(0);file.read(reinterpret_cast<char *>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
  if(!file)return fail(error,"Cannot read city scene index");
  return deserializeCityVoxelSceneIndex(bytes.data(),bytes.size(),limits,index,error);
}

} // namespace voxel::svraster
