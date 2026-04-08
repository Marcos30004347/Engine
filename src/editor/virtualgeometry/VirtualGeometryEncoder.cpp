#include "VirtualGeometryEncoder.hpp"
#include "VirtualGeometryCompressor.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader/tiny_obj_loader.h"

namespace virtualgeometry
{

void computeNormals(std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);

namespace fs = std::filesystem;

namespace
{

struct OBJAssetData
{
  std::vector<Vertex> vertices;
  std::vector<ShapeBuildInput> shapes;
  std::vector<std::string> materialFiles;
};

static std::string sanitizeMaterialName(const std::string &name, uint32_t materialIndex)
{
  std::string result;
  result.reserve(name.size() + 8u);
  for (char c : name)
  {
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '-')
    {
      result.push_back(c);
    }
    else
    {
      result.push_back('_');
    }
  }

  if (result.empty())
    result = "material_" + std::to_string(materialIndex);
  return result;
}

static void appendMaterialTexture(
    VirtualTextureSystem::MaterialCreateInfo &createInfo,
    const fs::path &baseDirectory,
    const std::string &texturePath,
    bool flipVertically = false)
{
  if (texturePath.empty() || createInfo.textureCount >= MAX_TEXTURES_PER_MATERIAL)
    return;

  fs::path resolvedPath = texturePath;
  if (resolvedPath.is_relative())
    resolvedPath = baseDirectory / resolvedPath;
  if (!fs::exists(resolvedPath))
    return;

  auto &texture = createInfo.textures[createInfo.textureCount++];
  texture.source.path = resolvedPath.lexically_normal().string();
  texture.source.flipVertically = flipVertically;
}

static bool createOBJMaterialFiles(
    const fs::path &objPath,
    const std::vector<tinyobj::material_t> &materials,
    const std::string &materialOutputDirectory,
    std::vector<std::string> &outMaterialFiles)
{
  outMaterialFiles.assign(materials.size(), std::string{});
  if (materials.empty() || materialOutputDirectory.empty())
    return true;

  const fs::path outputDirectory(materialOutputDirectory);
  std::error_code error;
  fs::create_directories(outputDirectory, error);
  if (error)
    return false;

  const fs::path textureBaseDirectory = objPath.parent_path();
  for (uint32_t materialIndex = 0u; materialIndex < materials.size(); ++materialIndex)
  {
    const tinyobj::material_t &material = materials[materialIndex];
    VirtualTextureSystem::MaterialCreateInfo createInfo{};

    appendMaterialTexture(createInfo, textureBaseDirectory, material.diffuse_texname, false);
    appendMaterialTexture(
        createInfo,
        textureBaseDirectory,
        !material.normal_texname.empty() ? material.normal_texname : material.bump_texname,
        false);

    if (createInfo.textureCount == 0u)
      continue;

    const fs::path materialPath = outputDirectory / (sanitizeMaterialName(material.name, materialIndex) + ".vmat");
    if (!VirtualMaterialFile::saveFromCreateInfo(materialPath.string(), createInfo))
      return false;

    outMaterialFiles[materialIndex] = materialPath.string();
  }

  return true;
}

static void loadOBJAsset(
    const std::string &source,
    bool from_file,
    std::vector<Vertex> &out_vertices,
    std::vector<ShapeBuildInput> &out_shapes,
    std::vector<std::string> *outMaterialFiles,
    const std::string &materialOutputDirectory)
{
  tinyobj::ObjReaderConfig config;
  config.triangulate = true;
  config.vertex_color = false;

  tinyobj::ObjReader reader;

  const bool ok = from_file ? reader.ParseFromFile(source, config) : reader.ParseFromString(source, "", config);

  assert(ok && "Failed to load OBJ");

  const auto &attrib = reader.GetAttrib();
  const auto &shapes = reader.GetShapes();
  const auto &materials = reader.GetMaterials();

  bool has_normals = !attrib.normals.empty();

  std::unordered_map<Vertex, uint32_t> vertex_map;

  out_vertices.clear();
  out_shapes.clear();
  if (outMaterialFiles != nullptr)
  {
    if (from_file)
    {
      const bool created = createOBJMaterialFiles(fs::path(source), materials, materialOutputDirectory, *outMaterialFiles);
      assert(created && "Failed to generate OBJ virtual material files");
      (void)created;
    }
    else
    {
      outMaterialFiles->assign(materials.size(), std::string{});
    }
  }

  for (const auto &shape : shapes)
  {
    std::unordered_map<int32_t, size_t> materialShapeMap;
    size_t index_offset = 0;

    for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f)
    {
      const size_t fv = shape.mesh.num_face_vertices[f];
      assert(fv == 3); // triangulated

      const int32_t materialId = f < shape.mesh.material_ids.size() ? shape.mesh.material_ids[f] : -1;
      const int32_t normalizedMaterialId =
          materialId >= 0 && materialId < static_cast<int32_t>(materials.size()) ? materialId : 0;

      auto it = materialShapeMap.find(normalizedMaterialId);
      if (it == materialShapeMap.end())
      {
        ShapeBuildInput input;
        input.materialIndex = static_cast<uint32_t>(normalizedMaterialId);
        input.meshParts.push_back(MeshPart{Shape{}, UINT32_MAX});
        out_shapes.push_back(std::move(input));
        it = materialShapeMap.emplace(normalizedMaterialId, out_shapes.size() - 1u).first;
      }

      Shape &outShape = out_shapes[it->second].meshParts[0].shape;

      for (size_t v = 0; v < fv; ++v)
      {
        const tinyobj::index_t idx = shape.mesh.indices[index_offset + v];

        Vertex vert = {};

        vert.pos[0] = attrib.vertices[3 * idx.vertex_index + 0];
        vert.pos[1] = attrib.vertices[3 * idx.vertex_index + 1];
        vert.pos[2] = attrib.vertices[3 * idx.vertex_index + 2];

        if (idx.normal_index >= 0)
        {
          vert.norm[0] = attrib.normals[3 * idx.normal_index + 0];
          vert.norm[1] = attrib.normals[3 * idx.normal_index + 1];
          vert.norm[2] = attrib.normals[3 * idx.normal_index + 2];
        }

        if (idx.texcoord_index >= 0)
        {
          vert.uv[0] = attrib.texcoords[2 * idx.texcoord_index + 0];
          vert.uv[1] = attrib.texcoords[2 * idx.texcoord_index + 1];
        }

        auto vertexIt = vertex_map.find(vert);
        if (vertexIt == vertex_map.end())
        {
          const uint32_t newIndex = static_cast<uint32_t>(out_vertices.size());
          out_vertices.push_back(vert);
          vertex_map[vert] = newIndex;
          outShape.indices.push_back(newIndex);
        }
        else
        {
          outShape.indices.push_back(vertexIt->second);
        }
      }

      index_offset += fv;
    }
  }

  if (!has_normals && !out_shapes.empty())
  {
    std::vector<uint32_t> combinedIndices;
    for (const ShapeBuildInput &shape : out_shapes)
      for (const MeshPart &meshPart : shape.meshParts)
        combinedIndices.insert(combinedIndices.end(), meshPart.shape.indices.begin(), meshPart.shape.indices.end());
    computeNormals(out_vertices, combinedIndices);
  }
}

} // namespace

VirtualGeometryEncodedData VirtualGeometryEncoder::encode(
    const std::vector<Vertex> &vertices,
    const Shape &shape,
    const QuantizationConfig &config,
    const VirtualGeometryBuildSettings &buildSettings)
{
  return encode(vertices, std::vector<Shape>{shape}, config, buildSettings);
}

VirtualGeometryEncodedData VirtualGeometryEncoder::encode(
    const std::vector<Vertex> &vertices,
    const std::vector<Shape> &shapes,
    const QuantizationConfig &config,
    const VirtualGeometryBuildSettings &buildSettings)
{
  VirtualGeometryBuildData vm = buildVirtualMesh(vertices, shapes, buildSettings);
  return VirtualGeometryCompressor::encode(vm, config);
}

VirtualGeometryEncodedData VirtualGeometryEncoder::encodeFromOBJFile(
    const std::string &path,
    const QuantizationConfig &config,
    const VirtualGeometryBuildSettings &buildSettings,
    const std::string &materialOutputDirectory)
{
  VirtualGeometryBuildData build = buildFromOBJFile(path, buildSettings, materialOutputDirectory);
  return VirtualGeometryCompressor::encode(build, config);
}

VirtualGeometryEncodedData VirtualGeometryEncoder::encodeFromOBJString(
    const std::string &obj_source,
    const QuantizationConfig &config,
    const VirtualGeometryBuildSettings &buildSettings)
{
  std::vector<Vertex> vertices;
  std::vector<Shape> shapes;

  loadOBJ(obj_source, false, vertices, shapes);
  return encode(vertices, shapes, config, buildSettings);
}

VirtualGeometryEncodedData VirtualGeometryEncoder::encodeFromGLTFFile(
    const std::string &path,
    const QuantizationConfig &config,
    const VirtualGeometryBuildSettings &buildSettings)
{
  VirtualGeometryBuildData vm = VirtualGeometryBuilder::buildFromGLTFFile(path, buildSettings);
  return VirtualGeometryCompressor::encode(vm, config);
}

VirtualGeometryBuildData VirtualGeometryEncoder::buildFromOBJFile(
    const std::string &path,
    const VirtualGeometryBuildSettings &buildSettings,
    const std::string &materialOutputDirectory)
{
  OBJAssetData asset;
  loadOBJAsset(path, true, asset.vertices, asset.shapes, &asset.materialFiles, materialOutputDirectory);
  VirtualGeometryBuildData buildData = VirtualGeometryBuilder::build(asset.vertices, asset.shapes, buildSettings);
  buildData.materialFiles = asset.materialFiles;
  return buildData;
}

VirtualGeometryBuildData VirtualGeometryEncoder::buildVirtualMesh(
    const std::vector<Vertex> &vertices,
    const std::vector<Shape> &shapes,
    const VirtualGeometryBuildSettings &buildSettings)
{
  std::vector<Vertex> mutable_vertices = vertices;
  return VirtualGeometryBuilder::build(mutable_vertices, shapes, buildSettings);
}

void computeNormals(std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices)
{
  for (auto &v : vertices)
  {
    v.norm[0] = 0.0f;
    v.norm[1] = 0.0f;
    v.norm[2] = 0.0f;
  }

  for (size_t i = 0; i < indices.size(); i += 3)
  {
    Vertex &v0 = vertices[indices[i + 0]];
    Vertex &v1 = vertices[indices[i + 1]];
    Vertex &v2 = vertices[indices[i + 2]];

    const float x1 = v1.pos[0] - v0.pos[0];
    const float y1 = v1.pos[1] - v0.pos[1];
    const float z1 = v1.pos[2] - v0.pos[2];

    const float x2 = v2.pos[0] - v0.pos[0];
    const float y2 = v2.pos[1] - v0.pos[1];
    const float z2 = v2.pos[2] - v0.pos[2];

    // Cross product
    const float nx = y1 * z2 - z1 * y2;
    const float ny = z1 * x2 - x1 * z2;
    const float nz = x1 * y2 - y1 * x2;

    v0.norm[0] += nx;
    v0.norm[1] += ny;
    v0.norm[2] += nz;
    v1.norm[0] += nx;
    v1.norm[1] += ny;
    v1.norm[2] += nz;
    v2.norm[0] += nx;
    v2.norm[1] += ny;
    v2.norm[2] += nz;
  }

  // Normalize
  for (auto &v : vertices)
  {
    const float len = std::sqrt(v.norm[0] * v.norm[0] + v.norm[1] * v.norm[1] + v.norm[2] * v.norm[2]);

    if (len > 0.0f)
    {
      v.norm[0] /= len;
      v.norm[1] /= len;
      v.norm[2] /= len;
    }
  }
}

void VirtualGeometryEncoder::loadOBJ(const std::string &source, bool from_file, std::vector<Vertex> &out_vertices, std::vector<Shape> &out_shapes)
{
  tinyobj::ObjReaderConfig config;
  config.triangulate = true;
  config.vertex_color = false;

  tinyobj::ObjReader reader;

  const bool ok = from_file ? reader.ParseFromFile(source, config) : reader.ParseFromString(source, "", config);

  assert(ok && "Failed to load OBJ");

  const auto &attrib = reader.GetAttrib();
  const auto &shapes = reader.GetShapes();

  const bool has_normals = !attrib.normals.empty();

  std::unordered_map<Vertex, uint32_t> vertex_map;

  out_vertices.clear();
  out_shapes.clear();

  for (const auto &shape : shapes)
  {
    Shape outShape;
    size_t index_offset = 0;

    for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f)
    {
      const size_t fv = shape.mesh.num_face_vertices[f];
      assert(fv == 3); // triangulated

      for (size_t v = 0; v < fv; ++v)
      {
        const tinyobj::index_t idx = shape.mesh.indices[index_offset + v];

        Vertex vert = {};

        vert.pos[0] = attrib.vertices[3 * idx.vertex_index + 0];
        vert.pos[1] = attrib.vertices[3 * idx.vertex_index + 1];
        vert.pos[2] = attrib.vertices[3 * idx.vertex_index + 2];

        if (idx.normal_index >= 0)
        {
          vert.norm[0] = attrib.normals[3 * idx.normal_index + 0];
          vert.norm[1] = attrib.normals[3 * idx.normal_index + 1];
          vert.norm[2] = attrib.normals[3 * idx.normal_index + 2];
        }

        if (idx.texcoord_index >= 0)
        {
          vert.uv[0] = attrib.texcoords[2 * idx.texcoord_index + 0];
          vert.uv[1] = attrib.texcoords[2 * idx.texcoord_index + 1];
        }

        auto it = vertex_map.find(vert);
        if (it == vertex_map.end())
        {
          const uint32_t newIndex = static_cast<uint32_t>(out_vertices.size());
          out_vertices.push_back(vert);
          vertex_map[vert] = newIndex;
          outShape.indices.push_back(newIndex);
        }
        else
        {
          outShape.indices.push_back(it->second);
        }
      }

      index_offset += fv;
    }

    if (!outShape.indices.empty())
      out_shapes.push_back(std::move(outShape));
  }

  if (!has_normals && !out_shapes.empty())
  {
    std::vector<uint32_t> combinedIndices;
    for (const Shape &shape : out_shapes)
      combinedIndices.insert(combinedIndices.end(), shape.indices.begin(), shape.indices.end());
    computeNormals(out_vertices, combinedIndices);
  }
}

void VirtualGeometryEncoder::loadOBJ(const std::string &source, bool from_file, std::vector<Vertex> &out_vertices, Shape &out_shape)
{
  std::vector<Shape> shapes;
  loadOBJ(source, from_file, out_vertices, shapes);
  out_shape.indices.clear();
  for (const auto &shape : shapes)
    out_shape.indices.insert(out_shape.indices.end(), shape.indices.begin(), shape.indices.end());
}

} // namespace virtualgeometry
