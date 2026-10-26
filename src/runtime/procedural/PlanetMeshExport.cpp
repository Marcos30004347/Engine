#include "procedural/PlanetMeshExport.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>

namespace procedural
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

void writeU32(std::ofstream &file, const uint32_t value)
{
  const uint8_t bytes[4]{
    static_cast<uint8_t>(value),
    static_cast<uint8_t>(value >> 8u),
    static_cast<uint8_t>(value >> 16u),
    static_cast<uint8_t>(value >> 24u)};
  file.write(reinterpret_cast<const char *>(bytes), sizeof(bytes));
}

void writeF32(std::ofstream &file, const float value)
{
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  writeU32(file, bits);
}

bool validMesh(
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    std::string *error)
{
  const size_t regionCount = maps.regionNodes.size();
  const size_t vertexCount = maps.surfacePositions.size();
  if (regionCount == 0u || vertexCount <= regionCount ||
      vertexCount > std::numeric_limits<uint32_t>::max() ||
      maps.surfaceElevations.size() != vertexCount ||
      maps.surfaceContinents.size() != vertexCount ||
      maps.surfaceTectonics.size() != vertexCount ||
      maps.surfaceMountains.size() != vertexCount ||
      maps.surfaceTemperature.size() != vertexCount ||
      maps.surfacePrecipitation.size() != vertexCount ||
      maps.surfacePlates.size() != vertexCount ||
      maps.surfaceRivers.size() != vertexCount ||
      maps.surfaceMaterials.size() != vertexCount)
    return fail(error, "Planet mesh attributes are missing or inconsistent");
  const size_t sourceTriangleCount = vertexCount - regionCount;
  if (sourceTriangleCount > std::numeric_limits<uint32_t>::max() ||
      sourceTriangleCount > std::numeric_limits<size_t>::max() / 9u ||
      maps.surfaceIndices.size() != sourceTriangleCount * 9u)
    return fail(error, "Planet mesh topology is inconsistent");
  if (!(metadata.planetRadiusMetres > 0.0) ||
      !std::isfinite(metadata.planetRadiusMetres) ||
      !(metadata.heightScaleMetres > 0.0) ||
      !std::isfinite(metadata.heightScaleMetres) ||
      !std::isfinite(metadata.heightBiasMetres))
    return fail(error, "Planet mesh physical metadata is invalid");
  for (const SpherePoint &direction : maps.surfacePositions)
  {
    const double lengthSquared = direction.x * direction.x +
        direction.y * direction.y + direction.z * direction.z;
    if (!std::isfinite(lengthSquared) ||
        std::abs(lengthSquared - 1.0) > 1.0e-5)
      return fail(error, "Planet mesh contains a non-unit direction");
  }
  for (size_t face = 0u; face < maps.surfaceIndices.size() / 3u; ++face)
  {
    const uint32_t a = maps.surfaceIndices[face * 3u];
    const uint32_t b = maps.surfaceIndices[face * 3u + 1u];
    const uint32_t c = maps.surfaceIndices[face * 3u + 2u];
    if (a >= regionCount || b >= vertexCount || c >= vertexCount)
      return fail(error, "Planet mesh contains an invalid face mapping");
  }
  return true;
}

} // namespace

bool writeSphericalTerrainMeshPly(
    const std::filesystem::path &path,
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    PlanetMeshExportStatistics *statistics,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (path.empty() || !validMesh(maps, metadata, error))
    return path.empty() ? fail(error, "Planet mesh path is empty") : false;

  const uint32_t regionCount =
      static_cast<uint32_t>(maps.regionNodes.size());
  const uint32_t vertexCount =
      static_cast<uint32_t>(maps.surfacePositions.size());
  const uint32_t sourceTriangleCount = vertexCount - regionCount;
  const uint32_t faceCount =
      static_cast<uint32_t>(maps.surfaceIndices.size() / 3u);
  PlanetMeshExportStatistics result{
    regionCount, sourceTriangleCount, vertexCount, faceCount,
    std::numeric_limits<double>::infinity(),
    -std::numeric_limits<double>::infinity(), 0u};

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file)
    return fail(error, "Could not create planet mesh PLY");
  file << "ply\n"
       << "format binary_little_endian 1.0\n"
       << "comment coordinates planet-centred XYZ kilometres\n"
       << "comment source_kind 0=Voronoi-region 1=Delaunay-triangle\n"
       << "comment winding counter-clockwise when viewed from space\n"
       << "element vertex " << vertexCount << "\n"
       << "property float x\nproperty float y\nproperty float z\n"
       << "property float elevation_metres\n"
       << "property uint source_index\nproperty uchar source_kind\n"
       << "property uchar material\nproperty uchar plate\n"
       << "property uchar continent\nproperty uchar tectonics\n"
       << "property uchar mountain\nproperty uchar temperature\n"
       << "property uchar precipitation\nproperty uchar river\n"
       << "element face " << faceCount << "\n"
       << "property list uchar uint vertex_indices\n"
       << "property uint region_id\n"
       << "property uint source_triangle_id\n"
       << "end_header\n";

  for (uint32_t vertex = 0u; vertex < vertexCount; ++vertex)
  {
    const double elevation = metadata.heightBiasMetres +
        maps.surfaceElevations[vertex] * metadata.heightScaleMetres;
    result.minimumElevationMetres =
        std::min(result.minimumElevationMetres, elevation);
    result.maximumElevationMetres =
        std::max(result.maximumElevationMetres, elevation);
    const double radiusKilometres =
        (metadata.planetRadiusMetres + elevation) / 1000.0;
    const SpherePoint &direction = maps.surfacePositions[vertex];
    writeF32(file, static_cast<float>(direction.x * radiusKilometres));
    writeF32(file, static_cast<float>(direction.y * radiusKilometres));
    writeF32(file, static_cast<float>(direction.z * radiusKilometres));
    writeF32(file, static_cast<float>(elevation));
    writeU32(file, vertex < regionCount ? vertex : vertex - regionCount);
    file.put(static_cast<char>(vertex < regionCount ? 0u : 1u));
    file.put(static_cast<char>(maps.surfaceMaterials[vertex]));
    file.put(static_cast<char>(maps.surfacePlates[vertex]));
    file.put(static_cast<char>(maps.surfaceContinents[vertex]));
    file.put(static_cast<char>(maps.surfaceTectonics[vertex]));
    file.put(static_cast<char>(maps.surfaceMountains[vertex]));
    file.put(static_cast<char>(maps.surfaceTemperature[vertex]));
    file.put(static_cast<char>(maps.surfacePrecipitation[vertex]));
    file.put(static_cast<char>(maps.surfaceRivers[vertex]));
  }
  for (uint32_t face = 0u; face < faceCount; ++face)
  {
    file.put(static_cast<char>(3u));
    writeU32(file, maps.surfaceIndices[face * 3u]);
    writeU32(file, maps.surfaceIndices[face * 3u + 1u]);
    writeU32(file, maps.surfaceIndices[face * 3u + 2u]);
    writeU32(file, maps.surfaceIndices[face * 3u]);
    writeU32(file, face / 3u);
  }
  const std::streampos end = file.tellp();
  file.flush();
  if (!file || end < 0)
    return fail(error, "Could not finalize planet mesh PLY");
  result.fileBytes = static_cast<uint64_t>(end);
  if (statistics != nullptr)
    *statistics = result;
  return true;
}

bool writePlanetMetadataManifest(
    const std::filesystem::path &path,
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    const PlanetMeshExportStatistics &mesh,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (path.empty() || maps.regionNodes.size() != mesh.regionCount ||
      maps.surfacePositions.size() != mesh.vertexCount)
    return fail(error, "Planet metadata manifest inputs are inconsistent");
  std::ofstream file(path, std::ios::trunc);
  if (!file)
    return fail(error, "Could not create planet metadata manifest");
  file << std::setprecision(17)
       << "{\n"
       << "  \"format\": \"engine-planet-metadata\",\n"
       << "  \"version\": 1,\n"
       << "  \"package\": \"planet.planetpkg\",\n"
       << "  \"mesh\": \"planet-mesh.ply\",\n"
       << "  \"coordinateSystem\": \"planet-centred right-handed XYZ\",\n"
       << "  \"meshCoordinateUnit\": \"kilometres\",\n"
       << "  \"planetRadiusMetres\": " << metadata.planetRadiusMetres << ",\n"
       << "  \"planetRadiusKilometres\": "
       << planetRadiusKilometres(metadata) << ",\n"
       << "  \"planetDiameterKilometres\": "
       << planetDiameterKilometres(metadata) << ",\n"
       << "  \"equatorialCircumferenceKilometres\": "
       << planetCircumferenceKilometres(metadata) << ",\n"
       << "  \"nominalSurfaceAreaSquareKilometres\": "
       << planetSurfaceAreaSquareKilometres(metadata) << ",\n"
       << "  \"heightScaleMetres\": " << metadata.heightScaleMetres << ",\n"
       << "  \"heightBiasMetres\": " << metadata.heightBiasMetres << ",\n"
       << "  \"minimumMeshElevationMetres\": "
       << mesh.minimumElevationMetres << ",\n"
       << "  \"maximumMeshElevationMetres\": "
       << mesh.maximumElevationMetres << ",\n"
       << "  \"regionCount\": " << mesh.regionCount << ",\n"
       << "  \"sourceTriangleCount\": " << mesh.sourceTriangleCount << ",\n"
       << "  \"meshVertexCount\": " << mesh.vertexCount << ",\n"
       << "  \"meshFaceCount\": " << mesh.faceCount << ",\n"
       << "  \"meshFileBytes\": " << mesh.fileBytes << ",\n"
       << "  \"meshReconstruction\": "
          "\"positionMetres = surfaceDirection * "
          "(planetRadiusMetres + decodedSurfaceElevationMetres)\",\n"
       << "  \"graphChannels\": {\n"
       << "    \"surfaceDirections\": "
       << static_cast<uint32_t>(PlanetGraphChannel::SurfaceDirections) << ",\n"
       << "    \"surfaceElevation\": "
       << static_cast<uint32_t>(PlanetGraphChannel::SurfaceElevation) << ",\n"
       << "    \"surfaceIndices\": "
       << static_cast<uint32_t>(PlanetGraphChannel::SurfaceIndices) << ",\n"
       << "    \"surfaceFaceRegion\": "
       << static_cast<uint32_t>(PlanetGraphChannel::SurfaceFaceRegion) << ",\n"
       << "    \"surfaceFaceTriangle\": "
       << static_cast<uint32_t>(PlanetGraphChannel::SurfaceFaceTriangle) << ",\n"
       << "    \"triangleRegions\": "
       << static_cast<uint32_t>(PlanetGraphChannel::TriangleRegions) << "\n"
       << "  }\n"
       << "}\n";
  if (!file)
    return fail(error, "Could not finalize planet metadata manifest");
  return true;
}

} // namespace procedural
