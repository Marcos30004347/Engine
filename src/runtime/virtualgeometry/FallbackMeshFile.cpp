#include "virtualgeometry/FallbackMeshFile.hpp"

#include <cstdio>
#include <cstring>
#include <limits>

#include "algorithm/BinaryIO.hpp"

namespace virtualgeometry
{

namespace
{

bool writeBytes(FILE *file, const void *data, size_t size)
{
  return size == 0u || std::fwrite(data, 1u, size, file) == size;
}

bool readBytes(FILE *file, void *data, size_t size)
{
  return size == 0u || std::fread(data, 1u, size, file) == size;
}

bool writeU32(FILE *file, uint32_t value)
{
  if (!isLittleEndian())
    value = byteswap(value);
  return writeBytes(file, &value, sizeof(value));
}

bool writeF32(FILE *file, float value)
{
  static_assert(sizeof(float) == sizeof(uint32_t), "Expected 32-bit float storage");
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return writeU32(file, bits);
}

bool readU32(FILE *file, uint32_t &value)
{
  if (!readBytes(file, &value, sizeof(value)))
    return false;
  if (!isLittleEndian())
    value = byteswap(value);
  return true;
}

bool readF32(FILE *file, float &value)
{
  uint32_t bits = 0u;
  if (!readU32(file, bits))
    return false;
  std::memcpy(&value, &bits, sizeof(value));
  return true;
}

bool writeString(FILE *file, const std::string &value)
{
  return writeU32(file, static_cast<uint32_t>(value.size())) &&
         writeBytes(file, value.data(), value.size());
}

bool readString(FILE *file, std::string &value)
{
  uint32_t length = 0u;
  if (!readU32(file, length))
    return false;
  value.resize(length);
  return readBytes(file, value.data(), value.size());
}

bool writeMatrix(FILE *file, const math::Mat4f &matrix)
{
  for (float element : matrix.data)
    if (!writeF32(file, element))
      return false;
  return true;
}

bool readMatrix(FILE *file, math::Mat4f &matrix)
{
  for (float &element : matrix.data)
    if (!readF32(file, element))
      return false;
  return true;
}

bool writeMeshPayload(FILE *file, const FallbackMeshData &data)
{
  const std::vector<uint32_t> &meshPartToBoneIndex = data.skeleton.getMeshPartToBoneIndex();
  bool ok = true;
  ok = ok && writeU32(file, data.lodLevel);
  ok = ok && writeU32(file, static_cast<uint32_t>(data.vertices.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(data.indices.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(data.sections.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(data.materialFiles.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(data.triangleSurfaceHints.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(meshPartToBoneIndex.size()));
  ok = ok && writeU32(file, data.skeleton.getBoneCount());
  ok = ok && writeF32(file, data.targetError);
  ok = ok && writeF32(file, data.relativeError);
  for (float value : data.boundsMin)
    ok = ok && writeF32(file, value);
  for (float value : data.boundsMax)
    ok = ok && writeF32(file, value);

  for (const std::string &materialFile : data.materialFiles)
    ok = ok && writeString(file, materialFile);

  for (const FallbackMeshSection &section : data.sections)
  {
    ok = ok && writeU32(file, section.indexOffset);
    ok = ok && writeU32(file, section.indexCount);
    ok = ok && writeU32(file, section.materialIndex);
  }

  ok = ok && writeMatrix(file, data.skeleton.getDefaultTransform());
  for (uint32_t meshPartBoneIndex : meshPartToBoneIndex)
    ok = ok && writeU32(file, meshPartBoneIndex);

  for (const rendering::animation::Skeleton::Bone &bone : data.skeleton.getBones())
  {
    ok = ok && writeU32(file, bone.parentIndex < 0 ? UINT32_MAX : static_cast<uint32_t>(bone.parentIndex));
    ok = ok && writeString(file, bone.name);
    ok = ok && writeMatrix(file, bone.defaultLocalTransform);
    ok = ok && writeMatrix(file, bone.inverseBindMatrix);
    ok = ok && writeF32(file, bone.capsuleStart[0]);
    ok = ok && writeF32(file, bone.capsuleStart[1]);
    ok = ok && writeF32(file, bone.capsuleStart[2]);
    ok = ok && writeF32(file, bone.capsuleEnd[0]);
    ok = ok && writeF32(file, bone.capsuleEnd[1]);
    ok = ok && writeF32(file, bone.capsuleEnd[2]);
    ok = ok && writeF32(file, bone.capsuleRadius);
  }

  for (const FallbackMeshVertex &vertex : data.vertices)
  {
    for (float value : vertex.position)
      ok = ok && writeF32(file, value);
    for (float value : vertex.normal)
      ok = ok && writeF32(file, value);
    for (float value : vertex.uv)
      ok = ok && writeF32(file, value);
    for (uint32_t value : vertex.boneIndices)
      ok = ok && writeU32(file, value);
    for (float value : vertex.boneWeights)
      ok = ok && writeF32(file, value);
    for (float value : vertex.surfaceBaseColorOpacity)
      ok = ok && writeF32(file, value);
    for (float value : vertex.surfaceNormalRoughness)
      ok = ok && writeF32(file, value);
    for (float value : vertex.surfaceEmissiveMetallic)
      ok = ok && writeF32(file, value);
    for (uint32_t value : vertex.surfaceFlags)
      ok = ok && writeU32(file, value);
  }

  for (uint32_t index : data.indices)
    ok = ok && writeU32(file, index);

  for (const FallbackMeshTriangleSurfaceHint &hint : data.triangleSurfaceHints)
  {
    ok = ok && writeF32(file, hint.uvCircleCenter[0]);
    ok = ok && writeF32(file, hint.uvCircleCenter[1]);
    ok = ok && writeF32(file, hint.uvCircleRadius);
    ok = ok && writeF32(file, hint.textureMipLevel);
    ok = ok && writeU32(file, hint.materialIndex);
    for (uint32_t value : hint._padding)
      ok = ok && writeU32(file, value);
  }

  return ok;
}

bool readMeshPayload(FILE *file, const uint32_t version, FallbackMeshData &outData)
{
  uint32_t vertexCount = 0u;
  uint32_t indexCount = 0u;
  uint32_t sectionCount = 0u;
  uint32_t materialCount = 0u;
  uint32_t triangleSurfaceHintCount = 0u;
  uint32_t meshPartCount = 0u;
  uint32_t boneCount = 0u;

  bool ok = true;
  ok = ok && readU32(file, outData.lodLevel);
  ok = ok && readU32(file, vertexCount);
  ok = ok && readU32(file, indexCount);
  ok = ok && readU32(file, sectionCount);
  ok = ok && readU32(file, materialCount);
  if (version >= FALLBACK_MESH_VERSION)
    ok = ok && readU32(file, triangleSurfaceHintCount);
  ok = ok && readU32(file, meshPartCount);
  ok = ok && readU32(file, boneCount);
  ok = ok && readF32(file, outData.targetError);
  ok = ok && readF32(file, outData.relativeError);
  for (float &value : outData.boundsMin)
    ok = ok && readF32(file, value);
  for (float &value : outData.boundsMax)
    ok = ok && readF32(file, value);

  outData.materialFiles.resize(materialCount);
  for (std::string &materialFile : outData.materialFiles)
    ok = ok && readString(file, materialFile);

  outData.sections.resize(sectionCount);
  for (FallbackMeshSection &section : outData.sections)
  {
    ok = ok && readU32(file, section.indexOffset);
    ok = ok && readU32(file, section.indexCount);
    ok = ok && readU32(file, section.materialIndex);
  }

  rendering::animation::Skeleton skeleton;
  math::Mat4f defaultTransform = math::Mat4f::identity();
  ok = ok && readMatrix(file, defaultTransform);
  skeleton.setDefaultTransform(defaultTransform);

  std::vector<uint32_t> meshPartToBoneIndex(meshPartCount, UINT32_MAX);
  for (uint32_t &meshPartBoneIndex : meshPartToBoneIndex)
    ok = ok && readU32(file, meshPartBoneIndex);

  for (uint32_t boneIndex = 0u; boneIndex < boneCount; ++boneIndex)
  {
    rendering::animation::Skeleton::Bone bone;
    uint32_t parentIndex = UINT32_MAX;
    ok = ok && readU32(file, parentIndex);
    bone.parentIndex = parentIndex == UINT32_MAX ? -1 : static_cast<int32_t>(parentIndex);
    ok = ok && readString(file, bone.name);
    ok = ok && readMatrix(file, bone.defaultLocalTransform);
    ok = ok && readMatrix(file, bone.inverseBindMatrix);
    if (version >= FALLBACK_MESH_VERSION)
    {
      ok = ok && readF32(file, bone.capsuleStart[0]);
      ok = ok && readF32(file, bone.capsuleStart[1]);
      ok = ok && readF32(file, bone.capsuleStart[2]);
      ok = ok && readF32(file, bone.capsuleEnd[0]);
      ok = ok && readF32(file, bone.capsuleEnd[1]);
      ok = ok && readF32(file, bone.capsuleEnd[2]);
      ok = ok && readF32(file, bone.capsuleRadius);
    }
    if (!ok)
      break;
    skeleton.addBone(bone);
  }
  skeleton.setMeshPartToBoneIndex(std::move(meshPartToBoneIndex));

  outData.vertices.resize(vertexCount);
  for (FallbackMeshVertex &vertex : outData.vertices)
  {
    for (float &value : vertex.position)
      ok = ok && readF32(file, value);
    for (float &value : vertex.normal)
      ok = ok && readF32(file, value);
    for (float &value : vertex.uv)
      ok = ok && readF32(file, value);
    for (uint32_t &value : vertex.boneIndices)
      ok = ok && readU32(file, value);
    for (float &value : vertex.boneWeights)
      ok = ok && readF32(file, value);
    if (version >= FALLBACK_MESH_VERSION)
    {
      for (float &value : vertex.surfaceBaseColorOpacity)
        ok = ok && readF32(file, value);
      for (float &value : vertex.surfaceNormalRoughness)
        ok = ok && readF32(file, value);
      for (float &value : vertex.surfaceEmissiveMetallic)
        ok = ok && readF32(file, value);
      for (uint32_t &value : vertex.surfaceFlags)
        ok = ok && readU32(file, value);
    }
  }

  outData.indices.resize(indexCount);
  for (uint32_t &index : outData.indices)
    ok = ok && readU32(file, index);

  outData.triangleSurfaceHints.resize(triangleSurfaceHintCount);
  for (FallbackMeshTriangleSurfaceHint &hint : outData.triangleSurfaceHints)
  {
    ok = ok && readF32(file, hint.uvCircleCenter[0]);
    ok = ok && readF32(file, hint.uvCircleCenter[1]);
    ok = ok && readF32(file, hint.uvCircleRadius);
    ok = ok && readF32(file, hint.textureMipLevel);
    ok = ok && readU32(file, hint.materialIndex);
    for (uint32_t &value : hint._padding)
      ok = ok && readU32(file, value);
  }

  if (ok)
    outData.skeleton = std::move(skeleton);

  return ok;
}

FallbackMeshData mergeMeshes(const std::vector<FallbackMeshData> &meshes)
{
  FallbackMeshData merged{};
  if (meshes.empty())
    return merged;

  merged = meshes.front();
  if (meshes.size() == 1u)
    return merged;

  merged.vertices.clear();
  merged.indices.clear();
  merged.sections.clear();
  merged.triangleSurfaceHints.clear();
  merged.materialFiles.clear();

  merged.boundsMin[0] = std::numeric_limits<float>::max();
  merged.boundsMin[1] = std::numeric_limits<float>::max();
  merged.boundsMin[2] = std::numeric_limits<float>::max();
  merged.boundsMax[0] = std::numeric_limits<float>::lowest();
  merged.boundsMax[1] = std::numeric_limits<float>::lowest();
  merged.boundsMax[2] = std::numeric_limits<float>::lowest();

  for (const FallbackMeshData &mesh : meshes)
  {
    const uint32_t vertexOffset = static_cast<uint32_t>(merged.vertices.size());
    const uint32_t indexOffset = static_cast<uint32_t>(merged.indices.size());
    merged.vertices.insert(merged.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
    merged.indices.reserve(merged.indices.size() + mesh.indices.size());
    for (uint32_t index : mesh.indices)
      merged.indices.push_back(index + vertexOffset);
    merged.triangleSurfaceHints.insert(merged.triangleSurfaceHints.end(), mesh.triangleSurfaceHints.begin(), mesh.triangleSurfaceHints.end());

    if (!mesh.sections.empty())
    {
      for (const FallbackMeshSection &section : mesh.sections)
      {
        FallbackMeshSection mergedSection = section;
        mergedSection.indexOffset += indexOffset;
        merged.sections.push_back(mergedSection);
      }
    }
    else if (!mesh.indices.empty())
    {
      merged.sections.push_back(FallbackMeshSection{
          .indexOffset = indexOffset,
          .indexCount = static_cast<uint32_t>(mesh.indices.size()),
          .materialIndex = 0u,
      });
    }

    if (mesh.materialFiles.size() > merged.materialFiles.size())
      merged.materialFiles.resize(mesh.materialFiles.size());
    for (size_t materialIndex = 0u; materialIndex < mesh.materialFiles.size(); ++materialIndex)
    {
      if (merged.materialFiles[materialIndex].empty())
        merged.materialFiles[materialIndex] = mesh.materialFiles[materialIndex];
    }

    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      merged.boundsMin[axis] = std::min(merged.boundsMin[axis], mesh.boundsMin[axis]);
      merged.boundsMax[axis] = std::max(merged.boundsMax[axis], mesh.boundsMax[axis]);
    }
  }

  return merged;
}

} // namespace

bool FallbackMeshFile::save(const std::string &path, const FallbackMeshData &data)
{
  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    return false;

  const auto closeFile = [&file]()
  {
    if (file != nullptr)
    {
      std::fclose(file);
      file = nullptr;
    }
  };

  const std::vector<uint32_t> &meshPartToBoneIndex = data.skeleton.getMeshPartToBoneIndex();
  bool ok = true;
  ok = ok && writeU32(file, FALLBACK_MESH_MAGIC);
  ok = ok && writeU32(file, FALLBACK_MESH_VERSION);
  ok = ok && writeU32(file, FALLBACK_MESH_ENDIAN_LITTLE);
  ok = ok && writeMeshPayload(file, data);

  closeFile();
  return ok;
}

bool FallbackMeshFile::save(const std::string &path, const std::vector<FallbackMeshData> &meshes)
{
  if (meshes.size() == 1u)
    return save(path, meshes.front());

  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    return false;

  const auto closeFile = [&file]()
  {
    if (file != nullptr)
    {
      std::fclose(file);
      file = nullptr;
    }
  };

  bool ok = true;
  ok = ok && writeU32(file, FALLBACK_MESH_MAGIC);
  ok = ok && writeU32(file, FALLBACK_MESH_VERSION_COLLECTION);
  ok = ok && writeU32(file, FALLBACK_MESH_ENDIAN_LITTLE);
  ok = ok && writeU32(file, static_cast<uint32_t>(meshes.size()));
  for (const FallbackMeshData &mesh : meshes)
    ok = ok && writeMeshPayload(file, mesh);

  closeFile();
  return ok;
}

bool FallbackMeshFile::load(const std::string &path, std::vector<FallbackMeshData> &outMeshes)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  const auto closeFile = [&file]()
  {
    if (file != nullptr)
    {
      std::fclose(file);
      file = nullptr;
    }
  };

  uint32_t magic = 0u;
  uint32_t version = 0u;
  uint32_t endianTag = 0u;
  bool ok = true;
  ok = ok && readU32(file, magic);
  ok = ok && readU32(file, version);
  ok = ok && readU32(file, endianTag);

  if (!ok || magic != FALLBACK_MESH_MAGIC ||
      (version != FALLBACK_MESH_VERSION &&
       version != FALLBACK_MESH_VERSION_1 &&
       version != FALLBACK_MESH_VERSION_2 &&
       version != FALLBACK_MESH_VERSION_COLLECTION &&
       version != FALLBACK_MESH_VERSION_COLLECTION_3) ||
      endianTag != FALLBACK_MESH_ENDIAN_LITTLE)
  {
    closeFile();
    return false;
  }

  outMeshes.clear();
  if (version == FALLBACK_MESH_VERSION_COLLECTION || version == FALLBACK_MESH_VERSION_COLLECTION_3)
  {
    uint32_t meshCount = 0u;
    ok = ok && readU32(file, meshCount);
    outMeshes.resize(meshCount);
    const uint32_t payloadVersion = version == FALLBACK_MESH_VERSION_COLLECTION_3 ? FALLBACK_MESH_VERSION_2 : FALLBACK_MESH_VERSION;
    for (FallbackMeshData &mesh : outMeshes)
      ok = ok && readMeshPayload(file, payloadVersion, mesh);
  }
  else
  {
    outMeshes.resize(1u);
    ok = ok && readMeshPayload(file, version, outMeshes.front());
  }

  closeFile();
  return ok;
}

bool FallbackMeshFile::load(const std::string &path, FallbackMeshData &outData)
{
  std::vector<FallbackMeshData> meshes;
  if (!load(path, meshes) || meshes.empty())
    return false;

  outData = mergeMeshes(meshes);
  return true;
}

bool FallbackMeshFile::loadEntry(const std::string &path, uint32_t meshIndex, FallbackMeshData &outData)
{
  std::vector<FallbackMeshData> meshes;
  if (!load(path, meshes) || meshIndex >= meshes.size())
    return false;

  outData = std::move(meshes[meshIndex]);
  return true;
}

} // namespace virtualgeometry
