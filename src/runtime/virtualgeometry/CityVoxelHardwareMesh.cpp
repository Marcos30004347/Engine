#include "virtualgeometry/CityVoxelHardwareMesh.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace virtualgeometry {
namespace {
struct CellHash {
  size_t operator()(const std::array<int32_t, 3> &v) const {
    uint64_t h = 14695981039346656037ull;
    for (int32_t x : v) { h ^= uint32_t(x); h *= 1099511628211ull; }
    return size_t(h);
  }
};
bool fail(std::string *e, const char *message) { if (e) *e = message; return false; }
}
bool buildCityVoxelHardwareMesh(const voxel::svraster::CityVoxelCompiledGeometry &geometry,
    CityVoxelHardwareMesh &result, std::string *error) {
  if (error) error->clear();
  std::vector<std::array<int32_t, 3>> cells;
  if (!voxel::svraster::decodeCityVoxelGeometry(geometry, cells, nullptr, error)) return false;
  std::unordered_set<std::array<int32_t, 3>, CellHash> occupied(cells.begin(), cells.end());
  if (occupied.size() != cells.size()) return fail(error, "Duplicate city hardware voxel cell");
  if (cells.size() > (UINT32_MAX >> 3u))
    return fail(error, "City hardware primitive address limit exceeded");
  CityVoxelHardwareMesh mesh;
  mesh.cells = std::move(cells);
  // Ray traversal needs only positions; the face address supplies the normal
  // and exact material cell. Share every lattice corner across neighbouring
  // faces instead of writing four duplicate vertices per exposed voxel face.
  std::unordered_map<std::array<int32_t, 3>, uint32_t, CellHash> vertexByCorner;
  const float pitch = float(double(geometry.pitchMicrometres) * 1.0e-6);
  // Face corners are wound outward. An internal face is never sent to BLAS.
  constexpr int faces[6][4][3] = {
    {{0,0,0},{0,0,1},{0,1,1},{0,1,0}},
    {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},
    {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
    {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},
    {{0,0,0},{0,1,0},{1,1,0},{1,0,0}},
    {{0,0,1},{1,0,1},{1,1,1},{0,1,1}}
  };
  constexpr int neighbor[6][3] = {{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
  for (uint32_t cellIndex=0;cellIndex<mesh.cells.size();++cellIndex) for (unsigned face = 0; face < 6; ++face) {
    const auto &cell=mesh.cells[cellIndex];
    const std::array<int32_t, 3> adjacent = {cell[0]+neighbor[face][0],
        cell[1]+neighbor[face][1],cell[2]+neighbor[face][2]};
    if (occupied.count(adjacent)) continue;
    uint32_t corners[4]{};
    for (unsigned corner = 0; corner < 4; ++corner) {
      const std::array<int32_t,3> point{
          cell[0]+faces[face][corner][0],
          cell[1]+faces[face][corner][1],
          cell[2]+faces[face][corner][2]};
      const auto found=vertexByCorner.find(point);
      if (found!=vertexByCorner.end()) { corners[corner]=found->second; continue; }
      if (mesh.positions.size()/3 >= UINT32_MAX)
        return fail(error, "City hardware BLAS vertex limit exceeded");
      corners[corner]=uint32_t(mesh.positions.size()/3);
      vertexByCorner.emplace(point,corners[corner]);
      for (unsigned axis = 0; axis < 3; ++axis)
        mesh.positions.push_back(float(point[axis])*pitch);
    }
    for (uint32_t local : {0u,1u,2u,0u,2u,3u}) mesh.indices.push_back(corners[local]);
    mesh.triangleAddress.push_back((cellIndex<<3u)|face);
    mesh.triangleAddress.push_back((cellIndex<<3u)|face);
  }
  result = std::move(mesh);
  return true;
}

bool splitCityVoxelHardwareMesh(const CityVoxelHardwareMesh &source,
    uint32_t maximumTriangles, std::vector<CityVoxelHardwareMesh> &result,
    std::string *error) {
  if (error) error->clear();
  if (maximumTriangles<2u || (maximumTriangles&1u) ||
      source.indices.size()!=source.triangleAddress.size()*3ull ||
      source.triangleAddress.empty() || (source.triangleAddress.size()&1u) ||
      source.positions.size()%3u || source.positions.size()/3u>UINT32_MAX)
    return fail(error,"Invalid city trace group input or triangle limit");
  const uint32_t vertexCount=uint32_t(source.positions.size()/3u);
  std::vector<CityVoxelHardwareMesh> groups;
  groups.reserve((source.triangleAddress.size()+maximumTriangles-1u)/maximumTriangles);
  for (size_t first=0;first<source.triangleAddress.size();first+=maximumTriangles) {
    const size_t end=std::min(source.triangleAddress.size(),first+maximumTriangles);
    CityVoxelHardwareMesh group;
    std::unordered_map<uint32_t,uint32_t> vertices, cells;
    for (size_t triangle=first;triangle<end;++triangle) {
      const uint32_t address=source.triangleAddress[triangle];
      if ((triangle&1u) && address!=source.triangleAddress[triangle-1u])
        return fail(error,"City trace group split a voxel face");
      const uint32_t sourceCell=address>>3u;
      if (sourceCell>=source.cells.size() || (address&7u)>=6u)
        return fail(error,"City trace group source address is invalid");
      auto [cell,inserted]=cells.emplace(sourceCell,uint32_t(group.cells.size()));
      if (inserted) group.cells.push_back(source.cells[sourceCell]);
      group.triangleAddress.push_back((cell->second<<3u)|(address&7u));
      for (unsigned corner=0;corner<3;++corner) {
        const uint32_t sourceVertex=source.indices[triangle*3u+corner];
        if (sourceVertex>=vertexCount)
          return fail(error,"City trace group source index is invalid");
        auto [vertex,newVertex]=vertices.emplace(sourceVertex,uint32_t(group.positions.size()/3u));
        if (newVertex) for(unsigned axis=0;axis<3;++axis)
          group.positions.push_back(source.positions[sourceVertex*3u+axis]);
        group.indices.push_back(vertex->second);
      }
    }
    groups.push_back(std::move(group));
  }
  result=std::move(groups);
  return true;
}

bool serializeCityVoxelHardwareMesh(uint64_t geometryId, uint64_t geometryHash,
    const CityVoxelHardwareMesh &mesh, std::vector<uint8_t> &result, std::string *error) {
  if (error) error->clear();
  const uint64_t cellCount=mesh.cells.size(), vertexCount=mesh.positions.size()/3,
      indexCount=mesh.indices.size(), triangleCount=mesh.triangleAddress.size();
  if (!geometryId || !geometryHash || !cellCount || !vertexCount ||
      mesh.positions.size()%3 || indexCount!=triangleCount*3 ||
      cellCount>(UINT32_MAX>>3u) || vertexCount>UINT32_MAX ||
      indexCount>UINT32_MAX || triangleCount>UINT32_MAX)
    return fail(error,"Invalid city hardware trace page counts");
  const uint64_t bytes=40ull+cellCount*12ull+vertexCount*12ull+indexCount*4ull+triangleCount*4ull;
  if (bytes>UINT32_MAX) return fail(error,"City hardware trace page is too large");
  std::vector<uint8_t> out;
  out.reserve(size_t(bytes));
  auto put=[&](uint64_t value,unsigned width) {
    for (unsigned i=0;i<width;++i) out.push_back(uint8_t(value>>(i*8)));
  };
  put(0x54485643u,4); put(1,4); put(geometryId,8); put(geometryHash,8);
  put(cellCount,4); put(vertexCount,4); put(indexCount,4); put(triangleCount,4);
  for (const auto &cell:mesh.cells) for (int32_t coordinate:cell) put(uint32_t(coordinate),4);
  for (float position:mesh.positions) {
    uint32_t bits; std::memcpy(&bits,&position,4); put(bits,4);
  }
  for (uint32_t index:mesh.indices) {
    if (index>=vertexCount) return fail(error,"City hardware trace index exceeds vertex count");
    put(index,4);
  }
  for (uint32_t address:mesh.triangleAddress) {
    if ((address>>3u)>=cellCount || (address&7u)>=6u)
      return fail(error,"City hardware trace primitive address is invalid");
    put(address,4);
  }
  result=std::move(out);
  return true;
}

bool deserializeCityVoxelHardwareMesh(const uint8_t *data, size_t bytes,
    uint64_t geometryId, uint64_t geometryHash, CityVoxelHardwareMesh &result,
    std::string *error) {
  if (error) error->clear();
  if (!data || bytes<40) return fail(error,"Truncated city hardware trace page");
  auto get=[&](size_t at,unsigned width) {
    uint64_t value=0; for (unsigned i=0;i<width;++i) value|=uint64_t(data[at+i])<<(i*8);
    return value;
  };
  if (get(0,4)!=0x54485643u || get(4,4)!=1 ||
      get(8,8)!=geometryId || get(16,8)!=geometryHash)
    return fail(error,"City hardware trace page identity or version differs");
  const uint64_t cellCount=get(24,4),vertexCount=get(28,4),
      indexCount=get(32,4),triangleCount=get(36,4);
  if (!cellCount || !vertexCount || cellCount>(UINT32_MAX>>3u) ||
      indexCount!=triangleCount*3 ||
      40ull+cellCount*12ull+vertexCount*12ull+indexCount*4ull+triangleCount*4ull!=bytes)
    return fail(error,"City hardware trace page length is invalid");
  CityVoxelHardwareMesh mesh;
  size_t at=40;
  mesh.cells.reserve(size_t(cellCount));
  for (uint64_t i=0;i<cellCount;++i) {
    std::array<int32_t,3> cell{};
    for (unsigned axis=0;axis<3;++axis) { cell[axis]=int32_t(uint32_t(get(at,4))); at+=4; }
    mesh.cells.push_back(cell);
  }
  mesh.positions.reserve(size_t(vertexCount)*3);
  for (uint64_t i=0;i<vertexCount*3;++i) {
    const uint32_t bits=uint32_t(get(at,4)); at+=4;
    float position; std::memcpy(&position,&bits,4);
    if (!std::isfinite(position)) return fail(error,"Nonfinite city hardware trace vertex");
    mesh.positions.push_back(position);
  }
  mesh.indices.reserve(size_t(indexCount));
  for (uint64_t i=0;i<indexCount;++i) {
    const uint32_t index=uint32_t(get(at,4)); at+=4;
    if (index>=vertexCount) return fail(error,"City hardware trace index exceeds vertex count");
    mesh.indices.push_back(index);
  }
  mesh.triangleAddress.reserve(size_t(triangleCount));
  for (uint64_t i=0;i<triangleCount;++i) {
    const uint32_t address=uint32_t(get(at,4)); at+=4;
    if ((address>>3u)>=cellCount || (address&7u)>=6u)
      return fail(error,"City hardware trace primitive address is invalid");
    mesh.triangleAddress.push_back(address);
  }
  result=std::move(mesh);
  return true;
}
} // namespace virtualgeometry
