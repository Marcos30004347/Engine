#include <cassert>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "editor/virtualgeometry/FallbackMeshBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"
#include "virtualgeometry/VirtualImageFile.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"

using namespace virtualgeometry;

namespace
{

namespace fs = std::filesystem;

bool floatEqual(float left, float right, float epsilon = 1e-5f)
{
  return std::fabs(left - right) <= epsilon;
}

fs::path sourceRoot()
{
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

fs::path teapotAssetPath()
{
  return sourceRoot() / "assets/meshes/obj/teapot.obj";
}

fs::path buildFallbackMeshPath(const fs::path &virtualGeometryPath, uint32_t lodLevel)
{
  return virtualGeometryPath.parent_path() /
         (virtualGeometryPath.stem().string() + ".lod" + std::to_string(lodLevel) + ".fallback.mesh");
}

uint32_t packRGBA8(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255u)
{
  return static_cast<uint32_t>(r) |
         (static_cast<uint32_t>(g) << 8u) |
         (static_cast<uint32_t>(b) << 16u) |
         (static_cast<uint32_t>(a) << 24u);
}

size_t sourceIndexCount(const VirtualGeometrySourceAsset &asset)
{
  size_t indexCount = 0u;
  for (const SourceShapeBuildInput &shape : asset.shapes)
    for (const MeshPart &meshPart : shape.meshParts)
      indexCount += meshPart.shape.indices.size();
  return indexCount;
}

size_t meshIndexCount(const std::vector<FallbackMeshData> &meshes)
{
  size_t indexCount = 0u;
  for (const FallbackMeshData &mesh : meshes)
    indexCount += mesh.indices.size();
  return indexCount;
}

VirtualGeometrySourceAsset buildTestAsset(uint32_t width, uint32_t height)
{
  VirtualGeometrySourceAsset asset;
  asset.materialFiles = {"materials/left.vmat", "materials/right.vmat"};

  rendering::animation::Skeleton skeleton;
  math::Mat4f defaultTransform = math::Mat4f::identity();
  defaultTransform.data[12] = 2.0f;
  defaultTransform.data[13] = -1.0f;
  skeleton.setDefaultTransform(defaultTransform);

  rendering::animation::Skeleton::Bone rootBone;
  rootBone.name = "root";
  rootBone.parentIndex = -1;
  rootBone.defaultLocalTransform.data[12] = 0.25f;
  rootBone.inverseBindMatrix.data[12] = -0.25f;
  skeleton.addBone(rootBone);

  rendering::animation::Skeleton::Bone childBone;
  childBone.name = "child";
  childBone.parentIndex = 0;
  childBone.defaultLocalTransform.data[13] = 0.5f;
  childBone.inverseBindMatrix.data[13] = -0.5f;
  skeleton.addBone(childBone);
  skeleton.setMeshPartToBoneIndex({0u, 1u});
  asset.skeleton = skeleton;

  asset.vertices.reserve(static_cast<size_t>(width + 1u) * static_cast<size_t>(height + 1u));
  for (uint32_t y = 0u; y <= height; ++y)
  {
    for (uint32_t x = 0u; x <= width; ++x)
    {
      const float fx = static_cast<float>(x) / static_cast<float>(width);
      const float fy = static_cast<float>(y) / static_cast<float>(height);

      Vertex vertex;
      vertex.pos[0] = fx * 10.0f;
      vertex.pos[1] = fy * 10.0f;
      vertex.pos[2] = std::sin(fx * 3.14159265f) * std::cos(fy * 3.14159265f) * 0.1f;
      vertex.norm[0] = 0.0f;
      vertex.norm[1] = 0.0f;
      vertex.norm[2] = 1.0f;
      vertex.uv[0] = fx;
      vertex.uv[1] = fy;

      if (x < width / 2u)
      {
        vertex.boneWeights.push_back(BoneWeight{1.0f, 0u});
      }
      else
      {
        vertex.boneWeights.push_back(BoneWeight{0.65f, 1u});
        vertex.boneWeights.push_back(BoneWeight{0.35f, 0u});
      }

      asset.vertices.push_back(std::move(vertex));
    }
  }

  SourceShapeBuildInput leftShape;
  leftShape.materialIndex = 0u;
  leftShape.meshParts.push_back(MeshPart{Shape{}, 0u});

  SourceShapeBuildInput rightShape;
  rightShape.materialIndex = 1u;
  rightShape.meshParts.push_back(MeshPart{Shape{}, 1u});

  const auto vertexIndex = [width](uint32_t x, uint32_t y)
  {
    return y * (width + 1u) + x;
  };

  for (uint32_t y = 0u; y < height; ++y)
  {
    for (uint32_t x = 0u; x < width; ++x)
    {
      Shape &shape = x < width / 2u ? leftShape.meshParts[0].shape : rightShape.meshParts[0].shape;

      const uint32_t i0 = vertexIndex(x + 0u, y + 0u);
      const uint32_t i1 = vertexIndex(x + 1u, y + 0u);
      const uint32_t i2 = vertexIndex(x + 0u, y + 1u);
      const uint32_t i3 = vertexIndex(x + 1u, y + 1u);

      shape.indices.push_back(i0);
      shape.indices.push_back(i2);
      shape.indices.push_back(i1);

      shape.indices.push_back(i1);
      shape.indices.push_back(i2);
      shape.indices.push_back(i3);
    }
  }

  asset.shapes.push_back(std::move(leftShape));
  asset.shapes.push_back(std::move(rightShape));
  return asset;
}

VirtualGeometrySourceAsset buildOctagonalPrismAsset()
{
  VirtualGeometrySourceAsset asset;
  asset.materialFiles = {"materials/prism.vmat"};
  constexpr uint32_t ringVertexCount = 8u;
  constexpr float pi = 3.14159265358979323846f;

  for (uint32_t z = 0u; z < 2u; ++z)
  {
    for (uint32_t vertexIndex = 0u; vertexIndex < ringVertexCount; ++vertexIndex)
    {
      const float angle = static_cast<float>(vertexIndex) * 2.0f * pi / static_cast<float>(ringVertexCount);
      Vertex vertex;
      vertex.pos[0] = std::cos(angle);
      vertex.pos[1] = std::sin(angle);
      vertex.pos[2] = z == 0u ? -0.5f : 0.5f;
      vertex.norm[0] = vertex.pos[0];
      vertex.norm[1] = vertex.pos[1];
      vertex.norm[2] = 0.0f;
      vertex.uv[0] = static_cast<float>(vertexIndex) / static_cast<float>(ringVertexCount);
      vertex.uv[1] = static_cast<float>(z);
      asset.vertices.push_back(vertex);
    }
  }

  const uint32_t bottomCenter = static_cast<uint32_t>(asset.vertices.size());
  Vertex bottom;
  bottom.pos[2] = -0.5f;
  bottom.norm[2] = -1.0f;
  asset.vertices.push_back(bottom);

  const uint32_t topCenter = static_cast<uint32_t>(asset.vertices.size());
  Vertex top;
  top.pos[2] = 0.5f;
  top.norm[2] = 1.0f;
  asset.vertices.push_back(top);

  SourceShapeBuildInput shape;
  shape.materialIndex = 0u;
  shape.meshParts.push_back(MeshPart{Shape{}, UINT32_MAX});
  Shape &mesh = shape.meshParts[0].shape;

  for (uint32_t vertexIndex = 0u; vertexIndex < ringVertexCount; ++vertexIndex)
  {
    const uint32_t nextVertexIndex = (vertexIndex + 1u) % ringVertexCount;
    const uint32_t bottom0 = vertexIndex;
    const uint32_t bottom1 = nextVertexIndex;
    const uint32_t top0 = ringVertexCount + vertexIndex;
    const uint32_t top1 = ringVertexCount + nextVertexIndex;

    mesh.indices.insert(mesh.indices.end(), {bottom0, top0, bottom1});
    mesh.indices.insert(mesh.indices.end(), {bottom1, top0, top1});
    mesh.indices.insert(mesh.indices.end(), {bottomCenter, bottom1, bottom0});
    mesh.indices.insert(mesh.indices.end(), {topCenter, top0, top1});
  }

  asset.shapes.push_back(std::move(shape));
  return asset;
}

float directionalExtent(const FallbackMeshData &mesh, const std::array<float, 3> &direction, bool maxExtent)
{
  float result = maxExtent ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
  for (const FallbackMeshVertex &vertex : mesh.vertices)
  {
    const float projection =
        vertex.position[0] * direction[0] +
        vertex.position[1] * direction[1] +
        vertex.position[2] * direction[2];
    result = maxExtent ? std::max(result, projection) : std::min(result, projection);
  }
  return result;
}

void compareSkeletons(const rendering::animation::Skeleton &expected, const rendering::animation::Skeleton &actual)
{
  assert(expected.getBoneCount() == actual.getBoneCount());
  assert(expected.getMeshPartToBoneIndex() == actual.getMeshPartToBoneIndex());
  for (uint32_t element = 0u; element < 16u; ++element)
    assert(floatEqual(expected.getDefaultTransform().data[element], actual.getDefaultTransform().data[element]));

  for (uint32_t boneIndex = 0u; boneIndex < expected.getBoneCount(); ++boneIndex)
  {
    const auto &expectedBone = expected.getBone(boneIndex);
    const auto &actualBone = actual.getBone(boneIndex);
    assert(expectedBone.name == actualBone.name);
    assert(expectedBone.parentIndex == actualBone.parentIndex);
    for (uint32_t element = 0u; element < 16u; ++element)
    {
      assert(floatEqual(expectedBone.defaultLocalTransform.data[element], actualBone.defaultLocalTransform.data[element]));
      assert(floatEqual(expectedBone.inverseBindMatrix.data[element], actualBone.inverseBindMatrix.data[element]));
    }
  }
}

void compareMeshes(const FallbackMeshData &expected, const FallbackMeshData &actual)
{
  assert(expected.lodLevel == actual.lodLevel);
  assert(floatEqual(expected.targetError, actual.targetError));
  assert(floatEqual(expected.relativeError, actual.relativeError));
  assert(expected.materialFiles == actual.materialFiles);
  assert(expected.indices == actual.indices);
  assert(expected.sections.size() == actual.sections.size());
  assert(expected.vertices.size() == actual.vertices.size());

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    assert(floatEqual(expected.boundsMin[axis], actual.boundsMin[axis]));
    assert(floatEqual(expected.boundsMax[axis], actual.boundsMax[axis]));
  }

  for (size_t sectionIndex = 0u; sectionIndex < expected.sections.size(); ++sectionIndex)
  {
    assert(expected.sections[sectionIndex].indexOffset == actual.sections[sectionIndex].indexOffset);
    assert(expected.sections[sectionIndex].indexCount == actual.sections[sectionIndex].indexCount);
    assert(expected.sections[sectionIndex].materialIndex == actual.sections[sectionIndex].materialIndex);
  }

  for (size_t vertexIndex = 0u; vertexIndex < expected.vertices.size(); ++vertexIndex)
  {
    const FallbackMeshVertex &expectedVertex = expected.vertices[vertexIndex];
    const FallbackMeshVertex &actualVertex = actual.vertices[vertexIndex];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      assert(floatEqual(expectedVertex.position[axis], actualVertex.position[axis]));
      assert(floatEqual(expectedVertex.normal[axis], actualVertex.normal[axis]));
    }
    for (uint32_t axis = 0u; axis < 2u; ++axis)
      assert(floatEqual(expectedVertex.uv[axis], actualVertex.uv[axis]));
    for (uint32_t influence = 0u; influence < 4u; ++influence)
    {
      assert(expectedVertex.boneIndices[influence] == actualVertex.boneIndices[influence]);
      assert(floatEqual(expectedVertex.boneWeights[influence], actualVertex.boneWeights[influence]));
      assert(floatEqual(expectedVertex.surfaceBaseColorOpacity[influence], actualVertex.surfaceBaseColorOpacity[influence]));
      assert(floatEqual(expectedVertex.surfaceNormalRoughness[influence], actualVertex.surfaceNormalRoughness[influence]));
      assert(floatEqual(expectedVertex.surfaceEmissiveMetallic[influence], actualVertex.surfaceEmissiveMetallic[influence]));
      assert(expectedVertex.surfaceFlags[influence] == actualVertex.surfaceFlags[influence]);
    }
  }

  assert(expected.triangleSurfaceHints.size() == actual.triangleSurfaceHints.size());
  for (size_t hintIndex = 0u; hintIndex < expected.triangleSurfaceHints.size(); ++hintIndex)
  {
    const FallbackMeshTriangleSurfaceHint &expectedHint = expected.triangleSurfaceHints[hintIndex];
    const FallbackMeshTriangleSurfaceHint &actualHint = actual.triangleSurfaceHints[hintIndex];
    assert(floatEqual(expectedHint.uvCircleCenter[0], actualHint.uvCircleCenter[0]));
    assert(floatEqual(expectedHint.uvCircleCenter[1], actualHint.uvCircleCenter[1]));
    assert(floatEqual(expectedHint.uvCircleRadius, actualHint.uvCircleRadius));
    assert(floatEqual(expectedHint.textureMipLevel, actualHint.textureMipLevel));
    assert(expectedHint.materialIndex == actualHint.materialIndex);
  }

  compareSkeletons(expected.skeleton, actual.skeleton);
}

void testSyntheticRoundTrip()
{
  const VirtualGeometrySourceAsset asset = buildTestAsset(112u, 112u);
  FallbackMeshBuilder::Settings settings{};
  settings.lod0TargetTriangleRatio = 0.25f;
  settings.maxLODCount = 6u;
  const std::vector<FallbackMeshData> lods = FallbackMeshBuilder::buildLODs(asset, settings);

  assert(!lods.empty());
  assert(lods.front().lodLevel == 0u);
  assert(lods.front().indices.size() <= sourceIndexCount(asset) / 4u);
  assert(lods.front().sections.size() == 2u);
  assert(lods.front().materialFiles.size() == 2u);
  assert(lods.front().sections[0].materialIndex == 0u);
  assert(lods.front().sections[1].materialIndex == 1u);

  for (size_t lodIndex = 1u; lodIndex < lods.size(); ++lodIndex)
  {
    assert(lods[lodIndex].lodLevel == lodIndex);
    assert(lods[lodIndex].indices.size() <= lods[lodIndex - 1u].indices.size());
    assert(lods[lodIndex].targetError >= lods[lodIndex - 1u].targetError);
  }

  const fs::path tempPath = fs::temp_directory_path() / "FallbackMeshFileTests.mesh";
  fs::remove(tempPath);

  assert(FallbackMeshFile::save(tempPath.string(), lods.front()));

  FallbackMeshData loaded;
  assert(FallbackMeshFile::load(tempPath.string(), loaded));
  compareMeshes(lods.front(), loaded);

  fs::remove(tempPath);
}

void testProxyFallbackPreservesBounds()
{
  const VirtualGeometrySourceAsset asset = buildTestAsset(112u, 112u);
  const std::vector<FallbackMeshData> sourceMeshes = FallbackMeshBuilder::buildOriginalPerShape(asset);
  const std::vector<FallbackMeshData> proxyMeshes = FallbackMeshBuilder::buildProxyPerShape(asset);

  assert(sourceMeshes.size() == proxyMeshes.size());
  assert(!proxyMeshes.empty());
  assert(meshIndexCount(proxyMeshes) <= sourceIndexCount(asset) / 2u);

  for (size_t meshIndex = 0u; meshIndex < proxyMeshes.size(); ++meshIndex)
  {
    const FallbackMeshData &sourceMesh = sourceMeshes[meshIndex];
    const FallbackMeshData &proxyMesh = proxyMeshes[meshIndex];
    assert(!proxyMesh.vertices.empty());
    assert(!proxyMesh.indices.empty());
    assert(proxyMesh.sections.size() == sourceMesh.sections.size());
    assert(proxyMesh.indices.size() <= sourceMesh.indices.size());
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      assert(floatEqual(proxyMesh.boundsMin[axis], sourceMesh.boundsMin[axis]));
      assert(floatEqual(proxyMesh.boundsMax[axis], sourceMesh.boundsMax[axis]));
    }
  }
}

void testProxyFallbackPreservesDirectionalSilhouette()
{
  const VirtualGeometrySourceAsset asset = buildOctagonalPrismAsset();
  const FallbackMeshData sourceMesh = FallbackMeshBuilder::buildOriginal(asset);

  FallbackMeshBuilder::Settings settings{};
  settings.proxyTargetTriangleRatio = 0.15f;
  settings.proxyMinTriangleCount = 0u;

  const FallbackMeshData proxyMesh = FallbackMeshBuilder::buildProxy(asset, settings);
  assert(!proxyMesh.vertices.empty());
  assert(!proxyMesh.indices.empty());
  assert(proxyMesh.indices.size() <= sourceMesh.indices.size());

  const std::array<std::array<float, 3>, 8u> directions = {{
      {1.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f},
      {0.0f, 0.0f, 1.0f},
      {0.70710677f, 0.70710677f, 0.0f},
      {-0.70710677f, 0.70710677f, 0.0f},
      {0.70710677f, 0.0f, 0.70710677f},
      {-0.70710677f, 0.0f, 0.70710677f},
      {0.0f, 0.70710677f, 0.70710677f},
  }};

  for (const std::array<float, 3> &direction : directions)
  {
    assert(floatEqual(directionalExtent(proxyMesh, direction, false), directionalExtent(sourceMesh, direction, false), 1e-4f));
    assert(floatEqual(directionalExtent(proxyMesh, direction, true), directionalExtent(sourceMesh, direction, true), 1e-4f));
  }
}

void testSdfProxyBakesSurfaceAttributes()
{
  VirtualGeometrySourceAsset asset = buildTestAsset(2u, 2u);
  FallbackMeshBuilder::Settings settings{};
  settings.proxyTargetTriangleRatio = 1.0f;
  settings.proxyMinTriangleCount = 0u;

  FallbackMeshBuilder::SurfaceBakeSettings surfaceSettings{};
  surfaceSettings.loadMaterialFiles = false;
  surfaceSettings.fallbackTextureWidth = 1024u;
  surfaceSettings.fallbackTextureHeight = 1024u;

  const FallbackMeshData mesh = FallbackMeshBuilder::buildSDFProxy(asset, settings, surfaceSettings);
  assert(!mesh.vertices.empty());
  assert(mesh.triangleSurfaceHints.size() == mesh.indices.size() / 3u);
  for (const FallbackMeshVertex &vertex : mesh.vertices)
  {
    assert((vertex.surfaceFlags[0] & FallbackMeshSurfaceFlag_BakedVertexShading) != 0u);
    assert(floatEqual(vertex.surfaceNormalRoughness[2], 1.0f));
  }
  for (const FallbackMeshTriangleSurfaceHint &hint : mesh.triangleSurfaceHints)
  {
    assert(hint.textureMipLevel >= 0.0f);
  }
}

void testSdfProxyPerShapeBakesSurfaceAttributes()
{
  VirtualGeometrySourceAsset asset = buildTestAsset(4u, 2u);
  FallbackMeshBuilder::Settings settings{};
  settings.proxyTargetTriangleRatio = 1.0f;
  settings.proxyMinTriangleCount = 0u;

  FallbackMeshBuilder::SurfaceBakeSettings surfaceSettings{};
  surfaceSettings.loadMaterialFiles = false;
  surfaceSettings.fallbackTextureWidth = 512u;
  surfaceSettings.fallbackTextureHeight = 512u;

  const std::vector<FallbackMeshData> meshes = FallbackMeshBuilder::buildSDFProxyPerShape(asset, settings, surfaceSettings);
  assert(meshes.size() == asset.shapes.size());
  for (const FallbackMeshData &mesh : meshes)
  {
    assert(!mesh.vertices.empty());
    assert(!mesh.indices.empty());
    assert(mesh.triangleSurfaceHints.size() == mesh.indices.size() / 3u);
    assert(mesh.sections.size() == 1u);
    for (const FallbackMeshVertex &vertex : mesh.vertices)
    {
      assert((vertex.surfaceFlags[0] & FallbackMeshSurfaceFlag_BakedVertexShading) != 0u);
      assert(floatEqual(vertex.surfaceNormalRoughness[2], 1.0f));
      assert(floatEqual(vertex.surfaceNormalRoughness[3], 1.0f));
    }
  }
}

void testSdfProxySamplesBaseColorTexture()
{
  const fs::path tempRoot = fs::temp_directory_path() / "FallbackMeshFileTests_texture_bake";
  const fs::path imagePath = tempRoot / "checker.vimg";
  const fs::path materialPath = tempRoot / "paint.vmat";
  std::error_code error;
  fs::remove_all(tempRoot, error);
  fs::create_directories(tempRoot, error);
  assert(!error);

  VirtualTextureSystem::PreparedImage image{};
  image.sourcePath = "checker";
  image.mips.resize(1u);
  image.mips[0].width = 2u;
  image.mips[0].height = 2u;
  image.mips[0].texels = {
      packRGBA8(255u, 0u, 0u),     // top-left
      packRGBA8(0u, 255u, 0u),     // top-right
      packRGBA8(0u, 0u, 255u),     // bottom-left
      packRGBA8(255u, 255u, 0u),   // bottom-right
  };
  assert(VirtualImageFile::save(imagePath.string(), image));

  Material::Definition material{};
  material.textureCount = 1u;
  material.textures[0].name = "baseColor";
  material.textures[0].imageFilePath = imagePath.filename().string();
  material.textures[0].sampling.addressModeU = VirtualTextureSystem::TextureAddressMode::ClampToEdge;
  material.textures[0].sampling.addressModeV = VirtualTextureSystem::TextureAddressMode::ClampToEdge;
  material.textures[0].sampling.filterMode = VirtualTextureSystem::TextureFilterMode::Nearest;
  assert(VirtualMaterialFile::save(materialPath.string(), material));

  VirtualGeometrySourceAsset asset;
  asset.materialFiles = {materialPath.string()};
  asset.vertices.resize(4u);
  asset.vertices[0].pos[0] = 0.0f; asset.vertices[0].pos[1] = 0.0f; asset.vertices[0].uv[0] = 0.0f; asset.vertices[0].uv[1] = 0.0f;
  asset.vertices[1].pos[0] = 1.0f; asset.vertices[1].pos[1] = 0.0f; asset.vertices[1].uv[0] = 1.0f; asset.vertices[1].uv[1] = 0.0f;
  asset.vertices[2].pos[0] = 0.0f; asset.vertices[2].pos[1] = 1.0f; asset.vertices[2].uv[0] = 0.0f; asset.vertices[2].uv[1] = 1.0f;
  asset.vertices[3].pos[0] = 1.0f; asset.vertices[3].pos[1] = 1.0f; asset.vertices[3].uv[0] = 1.0f; asset.vertices[3].uv[1] = 1.0f;
  for (Vertex &vertex : asset.vertices)
  {
    vertex.norm[0] = 0.0f;
    vertex.norm[1] = 0.0f;
    vertex.norm[2] = 1.0f;
  }

  SourceShapeBuildInput shape;
  shape.materialIndex = 0u;
  shape.meshParts.push_back(MeshPart{Shape{}, UINT32_MAX});
  Shape &quad = shape.meshParts[0].shape;
  quad.indices = {0u, 2u, 1u, 1u, 2u, 3u};
  asset.shapes.push_back(std::move(shape));

  FallbackMeshBuilder::Settings settings{};
  settings.proxyTargetTriangleRatio = 1.0f;
  settings.proxyMinTriangleCount = 0u;

  const FallbackMeshData mesh = FallbackMeshBuilder::buildSDFProxy(asset, settings, FallbackMeshBuilder::SurfaceBakeSettings{});
  assert(mesh.vertices.size() == 4u);
  assert(floatEqual(mesh.vertices[0].surfaceBaseColorOpacity[0], 0.0f));
  assert(floatEqual(mesh.vertices[0].surfaceBaseColorOpacity[1], 0.0f));
  assert(floatEqual(mesh.vertices[0].surfaceBaseColorOpacity[2], 1.0f));
  assert(floatEqual(mesh.vertices[1].surfaceBaseColorOpacity[0], 1.0f));
  assert(floatEqual(mesh.vertices[1].surfaceBaseColorOpacity[1], 1.0f));
  assert(floatEqual(mesh.vertices[1].surfaceBaseColorOpacity[2], 0.0f));
  assert(floatEqual(mesh.vertices[2].surfaceBaseColorOpacity[0], 1.0f));
  assert(floatEqual(mesh.vertices[2].surfaceBaseColorOpacity[1], 0.0f));
  assert(floatEqual(mesh.vertices[2].surfaceBaseColorOpacity[2], 0.0f));
  assert(floatEqual(mesh.vertices[3].surfaceBaseColorOpacity[0], 0.0f));
  assert(floatEqual(mesh.vertices[3].surfaceBaseColorOpacity[1], 1.0f));
  assert(floatEqual(mesh.vertices[3].surfaceBaseColorOpacity[2], 0.0f));

  fs::remove_all(tempRoot, error);
}

void testObjTeapotRoundTrip()
{
  const fs::path teapotPath = teapotAssetPath();
  assert(fs::exists(teapotPath));

  const fs::path tempRoot = fs::temp_directory_path() / "FallbackMeshFileTests_teapot";
  const fs::path materialsDir = tempRoot / "materials";
  const fs::path virtualGeometryPath = tempRoot / "teapot.obj.virtualgeometry";
  std::error_code error;
  fs::remove_all(tempRoot, error);
  fs::create_directories(materialsDir, error);
  assert(!error);

  const VirtualGeometrySourceAsset asset =
      VirtualGeometryEncoder::loadAssetFromOBJFile(teapotPath.string(), materialsDir.string());
  assert(!asset.vertices.empty());
  assert(!asset.shapes.empty());

  FallbackMeshBuilder::Settings settings{};
  settings.lod0TargetTriangleRatio = 0.25f;
  settings.maxLODCount = 6u;
  const std::vector<FallbackMeshData> lods = FallbackMeshBuilder::buildLODs(asset, settings);
  assert(!lods.empty());
  assert(lods.front().indices.size() <= sourceIndexCount(asset) / 4u);
  assert(!lods.front().vertices.empty());
  assert(!lods.front().indices.empty());
  assert(!lods.front().sections.empty());
  assert(lods.front().boundsMin[0] <= lods.front().boundsMax[0]);
  assert(lods.front().boundsMin[1] <= lods.front().boundsMax[1]);
  assert(lods.front().boundsMin[2] <= lods.front().boundsMax[2]);

  for (const FallbackMeshData &lod : lods)
  {
    const fs::path fallbackPath = buildFallbackMeshPath(virtualGeometryPath, lod.lodLevel);
    assert(FallbackMeshFile::save(fallbackPath.string(), lod));
    assert(fs::exists(fallbackPath));

    FallbackMeshData loaded;
    assert(FallbackMeshFile::load(fallbackPath.string(), loaded));
    compareMeshes(lod, loaded);
  }

  fs::remove_all(tempRoot, error);
}

} // namespace

int main()
{
  testSyntheticRoundTrip();
  testProxyFallbackPreservesBounds();
  testProxyFallbackPreservesDirectionalSilhouette();
  testSdfProxyBakesSurfaceAttributes();
  testSdfProxyPerShapeBakesSurfaceAttributes();
  testSdfProxySamplesBaseColorTexture();
  testObjTeapotRoundTrip();

  std::cout << "FallbackMeshFileTests passed\n";
  return 0;
}
