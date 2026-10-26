#include "editor/voxel/SparseVoxelOBJAnalysis.hpp"

#include "tiny_obj_loader/tiny_obj_loader.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

namespace voxel::svraster
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool cancelled(const std::atomic_bool *value)
{
  return value != nullptr && value->load(std::memory_order_relaxed);
}

bool checkedMultiply(uint64_t a, uint64_t b, uint64_t &result)
{
  if (a != 0u && b > std::numeric_limits<uint64_t>::max() / a)
    return false;
  result = a * b;
  return true;
}

uint64_t contentHash(const std::filesystem::path &path, bool &read)
{
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
  {
    read = false;
    return 0u;
  }

  constexpr uint64_t offset = 14695981039346656037ull;
  constexpr uint64_t prime = 1099511628211ull;
  uint64_t hash = offset;
  char buffer[64u * 1024u];
  while (stream)
  {
    stream.read(buffer, sizeof(buffer));
    const std::streamsize count = stream.gcount();
    for (std::streamsize index = 0; index < count; ++index)
    {
      hash ^= static_cast<uint8_t>(buffer[index]);
      hash *= prime;
    }
  }
  read = !stream.bad();
  return hash;
}

void appendHash(uint64_t &hash, const void *data, const size_t size)
{
  constexpr uint64_t prime = 1099511628211ull;
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0u; index < size; ++index)
  {
    hash ^= bytes[index];
    hash *= prime;
  }
}

void appendStringHash(uint64_t &hash, const std::string &value)
{
  const uint64_t size = value.size();
  appendHash(hash, &size, sizeof(size));
  appendHash(hash, value.data(), value.size());
}

uint64_t appendMaterialSemantics(uint64_t hash, const std::vector<tinyobj::material_t> &materials)
{
  const uint64_t count = materials.size();
  appendHash(hash, &count, sizeof(count));
  for (const tinyobj::material_t &material : materials)
  {
    appendStringHash(hash, material.name);
    appendHash(hash, material.diffuse, sizeof(material.diffuse));
    appendHash(hash, material.emission, sizeof(material.emission));
    appendHash(hash, &material.dissolve, sizeof(material.dissolve));
    appendHash(hash, &material.roughness, sizeof(material.roughness));
    appendHash(hash, &material.metallic, sizeof(material.metallic));
    appendStringHash(hash, material.diffuse_texname);
    appendStringHash(hash, material.roughness_texname);
    appendStringHash(hash, material.metallic_texname);
    appendStringHash(hash, material.emissive_texname);
  }
  return hash;
}

void finalizeBounds(SparseVoxelBoundsD &bounds)
{
  double diagonalSquared = 0.0;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.extent[axis] = bounds.max[axis] - bounds.min[axis];
    bounds.center[axis] = bounds.min[axis] + bounds.extent[axis] * 0.5;
    diagonalSquared += bounds.extent[axis] * bounds.extent[axis];
  }
  bounds.diagonal = std::sqrt(diagonalSquared);
}

} // namespace

double metersPerSourceUnit(const SparseVoxelLengthUnit unit, const double customMetersPerSourceUnit)
{
  switch (unit)
  {
  case SparseVoxelLengthUnit::Meters:
    return 1.0;
  case SparseVoxelLengthUnit::Centimeters:
    return 0.01;
  case SparseVoxelLengthUnit::Millimeters:
    return 0.001;
  case SparseVoxelLengthUnit::Inches:
    return 0.0254;
  case SparseVoxelLengthUnit::Feet:
    return 0.3048;
  case SparseVoxelLengthUnit::Custom:
    return customMetersPerSourceUnit;
  }
  return std::numeric_limits<double>::quiet_NaN();
}

const char *sparseVoxelLengthUnitName(const SparseVoxelLengthUnit unit)
{
  switch (unit)
  {
  case SparseVoxelLengthUnit::Meters:
    return "meters";
  case SparseVoxelLengthUnit::Centimeters:
    return "centimeters";
  case SparseVoxelLengthUnit::Millimeters:
    return "millimeters";
  case SparseVoxelLengthUnit::Inches:
    return "inches";
  case SparseVoxelLengthUnit::Feet:
    return "feet";
  case SparseVoxelLengthUnit::Custom:
    return "custom";
  }
  return "invalid";
}

bool analyzeSparseVoxelOBJ(
    const std::filesystem::path &sourcePath,
    SparseVoxelOBJAnalysis &analysis,
    std::string *error,
    const std::atomic_bool *cancelledFlag)
{
  if (error != nullptr)
    error->clear();
  if (cancelled(cancelledFlag))
    return fail(error, "OBJ analysis cancelled");
  if (sourcePath.empty())
    return fail(error, "OBJ source path is empty");

  bool read = false;
  const uint64_t hash = contentHash(sourcePath, read);
  if (!read)
    return fail(error, "Could not read OBJ source file");
  if (cancelled(cancelledFlag))
    return fail(error, "OBJ analysis cancelled");

  tinyobj::ObjReaderConfig config{};
  config.triangulate = true;
  tinyobj::ObjReader reader;
  if (!reader.ParseFromFile(sourcePath.string(), config))
    return fail(error, reader.Error().empty() ? "Could not parse OBJ source file" : reader.Error());

  const tinyobj::attrib_t &attributes = reader.GetAttrib();
  const std::vector<tinyobj::shape_t> &shapes = reader.GetShapes();
  if ((attributes.vertices.size() % 3u) != 0u)
    return fail(error, "OBJ vertex array is malformed");

  SparseVoxelOBJAnalysis next{};
  next.sourcePath = sourcePath;
  next.sourceContentHash = appendMaterialSemantics(hash, reader.GetMaterials());
  next.vertexCount = attributes.vertices.size() / 3u;
  next.materialCount = reader.GetMaterials().size();
  next.warning = reader.Warning();
  const double infinity = std::numeric_limits<double>::infinity();
  next.sourceBounds.min = {infinity, infinity, infinity};
  next.sourceBounds.max = {-infinity, -infinity, -infinity};

  for (const tinyobj::shape_t &shape : shapes)
  {
    if (cancelled(cancelledFlag))
      return fail(error, "OBJ analysis cancelled");
    if (!shape.mesh.num_face_vertices.empty())
      ++next.shapeCount;

    size_t indexOffset = 0u;
    for (const uint8_t faceVertexCount : shape.mesh.num_face_vertices)
    {
      if (faceVertexCount != 3u || indexOffset + 3u > shape.mesh.indices.size())
        return fail(error, "OBJ triangulation produced malformed geometry");

      std::array<std::array<double, 3>, 3> triangle{};
      for (uint32_t corner = 0u; corner < 3u; ++corner)
      {
        const int vertexIndex = shape.mesh.indices[indexOffset + corner].vertex_index;
        if (vertexIndex < 0 || static_cast<uint64_t>(vertexIndex) >= next.vertexCount)
          return fail(error, "OBJ contains an invalid vertex index");
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          const double value = static_cast<double>(attributes.vertices[3u * static_cast<size_t>(vertexIndex) + axis]);
          if (!std::isfinite(value))
            return fail(error, "OBJ contains a non-finite vertex");
          triangle[corner][axis] = value;
          next.sourceBounds.min[axis] = std::min(next.sourceBounds.min[axis], value);
          next.sourceBounds.max[axis] = std::max(next.sourceBounds.max[axis], value);
        }
      }

      const std::array<double, 3> edge0{
        triangle[1][0] - triangle[0][0], triangle[1][1] - triangle[0][1], triangle[1][2] - triangle[0][2]};
      const std::array<double, 3> edge1{
        triangle[2][0] - triangle[0][0], triangle[2][1] - triangle[0][1], triangle[2][2] - triangle[0][2]};
      const std::array<double, 3> cross{
        edge0[1] * edge1[2] - edge0[2] * edge1[1],
        edge0[2] * edge1[0] - edge0[0] * edge1[2],
        edge0[0] * edge1[1] - edge0[1] * edge1[0]};
      const double areaSquared = cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2];
      if (!std::isfinite(areaSquared))
        return fail(error, "OBJ geometry exceeds the supported numeric range");
      if (areaSquared == 0.0)
        ++next.degenerateTriangleCount;
      ++next.triangleCount;
      indexOffset += 3u;
    }
    if (indexOffset != shape.mesh.indices.size())
      return fail(error, "OBJ shape index data is malformed");
  }

  if (next.triangleCount == 0u)
    return fail(error, "OBJ contains no triangles");
  if (next.degenerateTriangleCount == next.triangleCount)
    return fail(error, "OBJ contains no non-degenerate triangles");
  finalizeBounds(next.sourceBounds);
  if (!std::isfinite(next.sourceBounds.diagonal))
    return fail(error, "OBJ bounds exceed the supported numeric range");
  if (cancelled(cancelledFlag))
    return fail(error, "OBJ analysis cancelled");

  analysis = std::move(next);
  return true;
}

bool deriveSparseVoxelOBJGrid(
    const SparseVoxelOBJAnalysis &analysis,
    const SparseVoxelOBJImportSettings &settings,
    SparseVoxelOBJGridEstimate &estimate,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const double scale = metersPerSourceUnit(settings.sourceUnit, settings.metersPerSourceUnit);
  if (!std::isfinite(scale) || scale <= 0.0)
    return fail(error, "Meters per source unit must be finite and greater than zero");
  if (!std::isfinite(settings.voxelSizeMeters) || settings.voxelSizeMeters <= 0.0)
    return fail(error, "Voxel size must be finite and greater than zero meters");
  if (settings.maximumLevel == 0u || settings.maximumLevel > 16u)
    return fail(error, "Maximum octree level must be between 1 and 16");
  if (settings.maximumEstimatedCells == 0u || settings.maximumEstimatedTemporaryBytes == 0u || settings.estimatedBytesPerCell == 0u || settings.targetPageBytes == 0u)
    return fail(error, "OBJ build budgets must be non-zero");
  if (analysis.triangleCount == 0u)
    return fail(error, "OBJ analysis contains no triangles");

  SparseVoxelOBJGridEstimate next{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    next.meterBounds.min[axis] = analysis.sourceBounds.min[axis] * scale;
    next.meterBounds.max[axis] = analysis.sourceBounds.max[axis] * scale;
    if (!std::isfinite(next.meterBounds.min[axis]) || !std::isfinite(next.meterBounds.max[axis]) || next.meterBounds.max[axis] < next.meterBounds.min[axis])
      return fail(error, "Meter-space OBJ bounds are invalid");
    const double minimumCell = std::floor(next.meterBounds.min[axis] / settings.voxelSizeMeters);
    const double maximumCell = std::ceil(next.meterBounds.max[axis] / settings.voxelSizeMeters);
    const double cellSpan = std::max(1.0, maximumCell - minimumCell);
    if (!std::isfinite(minimumCell) || !std::isfinite(maximumCell) || cellSpan > static_cast<double>(uint64_t{1} << 53u))
      return fail(error, "OBJ coordinates cannot be represented reliably at the requested voxel size");
    next.rootMinMeters[axis] = minimumCell * settings.voxelSizeMeters;
    next.occupiedCellsPerAxis[axis] = static_cast<uint64_t>(cellSpan);
  }
  finalizeBounds(next.meterBounds);

  const uint64_t requiredResolution = std::max({
    next.occupiedCellsPerAxis[0], next.occupiedCellsPerAxis[1], next.occupiedCellsPerAxis[2], uint64_t{2}});
  uint64_t resolution = 1u;
  uint32_t level = 0u;
  while (resolution < requiredResolution && level < settings.maximumLevel)
  {
    resolution <<= 1u;
    ++level;
  }
  if (resolution < requiredResolution || level == 0u)
    return fail(error, "Requested voxel size requires an octree deeper than the configured maximum level");

  next.maximumLevel = level;
  next.resolution = resolution;
  next.effectiveVoxelSizeMeters = settings.voxelSizeMeters;
  next.rootExtentMeters = settings.voxelSizeMeters * static_cast<double>(resolution);
  if (!std::isfinite(next.rootExtentMeters))
    return fail(error, "Derived octree root extent is not finite");
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    next.rootCenterMeters[axis] = next.rootMinMeters[axis] + next.rootExtentMeters * 0.5;

  uint64_t cellsXY = 0u;
  if (!checkedMultiply(next.occupiedCellsPerAxis[0], next.occupiedCellsPerAxis[1], cellsXY) ||
      !checkedMultiply(cellsXY, next.occupiedCellsPerAxis[2], next.conservativeCellCount))
    return fail(error, "Conservative voxel work estimate overflows 64 bits");
  if (next.conservativeCellCount > settings.maximumEstimatedCells)
    return fail(error, "Conservative voxel work estimate exceeds the configured cell budget");
  if (!checkedMultiply(next.conservativeCellCount, settings.estimatedBytesPerCell, next.conservativeTemporaryBytes))
    return fail(error, "Conservative temporary-memory estimate overflows 64 bits");
  if (next.conservativeTemporaryBytes > settings.maximumEstimatedTemporaryBytes)
    return fail(error, "Conservative temporary-memory estimate exceeds the configured budget");
  next.conservativePageCount = next.conservativeTemporaryBytes / settings.targetPageBytes +
                               (next.conservativeTemporaryBytes % settings.targetPageBytes != 0u ? 1u : 0u);

  estimate = next;
  return true;
}

} // namespace voxel::svraster
