#include "editor/voxel/SparseVoxelOBJBuilder.hpp"

#include "tiny_obj_loader/tiny_obj_loader.h"
#include "os/Logger.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"

// Declarations only. The implementation is compiled once, in
// VirtualGeometryGLTF.cpp, which defines STB_IMAGE_IMPLEMENTATION.
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace voxel::svraster
{
namespace
{

using Vec3 = std::array<double, 3>;

struct MaterialAccumulator
{
  std::array<double, 3> linearAlbedo{};
  std::array<uint64_t, 2> properties{};
  std::array<double, 3> emissive{};
  uint64_t count = 0u;
  uint32_t materialId = 0u;
};

double srgbToLinear(const double value)
{
  return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double linearToSrgb(const double value)
{
  const double clamped = std::clamp(value, 0.0, 1.0);
  return clamped <= 0.0031308 ? clamped * 12.92 : 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
}

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

Vec3 subtract(const Vec3 &a, const Vec3 &b)
{
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

double dot(const Vec3 &a, const Vec3 &b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(const Vec3 &a, const Vec3 &b)
{
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

bool overlapAxis(const Vec3 &axis, const Vec3 &v0, const Vec3 &v1, const Vec3 &v2)
{
  if (dot(axis, axis) <= 1.0e-24)
    return true;
  const double p0 = dot(v0, axis);
  const double p1 = dot(v1, axis);
  const double p2 = dot(v2, axis);
  const double radius = 0.5 * (std::fabs(axis[0]) + std::fabs(axis[1]) + std::fabs(axis[2]));
  return std::min({p0, p1, p2}) <= radius && std::max({p0, p1, p2}) >= -radius;
}

bool planeOverlapsBox(const Vec3 &normal, const Vec3 &point)
{
  Vec3 minimum{};
  Vec3 maximum{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (normal[axis] > 0.0)
    {
      minimum[axis] = -0.5 - point[axis];
      maximum[axis] = 0.5 - point[axis];
    }
    else
    {
      minimum[axis] = 0.5 - point[axis];
      maximum[axis] = -0.5 - point[axis];
    }
  }
  return dot(normal, minimum) <= 0.0 && dot(normal, maximum) >= 0.0;
}

bool triangleOverlapsUnitCell(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2, const Vec3 &center)
{
  const Vec3 v0 = subtract(p0, center);
  const Vec3 v1 = subtract(p1, center);
  const Vec3 v2 = subtract(p2, center);
  const Vec3 edges[] = {subtract(v1, v0), subtract(v2, v1), subtract(v0, v2)};
  for (const Vec3 &edge : edges)
  {
    if (!overlapAxis({0.0, -edge[2], edge[1]}, v0, v1, v2) ||
        !overlapAxis({edge[2], 0.0, -edge[0]}, v0, v1, v2) ||
        !overlapAxis({-edge[1], edge[0], 0.0}, v0, v1, v2))
      return false;
  }
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (std::min({v0[axis], v1[axis], v2[axis]}) > 0.5 || std::max({v0[axis], v1[axis], v2[axis]}) < -0.5)
      return false;
  return planeOverlapsBox(cross(edges[0], subtract(v2, v0)), v0);
}

uint32_t packByte(const double value)
{
  return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

// A decoded diffuse map, kept as 8-bit RGBA in whatever space the file was in
// (sRGB for every OBJ texture in practice). The caller converts on the way into
// the linear accumulator, exactly as it already does for a flat `Kd`.
struct DiffuseTexture
{
  int width = 0;
  int height = 0;
  std::vector<unsigned char> pixels;
};

// Decoded diffuse maps by material index. Decoding is by far the most expensive
// part of importing a textured OBJ, and the same map is referenced by many
// faces, so it is done once per material and shared.
class DiffuseTextureCache
{
public:
  DiffuseTextureCache(
      const std::vector<tinyobj::material_t> &materials,
      const std::filesystem::path &baseDirectory)
  {
    textures_.resize(materials.size());
    for (size_t index = 0u; index < materials.size(); ++index)
    {
      const std::string &name = materials[index].diffuse_texname;
      if (name.empty()) continue;
      // MTL paths are relative to the material file, which sits beside the OBJ.
      // They also routinely use Windows separators even on disk layouts that do
      // not, so normalise before touching the filesystem.
      std::string relative = name;
      std::replace(relative.begin(), relative.end(), '\\', '/');
      const std::filesystem::path path = baseDirectory / relative;
      auto shared = std::find_if(
          decoded_.begin(), decoded_.end(),
          [&](const auto &entry) { return entry.first == path.string(); });
      if (shared != decoded_.end())
      {
        textures_[index] = shared->second;
        continue;
      }
      int width = 0;
      int height = 0;
      int channels = 0;
      unsigned char *data = stbi_load(
          path.string().c_str(), &width, &height, &channels, 4);
      if (data == nullptr) continue;
      auto texture = std::make_shared<DiffuseTexture>();
      texture->width = width;
      texture->height = height;
      texture->pixels.assign(
          data, data + static_cast<size_t>(width) * height * 4u);
      stbi_image_free(data);
      decoded_.emplace_back(path.string(), texture);
      textures_[index] = texture;
    }
  }

  bool empty() const { return decoded_.empty(); }
  size_t decodedCount() const { return decoded_.size(); }

  // Nearest-neighbour lookup with wrapped coordinates. Bilinear would be the
  // wrong tool: a voxel already covers many texels, so the meaningful filtering
  // is the averaging the per-voxel accumulator does over every sample that
  // lands in the cell, and interpolating each of those first only blurs inside
  // one texel.
  bool sample(
      const int materialIndex, const float u, const float v,
      std::array<double, 3> &albedo) const
  {
    if (materialIndex < 0 ||
        static_cast<size_t>(materialIndex) >= textures_.size())
      return false;
    const std::shared_ptr<DiffuseTexture> &texture =
        textures_[static_cast<size_t>(materialIndex)];
    if (!texture || texture->width <= 0 || texture->height <= 0) return false;
    const auto wrap = [](const float coordinate, const int extent)
    {
      const float wrapped = coordinate - std::floor(coordinate);
      const int texel = static_cast<int>(wrapped * static_cast<float>(extent));
      return std::clamp(texel, 0, extent - 1);
    };
    // OBJ texture coordinates put v at the bottom; image rows start at the top.
    const size_t offset = (static_cast<size_t>(
                               wrap(1.0f - v, texture->height)) *
                               static_cast<size_t>(texture->width) +
                           static_cast<size_t>(wrap(u, texture->width))) * 4u;
    for (uint32_t channel = 0u; channel < 3u; ++channel)
      albedo[channel] =
          static_cast<double>(texture->pixels[offset + channel]) / 255.0;
    return true;
  }

private:
  std::vector<std::shared_ptr<DiffuseTexture>> textures_;
  std::vector<std::pair<std::string, std::shared_ptr<DiffuseTexture>>> decoded_;
};

// Barycentric weights of `point` projected onto the triangle's plane, clamped
// to the triangle. Cells the conservative overlap test accepts can sit slightly
// outside the triangle - that is the point of a conservative test - so the
// weights are clamped and renormalised rather than trusted raw, which would
// otherwise read a texel from well outside the face.
void triangleBarycentric(
    const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &point,
    std::array<double, 3> &weights)
{
  const Vec3 ab = subtract(b, a);
  const Vec3 ac = subtract(c, a);
  const Vec3 ap = subtract(point, a);
  const double d00 = dot(ab, ab);
  const double d01 = dot(ab, ac);
  const double d11 = dot(ac, ac);
  const double d20 = dot(ap, ab);
  const double d21 = dot(ap, ac);
  const double denominator = d00 * d11 - d01 * d01;
  if (std::abs(denominator) < 1.0e-12)
  {
    weights = {1.0, 0.0, 0.0};
    return;
  }
  const double v = std::clamp((d11 * d20 - d01 * d21) / denominator, 0.0, 1.0);
  const double w = std::clamp((d00 * d21 - d01 * d20) / denominator, 0.0, 1.0);
  const double u = std::clamp(1.0 - v - w, 0.0, 1.0);
  const double total = u + v + w;
  weights = total > 1.0e-12
      ? std::array<double, 3>{u / total, v / total, w / total}
      : std::array<double, 3>{1.0, 0.0, 0.0};
}

SparseVoxelMaterialGPU sourceMaterial(const std::vector<tinyobj::material_t> &materials, const int materialIndex)
{
  SparseVoxelMaterialGPU result{};
  double albedo[3] = {0.75, 0.75, 0.75};
  double roughness = 0.6;
  double metallic = 0.0;
  double emission[3] = {0.0, 0.0, 0.0};
  if (materialIndex >= 0 && static_cast<size_t>(materialIndex) < materials.size())
  {
    const tinyobj::material_t &material = materials[static_cast<size_t>(materialIndex)];
    for (uint32_t channel = 0u; channel < 3u; ++channel)
    {
      albedo[channel] = material.diffuse[channel];
      emission[channel] = std::max(0.0, static_cast<double>(material.emission[channel]));
    }
    if (material.roughness > 0.0)
      roughness = material.roughness;
    metallic = material.metallic;
  }
  result.albedoRGBA8 = packByte(albedo[0]) | (packByte(albedo[1]) << 8u) | (packByte(albedo[2]) << 16u) | 0xff000000u;
  result.roughnessMetallicUNorm8 = packByte(roughness) | (packByte(metallic) << 8u);
  result.emissiveRG16F = static_cast<uint32_t>(floatToHalfRoundToNearestEven(static_cast<float>(emission[0]))) |
                         (static_cast<uint32_t>(floatToHalfRoundToNearestEven(static_cast<float>(emission[1]))) << 16u);
  result.emissiveB16F = floatToHalfRoundToNearestEven(static_cast<float>(emission[2]));
  result.materialId = materialIndex >= 0 ? static_cast<uint32_t>(materialIndex) : 0xffffffffu;
  return result;
}

void accumulate(MaterialAccumulator &accumulator, const SparseVoxelMaterialGPU &material)
{
  accumulator.linearAlbedo[0] += srgbToLinear(static_cast<double>(material.albedoRGBA8 & 0xffu) / 255.0);
  accumulator.linearAlbedo[1] += srgbToLinear(static_cast<double>((material.albedoRGBA8 >> 8u) & 0xffu) / 255.0);
  accumulator.linearAlbedo[2] += srgbToLinear(static_cast<double>((material.albedoRGBA8 >> 16u) & 0xffu) / 255.0);
  accumulator.properties[0] += material.roughnessMetallicUNorm8 & 0xffu;
  accumulator.properties[1] += (material.roughnessMetallicUNorm8 >> 8u) & 0xffu;
  accumulator.emissive[0] += halfToFloat(static_cast<uint16_t>(material.emissiveRG16F));
  accumulator.emissive[1] += halfToFloat(static_cast<uint16_t>(material.emissiveRG16F >> 16u));
  accumulator.emissive[2] += halfToFloat(static_cast<uint16_t>(material.emissiveB16F));
  if (accumulator.count == 0u)
    accumulator.materialId = material.materialId;
  ++accumulator.count;
}

SparseVoxelMaterialGPU finishMaterial(const MaterialAccumulator &accumulator)
{
  SparseVoxelMaterialGPU result{};
  const uint64_t count = std::max(uint64_t{1}, accumulator.count);
  const auto averageByte = [&](const uint64_t value) { return static_cast<uint32_t>((value + count / 2u) / count); };
  result.albedoRGBA8 = packByte(linearToSrgb(accumulator.linearAlbedo[0] / count)) |
                       (packByte(linearToSrgb(accumulator.linearAlbedo[1] / count)) << 8u) |
                       (packByte(linearToSrgb(accumulator.linearAlbedo[2] / count)) << 16u) | 0xff000000u;
  result.roughnessMetallicUNorm8 = averageByte(accumulator.properties[0]) | (averageByte(accumulator.properties[1]) << 8u);
  result.emissiveRG16F = static_cast<uint32_t>(floatToHalfRoundToNearestEven(static_cast<float>(accumulator.emissive[0] / count))) |
                         (static_cast<uint32_t>(floatToHalfRoundToNearestEven(static_cast<float>(accumulator.emissive[1] / count))) << 16u);
  result.emissiveB16F = floatToHalfRoundToNearestEven(static_cast<float>(accumulator.emissive[2] / count));
  result.materialId = accumulator.materialId;
  return result;
}

uint64_t ancestorPath(const uint64_t path, const uint32_t level)
{
  const uint32_t unusedBits = 3u * (kMaxOctreeLevels - level);
  return (path >> unusedBits) << unusedBits;
}

SparseVoxelCenterLengthGPU centerLength(const SparseVoxelBLAS &model, const uint64_t path, const uint32_t level)
{
  const auto coordinate = decodeOctreePath(path, level);
  const float length = model.rootExtent / static_cast<float>(uint64_t{1} << level);
  return {{
            model.rootCenter[0] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[0]) + 0.5f) * length,
            model.rootCenter[1] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[1]) + 0.5f) * length,
            model.rootCenter[2] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[2]) + 0.5f) * length,
          },
          length};
}

std::array<float, 16> identityMatrix()
{
  return {1.0f, 0.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 0.0f, 1.0f};
}

uint64_t settingsHash(const SparseVoxelOBJImportSettings &settings)
{
  uint64_t hash = 14695981039346656037ull;
  const auto append = [&](const void *data, const size_t size)
  {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t index = 0u; index < size; ++index)
    {
      hash ^= bytes[index];
      hash *= 1099511628211ull;
    }
  };
  const double scale = metersPerSourceUnit(settings.sourceUnit, settings.metersPerSourceUnit);
  append(&scale, sizeof(scale));
  append(&settings.voxelSizeMeters, sizeof(settings.voxelSizeMeters));
  append(&settings.maximumLevel, sizeof(settings.maximumLevel));
  append(&settings.splitByShape, sizeof(settings.splitByShape));
  append(&settings.splitByMaterial, sizeof(settings.splitByMaterial));
  return hash;
}

} // namespace

bool buildSparseVoxelOBJScene(
    const SparseVoxelOBJAnalysis &analysis,
    const SparseVoxelOBJImportSettings &settings,
    SparseVoxelScene &scene,
    SparseVoxelOBJBuildStatistics *statistics,
    std::string *error,
    const std::atomic_bool *cancelled,
    const SparseVoxelOBJBuildProgressCallback &progress,
    bool revalidateSource)
{
  const auto report = [&progress](const char *phase, const float fraction)
  {
    if (progress)
      progress(phase, fraction);
  };

  if (error != nullptr)
    error->clear();
  SparseVoxelOBJGridEstimate grid{};
  if (!deriveSparseVoxelOBJGrid(analysis, settings, grid, error))
    return false;
  if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed))
    return fail(error, "OBJ voxelization cancelled");

  if (revalidateSource)
  {
    report("revalidating source", -1.0f);
    SparseVoxelOBJAnalysis currentAnalysis{};
    if (!analyzeSparseVoxelOBJ(analysis.sourcePath, currentAnalysis, error, cancelled))
      return false;
    if (currentAnalysis.sourceContentHash != analysis.sourceContentHash)
      return fail(error, "OBJ or material source changed after analysis; analyze it again before voxelizing");
  }

  report("parsing OBJ", -1.0f);
  tinyobj::ObjReaderConfig config{};
  config.triangulate = true;
  tinyobj::ObjReader reader;
  if (!reader.ParseFromFile(analysis.sourcePath.string(), config))
    return fail(error, reader.Error().empty() ? "Could not parse OBJ source file" : reader.Error());
  const auto &attributes = reader.GetAttrib();
  const auto &materials = reader.GetMaterials();
  // Decoded once for the whole import and shared across shapes and faces.
  const DiffuseTextureCache textures(
      materials, std::filesystem::path(analysis.sourcePath).parent_path());
  std::array<double, 3> scratchAlbedo{};
  // Worth reporting rather than failing quietly: a missing or mis-pathed
  // texture set is indistinguishable from a correctly imported untextured model
  // once the voxels exist, and the difference is the entire look of the scene.
  os::Logger::logf(
      "Sparse voxel OBJ diffuse maps: decoded %zu of %zu materials",
      textures.decodedCount(), materials.size());
  const double scale = metersPerSourceUnit(settings.sourceUnit, settings.metersPerSourceUnit);
  const double voxelSize = grid.effectiveVoxelSizeMeters;

  const auto &shapes = reader.GetShapes();

  // One group becomes one BLAS plus one instance. Splitting per shape gives
  // every model a root cube tight to its own geometry, which is the only way
  // instance culling can reject anything: a single whole-file model has a root
  // spanning the scene, so its instance is always visible.
  //
  // Splitting per *material* instead filters faces by material id across every
  // shape, so one instance holds exactly the geometry sharing an appearance -
  // which is what lets a single 256-entry palette per instance describe it
  // without visible quantisation.
  struct BuildGroup
  {
    std::vector<size_t> shapes;
    // -1 accepts every face; otherwise only faces carrying this material id.
    int materialFilter = -1;
  };
  std::vector<BuildGroup> groups;
  if (settings.splitByMaterial)
  {
    // Ordered so the emitted instance order is stable across runs, and so a
    // material that no face references never produces an empty model.
    std::set<int> usedMaterials;
    for (const tinyobj::shape_t &shape : shapes)
      for (const int materialId : shape.mesh.material_ids)
        usedMaterials.insert(materialId);
    if (usedMaterials.empty())
      usedMaterials.insert(-1);
    std::vector<size_t> everything(shapes.size());
    std::iota(everything.begin(), everything.end(), size_t{0});
    groups.reserve(usedMaterials.size());
    for (const int materialId : usedMaterials)
      groups.push_back({everything, materialId});
  }
  else if (settings.splitByShape)
  {
    groups.reserve(shapes.size());
    for (size_t shapeIndex = 0u; shapeIndex < shapes.size(); ++shapeIndex)
      groups.push_back({{shapeIndex}, -1});
  }
  else
  {
    std::vector<size_t> everything(shapes.size());
    std::iota(everything.begin(), everything.end(), size_t{0});
    groups.push_back({std::move(everything), -1});
  }

  SparseVoxelScene next{};
  SparseVoxelOBJBuildStatistics nextStatistics{};
  size_t totalFaces = 0u;
  for (const tinyobj::shape_t &shape : shapes)
    totalFaces += shape.mesh.num_face_vertices.size();
  size_t processedFaces = 0u;
  // Reporting every face would cost more than the work it describes, so the
  // callback fires on a coarse stride instead.
  const size_t progressStride = std::max<size_t>(1u, totalFaces / 200u);
  report("voxelizing", 0.0f);

  for (const BuildGroup &buildGroup : groups)
  {
  const std::vector<size_t> &group = buildGroup.shapes;
  const int materialFilter = buildGroup.materialFilter;
  // Whether this face belongs to the group being built. Reading the id the
  // same way the voxelization loop does keeps the bounds and the geometry in
  // agreement; disagreeing would size the grid for faces that are never
  // rasterized into it.
  const auto faceInGroup =
      [materialFilter](const tinyobj::shape_t &shape, const size_t face)
  {
    if (materialFilter < 0)
      return true;
    const int materialId =
        face < shape.mesh.material_ids.size() ? shape.mesh.material_ids[face] : -1;
    return materialId == materialFilter;
  };
  // Each group is voxelized against its own grid, derived from its own bounds
  // at the shared voxel size so density stays uniform across models.
  SparseVoxelOBJGridEstimate groupGrid = grid;
  if (settings.splitByShape || settings.splitByMaterial)
  {
    SparseVoxelOBJAnalysis groupAnalysis = analysis;
    bool hasBounds = false;
    uint64_t groupTriangles = 0u;
    for (const size_t shapeIndex : group)
    {
      const tinyobj::shape_t &shape = shapes[shapeIndex];
      // Walked per face rather than over the flat index list, because the
      // material id that selects a face is per face.
      size_t boundsIndexOffset = 0u;
      for (size_t face = 0u; face < shape.mesh.num_face_vertices.size(); ++face)
      {
        const uint32_t faceVertices = shape.mesh.num_face_vertices[face];
        if (!faceInGroup(shape, face))
        {
          boundsIndexOffset += faceVertices;
          continue;
        }
        ++groupTriangles;
        for (uint32_t corner = 0u; corner < faceVertices; ++corner)
        {
          const tinyobj::index_t &index =
              shape.mesh.indices[boundsIndexOffset + corner];
          if (index.vertex_index < 0)
            continue;
          for (uint32_t axis = 0u; axis < 3u; ++axis)
          {
            const double value = static_cast<double>(attributes.vertices[3u * static_cast<size_t>(index.vertex_index) + axis]);
            groupAnalysis.sourceBounds.min[axis] = hasBounds ? std::min(groupAnalysis.sourceBounds.min[axis], value) : value;
            groupAnalysis.sourceBounds.max[axis] = hasBounds ? std::max(groupAnalysis.sourceBounds.max[axis], value) : value;
          }
          hasBounds = true;
        }
        boundsIndexOffset += faceVertices;
      }
    }
    // A shape with no usable geometry contributes no model rather than
    // failing the whole import.
    if (!hasBounds || groupTriangles == 0u)
      continue;
    groupAnalysis.triangleCount = groupTriangles;
    if (!deriveSparseVoxelOBJGrid(groupAnalysis, settings, groupGrid, error))
      return false;
  }
  const double voxelSize = groupGrid.effectiveVoxelSizeMeters;

  // Ordering is re-established when these feed the ordered levels[] maps
  // below, so the accumulation containers only need lookup, not sorting.
  std::unordered_map<uint64_t, MaterialAccumulator> leaves;
  for (const size_t shapeIndex : group)
  {
    const tinyobj::shape_t &shape = shapes[shapeIndex];
    size_t indexOffset = 0u;
    for (size_t face = 0u; face < shape.mesh.num_face_vertices.size(); ++face)
    {
      if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed))
        return fail(error, "OBJ voxelization cancelled");
      if (++processedFaces % progressStride == 0u)
        report("voxelizing", static_cast<float>(processedFaces) / static_cast<float>(std::max<size_t>(1u, totalFaces)));
      const uint32_t faceVertices = shape.mesh.num_face_vertices[face];
      if (faceVertices != 3u)
        return fail(error, "OBJ triangulation produced malformed geometry");
      if (!faceInGroup(shape, face))
      {
        indexOffset += faceVertices;
        continue;
      }
      Vec3 triangle[3]{};
      for (uint32_t corner = 0u; corner < 3u; ++corner)
      {
        const int vertexIndex = shape.mesh.indices[indexOffset + corner].vertex_index;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
          triangle[corner][axis] = (static_cast<double>(attributes.vertices[3u * static_cast<size_t>(vertexIndex) + axis]) * scale - groupGrid.rootMinMeters[axis]) / voxelSize;
      }
      indexOffset += 3u;
      const Vec3 normal = cross(subtract(triangle[1], triangle[0]), subtract(triangle[2], triangle[0]));
      if (dot(normal, normal) == 0.0)
        continue;
      std::array<int64_t, 3> minimum{};
      std::array<int64_t, 3> maximum{};
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        // Touching a voxel face counts as conservative surface coverage, so
        // an integer minimum must test the cell on both sides of that face.
        minimum[axis] = std::max<int64_t>(0, static_cast<int64_t>(std::ceil(std::min({triangle[0][axis], triangle[1][axis], triangle[2][axis]}))) - 1);
        maximum[axis] = std::min<int64_t>(static_cast<int64_t>(groupGrid.resolution) - 1,
                                         static_cast<int64_t>(std::floor(std::max({triangle[0][axis], triangle[1][axis], triangle[2][axis]}))));
      }
      const int materialIndex = face < shape.mesh.material_ids.size() ? shape.mesh.material_ids[face] : -1;
      const SparseVoxelMaterialGPU material = sourceMaterial(materials, materialIndex);
      // Corner texture coordinates, when the face has them and the material has
      // a diffuse map. Sponza's colour lives entirely in those maps - every one
      // of its materials carries the same flat grey `Kd` - so without this the
      // whole model voxelises one uniform colour.
      std::array<std::array<float, 2>, 3> cornerUV{};
      bool faceHasUV = textures.sample(materialIndex, 0.0f, 0.0f, scratchAlbedo);
      if (faceHasUV)
      {
        for (uint32_t corner = 0u; corner < 3u && faceHasUV; ++corner)
        {
          const int texcoordIndex =
              shape.mesh.indices[indexOffset - 3u + corner].texcoord_index;
          if (texcoordIndex < 0 ||
              static_cast<size_t>(2 * texcoordIndex + 1) >=
                  attributes.texcoords.size())
          {
            faceHasUV = false;
            break;
          }
          cornerUV[corner][0] =
              attributes.texcoords[2u * static_cast<size_t>(texcoordIndex)];
          cornerUV[corner][1] =
              attributes.texcoords[2u * static_cast<size_t>(texcoordIndex) + 1u];
        }
      }
      for (int64_t z = minimum[2]; z <= maximum[2]; ++z)
        for (int64_t y = minimum[1]; y <= maximum[1]; ++y)
          for (int64_t x = minimum[0]; x <= maximum[0]; ++x)
          {
            ++nextStatistics.testedCells;
            if (nextStatistics.testedCells > settings.maximumEstimatedCells)
              return fail(error, "OBJ voxelization exceeded the configured cell budget");
            if (!triangleOverlapsUnitCell(triangle[0], triangle[1], triangle[2], {static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5, static_cast<double>(z) + 0.5}))
              continue;
            const uint64_t path = encodeOctreePath(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z), groupGrid.maximumLevel);
            SparseVoxelMaterialGPU cellMaterial = material;
            if (faceHasUV)
            {
              // Barycentric of the cell centre projected onto the triangle,
              // never the triangle's centroid. Sponza's floors and walls are
              // single enormous quads spanning hundreds of cells, and a
              // centroid UV would paint each one a single colour - which is the
              // very flatness this exists to remove.
              const Vec3 centre{
                static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5,
                static_cast<double>(z) + 0.5};
              std::array<double, 3> weights{};
              triangleBarycentric(
                  triangle[0], triangle[1], triangle[2], centre, weights);
              const float u = static_cast<float>(
                  weights[0] * cornerUV[0][0] + weights[1] * cornerUV[1][0] +
                  weights[2] * cornerUV[2][0]);
              const float v = static_cast<float>(
                  weights[0] * cornerUV[0][1] + weights[1] * cornerUV[1][1] +
                  weights[2] * cornerUV[2][1]);
              std::array<double, 3> albedo{};
              if (textures.sample(materialIndex, u, v, albedo))
                cellMaterial.albedoRGBA8 =
                    packByte(albedo[0]) | (packByte(albedo[1]) << 8u) |
                    (packByte(albedo[2]) << 16u) | 0xff000000u;
            }
            accumulate(leaves[path], cellMaterial);
          }
    }
  }
  // A shape whose surface misses every cell contributes no model instead of
  // failing the import; the whole-file case still has to produce something.
  if (leaves.empty())
  {
    if (!settings.splitByShape && !settings.splitByMaterial)
      return fail(error, "OBJ surface voxelization produced no occupied cells");
    continue;
  }
  nextStatistics.occupiedLeafCount += leaves.size();

  // Reported per level rather than once for the whole phase: this is the
  // longest stretch of the import, and a single indeterminate line followed by
  // many seconds of silence is indistinguishable from a hang.
  report("building hierarchy", 0.0f);
  std::vector<std::map<uint64_t, SparseVoxelMaterialGPU>> levels(groupGrid.maximumLevel + 1u);
  for (const auto &[path, material] : leaves)
    levels[groupGrid.maximumLevel][path] = finishMaterial(material);
  const uint32_t hierarchyLevelCount = groupGrid.maximumLevel > 1u ? groupGrid.maximumLevel - 1u : 1u;
  for (uint32_t childLevel = groupGrid.maximumLevel; childLevel > 1u; --childLevel)
  {
    report(
        "building hierarchy",
        static_cast<float>(groupGrid.maximumLevel - childLevel) / static_cast<float>(hierarchyLevelCount));
    struct ParentData
    {
      std::vector<uint64_t> children;
      std::vector<SparseVoxelMaterialGPU> childMaterials;
      bool identical = true;
      bool allLeaves = true;
      SparseVoxelMaterialGPU firstMaterial{};
    };
    std::unordered_set<uint64_t> nodesWithChildren;
    if (childLevel < groupGrid.maximumLevel)
      for (const auto &[descendantPath, _] : levels[childLevel + 1u])
        nodesWithChildren.insert(ancestorPath(descendantPath, childLevel));
    std::unordered_map<uint64_t, ParentData> parents;
    for (const auto &[path, material] : levels[childLevel])
    {
      ParentData &parent = parents[ancestorPath(path, childLevel - 1u)];
      if (parent.children.empty())
        parent.firstMaterial = material;
      else if (std::memcmp(&parent.firstMaterial, &material, sizeof(material)) != 0)
        parent.identical = false;
      parent.children.push_back(path);
      parent.childMaterials.push_back(material);
      parent.allLeaves &= nodesWithChildren.count(path) == 0u;
    }
    for (const auto &[path, parent] : parents)
    {
      levels[childLevel - 1u][path] =
          reduceSparseVoxelLODMaterials(parent.childMaterials, parent.firstMaterial.materialId);
      if (parent.children.size() == 8u && parent.identical && parent.allLeaves)
        for (const uint64_t childPath : parent.children)
          levels[childLevel].erase(childPath);
    }
  }

  SparseVoxelBLAS model{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    model.rootCenter[axis] = static_cast<float>(groupGrid.rootCenterMeters[axis]);
  model.rootExtent = static_cast<float>(groupGrid.rootExtentMeters);
  model.maxLevel = groupGrid.maximumLevel;
  std::map<std::pair<uint32_t, uint64_t>, uint32_t> indices;
  for (uint32_t level = 1u; level <= groupGrid.maximumLevel; ++level)
    for (const auto &[path, material] : levels[level])
    {
      const uint32_t nodeIndex = static_cast<uint32_t>(model.hierarchyNodes.size());
      indices[{level, path}] = nodeIndex;
      model.hierarchyNodes.push_back({0u, 0u, nodeIndex, level});
      model.octreePaths.push_back(path);
      model.octreeLevels.push_back(static_cast<uint8_t>(level));
      model.centersLengths.push_back(centerLength(model, path, level));
      model.materials.push_back(material);
    }
  model.firstRoot = 0u;
  model.rootCount = static_cast<uint32_t>(levels[1].size());
  std::vector<std::vector<uint32_t>> children(model.hierarchyNodes.size());
  for (uint32_t childIndex = model.rootCount; childIndex < model.hierarchyNodes.size(); ++childIndex)
  {
    const uint32_t level = model.octreeLevels[childIndex];
    children[indices.at({level - 1u, ancestorPath(model.octreePaths[childIndex], level - 1u)})].push_back(childIndex);
  }
  for (uint32_t nodeIndex = 0u; nodeIndex < model.hierarchyNodes.size(); ++nodeIndex)
    if (!children[nodeIndex].empty())
    {
      model.hierarchyNodes[nodeIndex].firstChild = children[nodeIndex].front();
      model.hierarchyNodes[nodeIndex].childCount = static_cast<uint32_t>(children[nodeIndex].size());
    }
  nextStatistics.hierarchyNodeCount += model.hierarchyNodes.size();
  // Geometry is already baked in world space, so every instance is identity and
  // the per-model root cube alone gives the TLAS its separation.
  next.instances.push_back({static_cast<uint32_t>(next.models.size()), 0u, identityMatrix()});
  next.models.push_back(std::move(model));
  }

  if (next.models.empty())
    return fail(error, "OBJ surface voxelization produced no occupied cells");
  next.sourceInfo.importerVersion = 1u;
  next.sourceInfo.sourceRelativePath = analysis.sourcePath.filename().generic_string();
  next.sourceInfo.normalizedBuildOptions = "meters-per-unit=" + std::to_string(scale) + ";voxel-size-m=" + std::to_string(grid.effectiveVoxelSizeMeters) + ";surface=1";
  next.sourceContentHash = analysis.sourceContentHash;
  next.buildSettingsHash = settingsHash(settings);
  std::string validationError;
  if (!validateSparseVoxelSceneSource(next, &validationError))
    return fail(error, "Generated sparse voxel scene is invalid: " + validationError);

  scene = std::move(next);
  if (statistics != nullptr)
    *statistics = nextStatistics;
  return true;
}

bool makeBootstrapSparseVoxelPreview(
    const SparseVoxelScene &scene,
    BootstrapSparseVoxelModel &model,
    BootstrapSparseVoxelLOD &hierarchy,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (scene.models.empty() || scene.instances.empty())
    return fail(error, "Preview conversion requires at least one BLAS and one instance");

  // Models are concatenated in scene order because SparseVoxelSceneManager
  // derives blasRootRanges the same way (running nodeBase over models), and the
  // GPU indexes this flat array through those ranges. Any other order here
  // would silently point instances at another model's nodes.
  BootstrapSparseVoxelLOD nextHierarchy{};
  BootstrapSparseVoxelModel nextModel{};
  uint32_t nodeBase = 0u;
  std::array<float, 3> sceneMinimum{};
  std::array<float, 3> sceneMaximum{};
  bool hasSceneBounds = false;
  uint32_t maximumLevel = 0u;

  for (const SparseVoxelBLAS &source : scene.models)
  {
    for (SparseVoxelLODNodeGPU node : source.hierarchyNodes)
    {
      // childCount == 0 leaves firstChild meaningless, so only real links move.
      if (node.childCount != 0u)
        node.firstChild += nodeBase;
      // voxelIndex is written model-local by the builder but addresses the
      // flattened centersLengths/materials/pathsLevels arrays on the GPU, so it
      // has to be rebased too. Missing this makes every model past the first
      // read another model's voxel centres.
      node.voxelIndex += nodeBase;
      nextHierarchy.nodes.push_back(node);
    }
    nextHierarchy.centersLengths.insert(nextHierarchy.centersLengths.end(), source.centersLengths.begin(), source.centersLengths.end());
    nextHierarchy.materials.insert(nextHierarchy.materials.end(), source.materials.begin(), source.materials.end());
    for (uint32_t index = 0u; index < source.octreePaths.size(); ++index)
      nextHierarchy.pathsLevels.push_back({static_cast<uint32_t>(source.octreePaths[index]), static_cast<uint32_t>(source.octreePaths[index] >> 32u), source.octreeLevels[index], 0u});

    // BootstrapSparseVoxelModel represents the active solid cut, whereas the
    // parallel LOD object owns every hierarchy level. Keep only terminal nodes
    // here so an ancestor and its descendants are never active simultaneously.
    for (uint32_t nodeIndex = 0u; nodeIndex < source.hierarchyNodes.size(); ++nodeIndex)
    {
      if (source.hierarchyNodes[nodeIndex].childCount != 0u)
        continue;
      const uint32_t flatIndex = nodeBase + nodeIndex;
      nextModel.pathsLevels.push_back(nextHierarchy.pathsLevels[flatIndex]);
      nextModel.centersLengths.push_back(nextHierarchy.centersLengths[flatIndex]);
      nextModel.materials.push_back(nextHierarchy.materials[flatIndex]);
    }

    const float half = source.rootExtent * 0.5f;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float low = source.rootCenter[axis] - half;
      const float high = source.rootCenter[axis] + half;
      sceneMinimum[axis] = hasSceneBounds ? std::min(sceneMinimum[axis], low) : low;
      sceneMaximum[axis] = hasSceneBounds ? std::max(sceneMaximum[axis], high) : high;
    }
    hasSceneBounds = true;
    maximumLevel = std::max(maximumLevel, source.maxLevel);
    nodeBase += static_cast<uint32_t>(source.hierarchyNodes.size());
  }

  // Roots are per model and therefore not one contiguous run once the scene is
  // split. The sample seeds the LOD chain from visible instances rather than
  // from this range, so it stays descriptive of the first model only.
  nextHierarchy.firstRoot = scene.models.front().firstRoot;
  nextHierarchy.rootCount = scene.models.front().rootCount;

  // A root cube covering every model, so camera framing sees the whole scene.
  float sceneExtent = 0.0f;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    sceneExtent = std::max(sceneExtent, sceneMaximum[axis] - sceneMinimum[axis]);
  nextModel.blas.rootCenterExtent = {
    0.5f * (sceneMinimum[0] + sceneMaximum[0]),
    0.5f * (sceneMinimum[1] + sceneMaximum[1]),
    0.5f * (sceneMinimum[2] + sceneMaximum[2]),
    sceneExtent};
  nextModel.blas.firstVoxel = 0u;
  nextModel.blas.maxLevel = maximumLevel;
  nextModel.blas.voxelCount = static_cast<uint32_t>(nextModel.pathsLevels.size());
  nextModel.instance.modelToWorld = scene.instances[0].modelToWorld;
  nextModel.instance.worldToModel = scene.instances[0].modelToWorld;
  nextModel.instance.previousModelToWorld = scene.instances[0].modelToWorld;
  nextModel.instance.worldBoundsMin = {sceneMinimum[0], sceneMinimum[1], sceneMinimum[2], 0.0f};
  nextModel.instance.worldBoundsMax = {sceneMaximum[0], sceneMaximum[1], sceneMaximum[2], 0.0f};
  model = std::move(nextModel);
  hierarchy = std::move(nextHierarchy);
  return true;
}

} // namespace voxel::svraster
