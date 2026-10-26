#include "softwarebvh/SWRTFallbackMeshFile.hpp"

#include <cstdio>
#include <cstring>

#include "algorithm/BinaryIO.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"

namespace softwarebvh
{

namespace
{

bool writeBytes(FILE *file, const void *data, const size_t size)
{
  return size == 0u || std::fwrite(data, 1u, size, file) == size;
}

bool readBytes(FILE *file, void *data, const size_t size)
{
  return size == 0u || std::fread(data, 1u, size, file) == size;
}

bool writeU32(FILE *file, uint32_t value)
{
  if (!virtualgeometry::isLittleEndian())
  {
    value = virtualgeometry::byteswap(value);
  }
  return writeBytes(file, &value, sizeof(value));
}

bool readU32(FILE *file, uint32_t &value)
{
  if (!readBytes(file, &value, sizeof(value)))
  {
    return false;
  }
  if (!virtualgeometry::isLittleEndian())
  {
    value = virtualgeometry::byteswap(value);
  }
  return true;
}

bool writeF32(FILE *file, const float value)
{
  static_assert(sizeof(float) == sizeof(uint32_t), "Expected 32-bit float storage");
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return writeU32(file, bits);
}

bool readF32(FILE *file, float &value)
{
  uint32_t bits = 0u;
  if (!readU32(file, bits))
  {
    return false;
  }
  std::memcpy(&value, &bits, sizeof(value));
  return true;
}

bool writeCanonicalNode(FILE *file, const CWBVHBuilder::CanonicalNode &node)
{
  bool ok = true;
  for (const float value : node.bounds.minPoint)
  {
    ok = ok && writeF32(file, value);
  }
  for (const float value : node.bounds.maxPoint)
  {
    ok = ok && writeF32(file, value);
  }
  ok = ok && writeU32(file, node.parent);
  ok = ok && writeU32(file, node.left);
  ok = ok && writeU32(file, node.right);
  ok = ok && writeU32(file, node.primitiveIndex);
  ok = ok && writeU32(file, node.subtreePrimitiveCount);
  ok = ok && writeU32(file, node.isLeaf ? 1u : 0u);
  return ok;
}

bool readCanonicalNode(FILE *file, CWBVHBuilder::CanonicalNode &node)
{
  bool ok = true;
  for (float &value : node.bounds.minPoint)
  {
    ok = ok && readF32(file, value);
  }
  for (float &value : node.bounds.maxPoint)
  {
    ok = ok && readF32(file, value);
  }
  uint32_t isLeaf = 0u;
  ok = ok && readU32(file, node.parent);
  ok = ok && readU32(file, node.left);
  ok = ok && readU32(file, node.right);
  ok = ok && readU32(file, node.primitiveIndex);
  ok = ok && readU32(file, node.subtreePrimitiveCount);
  ok = ok && readU32(file, isLeaf);
  node.isLeaf = isLeaf != 0u;
  return ok;
}

bool writeTriTransform(FILE *file, const CWBVHBuilder::TriBaryTransform &transform)
{
  bool ok = true;
  for (const float value : transform.row0)
  {
    ok = ok && writeF32(file, value);
  }
  for (const float value : transform.row1)
  {
    ok = ok && writeF32(file, value);
  }
  for (const float value : transform.row2)
  {
    ok = ok && writeF32(file, value);
  }
  return ok;
}

bool readTriTransform(FILE *file, CWBVHBuilder::TriBaryTransform &transform)
{
  bool ok = true;
  for (float &value : transform.row0)
  {
    ok = ok && readF32(file, value);
  }
  for (float &value : transform.row1)
  {
    ok = ok && readF32(file, value);
  }
  for (float &value : transform.row2)
  {
    ok = ok && readF32(file, value);
  }
  return ok;
}

bool writeFallbackBlob(FILE *file, const std::string &path, const std::vector<virtualgeometry::FallbackMeshData> &meshes)
{
  const std::string tempFallbackPath = path + ".fallback.tmp";

  const bool saved = virtualgeometry::FallbackMeshFile::save(tempFallbackPath, meshes);
  std::FILE *fallbackFile = saved ? std::fopen(tempFallbackPath.c_str(), "rb") : nullptr;
  bool ok = saved && fallbackFile != nullptr;

  if (ok)
  {
    std::fseek(fallbackFile, 0, SEEK_END);
    const long fallbackFileSize = std::ftell(fallbackFile);
    std::fseek(fallbackFile, 0, SEEK_SET);
    ok = ok && writeU32(file, fallbackFileSize < 0 ? 0u : static_cast<uint32_t>(fallbackFileSize));
    std::vector<uint8_t> fallbackBytes(fallbackFileSize < 0 ? 0u : static_cast<size_t>(fallbackFileSize));
    if (!fallbackBytes.empty())
    {
      ok = ok && readBytes(fallbackFile, fallbackBytes.data(), fallbackBytes.size());
      ok = ok && writeBytes(file, fallbackBytes.data(), fallbackBytes.size());
    }
  }

  if (fallbackFile != nullptr)
    std::fclose(fallbackFile);
  std::remove(tempFallbackPath.c_str());
  return ok;
}

bool readFallbackBlob(FILE *file, const std::string &path, std::vector<virtualgeometry::FallbackMeshData> &outMeshes)
{
  uint32_t fallbackBlobSize = 0u;
  bool ok = readU32(file, fallbackBlobSize);
  std::vector<uint8_t> fallbackBytes(fallbackBlobSize);
  if (ok && !fallbackBytes.empty())
    ok = ok && readBytes(file, fallbackBytes.data(), fallbackBytes.size());

  const std::string tempFallbackPath = path + ".fallback.load.tmp";
  if (ok)
  {
    FILE *tempFallbackFile = std::fopen(tempFallbackPath.c_str(), "wb");
    if (tempFallbackFile == nullptr)
    {
      ok = false;
    }
    else
    {
      ok = ok && writeBytes(tempFallbackFile, fallbackBytes.data(), fallbackBytes.size());
      std::fclose(tempFallbackFile);
    }
  }

  if (ok)
    ok = virtualgeometry::FallbackMeshFile::load(tempFallbackPath, outMeshes);

  std::remove(tempFallbackPath.c_str());
  return ok;
}

bool writeBuildResult(FILE *file, const CWBVHBuilder::BuildResult &build)
{
  bool ok = true;
  ok = ok && writeU32(file, build.primitiveCount);
  ok = ok && writeU32(file, build.rootNodeIndex);
  ok = ok && writeU32(file, static_cast<uint32_t>(build.primitiveRemap.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(build.triangleTransforms.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(build.canonicalNodes.size()));

  for (const uint32_t primitiveIndex : build.primitiveRemap)
    ok = ok && writeU32(file, primitiveIndex);
  for (const CWBVHBuilder::TriBaryTransform &transform : build.triangleTransforms)
    ok = ok && writeTriTransform(file, transform);
  for (const CWBVHBuilder::CanonicalNode &node : build.canonicalNodes)
    ok = ok && writeCanonicalNode(file, node);
  return ok;
}

bool readBuildResult(FILE *file, const virtualgeometry::FallbackMeshData &fallbackMesh, CWBVHBuilder::BuildResult &outBuild)
{
  uint32_t primitiveRemapCount = 0u;
  uint32_t triangleTransformCount = 0u;
  uint32_t canonicalNodeCount = 0u;

  bool ok = true;
  ok = ok && readU32(file, outBuild.primitiveCount);
  ok = ok && readU32(file, outBuild.rootNodeIndex);
  ok = ok && readU32(file, primitiveRemapCount);
  ok = ok && readU32(file, triangleTransformCount);
  ok = ok && readU32(file, canonicalNodeCount);

  outBuild.localBounds.minPoint[0] = fallbackMesh.boundsMin[0];
  outBuild.localBounds.minPoint[1] = fallbackMesh.boundsMin[1];
  outBuild.localBounds.minPoint[2] = fallbackMesh.boundsMin[2];
  outBuild.localBounds.maxPoint[0] = fallbackMesh.boundsMax[0];
  outBuild.localBounds.maxPoint[1] = fallbackMesh.boundsMax[1];
  outBuild.localBounds.maxPoint[2] = fallbackMesh.boundsMax[2];

  outBuild.primitiveRemap.resize(primitiveRemapCount);
  outBuild.triangleTransforms.resize(triangleTransformCount);
  outBuild.canonicalNodes.resize(canonicalNodeCount);

  for (uint32_t &primitiveIndex : outBuild.primitiveRemap)
    ok = ok && readU32(file, primitiveIndex);
  for (CWBVHBuilder::TriBaryTransform &transform : outBuild.triangleTransforms)
    ok = ok && readTriTransform(file, transform);
  for (CWBVHBuilder::CanonicalNode &node : outBuild.canonicalNodes)
    ok = ok && readCanonicalNode(file, node);

  return ok;
}

bool validateBuildResult(const virtualgeometry::FallbackMeshData &fallbackMesh, const CWBVHBuilder::BuildResult &build)
{
  const uint32_t primitiveCount = static_cast<uint32_t>(fallbackMesh.indices.size() / 3u);
  if (build.primitiveCount != primitiveCount ||
      build.primitiveRemap.size() != primitiveCount ||
      build.triangleTransforms.size() != primitiveCount)
  {
    return false;
  }

  if (primitiveCount == 0u)
  {
    return build.rootNodeIndex == CWBVHBuilder::INVALID_NODE && build.canonicalNodes.empty();
  }

  if (build.rootNodeIndex >= build.canonicalNodes.size())
  {
    return false;
  }

  uint32_t leafCount = 0u;
  for (const CWBVHBuilder::CanonicalNode &node : build.canonicalNodes)
  {
    if (node.isLeaf)
    {
      if (node.primitiveIndex >= primitiveCount || node.subtreePrimitiveCount != 1u)
      {
        return false;
      }
      ++leafCount;
    }
    else if (node.left >= build.canonicalNodes.size() || node.right >= build.canonicalNodes.size())
    {
      return false;
    }
  }

  return leafCount == primitiveCount;
}

void repairBuildResultIfNeeded(SWRTFallbackMeshData &entry)
{
  if (!validateBuildResult(entry.fallbackMesh, entry.build))
  {
    entry.build = CWBVHBuilder::build(entry.fallbackMesh);
  }
}

} // namespace

SWRTFallbackMeshData SWRTFallbackMeshFile::build(const virtualgeometry::FallbackMeshData &fallbackMesh)
{
  SWRTFallbackMeshData data{};
  data.fallbackMesh = fallbackMesh;
  data.build = CWBVHBuilder::build(fallbackMesh);
  return data;
}

bool SWRTFallbackMeshFile::save(const std::string &path, const SWRTFallbackMeshData &data)
{
  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
  {
    return false;
  }

  const auto closeFile = [&file]()
  {
    if (file != nullptr)
    {
      std::fclose(file);
      file = nullptr;
    }
  };

  bool ok = true;
  ok = ok && writeU32(file, SWRT_FALLBACK_MESH_MAGIC);
  ok = ok && writeU32(file, SWRT_FALLBACK_MESH_VERSION);
  ok = ok && writeU32(file, SWRT_FALLBACK_MESH_ENDIAN_LITTLE);
  ok = ok && writeFallbackBlob(file, path, std::vector<virtualgeometry::FallbackMeshData>{data.fallbackMesh});
  ok = ok && writeBuildResult(file, data.build);

  closeFile();
  return ok;
}

bool SWRTFallbackMeshFile::save(const std::string &path, const std::vector<SWRTFallbackMeshData> &data)
{
  if (data.size() == 1u)
    return save(path, data.front());

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
  ok = ok && writeU32(file, SWRT_FALLBACK_MESH_MAGIC);
  ok = ok && writeU32(file, SWRT_FALLBACK_MESH_VERSION_COLLECTION);
  ok = ok && writeU32(file, SWRT_FALLBACK_MESH_ENDIAN_LITTLE);
  ok = ok && writeU32(file, static_cast<uint32_t>(data.size()));
  for (const SWRTFallbackMeshData &entry : data)
  {
    ok = ok && writeFallbackBlob(file, path, std::vector<virtualgeometry::FallbackMeshData>{entry.fallbackMesh});
    ok = ok && writeBuildResult(file, entry.build);
  }

  closeFile();
  return ok;
}

bool SWRTFallbackMeshFile::load(const std::string &path, std::vector<SWRTFallbackMeshData> &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
  {
    return false;
  }

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

  if (!ok || magic != SWRT_FALLBACK_MESH_MAGIC ||
      (version != SWRT_FALLBACK_MESH_VERSION && version != SWRT_FALLBACK_MESH_VERSION_COLLECTION) ||
      endianTag != SWRT_FALLBACK_MESH_ENDIAN_LITTLE)
  {
    closeFile();
    return false;
  }

  outData.clear();
  if (version == SWRT_FALLBACK_MESH_VERSION_COLLECTION)
  {
    uint32_t entryCount = 0u;
    ok = ok && readU32(file, entryCount);
    outData.resize(entryCount);
    for (SWRTFallbackMeshData &entry : outData)
    {
      std::vector<virtualgeometry::FallbackMeshData> meshes;
      ok = ok && readFallbackBlob(file, path, meshes);
      if (meshes.size() != 1u)
      {
        ok = false;
        break;
      }
      entry.fallbackMesh = std::move(meshes.front());
      ok = ok && readBuildResult(file, entry.fallbackMesh, entry.build);
      if (ok)
        repairBuildResultIfNeeded(entry);
    }
  }
  else
  {
    outData.resize(1u);
    std::vector<virtualgeometry::FallbackMeshData> meshes;
    ok = ok && readFallbackBlob(file, path, meshes);
    if (meshes.empty())
    {
      ok = false;
    }
    else
    {
      outData.front().fallbackMesh = std::move(meshes.front());
      ok = ok && readBuildResult(file, outData.front().fallbackMesh, outData.front().build);
      if (ok)
        repairBuildResultIfNeeded(outData.front());
    }
  }

  closeFile();
  return ok;
}

bool SWRTFallbackMeshFile::load(const std::string &path, SWRTFallbackMeshData &outData)
{
  std::vector<SWRTFallbackMeshData> entries;
  if (!load(path, entries) || entries.empty())
    return false;

  outData = std::move(entries.front());
  return true;
}

bool SWRTFallbackMeshFile::loadEntry(const std::string &path, uint32_t meshIndex, SWRTFallbackMeshData &outData)
{
  std::vector<SWRTFallbackMeshData> entries;
  if (!load(path, entries) || meshIndex >= entries.size())
    return false;

  outData = std::move(entries[meshIndex]);
  return true;
}

} // namespace softwarebvh
