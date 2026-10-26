#include "softwarebvh/SoftwareRayTraceSceneFile.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "algorithm/BinaryIO.hpp"
#include "lz4.h"

namespace softwarebvh
{

namespace
{

struct ByteWriter
{
  std::vector<uint8_t> bytes;

  void writeBytes(const void *data, size_t size)
  {
    const uint8_t *src = static_cast<const uint8_t *>(data);
    bytes.insert(bytes.end(), src, src + size);
  }

  void writeU32(uint32_t value)
  {
    if (!virtualgeometry::isLittleEndian())
      value = virtualgeometry::byteswap(value);
    writeBytes(&value, sizeof(value));
  }

  void writeU64(uint64_t value)
  {
    if (!virtualgeometry::isLittleEndian())
      value = virtualgeometry::byteswap(value);
    writeBytes(&value, sizeof(value));
  }

  void writeF32(float value)
  {
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(bits);
  }

  void writeString(const std::string &value)
  {
    const size_t writeSize = std::min<size_t>(value.size(), std::numeric_limits<uint32_t>::max());
    writeU32(static_cast<uint32_t>(writeSize));
    writeBytes(value.data(), writeSize);
  }
};

struct ByteReader
{
  const uint8_t *data = nullptr;
  size_t size = 0u;
  size_t cursor = 0u;

  bool readBytes(void *outData, size_t readSize)
  {
    if (cursor > size || readSize > size - cursor)
      return false;
    if (readSize != 0u)
      std::memcpy(outData, data + cursor, readSize);
    cursor += readSize;
    return true;
  }

  bool readU32(uint32_t &outValue)
  {
    if (!readBytes(&outValue, sizeof(outValue)))
      return false;
    if (!virtualgeometry::isLittleEndian())
      outValue = virtualgeometry::byteswap(outValue);
    return true;
  }

  bool readU64(uint64_t &outValue)
  {
    if (!readBytes(&outValue, sizeof(outValue)))
      return false;
    if (!virtualgeometry::isLittleEndian())
      outValue = virtualgeometry::byteswap(outValue);
    return true;
  }

  bool readF32(float &outValue)
  {
    uint32_t bits = 0u;
    if (!readU32(bits))
      return false;
    std::memcpy(&outValue, &bits, sizeof(outValue));
    return true;
  }

  bool readString(std::string &outValue)
  {
    uint32_t stringSize = 0u;
    if (!readU32(stringSize) || cursor > size || stringSize > size - cursor)
      return false;
    outValue.assign(reinterpret_cast<const char *>(data + cursor), reinterpret_cast<const char *>(data + cursor + stringSize));
    cursor += stringSize;
    return true;
  }
};

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
  if (!virtualgeometry::isLittleEndian())
    value = virtualgeometry::byteswap(value);
  return writeBytes(file, &value, sizeof(value));
}

bool writeU64(FILE *file, uint64_t value)
{
  if (!virtualgeometry::isLittleEndian())
    value = virtualgeometry::byteswap(value);
  return writeBytes(file, &value, sizeof(value));
}

bool readU32(FILE *file, uint32_t &value)
{
  if (!readBytes(file, &value, sizeof(value)))
    return false;
  if (!virtualgeometry::isLittleEndian())
    value = virtualgeometry::byteswap(value);
  return true;
}

bool readU64(FILE *file, uint64_t &value)
{
  if (!readBytes(file, &value, sizeof(value)))
    return false;
  if (!virtualgeometry::isLittleEndian())
    value = virtualgeometry::byteswap(value);
  return true;
}

void writeMesh(ByteWriter &writer, const virtualgeometry::FallbackMeshData &mesh)
{
  writer.writeU32(mesh.lodLevel);
  writer.writeF32(mesh.targetError);
  writer.writeF32(mesh.relativeError);
  for (float value : mesh.boundsMin)
    writer.writeF32(value);
  for (float value : mesh.boundsMax)
    writer.writeF32(value);

  writer.writeU32(static_cast<uint32_t>(mesh.vertices.size()));
  writer.writeU32(static_cast<uint32_t>(mesh.indices.size()));
  writer.writeU32(static_cast<uint32_t>(mesh.sections.size()));
  writer.writeU32(static_cast<uint32_t>(mesh.materialFiles.size()));
  writer.writeU32(static_cast<uint32_t>(mesh.triangleSurfaceHints.size()));

  for (const virtualgeometry::FallbackMeshVertex &vertex : mesh.vertices)
  {
    for (float value : vertex.position)
      writer.writeF32(value);
    for (float value : vertex.normal)
      writer.writeF32(value);
    for (float value : vertex.uv)
      writer.writeF32(value);
    for (uint32_t value : vertex.boneIndices)
      writer.writeU32(value);
    for (float value : vertex.boneWeights)
      writer.writeF32(value);
    for (float value : vertex.surfaceBaseColorOpacity)
      writer.writeF32(value);
    for (float value : vertex.surfaceNormalRoughness)
      writer.writeF32(value);
    for (float value : vertex.surfaceEmissiveMetallic)
      writer.writeF32(value);
    for (uint32_t value : vertex.surfaceFlags)
      writer.writeU32(value);
  }

  for (uint32_t index : mesh.indices)
    writer.writeU32(index);

  for (const virtualgeometry::FallbackMeshTriangleSurfaceHint &hint : mesh.triangleSurfaceHints)
  {
    writer.writeF32(hint.uvCircleCenter[0]);
    writer.writeF32(hint.uvCircleCenter[1]);
    writer.writeF32(hint.uvCircleRadius);
    writer.writeF32(hint.textureMipLevel);
    writer.writeU32(hint.materialIndex);
    for (uint32_t value : hint._padding)
      writer.writeU32(value);
  }

  for (const virtualgeometry::FallbackMeshSection &section : mesh.sections)
  {
    writer.writeU32(section.indexOffset);
    writer.writeU32(section.indexCount);
    writer.writeU32(section.materialIndex);
  }

  for (const std::string &materialFile : mesh.materialFiles)
    writer.writeString(materialFile);
}

bool readMesh(ByteReader &reader, const uint32_t fileVersion, virtualgeometry::FallbackMeshData &mesh)
{
  uint32_t vertexCount = 0u;
  uint32_t indexCount = 0u;
  uint32_t sectionCount = 0u;
  uint32_t materialCount = 0u;
  uint32_t triangleSurfaceHintCount = 0u;
  bool ok = true;
  ok = ok && reader.readU32(mesh.lodLevel);
  ok = ok && reader.readF32(mesh.targetError);
  ok = ok && reader.readF32(mesh.relativeError);
  for (float &value : mesh.boundsMin)
    ok = ok && reader.readF32(value);
  for (float &value : mesh.boundsMax)
    ok = ok && reader.readF32(value);
  ok = ok && reader.readU32(vertexCount);
  ok = ok && reader.readU32(indexCount);
  ok = ok && reader.readU32(sectionCount);
  ok = ok && reader.readU32(materialCount);
  if (fileVersion >= SOFTWARE_RAYTRACE_SCENE_FILE_VERSION_2)
    ok = ok && reader.readU32(triangleSurfaceHintCount);
  if (!ok)
    return false;

  mesh.vertices.resize(vertexCount);
  mesh.indices.resize(indexCount);
  mesh.sections.resize(sectionCount);
  mesh.materialFiles.resize(materialCount);
  mesh.triangleSurfaceHints.resize(triangleSurfaceHintCount);
  mesh.skeleton.clear();

  for (virtualgeometry::FallbackMeshVertex &vertex : mesh.vertices)
  {
    for (float &value : vertex.position)
      ok = ok && reader.readF32(value);
    for (float &value : vertex.normal)
      ok = ok && reader.readF32(value);
    for (float &value : vertex.uv)
      ok = ok && reader.readF32(value);
    for (uint32_t &value : vertex.boneIndices)
      ok = ok && reader.readU32(value);
    for (float &value : vertex.boneWeights)
      ok = ok && reader.readF32(value);
    if (fileVersion >= SOFTWARE_RAYTRACE_SCENE_FILE_VERSION_2)
    {
      for (float &value : vertex.surfaceBaseColorOpacity)
        ok = ok && reader.readF32(value);
      for (float &value : vertex.surfaceNormalRoughness)
        ok = ok && reader.readF32(value);
      for (float &value : vertex.surfaceEmissiveMetallic)
        ok = ok && reader.readF32(value);
      for (uint32_t &value : vertex.surfaceFlags)
        ok = ok && reader.readU32(value);
    }
  }

  for (uint32_t &index : mesh.indices)
    ok = ok && reader.readU32(index);

  for (virtualgeometry::FallbackMeshTriangleSurfaceHint &hint : mesh.triangleSurfaceHints)
  {
    ok = ok && reader.readF32(hint.uvCircleCenter[0]);
    ok = ok && reader.readF32(hint.uvCircleCenter[1]);
    ok = ok && reader.readF32(hint.uvCircleRadius);
    ok = ok && reader.readF32(hint.textureMipLevel);
    ok = ok && reader.readU32(hint.materialIndex);
    for (uint32_t &value : hint._padding)
      ok = ok && reader.readU32(value);
  }

  for (virtualgeometry::FallbackMeshSection &section : mesh.sections)
  {
    ok = ok && reader.readU32(section.indexOffset);
    ok = ok && reader.readU32(section.indexCount);
    ok = ok && reader.readU32(section.materialIndex);
  }

  for (std::string &materialFile : mesh.materialFiles)
    ok = ok && reader.readString(materialFile);
  return ok;
}

void writeAabb(ByteWriter &writer, const virtualgeometry::AABB &bounds)
{
  for (float value : bounds.minPoint)
    writer.writeF32(value);
  for (float value : bounds.maxPoint)
    writer.writeF32(value);
}

bool readAabb(ByteReader &reader, virtualgeometry::AABB &bounds)
{
  bool ok = true;
  for (float &value : bounds.minPoint)
    ok = ok && reader.readF32(value);
  for (float &value : bounds.maxPoint)
    ok = ok && reader.readF32(value);
  return ok;
}

void writeBuild(ByteWriter &writer, const CWBVHBuilder::BuildResult &build)
{
  writeAabb(writer, build.localBounds);
  writer.writeU32(build.primitiveCount);
  writer.writeU32(build.rootNodeIndex);
  writer.writeU32(static_cast<uint32_t>(build.primitiveRemap.size()));
  writer.writeU32(static_cast<uint32_t>(build.triangleTransforms.size()));
  writer.writeU32(static_cast<uint32_t>(build.canonicalNodes.size()));
  writer.writeU32(static_cast<uint32_t>(build.bvh8Nodes.size()));
  writer.writeU32(static_cast<uint32_t>(build.bvh8LeafRemap.size()));

  for (uint32_t primitiveIndex : build.primitiveRemap)
    writer.writeU32(primitiveIndex);

  for (const CWBVHBuilder::TriBaryTransform &transform : build.triangleTransforms)
  {
    for (float value : transform.row0)
      writer.writeF32(value);
    for (float value : transform.row1)
      writer.writeF32(value);
    for (float value : transform.row2)
      writer.writeF32(value);
  }

  for (const CWBVHBuilder::CanonicalNode &node : build.canonicalNodes)
  {
    writeAabb(writer, node.bounds);
    writer.writeU32(node.parent);
    writer.writeU32(node.left);
    writer.writeU32(node.right);
    writer.writeU32(node.primitiveIndex);
    writer.writeU32(node.subtreePrimitiveCount);
    writer.writeU32(node.isLeaf ? 1u : 0u);
  }

  for (const CWBVHBuilder::BVH8Node &node : build.bvh8Nodes)
  {
    writeAabb(writer, node.bounds);
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        writer.writeF32(node.childMin[childSlot][axis]);
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        writer.writeF32(node.childMax[childSlot][axis]);
      writer.writeU32(node.childIndex[childSlot]);
      writer.writeU32(node.childMeta[childSlot]);
    }
    writer.writeU32(node.cwbvhOutputIndex);
    writer.writeU32(node.internalChildBase);
    writer.writeU32(node.leafBase);
    writer.writeU32(node.validChildMask);
    writer.writeU32(node.internalChildMask);
    writer.writeU32(node.leafChildMask);
  }

  for (uint32_t primitiveIndex : build.bvh8LeafRemap)
    writer.writeU32(primitiveIndex);
}

bool readBuild(ByteReader &reader, CWBVHBuilder::BuildResult &build)
{
  uint32_t primitiveRemapCount = 0u;
  uint32_t triangleTransformCount = 0u;
  uint32_t canonicalNodeCount = 0u;
  uint32_t bvh8NodeCount = 0u;
  uint32_t bvh8LeafRemapCount = 0u;
  bool ok = true;
  ok = ok && readAabb(reader, build.localBounds);
  ok = ok && reader.readU32(build.primitiveCount);
  ok = ok && reader.readU32(build.rootNodeIndex);
  ok = ok && reader.readU32(primitiveRemapCount);
  ok = ok && reader.readU32(triangleTransformCount);
  ok = ok && reader.readU32(canonicalNodeCount);
  ok = ok && reader.readU32(bvh8NodeCount);
  ok = ok && reader.readU32(bvh8LeafRemapCount);
  if (!ok)
    return false;

  build.primitiveRemap.resize(primitiveRemapCount);
  build.triangleTransforms.resize(triangleTransformCount);
  build.canonicalNodes.resize(canonicalNodeCount);
  build.bvh8Nodes.resize(bvh8NodeCount);
  build.bvh8LeafRemap.resize(bvh8LeafRemapCount);

  for (uint32_t &primitiveIndex : build.primitiveRemap)
    ok = ok && reader.readU32(primitiveIndex);

  for (CWBVHBuilder::TriBaryTransform &transform : build.triangleTransforms)
  {
    for (float &value : transform.row0)
      ok = ok && reader.readF32(value);
    for (float &value : transform.row1)
      ok = ok && reader.readF32(value);
    for (float &value : transform.row2)
      ok = ok && reader.readF32(value);
  }

  for (CWBVHBuilder::CanonicalNode &node : build.canonicalNodes)
  {
    uint32_t isLeaf = 0u;
    ok = ok && readAabb(reader, node.bounds);
    ok = ok && reader.readU32(node.parent);
    ok = ok && reader.readU32(node.left);
    ok = ok && reader.readU32(node.right);
    ok = ok && reader.readU32(node.primitiveIndex);
    ok = ok && reader.readU32(node.subtreePrimitiveCount);
    ok = ok && reader.readU32(isLeaf);
    node.isLeaf = isLeaf != 0u;
  }

  for (CWBVHBuilder::BVH8Node &node : build.bvh8Nodes)
  {
    ok = ok && readAabb(reader, node.bounds);
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        ok = ok && reader.readF32(node.childMin[childSlot][axis]);
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        ok = ok && reader.readF32(node.childMax[childSlot][axis]);
      ok = ok && reader.readU32(node.childIndex[childSlot]);
      ok = ok && reader.readU32(node.childMeta[childSlot]);
    }
    ok = ok && reader.readU32(node.cwbvhOutputIndex);
    ok = ok && reader.readU32(node.internalChildBase);
    ok = ok && reader.readU32(node.leafBase);
    ok = ok && reader.readU32(node.validChildMask);
    ok = ok && reader.readU32(node.internalChildMask);
    ok = ok && reader.readU32(node.leafChildMask);
  }

  for (uint32_t &primitiveIndex : build.bvh8LeafRemap)
    ok = ok && reader.readU32(primitiveIndex);
  return ok;
}

std::vector<uint8_t> serializeFallbackPayload(const SoftwareRayTraceSceneInstanceData &entry)
{
  ByteWriter writer;
  writeMesh(writer, entry.fallbackMesh);
  return std::move(writer.bytes);
}

std::vector<uint8_t> serializeCWBVHPayload(const SoftwareRayTraceSceneInstanceData &entry)
{
  ByteWriter writer;
  writeBuild(writer, entry.build);
  return std::move(writer.bytes);
}

void populateEntryMetadata(const SoftwareRayTraceSceneFileEntryHeader &header, SoftwareRayTraceSceneInstanceData &outEntry)
{
  outEntry.objectInstanceId = header.objectInstanceId;
  outEntry.sceneInstanceId = header.sceneInstanceId;
  outEntry.shapeIndex = header.shapeIndex;
  outEntry.lodLevel = header.lodLevel;
  outEntry.flags = header.flags;
  outEntry.sourcePath = header.sourcePath;
}

bool deserializeLegacyPayload(const SoftwareRayTraceSceneFileEntryHeader &header, const uint32_t fileVersion, const std::vector<uint8_t> &bytes, SoftwareRayTraceSceneInstanceData &outEntry)
{
  ByteReader reader{bytes.data(), bytes.size(), 0u};
  bool ok = true;
  ok = ok && reader.readU32(outEntry.objectInstanceId);
  ok = ok && reader.readU32(outEntry.sceneInstanceId);
  ok = ok && reader.readU32(outEntry.shapeIndex);
  ok = ok && reader.readU32(outEntry.lodLevel);
  ok = ok && reader.readU32(outEntry.flags);
  ok = ok && reader.readString(outEntry.sourcePath);
  ok = ok && readMesh(reader, fileVersion, outEntry.fallbackMesh);
  ok = ok && readBuild(reader, outEntry.build);
  if (!ok || reader.cursor != reader.size)
    return false;

  return outEntry.objectInstanceId == header.objectInstanceId && outEntry.sceneInstanceId == header.sceneInstanceId && outEntry.shapeIndex == header.shapeIndex && outEntry.lodLevel == header.lodLevel &&
         outEntry.flags == header.flags && outEntry.sourcePath == header.sourcePath;
}

bool deserializeCWBVHPayload(const std::vector<uint8_t> &bytes, CWBVHBuilder::BuildResult &outBuild)
{
  ByteReader reader{bytes.data(), bytes.size(), 0u};
  return readBuild(reader, outBuild) && reader.cursor == reader.size;
}

bool deserializeFallbackPayload(const uint32_t fileVersion, const std::vector<uint8_t> &bytes, virtualgeometry::FallbackMeshData &outMesh)
{
  ByteReader reader{bytes.data(), bytes.size(), 0u};
  return readMesh(reader, fileVersion, outMesh) && reader.cursor == reader.size;
}

bool compressLz4(const std::vector<uint8_t> &input, std::vector<uint8_t> &output)
{
  if (input.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    return false;

  const int maxCompressedSize = LZ4_compressBound(static_cast<int>(input.size()));
  if (maxCompressedSize <= 0)
    return false;

  output.resize(static_cast<size_t>(maxCompressedSize));
  const int compressedSize = LZ4_compress_default(reinterpret_cast<const char *>(input.data()), reinterpret_cast<char *>(output.data()), static_cast<int>(input.size()), maxCompressedSize);
  if (compressedSize <= 0)
    return false;

  output.resize(static_cast<size_t>(compressedSize));
  return true;
}

bool decompressLz4(const std::vector<uint8_t> &input, uint32_t uncompressedSize, std::vector<uint8_t> &output)
{
  if (input.size() > static_cast<size_t>(std::numeric_limits<int>::max()) || uncompressedSize > static_cast<uint32_t>(std::numeric_limits<int>::max()))
  {
    return false;
  }

  output.resize(uncompressedSize);
  const int decodedSize = LZ4_decompress_safe(reinterpret_cast<const char *>(input.data()), reinterpret_cast<char *>(output.data()), static_cast<int>(input.size()), static_cast<int>(uncompressedSize));
  return decodedSize >= 0 && static_cast<uint32_t>(decodedSize) == uncompressedSize;
}

bool readCompressedPayload(FILE *file, const uint64_t payloadOffset, const uint32_t compressedSize, const uint32_t uncompressedSize, std::vector<uint8_t> &outDecompressed)
{
  if (compressedSize == 0u && uncompressedSize == 0u)
  {
    outDecompressed.clear();
    return false;
  }

  if (std::fseek(file, static_cast<long>(payloadOffset), SEEK_SET) != 0)
    return false;

  std::vector<uint8_t> compressed(compressedSize);
  if (!readBytes(file, compressed.data(), compressed.size()))
    return false;

  return decompressLz4(compressed, uncompressedSize, outDecompressed);
}

bool readLegacyEntryPayload(FILE *file, const uint32_t fileVersion, const SoftwareRayTraceSceneFileEntryHeader &header, SoftwareRayTraceSceneInstanceData &outEntry)
{
  std::vector<uint8_t> decompressed;
  if (!readCompressedPayload(file, header.payloadOffset, header.compressedSize, header.uncompressedSize, decompressed))
    return false;

  return deserializeLegacyPayload(header, fileVersion, decompressed, outEntry);
}

bool readEntryCWBVHPayload(FILE *file, const uint32_t fileVersion, const SoftwareRayTraceSceneFileEntryHeader &header, SoftwareRayTraceSceneInstanceData &outEntry)
{
  if (fileVersion < SOFTWARE_RAYTRACE_SCENE_FILE_VERSION)
  {
    if (!readLegacyEntryPayload(file, fileVersion, header, outEntry))
      return false;
    outEntry.fallbackMesh = {};
    return true;
  }

  populateEntryMetadata(header, outEntry);
  outEntry.fallbackMesh = {};
  outEntry.build = {};
  std::vector<uint8_t> decompressed;
  return readCompressedPayload(file, header.cwbvhPayloadOffset, header.cwbvhCompressedSize, header.cwbvhUncompressedSize, decompressed) && deserializeCWBVHPayload(decompressed, outEntry.build);
}

bool readEntryFallbackPayload(FILE *file, const uint32_t fileVersion, const SoftwareRayTraceSceneFileEntryHeader &header, SoftwareRayTraceSceneInstanceData &outEntry)
{
  if (fileVersion < SOFTWARE_RAYTRACE_SCENE_FILE_VERSION)
  {
    if (!readLegacyEntryPayload(file, fileVersion, header, outEntry))
      return false;
    outEntry.build = {};
    return true;
  }

  populateEntryMetadata(header, outEntry);
  outEntry.fallbackMesh = {};
  outEntry.build = {};
  std::vector<uint8_t> decompressed;
  return readCompressedPayload(file, header.fallbackPayloadOffset, header.fallbackCompressedSize, header.fallbackUncompressedSize, decompressed) &&
         deserializeFallbackPayload(fileVersion, decompressed, outEntry.fallbackMesh);
}

bool readEntryPayload(FILE *file, const uint32_t fileVersion, const SoftwareRayTraceSceneFileEntryHeader &header, SoftwareRayTraceSceneInstanceData &outEntry)
{
  if (fileVersion < SOFTWARE_RAYTRACE_SCENE_FILE_VERSION)
    return readLegacyEntryPayload(file, fileVersion, header, outEntry);

  populateEntryMetadata(header, outEntry);
  outEntry.fallbackMesh = {};
  outEntry.build = {};

  std::vector<uint8_t> decompressedBuild;
  std::vector<uint8_t> decompressedFallback;
  return readCompressedPayload(file, header.cwbvhPayloadOffset, header.cwbvhCompressedSize, header.cwbvhUncompressedSize, decompressedBuild) && deserializeCWBVHPayload(decompressedBuild, outEntry.build) &&
         readCompressedPayload(file, header.fallbackPayloadOffset, header.fallbackCompressedSize, header.fallbackUncompressedSize, decompressedFallback) &&
         deserializeFallbackPayload(fileVersion, decompressedFallback, outEntry.fallbackMesh);
}

bool readHeaderFromFile(FILE *file, SoftwareRayTraceSceneFileHeader &outHeader)
{
  uint32_t magic = 0u;
  uint32_t version = 0u;
  uint32_t endianTag = 0u;
  uint32_t headerSize = 0u;
  uint32_t entryCount = 0u;
  bool ok = true;
  ok = ok && readU32(file, magic);
  ok = ok && readU32(file, version);
  ok = ok && readU32(file, endianTag);
  ok = ok && readU32(file, headerSize);
  ok = ok && readU32(file, entryCount);
  if (!ok || magic != SOFTWARE_RAYTRACE_SCENE_FILE_MAGIC || (version != SOFTWARE_RAYTRACE_SCENE_FILE_VERSION && version != SOFTWARE_RAYTRACE_SCENE_FILE_VERSION_2 && version != SOFTWARE_RAYTRACE_SCENE_FILE_VERSION_1) ||
      endianTag != SOFTWARE_RAYTRACE_SCENE_FILE_ENDIAN_LITTLE || headerSize < 20u)
  {
    return false;
  }

  outHeader = SoftwareRayTraceSceneFileHeader{};
  outHeader.version = version;
  outHeader.headerSize = headerSize;
  outHeader.entries.resize(entryCount);
  for (SoftwareRayTraceSceneFileEntryHeader &entry : outHeader.entries)
  {
    uint32_t sourcePathSize = 0u;
    ok = ok && readU32(file, entry.objectInstanceId);
    ok = ok && readU32(file, entry.sceneInstanceId);
    ok = ok && readU32(file, entry.shapeIndex);
    ok = ok && readU32(file, entry.lodLevel);
    ok = ok && readU32(file, entry.flags);
    ok = ok && readU32(file, sourcePathSize);
    if (version >= SOFTWARE_RAYTRACE_SCENE_FILE_VERSION)
    {
      ok = ok && readU64(file, entry.cwbvhPayloadOffset);
      ok = ok && readU32(file, entry.cwbvhCompressedSize);
      ok = ok && readU32(file, entry.cwbvhUncompressedSize);
      ok = ok && readU64(file, entry.fallbackPayloadOffset);
      ok = ok && readU32(file, entry.fallbackCompressedSize);
      ok = ok && readU32(file, entry.fallbackUncompressedSize);
      entry.payloadOffset = entry.cwbvhPayloadOffset;
      entry.compressedSize = entry.cwbvhCompressedSize + entry.fallbackCompressedSize;
      entry.uncompressedSize = entry.cwbvhUncompressedSize + entry.fallbackUncompressedSize;
    }
    else
    {
      ok = ok && readU64(file, entry.payloadOffset);
      ok = ok && readU32(file, entry.compressedSize);
      ok = ok && readU32(file, entry.uncompressedSize);
      entry.cwbvhPayloadOffset = entry.payloadOffset;
      entry.cwbvhCompressedSize = entry.compressedSize;
      entry.cwbvhUncompressedSize = entry.uncompressedSize;
      entry.fallbackPayloadOffset = entry.payloadOffset;
      entry.fallbackCompressedSize = entry.compressedSize;
      entry.fallbackUncompressedSize = entry.uncompressedSize;
    }
    if (!ok)
      return false;

    entry.sourcePath.resize(sourcePathSize);
    if (!readBytes(file, entry.sourcePath.data(), entry.sourcePath.size()))
      return false;
  }

  return std::ftell(file) == static_cast<long>(headerSize);
}

} // namespace

SoftwareRayTraceSceneInstanceData SoftwareRayTraceSceneFile::buildInstance(
    const virtualgeometry::FallbackMeshData &fallbackMesh,
    virtualgeometry::InstanceId objectInstanceId,
    virtualgeometry::SceneInstanceId sceneInstanceId,
    uint32_t shapeIndex,
    std::string sourcePath)
{
  SoftwareRayTraceSceneInstanceData data{};
  data.objectInstanceId = objectInstanceId;
  data.sceneInstanceId = sceneInstanceId;
  data.shapeIndex = shapeIndex;
  data.lodLevel = fallbackMesh.lodLevel;
  data.sourcePath = std::move(sourcePath);
  data.fallbackMesh = fallbackMesh;
  data.build = CWBVHBuilder::buildOffline(fallbackMesh);
  return data;
}

bool SoftwareRayTraceSceneFile::save(const std::string &path, const SoftwareRayTraceSceneInstanceData &data)
{
  return save(path, std::vector<SoftwareRayTraceSceneInstanceData>{data});
}

bool SoftwareRayTraceSceneFile::save(const std::string &path, const std::vector<SoftwareRayTraceSceneInstanceData> &data)
{
  struct Payload
  {
    std::vector<uint8_t> cwbvhUncompressed;
    std::vector<uint8_t> cwbvhCompressed;
    std::vector<uint8_t> fallbackUncompressed;
    std::vector<uint8_t> fallbackCompressed;
  };

  std::vector<Payload> payloads(data.size());
  std::vector<SoftwareRayTraceSceneFileEntryHeader> headers(data.size());
  uint64_t headerSize = 20u;
  for (size_t entryIndex = 0u; entryIndex < data.size(); ++entryIndex)
  {
    const SoftwareRayTraceSceneInstanceData &entry = data[entryIndex];
    payloads[entryIndex].cwbvhUncompressed = serializeCWBVHPayload(entry);
    payloads[entryIndex].fallbackUncompressed = serializeFallbackPayload(entry);
    if (payloads[entryIndex].cwbvhUncompressed.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) ||
        payloads[entryIndex].fallbackUncompressed.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) || !compressLz4(payloads[entryIndex].cwbvhUncompressed, payloads[entryIndex].cwbvhCompressed) ||
        !compressLz4(payloads[entryIndex].fallbackUncompressed, payloads[entryIndex].fallbackCompressed) || payloads[entryIndex].cwbvhCompressed.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) ||
        payloads[entryIndex].fallbackCompressed.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    {
      return false;
    }

    headers[entryIndex].objectInstanceId = entry.objectInstanceId;
    headers[entryIndex].sceneInstanceId = entry.sceneInstanceId;
    headers[entryIndex].shapeIndex = entry.shapeIndex;
    headers[entryIndex].lodLevel = entry.lodLevel;
    headers[entryIndex].flags = entry.flags;
    headers[entryIndex].sourcePath = entry.sourcePath;
    headers[entryIndex].cwbvhCompressedSize = static_cast<uint32_t>(payloads[entryIndex].cwbvhCompressed.size());
    headers[entryIndex].cwbvhUncompressedSize = static_cast<uint32_t>(payloads[entryIndex].cwbvhUncompressed.size());
    headers[entryIndex].fallbackCompressedSize = static_cast<uint32_t>(payloads[entryIndex].fallbackCompressed.size());
    headers[entryIndex].fallbackUncompressedSize = static_cast<uint32_t>(payloads[entryIndex].fallbackUncompressed.size());
    if (headers[entryIndex].cwbvhCompressedSize > std::numeric_limits<uint32_t>::max() - headers[entryIndex].fallbackCompressedSize ||
        headers[entryIndex].cwbvhUncompressedSize > std::numeric_limits<uint32_t>::max() - headers[entryIndex].fallbackUncompressedSize)
    {
      return false;
    }
    headers[entryIndex].compressedSize = headers[entryIndex].cwbvhCompressedSize + headers[entryIndex].fallbackCompressedSize;
    headers[entryIndex].uncompressedSize = headers[entryIndex].cwbvhUncompressedSize + headers[entryIndex].fallbackUncompressedSize;
    headerSize += 56u + entry.sourcePath.size();
  }

  if (headerSize > std::numeric_limits<uint32_t>::max())
    return false;

  uint64_t payloadOffset = headerSize;
  for (size_t entryIndex = 0u; entryIndex < headers.size(); ++entryIndex)
  {
    SoftwareRayTraceSceneFileEntryHeader &header = headers[entryIndex];
    header.cwbvhPayloadOffset = payloadOffset;
    header.payloadOffset = payloadOffset;
    payloadOffset += payloads[entryIndex].cwbvhCompressed.size();
    header.fallbackPayloadOffset = payloadOffset;
    payloadOffset += payloads[entryIndex].fallbackCompressed.size();
  }

  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    return false;

  bool ok = true;
  ok = ok && writeU32(file, SOFTWARE_RAYTRACE_SCENE_FILE_MAGIC);
  ok = ok && writeU32(file, SOFTWARE_RAYTRACE_SCENE_FILE_VERSION);
  ok = ok && writeU32(file, SOFTWARE_RAYTRACE_SCENE_FILE_ENDIAN_LITTLE);
  ok = ok && writeU32(file, static_cast<uint32_t>(headerSize));
  ok = ok && writeU32(file, static_cast<uint32_t>(headers.size()));
  for (const SoftwareRayTraceSceneFileEntryHeader &header : headers)
  {
    ok = ok && writeU32(file, header.objectInstanceId);
    ok = ok && writeU32(file, header.sceneInstanceId);
    ok = ok && writeU32(file, header.shapeIndex);
    ok = ok && writeU32(file, header.lodLevel);
    ok = ok && writeU32(file, header.flags);
    ok = ok && writeU32(file, static_cast<uint32_t>(header.sourcePath.size()));
    ok = ok && writeU64(file, header.cwbvhPayloadOffset);
    ok = ok && writeU32(file, header.cwbvhCompressedSize);
    ok = ok && writeU32(file, header.cwbvhUncompressedSize);
    ok = ok && writeU64(file, header.fallbackPayloadOffset);
    ok = ok && writeU32(file, header.fallbackCompressedSize);
    ok = ok && writeU32(file, header.fallbackUncompressedSize);
    ok = ok && writeBytes(file, header.sourcePath.data(), header.sourcePath.size());
  }
  for (const Payload &payload : payloads)
  {
    ok = ok && writeBytes(file, payload.cwbvhCompressed.data(), payload.cwbvhCompressed.size());
    ok = ok && writeBytes(file, payload.fallbackCompressed.data(), payload.fallbackCompressed.size());
  }

  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::loadHeader(const std::string &path, SoftwareRayTraceSceneFileHeader &outHeader)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;
  const bool ok = readHeaderFromFile(file, outHeader);
  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::load(const std::string &path, SoftwareRayTraceSceneInstanceData &outData)
{
  std::vector<SoftwareRayTraceSceneInstanceData> entries;
  if (!load(path, entries) || entries.empty())
    return false;
  outData = std::move(entries.front());
  return true;
}

bool SoftwareRayTraceSceneFile::load(const std::string &path, std::vector<SoftwareRayTraceSceneInstanceData> &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  SoftwareRayTraceSceneFileHeader header{};
  bool ok = readHeaderFromFile(file, header);
  outData.clear();
  outData.resize(ok ? header.entries.size() : 0u);
  for (uint32_t entryIndex = 0u; ok && entryIndex < static_cast<uint32_t>(header.entries.size()); ++entryIndex)
  {
    ok = ok && readEntryPayload(file, header.version, header.entries[entryIndex], outData[entryIndex]);
  }

  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::loadEntry(const std::string &path, uint32_t entryIndex, SoftwareRayTraceSceneInstanceData &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  SoftwareRayTraceSceneFileHeader header{};
  bool ok = readHeaderFromFile(file, header);
  ok = ok && entryIndex < header.entries.size();
  if (ok)
    ok = readEntryPayload(file, header.version, header.entries[entryIndex], outData);

  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::loadEntryCWBVH(const std::string &path, uint32_t entryIndex, SoftwareRayTraceSceneInstanceData &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  SoftwareRayTraceSceneFileHeader header{};
  bool ok = readHeaderFromFile(file, header);
  ok = ok && entryIndex < header.entries.size();
  if (ok)
    ok = readEntryCWBVHPayload(file, header.version, header.entries[entryIndex], outData);

  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::loadEntryFallbackMesh(const std::string &path, uint32_t entryIndex, SoftwareRayTraceSceneInstanceData &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  SoftwareRayTraceSceneFileHeader header{};
  bool ok = readHeaderFromFile(file, header);
  ok = ok && entryIndex < header.entries.size();
  if (ok)
    ok = readEntryFallbackPayload(file, header.version, header.entries[entryIndex], outData);

  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::findEntryIndex(const SoftwareRayTraceSceneFileHeader &header, uint32_t lodLevel, uint32_t shapeIndex, uint32_t &outEntryIndex)
{
  for (uint32_t entryIndex = 0u; entryIndex < static_cast<uint32_t>(header.entries.size()); ++entryIndex)
  {
    const SoftwareRayTraceSceneFileEntryHeader &entry = header.entries[entryIndex];
    if (entry.lodLevel == lodLevel && entry.shapeIndex == shapeIndex)
    {
      outEntryIndex = entryIndex;
      return true;
    }
  }
  return false;
}

bool SoftwareRayTraceSceneFile::findEntryIndexForShape(const SoftwareRayTraceSceneFileHeader &header, uint32_t shapeIndex, uint32_t &outEntryIndex)
{
  for (uint32_t entryIndex = 0u; entryIndex < static_cast<uint32_t>(header.entries.size()); ++entryIndex)
  {
    if (header.entries[entryIndex].shapeIndex == shapeIndex)
    {
      outEntryIndex = entryIndex;
      return true;
    }
  }
  return false;
}

bool SoftwareRayTraceSceneFile::loadEntry(const std::string &path, uint32_t lodLevel, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  SoftwareRayTraceSceneFileHeader header{};
  uint32_t entryIndex = UINT32_MAX;
  bool ok = readHeaderFromFile(file, header);
  ok = ok && findEntryIndex(header, lodLevel, shapeIndex, entryIndex);
  if (ok)
    ok = readEntryPayload(file, header.version, header.entries[entryIndex], outData);

  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::loadEntryCWBVH(const std::string &path, uint32_t lodLevel, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  SoftwareRayTraceSceneFileHeader header{};
  uint32_t entryIndex = UINT32_MAX;
  bool ok = readHeaderFromFile(file, header);
  ok = ok && findEntryIndex(header, lodLevel, shapeIndex, entryIndex);
  if (ok)
    ok = readEntryCWBVHPayload(file, header.version, header.entries[entryIndex], outData);

  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::loadEntryFallbackMesh(const std::string &path, uint32_t lodLevel, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  SoftwareRayTraceSceneFileHeader header{};
  uint32_t entryIndex = UINT32_MAX;
  bool ok = readHeaderFromFile(file, header);
  ok = ok && findEntryIndex(header, lodLevel, shapeIndex, entryIndex);
  if (ok)
    ok = readEntryFallbackPayload(file, header.version, header.entries[entryIndex], outData);

  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::loadEntryForShape(const std::string &path, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  SoftwareRayTraceSceneFileHeader header{};
  uint32_t entryIndex = UINT32_MAX;
  bool ok = readHeaderFromFile(file, header);
  ok = ok && findEntryIndexForShape(header, shapeIndex, entryIndex);
  if (ok)
    ok = readEntryPayload(file, header.version, header.entries[entryIndex], outData);

  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::loadEntryCWBVHForShape(const std::string &path, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  SoftwareRayTraceSceneFileHeader header{};
  uint32_t entryIndex = UINT32_MAX;
  bool ok = readHeaderFromFile(file, header);
  ok = ok && findEntryIndexForShape(header, shapeIndex, entryIndex);
  if (ok)
    ok = readEntryCWBVHPayload(file, header.version, header.entries[entryIndex], outData);

  std::fclose(file);
  return ok;
}

bool SoftwareRayTraceSceneFile::loadEntryFallbackMeshForShape(const std::string &path, uint32_t shapeIndex, SoftwareRayTraceSceneInstanceData &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;

  SoftwareRayTraceSceneFileHeader header{};
  uint32_t entryIndex = UINT32_MAX;
  bool ok = readHeaderFromFile(file, header);
  ok = ok && findEntryIndexForShape(header, shapeIndex, entryIndex);
  if (ok)
    ok = readEntryFallbackPayload(file, header.version, header.entries[entryIndex], outData);

  std::fclose(file);
  return ok;
}

} // namespace softwarebvh
