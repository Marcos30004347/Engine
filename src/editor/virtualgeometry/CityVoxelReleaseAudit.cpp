#include "editor/virtualgeometry/CityVoxelReleaseAudit.hpp"
#include "virtualgeometry/CityVoxelHardwareMesh.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include "voxel/svraster/CityVoxelFallback.hpp"
#include "voxel/svraster/CityVoxelVGPage.hpp"
#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace virtualgeometry {
namespace {
bool fail(std::string *error, const std::string &message)
{
  if (error)
  {
    *error = message;
  }
  return false;
}

uint32_t read32(const std::vector<uint8_t> &bytes, size_t at)
{
  return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8u) | (uint32_t(bytes[at + 2]) << 16u) | (uint32_t(bytes[at + 3]) << 24u);
}

uint32_t read32(const uint8_t *bytes, size_t at)
{
  return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8u) | (uint32_t(bytes[at + 2]) << 16u) | (uint32_t(bytes[at + 3]) << 24u);
}

uint64_t hashBytes(const std::vector<uint8_t> &bytes)
{
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t value : bytes)
  {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::array<int64_t, 3> rotate(uint8_t turns, int64_t x, int64_t y, int64_t z)
{
  switch (turns)
  {
  case 1:
    return {-z, y, x};
  case 2:
    return {-x, y, -z};
  case 3:
    return {z, y, -x};
  default:
    return {x, y, z};
  }
}
} // namespace

bool auditCityVoxelSceneForRelease(
    const std::string &path,
    const std::vector<CityVoxelExpectedOwner> &expectedOwners,
    const voxel::svraster::CityVoxelSceneLimits &limits,
    uint32_t maximumDecodedCellsPerAsset,
    CityVoxelReleaseAuditReport &report,
    std::string *error)
{
  using namespace voxel::svraster;
  if (error)
  {
    error->clear();
  }
  if (expectedOwners.empty() || !maximumDecodedCellsPerAsset)
  {
    return fail(error, "City release audit needs an authored owner manifest and cell cap");
  }
  CityVoxelSceneFileReader reader;
  if (!reader.open(path, limits, error))
  {
    return false;
  }
  const auto &index = reader.index();
  CityVoxelReleaseAuditReport next;
  next.instances = index.instances.size();
  next.geometry = index.geometry.size();
  next.appearances = index.appearances.size();
  std::unordered_set<uint64_t> expected, actual, profiles;
  for (const auto &owner : expectedOwners)
  {
    if (!owner.id || owner.category.empty() || !expected.insert(owner.id).second)
    {
      return fail(error, "City owner manifest has zero, duplicate or uncategorized owner");
    }
    ++next.expectedByCategory[owner.category];
  }
  for (const auto &material : index.materials)
  {
    profiles.insert(material.profileId);
  }
  for (const auto &instance : index.instances)
  {
    actual.insert(instance.ownerId);
  }
  next.owners = actual.size();
  for (uint64_t owner : expected)
  {
    if (!actual.count(owner))
    {
      next.missingOwnerIds.push_back(owner);
    }
  }
  for (uint64_t owner : actual)
  {
    if (!expected.count(owner))
    {
      next.unexpectedOwnerIds.push_back(owner);
    }
  }
  for (const auto &owner : expectedOwners)
  {
    ++(actual.count(owner.id) ? next.coveredByCategory : next.missingByCategory)[owner.category];
  }
  std::sort(next.missingOwnerIds.begin(), next.missingOwnerIds.end());
  std::sort(next.unexpectedOwnerIds.begin(), next.unexpectedOwnerIds.end());
  next.missingOwners = next.missingOwnerIds.size();
  next.unexpectedOwners = next.unexpectedOwnerIds.size();
  std::unordered_map<uint64_t, const CityVoxelGeometryAsset *> geometryById;
  std::unordered_map<uint64_t, const CityVoxelAppearanceAsset *> appearanceById;
  std::unordered_map<uint64_t, uint32_t> occupiedByGeometry;
  std::unordered_set<uint64_t> tracedGeometry, vgGeometry;
  for (const auto &geometry : index.geometry)
  {
    if (geometry.pitchMicrometres != index.nominalPitchMicrometres)
    {
      return fail(error, "City release has a mixed pitch without a frame transform");
    }
    geometryById.emplace(geometry.id, &geometry);
  }
  for (const auto &appearance : index.appearances)
  {
    appearanceById.emplace(appearance.id, &appearance);
  }
  std::vector<uint8_t> bytes;
  for (uint32_t page = 0; page < index.pages.size(); ++page)
  {
    if (!reader.readPage(page, bytes, error))
    {
      return false;
    }
    ++next.checkedPages;
    const auto &record = index.pages[page];
    if (record.kind == CityVoxelPageKind::Geometry)
    {
      if (bytes.size() < 28u || (bytes.size() - 28u) % 76u || read32(bytes, 0) != 0x31475643u || 28ull + uint64_t(read32(bytes, 24)) * 76ull != bytes.size())
      {
        return fail(error, "City release geometry stream is malformed");
      }
      const auto found = geometryById.find(record.assetId);
      if (found == geometryById.end() || read32(bytes, 4) != found->second->pitchMicrometres || hashBytes(bytes) != found->second->contentHash)
      {
        return fail(error, "City release geometry identity differs from index");
      }
      uint64_t occupied = 0;
      for (uint32_t brick = 0; brick < read32(bytes, 24); ++brick)
      {
        for (uint32_t word = 0; word < 16u; ++word)
        {
          occupied += __builtin_popcount(read32(bytes, 28ull + uint64_t(brick) * 76ull + 12ull + word * 4ull));
        }
      }
      if (!occupied || occupied > maximumDecodedCellsPerAsset || occupied != read32(bytes, 20))
      {
        return fail(error, "City release geometry occupancy exceeds cap or differs from header");
      }
      occupiedByGeometry[record.assetId] = uint32_t(occupied);
      next.occupiedCells += occupied;
    }
    else if (record.kind == CityVoxelPageKind::Appearance)
    {
      const auto found = appearanceById.find(record.assetId);
      if (found == appearanceById.end() || !occupiedByGeometry.count(found->second->geometryId))
      {
        return fail(error, "City release appearance is missing geometry");
      }
      const auto *appearance = found->second;
      CityVoxelCompiledAppearance compiled;
      compiled.id = appearance->id;
      compiled.contentHash = appearance->contentHash;
      compiled.geometryAddressHash = appearance->geometryAddressHash;
      compiled.geometryId = appearance->geometryId;
      compiled.codec = appearance->codec;
      compiled.occupiedCount = occupiedByGeometry.at(appearance->geometryId);
      compiled.bytes = bytes;
      std::vector<CityVoxelCell> values;
      if (!decodeCityVoxelAppearance(compiled, values, error))
      {
        return false;
      }
      for (const auto &value : values)
      {
        next.unknownMaterialProfiles += !profiles.count(value.materialProfile);
        next.nonOpaqueVoxels += (value.rgba >> 24u) != 255u;
      }
    }
    else if (record.kind == CityVoxelPageKind::Trace)
    {
      const auto found = geometryById.find(record.assetId);
      if (found == geometryById.end())
      {
        return fail(error, "City release trace has no geometry");
      }
      CityVoxelHardwareMesh mesh;
      if (!deserializeCityVoxelHardwareMesh(bytes.data(), bytes.size(), found->second->id, found->second->contentHash, mesh, error))
      {
        return false;
      }
      tracedGeometry.insert(record.assetId);
    }
    else if (record.kind == CityVoxelPageKind::VirtualGeometry)
    {
      const auto found = geometryById.find(record.assetId);
      if (found == geometryById.end())
      {
        return fail(error, "City release VG has no geometry");
      }
      CityVoxelVGPageView view;
      if (!decodeCityVoxelVGPage(bytes.data(), bytes.size(), found->second->id, found->second->contentHash, view, error))
      {
        return false;
      }
      if (view.clusterSources.empty() || view.vgFileSize < 8u || read32(view.vgFileBytes, 0) != VMESH_MAGIC || read32(view.vgFileBytes, 4) != VMESH_VERSION)
      {
        return fail(error, "City release VG file header is invalid");
      }
      vgGeometry.insert(record.assetId);
    }
    else if (record.kind == CityVoxelPageKind::Fallback)
    {
      CityVoxelFallbackProxy proxy;
      if (!decodeCityVoxelFallback(bytes.data(), bytes.size(), proxy, error))
      {
        return false;
      }
    }
  }
  for (const auto &cell : index.spatialCells)
  {
    if (!cell.instanceCount)
    {
      continue;
    }
    if (cell.fallbackPage == kCityVoxelInvalidPage)
    {
      return fail(error, "City release cell has no fallback page");
    }
    ++next.fallbackCells;
    std::array<int64_t, 3> minimum{INT64_MAX, INT64_MAX, INT64_MAX};
    std::array<int64_t, 3> maximum{INT64_MIN, INT64_MIN, INT64_MIN};
    for (uint32_t i = cell.firstInstance; i < cell.firstInstance + cell.instanceCount; ++i)
    {
      const auto &instance = index.instances[i];
      const auto found = geometryById.find(instance.geometryId);
      if (found == geometryById.end())
      {
        return fail(error, "City release instance geometry is missing");
      }
      const auto &geometry = *found->second;
      for (unsigned corner = 0; corner < 8; ++corner)
      {
        if(instance.flags & kCityVoxelInstanceLocalFrame)
        {
          constexpr double tau=6.2831853071795864769;
          const double angle=tau*double(instance.frameId)/65536.0;
          const double c=std::cos(angle),s=std::sin(angle);
          const double pitch=double(index.nominalPitchMicrometres)*0.001;
          const double x=double(instance.gridPosition[0])+((corner&1u)?geometry.maximumCell[0]:geometry.minimumCell[0]);
          const double y=double(instance.gridPosition[1])+((corner&2u)?geometry.maximumCell[1]:geometry.minimumCell[1]);
          const double z=double(instance.gridPosition[2])+((corner&4u)?geometry.maximumCell[2]:geometry.minimumCell[2]);
          const double values[3]={
              (double(cell.originMillimetres[0])+pitch*(c*x-s*z))/pitch,
              (double(cell.originMillimetres[1])+pitch*y)/pitch,
              (double(cell.originMillimetres[2])+pitch*(s*x+c*z))/pitch};
          for(unsigned axis=0;axis<3;++axis) {
            minimum[axis]=std::min(minimum[axis],int64_t(std::floor(values[axis])));
            maximum[axis]=std::max(maximum[axis],int64_t(std::ceil(values[axis])));
          }
          continue;
        }
        const auto point = rotate(
            instance.quarterTurn,
            (corner & 1u) ? geometry.maximumCell[0] : geometry.minimumCell[0],
            (corner & 2u) ? geometry.maximumCell[1] : geometry.minimumCell[1],
            (corner & 4u) ? geometry.maximumCell[2] : geometry.minimumCell[2]);
        for (unsigned axis = 0; axis < 3; ++axis)
        {
          const int64_t world = int64_t(instance.gridPosition[axis]) + point[axis];
          minimum[axis] = std::min(minimum[axis], world);
          maximum[axis] = std::max(maximum[axis], world);
        }
      }
    }
    if (!reader.readPage(cell.fallbackPage, bytes, error))
    {
      return false;
    }
    CityVoxelFallbackProxy proxy;
    if (!decodeCityVoxelFallback(bytes.data(), bytes.size(), proxy, error) || proxy.instanceCount != cell.instanceCount)
    {
      return fail(error, "City release fallback instance count differs");
    }
    for (unsigned axis = 0; axis < 3; ++axis)
    {
      if (proxy.minimumCell[axis] > minimum[axis] || proxy.maximumCell[axis] < maximum[axis])
      {
        return fail(error, "City release fallback does not enclose detailed geometry");
      }
    }
  }
  next.traceAssets = tracedGeometry.size();
  next.vgAssets = vgGeometry.size();
  if (next.traceAssets != index.geometry.size() || next.vgAssets != index.geometry.size())
  {
    return fail(error, "City release has missing trace or VG assets");
  }
  // Verify the independently stored ray groups form exactly the LOD0 surface
  // of each shared geometry. This catches duplicate or missing boundary faces
  // even when every individual group page is well formed.
  struct FaceKey {
    std::array<int32_t,3> cell{};
    uint32_t face=0;
    bool operator<(const FaceKey &other) const {
      return std::tie(cell,face)<std::tie(other.cell,other.face);
    }
    bool operator==(const FaceKey &other) const {
      return cell==other.cell && face==other.face;
    }
  };
  auto faces=[](const CityVoxelHardwareMesh &mesh,std::vector<FaceKey> &out) {
    if (mesh.triangleAddress.size()&1u) return false;
    for (size_t t=0;t<mesh.triangleAddress.size();t+=2u) {
      const uint32_t address=mesh.triangleAddress[t];
      if (mesh.triangleAddress[t+1u]!=address ||
          (address>>3u)>=mesh.cells.size() || (address&7u)>=6u)
        return false;
      out.push_back({mesh.cells[address>>3u],address&7u});
    }
    return true;
  };
  std::unordered_map<uint64_t,std::vector<uint32_t>> tracePages;
  for(uint32_t page=0;page<index.pages.size();++page)
    if(index.pages[page].kind==CityVoxelPageKind::Trace)
      tracePages[index.pages[page].assetId].push_back(page);
  for (const auto &record:index.geometry) {
    if (record.pageCount!=1u || !reader.readPage(record.firstPage,bytes,error))
      return fail(error,"City release geometry page layout is unsupported");
    CityVoxelCompiledGeometry compiled;
    compiled.id=record.id;compiled.contentHash=record.contentHash;
    compiled.addressHash=record.voxelAddressHash;
    compiled.pitchMicrometres=record.pitchMicrometres;
    compiled.occupiedCount=read32(bytes,20u);
    compiled.bytes=std::move(bytes);
    for (unsigned axis=0;axis<3;++axis)
      compiled.extent[axis]=record.maximumCell[axis]-record.minimumCell[axis];
    CityVoxelHardwareMesh source;
    if (!buildCityVoxelHardwareMesh(compiled,source,error)) return false;
    std::vector<FaceKey> expected,actual;
    expected.reserve(source.triangleAddress.size()/2u);
    if (!faces(source,expected))
      return fail(error,"City release source trace faces are malformed");
    source={};compiled.bytes.clear();
    for (uint32_t page:tracePages[record.id]) {
      if(!reader.readPage(page,bytes,error))return false;
      CityVoxelHardwareMesh group;
      if(!deserializeCityVoxelHardwareMesh(bytes.data(),bytes.size(),
          record.id,record.contentHash,group,error) || !faces(group,actual))
        return fail(error,"City release trace group faces are malformed");
    }
    std::sort(expected.begin(),expected.end());
    std::sort(actual.begin(),actual.end());
    if(actual!=expected)
      return fail(error,"City release trace groups have missing or duplicate voxel faces");
  }
  report = std::move(next);
  if (report.missingOwners || report.unexpectedOwners || report.unknownMaterialProfiles || report.nonOpaqueVoxels)
  {
    return fail(error, "City release owner, material or opacity coverage failed");
  }
  return true;
}
} // namespace virtualgeometry
