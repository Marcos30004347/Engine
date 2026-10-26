// // ============================================================================
// // VirtualGeometryScene CPU-driven Simulation Test
// // ============================================================================
// // Uses VirtualGeometryScene + a real VulkanRHI.
// // The GPU culling pipeline is simulated on the CPU:
// //   - GPU buffers (counters, queues, page-table, priorities) are read/written
// //     via rhi->bufferRead / rhi->bufferWrite at the correct byte offsets.
// //   - processHierarchyNodes runs on the CPU reading/writing those buffers.
// //   - VirtualGeometryScene::updatePageStreaming is called when missing pages
// //     are detected, installing pages and patching the hierarchy buffer.
// //
// // For each mesh:
// //   1. Encode .obj → .virtualgeometry file
// //   2. registerObjectForStreaming + instantiateObjectInstance
// //   3. Place camera so the object is fully visible
// //   4. Run init → setupRootNodes → processHierarchyNodes loop (CPU)
// //      → updatePageStreaming whenever new pages are needed
// //   5. processClusters to collect visible clusters
// //   6. Validate + report
// // ============================================================================

// #include <algorithm>
// #include <cassert>
// #include <cmath>
// #include <cstring>
// #include <iomanip>
// #include <iostream>
// #include <stdexcept>
// #include <string>
// #include <vector>

// #include "./utils/File.hpp"
// #include "math/math.hpp"
// #include "rendering/core/Camera.hpp"
// #include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
// #include "virtualgeometry/VirtualGeometryBuilder.hpp"
// #include "virtualgeometry/VirtualGeometryData.hpp"
// #include "virtualgeometry/VirtualGeometryEncoder.hpp"
// #include "virtualgeometry/VirtualGeometryFile.hpp"
// #include "virtualgeometry/VirtualGeometryScene.hpp"

// using namespace virtualgeometry;

// // ============================================================================
// // Constants mirrored from the WGSL shader
// // ============================================================================

// static constexpr uint32_t SENTINEL_VALUE = 0xFFFFFFFFu;
// static constexpr uint32_t MAX_QUEUE_ELEMENTS = 1u << 20;
// static constexpr uint32_t MAX_VISIBLE_CLUSTERS = 1u << 20;

// // ============================================================================
// // CPU-side mirror structs
// // ============================================================================

// struct QueueElement
// {
//   uint32_t index;
//   uint32_t instanceIndex;
//   uint32_t pageIndex;
//   uint32_t _padding;
// };
// static_assert(sizeof(QueueElement) == 16, "");

// struct CullingCounters
// {
//   uint32_t hierarchyQueueSize;
//   uint32_t clusterQueueSize;
//   uint32_t visibleClusterCount;
//   uint32_t iteration;
// };
// static_assert(sizeof(CullingCounters) == 16, "");

// // ============================================================================
// // LOD helpers
// // ============================================================================

// static float getInstanceScale(const math::Mat4f &m)
// {
//   float x = m.at(0, 0), y = m.at(1, 0), z = m.at(2, 0);
//   return std::sqrt(x * x + y * y + z * z);
// }

// static float calculateProjectedError(
//     const math::Vec3f &centerLocal,
//     float radiusLocal,
//     float errorLocal,
//     const math::Mat4f &modelMatrix,
//     const math::Vec3f &viewPos,
//     float projY1,
//     uint32_t viewportH,
//     float nearPlane)
// {
//   float worldScale = getInstanceScale(modelMatrix);
//   math::Vec4f wc4 = modelMatrix * math::Vec4f(centerLocal[0], centerLocal[1], centerLocal[2], 1.0f);
//   math::Vec3f worldCenter(wc4[0], wc4[1], wc4[2]);
//   float worldError = errorLocal * worldScale;
//   float worldRadius = radiusLocal * worldScale;
//   float distToCenter = (worldCenter - viewPos).length();
//   float distToSurface = distToCenter - worldRadius;
//   float distClamped = std::max(distToSurface, nearPlane);
//   float pe = worldError / distClamped * projY1 * 0.5f * static_cast<float>(viewportH);
//   return pe;
// }

// static bool shouldRenderAtLOD(
//     const VirtualGeometryHierarchy &node,
//     const math::Mat4f &modelMatrix,
//     const math::Vec3f &viewPos,
//     float projY1,
//     uint32_t viewportH,
//     float nearPlane,
//     float errorThreshold)
// {
//   math::Vec3f center(node.center_x, node.center_y, node.center_z);
//   float selfError = calculateProjectedError(center, node.radius, node.min_lod_error, modelMatrix, viewPos, projY1, viewportH, nearPlane);
//   float parentError = calculateProjectedError(center, node.radius, node.max_parent_lod_error, modelMatrix, viewPos, projY1, viewportH, nearPlane);

//   printf(
//       "     leaf=%s, shouldRenderAtLOD min_lod_error=%f, parent_lod_error=%f, "
//       "(selfError=%f < errorThreshold=%f && parentError=%f >= errorThreshold=%f)=%s\n",
//       (node.flags & 1L) ? "true" : "false",
//       node.min_lod_error,
//       node.max_parent_lod_error == std::numeric_limits<float>::max() ? -1.0f : node.max_parent_lod_error,
//       selfError,
//       errorThreshold,
//       parentError,
//       errorThreshold,
//       (selfError < errorThreshold && parentError >= errorThreshold) ? "true" : "false");

//   return selfError < errorThreshold && parentError >= errorThreshold;
// }

// static void computeHierarchyAABB(const std::vector<VirtualGeometryHierarchy> &nodes, math::Vec3f &outMin, math::Vec3f &outMax)
// {
//   outMin = math::Vec3f(1e30f, 1e30f, 1e30f);
//   outMax = math::Vec3f(-1e30f, -1e30f, -1e30f);
//   for (const auto &n : nodes)
//   {
//     if (outMin[0] > n.min_x)
//       outMin[0] = n.min_x;
//     if (outMin[1] > n.min_y)
//       outMin[1] = n.min_y;
//     if (outMin[2] > n.min_z)
//       outMin[2] = n.min_z;
//     if (outMax[0] < n.max_x)
//       outMax[0] = n.max_x;
//     if (outMax[1] < n.max_y)
//       outMax[1] = n.max_y;
//     if (outMax[2] < n.max_z)
//       outMax[2] = n.max_z;
//   }
// }

// // ============================================================================
// // Test result
// // ============================================================================
// struct TestResult
// {
//   std::string meshName;
//   uint32_t visibleClusters = 0;
//   uint32_t iterations = 0;
//   uint32_t pagesInstalled = 0;
//   bool success = false;
// };

// // ============================================================================
// // GPU page decoder
// //
// // Mirrors the exact binary layout written by PageBuffer::encode and the
// // encoding done by VirtualGeometryCompressor, so we can validate what the
// // GPU holds before the WGSL shader ever runs.
// //
// // Binary layout (uint32 words):
// //
// //  Header (6 words)
// //    [0] num_meshlets
// //    [1] position_data_size  (words)
// //    [2] normal_data_size    (words)
// //    [3] uv_data_size        (words)
// //    [4] index_data_size     (words)
// //    [5] dependency_count
// //
// //  Meshlet descriptor table: num_meshlets × 17 words
// //    Per meshlet (word offsets from descriptor base):
// //     [0]  posOff    = start_vertex_position_bit / 32   → word offset into posData
// //     [1]  posWords
// //     [2]  normOff   = start_vertex_attribute_id        → element index into normData
// //     [3]  normCount = vertex_count
// //     [4]  uvOff     = start_vertex_attribute_id * 2    → element index into uvData
// //     [5]  uvCount   = vertex_count * 2
// //     [6]  idxOff    = start_index_id / 4               → word offset into idxData
// //     [7]  idxWords
// //     [8]  vertex_count
// //     [9]  triangle_count
// //    [10]  bits_per_channel_x
// //    [11]  bits_per_channel_y
// //    [12]  bits_per_channel_z
// //    [13]  quantization_factor
// //    [14]  min_position_x    (float bits — raw quantised integer stored as f32)
// //    [15]  min_position_y
// //    [16]  min_position_z
// //
// //  posData  block (position_data_size words)  — packed XYZ bitstream
// //  normData block (normal_data_size words)    — one uint32 per vertex (oct 2×16 SNORM)
// //  uvData   block (uv_data_size words)        — bitcast<f32> pairs
// //  idxData  block (index_data_size words)     — 4 uint8 per word
// //  deps     block (dependency_count words)
// //
// // Position decode (mirrors VirtualGeometryCompressor::decodePositions and
// //                           WGSL decodePosition):
// //   world = (minQ_as_f32 + float(delta_q)) / ((1 << qFactor) * unitScale)
// //   where minQ_as_f32 is the raw quantised integer cast to float,
// //   and delta_q is the per-vertex unsigned offset read from the bitstream.
// //
// // Normal decode (mirrors WGSL decodeNormal / VirtualGeometryCompressor::decodeNormals):
// //   unpack two int16 SNORM → octahedral decode → normalize
// //
// // Index decode: byte (indexIdx % 4) of word at (idxOff + indexIdx/4)
// // ============================================================================

// // ---------------------------------------------------------------------------
// // cpuExtractBits — mirrors WGSL extractBits(baseWord, bitOffset, numBits)
// //   baseWord  : absolute word index of the start of this meshlet's data
// //   bitOffset : bit offset *from* baseWord (NOT from page start)
// // ---------------------------------------------------------------------------
// static uint32_t cpuExtractBits(const uint32_t *pageWords, uint32_t baseWord, uint32_t bitOffset, uint32_t numBits)
// {
//   if (numBits == 0u)
//     return 0u;
//   uint32_t wordIdx = baseWord + bitOffset / 32u;
//   uint32_t bitInWord = bitOffset % 32u;
//   uint32_t value = pageWords[wordIdx] >> bitInWord;
//   if (bitInWord + numBits > 32u)
//   {
//     uint32_t overflow = (bitInWord + numBits) - 32u;
//     value |= pageWords[wordIdx + 1u] << (numBits - overflow);
//   }
//   uint32_t mask = (numBits == 32u) ? 0xFFFFFFFFu : ((1u << numBits) - 1u);
//   return value & mask;
// }

// // ---------------------------------------------------------------------------
// // cpuDecodePosition — mirrors WGSL decodePosition
// //   posDataBase : absolute word index of the posData block start
// //   posOff      : word offset of THIS meshlet's data within posData (descriptor[0])
// //   vertexIdx   : which vertex to decode
// // ---------------------------------------------------------------------------
// static math::Vec3f cpuDecodePosition(
//     const uint32_t *pageWords,
//     uint32_t posDataBase,
//     uint32_t posOff,
//     uint32_t bitsX,
//     uint32_t bitsY,
//     uint32_t bitsZ,
//     uint32_t qFactor,
//     float minQX,
//     float minQY,
//     float minQZ,
//     float unitScale,
//     uint32_t vertexIdx)
// {
//   const uint32_t bitsPerVertex = bitsX + bitsY + bitsZ;
//   const uint32_t startBit = vertexIdx * bitsPerVertex; // bit offset within this meshlet's block
//   const uint32_t base = posDataBase + posOff;          // absolute word base

//   uint32_t bc = startBit;
//   uint32_t qx = cpuExtractBits(pageWords, base, bc, bitsX);
//   bc += bitsX;
//   uint32_t qy = cpuExtractBits(pageWords, base, bc, bitsY);
//   bc += bitsY;
//   uint32_t qz = cpuExtractBits(pageWords, base, bc, bitsZ);

//   // Decode: world = (minQ + delta) / dequantScale
//   // minQ is already a float holding the raw quantised integer (e.g. −312.0)
//   float dequantScale = static_cast<float>(1u << qFactor) * unitScale;
//   return math::Vec3f((minQX + static_cast<float>(qx)) / dequantScale, (minQY + static_cast<float>(qy)) / dequantScale, (minQZ + static_cast<float>(qz)) / dequantScale);
// }

// // ---------------------------------------------------------------------------
// // cpuDecodeNormal — mirrors WGSL decodeNormal
// //   normDataBase : absolute word index of the normData block start
// //   normOff      : element offset of THIS meshlet within normData (descriptor[2])
// //   vertexIdx    : which vertex
// // ---------------------------------------------------------------------------
// static math::Vec3f cpuDecodeNormal(const uint32_t *pageWords, uint32_t normDataBase, uint32_t normOff, uint32_t vertexIdx)
// {
//   uint32_t packed = pageWords[normDataBase + normOff + vertexIdx];

//   // Sign-extend two 16-bit values
//   uint32_t rawX = packed & 0xFFFFu;
//   uint32_t rawY = (packed >> 16u) & 0xFFFFu;
//   int32_t sX = (rawX >= 32768u) ? (int32_t)rawX - 65536 : (int32_t)rawX;
//   int32_t sY = (rawY >= 32768u) ? (int32_t)rawY - 65536 : (int32_t)rawY;

//   float ox = std::max(-1.0f, std::min(1.0f, (float)sX / 32767.0f));
//   float oy = std::max(-1.0f, std::min(1.0f, (float)sY / 32767.0f));

//   // Octahedral decode (mirrors WGSL)
//   float nx = ox, ny = oy, nz = 1.0f - std::abs(ox) - std::abs(oy);
//   if (nz < 0.0f)
//   {
//     float ox2 = nx, oy2 = ny;
//     nx = (1.0f - std::abs(oy2)) * (ox2 >= 0.0f ? 1.0f : -1.0f);
//     ny = (1.0f - std::abs(ox2)) * (oy2 >= 0.0f ? 1.0f : -1.0f);
//   }
//   float len = std::sqrt(nx * nx + ny * ny + nz * nz);
//   if (len > 1e-6f)
//   {
//     nx /= len;
//     ny /= len;
//     nz /= len;
//   }
//   return math::Vec3f(nx, ny, nz);
// }

// // ---------------------------------------------------------------------------
// // cpuDecodeIndex — CORRECT index decode using absolute byte offset
// //
// // ⚠ BUG HISTORY:
// //
// //   v1 (original): used descriptor idxOff directly — broke on non-word-aligned
// //      meshlets because idxOff = start_index_id / 4 discards the sub-word byte.
// //
// //   v2 (after encoder padding): encoder now pads each meshlet's indices to a
// //      word boundary (triCount*3 rounded up to next multiple of 4), so
// //      start_index_id is always a multiple of 4 and idxOff is lossless.
// //      However, the test's cumulativeIndexBytes must advance by the PADDED
// //      stride (idxWords * 4), NOT the raw triCount*3, or it overshoots into
// //      the padding bytes of the previous meshlet's slot.
// //
// // Parameters:
// //   idxDataBase    : absolute word index of the idxData block start
// //   startIndexByte : accumulated PADDED byte offset of THIS meshlet's first index
// //                    (= sum of idxWords*4 for all previous meshlets)
// //   indexIdx       : 0-based corner index within this meshlet
// // ---------------------------------------------------------------------------
// static uint32_t cpuDecodeIndex(const uint32_t *pageWords, uint32_t idxDataBase, uint32_t startIndexByte, uint32_t indexIdx)
// {
//   const uint32_t bytePos = startIndexByte + indexIdx;
//   const uint32_t word = pageWords[idxDataBase + bytePos / 4u];
//   const uint32_t shift = (bytePos % 4u) * 8u;
//   return (word >> shift) & 0xFFu;
// }

// // ---------------------------------------------------------------------------
// // cpuDecodeIndex_ShaderFormula — intentionally reproduces the WGSL shader's
// // naive formula so the test can report both the correct value and what the
// // shader would produce, pinpointing shader mismatches.
// //   idxOff   : descriptor word [6] = start_index_id / 4
// //   indexIdx : 0-based corner index
// // ---------------------------------------------------------------------------
// static uint32_t cpuDecodeIndex_ShaderFormula(const uint32_t *pageWords, uint32_t idxDataBase, uint32_t idxOff, uint32_t indexIdx)
// {
//   const uint32_t word = pageWords[idxDataBase + idxOff + indexIdx / 4u];
//   const uint32_t shift = (indexIdx % 4u) * 8u;
//   return (word >> shift) & 0xFFu;
// }

// // ---------------------------------------------------------------------------
// // cpuDecodeUV — mirrors WGSL decodeUV
// //   uvDataBase : absolute word index of uvData block start
// //   uvOff      : element offset (descriptor[4])
// //   vertexIdx  : which vertex
// // ---------------------------------------------------------------------------
// static math::Vec2f cpuDecodeUV(const uint32_t *pageWords, uint32_t uvDataBase, uint32_t uvOff, uint32_t vertexIdx)
// {
//   uint32_t base = uvDataBase + uvOff + vertexIdx * 2u;
//   float u, v;
//   std::memcpy(&u, &pageWords[base + 0u], sizeof(float));
//   std::memcpy(&v, &pageWords[base + 1u], sizeof(float));
//   auto res = math::Vec2f();
//   res[0] = u;
//   res[1] = v;
//   return res;
// }

// // ---------------------------------------------------------------------------
// // decodeMeshletsFromPageWords
// //
// // Decodes all meshlets from a decompressed page word buffer.
// // Uses the CORRECT byte-level index offset (accumulated from triangle_count*3)
// // rather than the descriptor's truncated word offset.
// //
// // Also records, per meshlet, whether the shader formula would produce different
// // index values — this directly exposes which meshlets have the alignment bug.
// // ---------------------------------------------------------------------------
// // ---------------------------------------------------------------------------
// // Decoded geometry types
// // ---------------------------------------------------------------------------
// struct DecodedVertex
// {
//   math::Vec3f pos;
//   math::Vec3f normal;
//   math::Vec2f uv;
// };

// struct IndexMismatchInfo
// {
//   uint32_t cornerIdx;
//   uint32_t correctValue;
//   uint32_t shaderValue;
// };

// struct DecodedMeshlet
// {
//   std::vector<DecodedVertex> vertices;
//   std::vector<uint32_t> indices;                   // decoded with CORRECT formula
//   std::vector<IndexMismatchInfo> shaderMismatches; // corners where shader formula differs
//   uint32_t startIndexByte;                         // true byte offset — exposed so caller can double-check
//   uint32_t idxOffFromDesc;                         // descriptor word [6] — the value the shader uses
//   uint32_t meshletIdxInPage;
//   uint32_t pageId;
// };

// static std::vector<DecodedMeshlet> decodeMeshletsFromPageWords(const uint32_t *pageWords, uint32_t pageWordCount, uint32_t pageId, float unitScale)
// {
//   std::vector<DecodedMeshlet> result;
//   if (pageWordCount < 6u)
//     return result;

//   const uint32_t numMeshlets = pageWords[0];
//   const uint32_t posDataSize = pageWords[1];
//   const uint32_t normDataSize = pageWords[2];
//   const uint32_t uvDataSize = pageWords[3];
//   // [4] index_data_size, [5] dependency_count — not needed here

//   // Absolute word offsets of each data block (mirrors PageBuffer::encode order)
//   const uint32_t meshletTableBase = 6u;
//   const uint32_t posDataBase = meshletTableBase + numMeshlets * 17u;
//   const uint32_t normDataBase = posDataBase + posDataSize;
//   const uint32_t uvDataBase = normDataBase + normDataSize;
//   const uint32_t idxDataBase = uvDataBase + uvDataSize;

//   result.reserve(numMeshlets);

//   // Track the true byte offset of the current meshlet's first index.
//   // This is identical to start_index_id in VirtualGeometryCompressor::encode.
//   uint32_t cumulativeIndexBytes = 0u;

//   for (uint32_t mi = 0; mi < numMeshlets; ++mi)
//   {
//     const uint32_t descBase = meshletTableBase + mi * 17u;
//     if (descBase + 17u > pageWordCount)
//       break;

//     const uint32_t posOff = pageWords[descBase + 0];
//     const uint32_t normOff = pageWords[descBase + 2];
//     const uint32_t uvOff = pageWords[descBase + 4];
//     const uint32_t idxOff = pageWords[descBase + 6];   // = start_index_id / 4 (may lose alignment)
//     const uint32_t idxWords = pageWords[descBase + 7]; // padded word count — used to advance byte cursor
//     const uint32_t vertCount = pageWords[descBase + 8];
//     const uint32_t triCount = pageWords[descBase + 9];
//     const uint32_t bitsX = pageWords[descBase + 10];
//     const uint32_t bitsY = pageWords[descBase + 11];
//     const uint32_t bitsZ = pageWords[descBase + 12];
//     const uint32_t qFactor = pageWords[descBase + 13];

//     float minQX, minQY, minQZ;
//     std::memcpy(&minQX, &pageWords[descBase + 14], sizeof(float));
//     std::memcpy(&minQY, &pageWords[descBase + 15], sizeof(float));
//     std::memcpy(&minQZ, &pageWords[descBase + 16], sizeof(float));

//     DecodedMeshlet dm;
//     dm.meshletIdxInPage = mi;
//     dm.pageId = pageId;
//     dm.startIndexByte = cumulativeIndexBytes;
//     dm.idxOffFromDesc = idxOff;

//     // Decode vertices
//     dm.vertices.resize(vertCount);
//     for (uint32_t vi = 0; vi < vertCount; ++vi)
//     {
//       dm.vertices[vi].pos = cpuDecodePosition(pageWords, posDataBase, posOff, bitsX, bitsY, bitsZ, qFactor, minQX, minQY, minQZ, unitScale, vi);
//       dm.vertices[vi].normal = cpuDecodeNormal(pageWords, normDataBase, normOff, vi);
//       dm.vertices[vi].uv = cpuDecodeUV(pageWords, uvDataBase, uvOff, vi);
//     }

//     // Decode indices using CORRECT byte-level formula, and cross-check with
//     // the shader's naive formula to surface any alignment mismatch.
//     const uint32_t cornerCount = triCount * 3u;
//     dm.indices.resize(cornerCount);
//     for (uint32_t ci = 0; ci < cornerCount; ++ci)
//     {
//       const uint32_t correct = cpuDecodeIndex(pageWords, idxDataBase, cumulativeIndexBytes, ci);
//       const uint32_t shader = cpuDecodeIndex_ShaderFormula(pageWords, idxDataBase, idxOff, ci);
//       dm.indices[ci] = correct;
//       if (correct != shader)
//         dm.shaderMismatches.push_back({ci, correct, shader});
//     }

//     cumulativeIndexBytes += idxWords * 4u; // advance by PADDED stride, not raw triCount*3

//     result.push_back(std::move(dm));
//   }
//   return result;
// }

// // ============================================================================
// // validateInstalledPages
// //
// // Three-pass validation for every currently installed page:
// //
// //   Pass A — Raw byte comparison: disk-decompressed bytes == GPU pagesBuffer bytes
// //            Catches any corruption in the write path.
// //
// //   Pass B — PageTableEntry field-by-field: bufferOffset, size, clusterOffset,
// //            clusterCount, isInstalled all match the in-memory PageAllocation.
// //            Catches bugs in updatePageTableEntry.
// //
// //   Pass C — Geometry decode cross-check:
// //            Re-decode every meshlet from the GPU page words (same bit operations
// //            the WGSL shader will execute) and verify:
// //              • All triangle indices are in [0, vertexCount)
// //              • All decoded positions are finite
// //              • All decoded normals are finite and approximately unit-length
// //              • All decoded UVs are finite
// //            Then compare GPU-decoded results against disk-decoded results to
// //            confirm the GPU holds the correct geometry and that the decode
// //            logic matches.  Any discrepancy here points to a shader offset bug.
// //
// // Returns true only when all installed pages pass all three passes.
// // ============================================================================
// static bool
// validateInstalledPages(rendering::RHI *rhi, VirtualGeometryScene &scene, const std::string &objectName, const std::string &vgFilePath, float unitScale, const std::string &label)
// {
//   std::cout << "\n  [GPU Validation – " << label << "]\n";

//   auto runtimeDataIt = scene.files.find(objectName);
//   if (runtimeDataIt == scene.files.end())
//   {
//     std::cout << "  ERROR: object not found in scene\n";
//     return false;
//   }
//   auto *runtimeData = runtimeDataIt.value();
//   const uint32_t pageCount = static_cast<uint32_t>(runtimeData->pageAllocations.size());
//   if (pageCount == 0)
//   {
//     std::cout << "  (no pages)\n";
//     return true;
//   }

//   VirtualGeometryFile vgFile(vgFilePath, /*write=*/false);
//   if (!vgFile.isOpen())
//   {
//     std::cout << "  ERROR: cannot open VG file: " << vgFilePath << "\n";
//     return false;
//   }
//   const uint32_t maxPageSize = vgFile.getMaxPageSize();

//   // Read entire GPU page table in one shot
//   std::vector<PageTableEntry> gpuPageTable(pageCount);
//   rhi->bufferRead(
//       scene.pageTableBuffer,
//       0,
//       pageCount * sizeof(PageTableEntry),
//       [&](const void *d)
//       {
//         std::memcpy(gpuPageTable.data(), d, pageCount * sizeof(PageTableEntry));
//       });

//   bool allPassed = true;
//   uint32_t validated = 0;

//   for (uint32_t pageId = 0; pageId < pageCount; ++pageId)
//   {
//     const PageAllocation &alloc = runtimeData->pageAllocations[pageId];
//     if (!alloc.isInstalled)
//       continue;
//     ++validated;
//     bool pagePassed = true;

//     std::cout << "    Page " << pageId << "  bufOffset=" << alloc.bufferOffset << "  size=" << alloc.size << "  clusters=[" << alloc.clusterOffset << ","
//               << alloc.clusterOffset + alloc.clusterCount << ")\n";

//     // ----------------------------------------------------------------
//     // Pass A — raw byte comparison
//     // ----------------------------------------------------------------
//     std::vector<uint8_t> diskBytes(maxPageSize, 0u);
//     VirtualGeometryStreamedPage streamedPage;
//     if (!vgFile.streamPageRaw(pageId, diskBytes.data(), maxPageSize, streamedPage))
//     {
//       std::cout << "      FAIL [PassA]: streamPageRaw failed\n";
//       allPassed = false;
//       continue;
//     }
//     const uint32_t pageDataBytes = streamedPage.getDataSizeInBytes();
//     if (pageDataBytes == 0)
//     {
//       std::cout << "      WARN: 0-byte page, skipping\n";
//       continue;
//     }

//     std::vector<uint8_t> gpuBytes(pageDataBytes, 0u);
//     rhi->bufferRead(
//         scene.pagesBuffer,
//         alloc.bufferOffset,
//         pageDataBytes,
//         [&](const void *d)
//         {
//           std::memcpy(gpuBytes.data(), d, pageDataBytes);
//         });

//     {
//       uint32_t mismatches = 0, firstAt = UINT32_MAX;
//       for (uint32_t b = 0; b < pageDataBytes; ++b)
//         if (diskBytes[b] != gpuBytes[b])
//         {
//           if (firstAt == UINT32_MAX)
//             firstAt = b;
//           ++mismatches;
//         }

//       if (mismatches == 0)
//       {
//         std::cout << "      ✓ PassA: " << pageDataBytes << " bytes match (disk == GPU)\n";
//       }
//       else
//       {
//         std::cout << "      ✗ PassA: " << mismatches << "/" << pageDataBytes << " bytes differ, first at byte " << firstAt << " (disk=0x" << std::hex << std::setw(2)
//                   << std::setfill('0') << (uint32_t)diskBytes[firstAt] << " gpu=0x" << std::setw(2) << std::setfill('0') << (uint32_t)gpuBytes[firstAt] << std::dec << ")\n";

//         uint32_t wS = (firstAt >= 8) ? firstAt - 8 : 0;
//         uint32_t wE = std::min(firstAt + 8, pageDataBytes);
//         std::cout << "        disk[" << wS << ".." << wE - 1 << "]: ";
//         for (uint32_t b = wS; b < wE; ++b)
//           std::cout << std::hex << std::setw(2) << std::setfill('0') << (uint32_t)diskBytes[b] << " ";
//         std::cout << "\n        gpu [" << wS << ".." << wE - 1 << "]: ";
//         for (uint32_t b = wS; b < wE; ++b)
//           std::cout << std::hex << std::setw(2) << std::setfill('0') << (uint32_t)gpuBytes[b] << " ";
//         std::cout << std::dec << "\n";
//         pagePassed = false;
//       }
//     }

//     // ----------------------------------------------------------------
//     // Pass B — PageTableEntry field comparison
//     // ----------------------------------------------------------------
//     {
//       const PageTableEntry &ge = gpuPageTable[pageId];
//       bool ok = true;
//       auto chk = [&](const char *name, uint32_t exp, uint32_t got)
//       {
//         if (exp != got)
//         {
//           std::cout << "      ✗ PassB PageTable." << name << ": expected " << exp << " got " << got << "\n";
//           return false;
//         }
//         return true;
//       };
//       ok &= chk("bufferOffset", alloc.bufferOffset, ge.bufferOffset);
//       ok &= chk("size", alloc.size, ge.size);
//       ok &= chk("clusterOffset", alloc.clusterOffset, ge.clusterOffset);
//       ok &= chk("clusterCount", alloc.clusterCount, ge.clusterCount);
//       ok &= chk("isInstalled", 1u, ge.isInstalled);
//       if (ok)
//         std::cout << "      ✓ PassB: PageTable entry matches allocation\n";
//       else
//         pagePassed = false;
//     }

//     // ----------------------------------------------------------------
//     // Pass C — decode and cross-compare meshlet geometry
//     //
//     // Three sub-checks:
//     //   C1 — CORRECT index decode: every index must be in [0, vertCount).
//     //        Uses accumulated byte offset, NOT the descriptor's truncated word.
//     //   C2 — Vertex sanity: finite values, unit normals, GPU==disk match.
//     //   C3 — SHADER formula check: reports meshlets where the WGSL decodeIndex
//     //        naive formula (idxOff + indexIdx/4) differs from the correct result.
//     //        These are the meshlets that will produce wrong geometry in the GPU.
//     // ----------------------------------------------------------------
//     {
//       const uint32_t pageWordCount = pageDataBytes / 4u;

//       std::vector<uint32_t> gpuWords(pageWordCount), diskWords(pageWordCount);
//       std::memcpy(gpuWords.data(), gpuBytes.data(), pageDataBytes);
//       std::memcpy(diskWords.data(), diskBytes.data(), pageDataBytes);

//       auto gpuMeshlets = decodeMeshletsFromPageWords(gpuWords.data(), pageWordCount, pageId, unitScale);
//       auto diskMeshlets = decodeMeshletsFromPageWords(diskWords.data(), pageWordCount, pageId, unitScale);

//       if (gpuMeshlets.size() != diskMeshlets.size())
//       {
//         std::cout << "      ✗ PassC: meshlet count gpu=" << gpuMeshlets.size() << " disk=" << diskMeshlets.size() << "\n";
//         pagePassed = false;
//       }
//       else
//       {
//         std::cout << "      PassC: decoding " << gpuMeshlets.size() << " meshlet(s)...\n";

//         constexpr float kPosTol = 1e-3f;
//         constexpr float kNormalTol = 0.02f;
//         constexpr float kUVTol = 1e-5f;

//         uint32_t meshletsFailed = 0;
//         uint32_t shaderBugMeshlets = 0;

//         for (size_t mi = 0; mi < gpuMeshlets.size(); ++mi)
//         {
//           const DecodedMeshlet &gm = gpuMeshlets[mi];
//           const DecodedMeshlet &dm = diskMeshlets[mi];
//           bool mOk = true;

//           const uint32_t vertCount = static_cast<uint32_t>(gm.vertices.size());
//           const uint32_t cornerCount = static_cast<uint32_t>(gm.indices.size());

//           // C3 — Shader formula mismatch (report first so it's visible even if C1 passes)
//           if (!gm.shaderMismatches.empty())
//           {
//             ++shaderBugMeshlets;
//             const uint32_t byteAlign = gm.startIndexByte % 4u;
//             const auto &sm = gm.shaderMismatches[0];
//             std::cout << "        ⚠ C3 meshlet[" << mi << "]"
//                       << "  startByte=" << gm.startIndexByte << "  align=" << byteAlign << "/4"
//                       << "  descIdxOff=" << gm.idxOffFromDesc << "  shader_diff=" << gm.shaderMismatches.size() << " corner(s)"
//                       << "  [first: corner" << sm.cornerIdx << " shader=" << sm.shaderValue << " correct=" << sm.correctValue;
//             if (sm.shaderValue >= vertCount)
//               std::cout << " ← OOB in shader! (vertCount=" << vertCount << ")";
//             std::cout << "]\n";
//           }

//           // C1 — Index range (using CORRECT byte-level decode)
//           for (uint32_t ci = 0; ci < cornerCount; ++ci)
//           {
//             if (gm.indices[ci] >= vertCount)
//             {
//               std::cout << "        ✗ C1 meshlet[" << mi << "] corner " << ci << ": correct index " << gm.indices[ci] << " OOB [0," << vertCount << ")"
//                         << "  startByte=" << gm.startIndexByte << "\n";
//               mOk = false;
//               break;
//             }
//           }

//           // C2 — Per-vertex sanity + GPU vs disk cross-compare
//           uint32_t posMismatch = 0, normMismatch = 0, uvMismatch = 0;

//           for (uint32_t vi = 0; vi < vertCount; ++vi)
//           {
//             const DecodedVertex &gv = gm.vertices[vi];
//             const DecodedVertex &dv = dm.vertices[vi];

//             bool posFinite = std::isfinite(gv.pos[0]) && std::isfinite(gv.pos[1]) && std::isfinite(gv.pos[2]);
//             bool normFinite = std::isfinite(gv.normal[0]) && std::isfinite(gv.normal[1]) && std::isfinite(gv.normal[2]);
//             bool uvFinite = std::isfinite(gv.uv[0]) && std::isfinite(gv.uv[1]);

//             if (!posFinite)
//             {
//               std::cout << "        ✗ C2 meshlet[" << mi << "] v" << vi << ": non-finite pos\n";
//               mOk = false;
//             }
//             if (!normFinite)
//             {
//               std::cout << "        ✗ C2 meshlet[" << mi << "] v" << vi << ": non-finite normal\n";
//               mOk = false;
//             }
//             if (!uvFinite)
//             {
//               std::cout << "        ✗ C2 meshlet[" << mi << "] v" << vi << ": non-finite UV\n";
//               mOk = false;
//             }

//             if (normFinite)
//             {
//               float nlen = std::sqrt(gv.normal[0] * gv.normal[0] + gv.normal[1] * gv.normal[1] + gv.normal[2] * gv.normal[2]);
//               if (std::abs(nlen - 1.0f) > kNormalTol)
//               {
//                 std::cout << "        ✗ C2 meshlet[" << mi << "] v" << vi << ": normal length=" << nlen << "\n";
//                 mOk = false;
//               }
//             }

//             auto d3 = [](const math::Vec3f &a, const math::Vec3f &b)
//             {
//               float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
//               return std::sqrt(dx * dx + dy * dy + dz * dz);
//             };
//             auto d2 = [](const math::Vec2f &a, const math::Vec2f &b)
//             {
//               float dx = a[0] - b[0], dy = a[1] - b[1];
//               return std::sqrt(dx * dx + dy * dy);
//             };

//             if (posFinite && d3(gv.pos, dv.pos) > kPosTol)
//               ++posMismatch;
//             if (normFinite && d3(gv.normal, dv.normal) > kNormalTol)
//               ++normMismatch;
//             if (uvFinite && d2(gv.uv, dv.uv) > kUVTol)
//               ++uvMismatch;
//           }

//           if (posMismatch || normMismatch || uvMismatch)
//           {
//             std::cout << "        ✗ C2 meshlet[" << mi << "] vertex mismatch:"
//                       << "  pos=" << posMismatch << "  norm=" << normMismatch << "  uv=" << uvMismatch << " / " << vertCount << "\n";
//             for (uint32_t vi = 0; vi < vertCount && posMismatch; ++vi)
//             {
//               auto d3 = [](const math::Vec3f &a, const math::Vec3f &b)
//               {
//                 float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
//                 return std::sqrt(dx * dx + dy * dy + dz * dz);
//               };
//               if (d3(gm.vertices[vi].pos, dm.vertices[vi].pos) > kPosTol)
//               {
//                 const auto &gv = gm.vertices[vi], &dv = dm.vertices[vi];
//                 std::cout << "          first pos v" << vi << ": gpu=(" << gv.pos[0] << "," << gv.pos[1] << "," << gv.pos[2] << ")"
//                           << " disk=(" << dv.pos[0] << "," << dv.pos[1] << "," << dv.pos[2] << ")\n";
//                 break;
//               }
//             }
//             mOk = false;
//           }

//           if (!mOk)
//             ++meshletsFailed;
//         }

//         // Page-level summary
//         if (meshletsFailed == 0 && shaderBugMeshlets == 0)
//         {
//           std::cout << "      ✓ PassC: all " << gpuMeshlets.size() << " meshlet(s) decode correctly; shader formula also correct\n";
//         }
//         else
//         {
//           if (meshletsFailed == 0)
//             std::cout << "      ✓ PassC geometry: all " << gpuMeshlets.size() << " meshlet(s) correct with fixed byte-offset formula\n";
//           else
//           {
//             std::cout << "      ✗ PassC geometry: " << meshletsFailed << "/" << gpuMeshlets.size() << " meshlet(s) have real decode errors\n";
//             pagePassed = false;
//           }

//           if (shaderBugMeshlets > 0)
//           {
//             std::cout << "      ⚠ PassC SHADER BUG: " << shaderBugMeshlets << "/" << gpuMeshlets.size() << " meshlet(s) get wrong indices from WGSL decodeIndex\n"
//                       << "        Root cause: actual_index_offset = start_index_id/4 discards the\n"
//                       << "        sub-word byte alignment when triangle_count*3 is not divisible by 4.\n"
//                       << "        Fix options:\n"
//                       << "          A) Pad each meshlet's index count to next multiple of 4 in encoder.\n"
//                       << "          B) Store start_index_id (byte offset) instead of /4 in descriptor[6].\n"
//                       << "          C) Pack idxByteOff%4 into descriptor[7] high bits and use in shader.\n";
//             // Shader bug is a separate issue from data correctness — don't fail page
//           }
//         }
//       }
//     } // Pass C

//     if (!pagePassed)
//       allPassed = false;
//   } // for each page

//   if (validated == 0)
//     std::cout << "    (no installed pages to validate)\n";
//   else
//     std::cout << "  Validated " << validated << "/" << pageCount << " pages — " << (allPassed ? "ALL PASSED ✓" : "FAILURES DETECTED ✗") << "\n";

//   return allPassed;
// }

// // ============================================================================
// // Per-mesh test
// // ============================================================================
// TestResult runTest(rendering::RHI *rhi, const std::string &meshPath, const std::string &objName)
// {
//   TestResult result;
//   result.meshName = objName;

//   std::cout << "\n==================================================\n";
//   std::cout << "Testing: " << objName << "\n";
//   std::cout << "==================================================\n";

//   // ------------------------------------------------------------------
//   // 1. Encode .obj → .virtualgeometry
//   // ------------------------------------------------------------------
//   std::cout << "[1] Encoding " << objName << "...\n";

//   std::vector<Vertex> vertices;
//   Shape shape;
//   VirtualGeometryEncoder::loadOBJ(meshPath, true, vertices, shape);

//   VirtualGeometryBuildData build = VirtualGeometryBuilder::build(vertices, shape);

//   QuantizationConfig qcfg;
//   qcfg.quantization_factor = 4;
//   qcfg.unit_scale = 100.0f;

//   VirtualGeometryEncodedData encoded = VirtualGeometryEncoder::encode(vertices, shape, qcfg);

//   std::string vgPath = meshPath + ".virtualgeometry";
//   {
//     VirtualGeometryFile writer(vgPath, true);
//     if (!writer.isOpen())
//       throw std::runtime_error("Cannot open VG file for writing: " + vgPath);
//     if (!writer.write(encoded, build.pages, MESHLET_MINIZ))
//       throw std::runtime_error("Failed to write VG file: " + vgPath);
//   }

//   const uint32_t pageCount = static_cast<uint32_t>(encoded.pages.size());
//   const uint32_t hierarchySize = static_cast<uint32_t>(encoded.hierarchy.size());
//   std::cout << "  Pages: " << pageCount << "  Hierarchy nodes: " << hierarchySize << "  Root page: " << encoded.rootPageIndex << "\n";

//   // ------------------------------------------------------------------
//   // 2. Build scene, register object
//   // ------------------------------------------------------------------
//   std::cout << "[2] Building VirtualGeometryScene...\n";

//   const uint32_t kHierarchyBufBytes = std::max(hierarchySize * (uint32_t)sizeof(VirtualGeometryHierarchy) * 4u, 4096u);
//   const uint64_t kPagesBufBytes = 256ull * 1024 * 1024;

//   rendering::RenderGraph *renderGraph = new rendering::RenderGraph(rhi);
//   VirtualGeometryScene scene(renderGraph, kHierarchyBufBytes, kPagesBufBytes);
//   scene.registerObjectForStreaming(objName, vgPath);

//   // ------------------------------------------------------------------
//   // 3. Create instance — we need unit_scale for the decoder
//   // ------------------------------------------------------------------
//   std::cout << "[3] Creating instance...\n";

//   InstanceId instId = scene.instantiateObjectInstance(objName, math::Vec3f(0.0f, 0.0f, 0.0f), math::Quatf::identity(), 1.0f);
//   scene.updateInstanceBuffer();

//   VirtualGeometryInstanceGPUData gpuInst{};
//   rhi->bufferRead(
//       scene.instanceBuffer,
//       0,
//       sizeof(VirtualGeometryInstanceGPUData),
//       [&](const void *d)
//       {
//         std::memcpy(&gpuInst, d, sizeof(gpuInst));
//       });

//   const uint32_t hierarchyStartOffset = gpuInst.hierarchyStartOffset;
//   const math::Mat4f modelMatrix = gpuInst.modelMatrix;

//   // unit_scale is stored as raw float bits (matches the GPU bitcast)
//   float unitScale;
//   std::memcpy(&unitScale, &gpuInst.unit_scale_bits, sizeof(float));
//   std::cout << "  hierarchyStartOffset: " << hierarchyStartOffset << "\n";
//   std::cout << "  unitScale: " << unitScale << "\n";

//   // ------------------------------------------------------------------
//   // 3b. Validate root page (always installed synchronously at registration)
//   // ------------------------------------------------------------------
//   result.success = validateInstalledPages(rhi, scene, objName, vgPath, unitScale, "after registerObjectForStreaming (root page)");

//   // ------------------------------------------------------------------
//   // 4. Camera placement
//   // ------------------------------------------------------------------
//   std::vector<VirtualGeometryHierarchy> cpuHierarchy(hierarchySize);
//   rhi->bufferRead(
//       scene.hierarchyBuffer,
//       0,
//       hierarchySize * sizeof(VirtualGeometryHierarchy),
//       [&](const void *d)
//       {
//         std::memcpy(cpuHierarchy.data(), d, hierarchySize * sizeof(VirtualGeometryHierarchy));
//       });

//   math::Vec3f aabbMin, aabbMax;
//   computeHierarchyAABB(cpuHierarchy, aabbMin, aabbMax);
//   math::Vec3f center = (aabbMin + aabbMax) * 0.5f;
//   float radius = (aabbMax - aabbMin).length() * 0.5f;

//   float camDist = radius * 3.0f + 1.0f;
//   math::Vec3f camPos(center[0], center[1], center[2] + camDist);

//   constexpr float kFovY = 60.0f * (3.14159265f / 180.0f);
//   constexpr float kAspect = 16.0f / 9.0f;
//   constexpr float kNear = 0.1f;
//   constexpr float kFar = 10000.0f;
//   constexpr uint32_t kVH = 1080u;
//   constexpr float kError = 1.0f;

//   rendering::Camera cam(kFovY, camPos, math::Vec3f(0, 0, -1), /*reverseZ=*/false);
//   cam.setAspectRatio(kAspect);
//   cam.setNearFar(kNear, kFar);
//   cam.lookAt(center);
//   cam.updateMatrices();

//   const math::Vec3f &vp = cam.getPosition();
//   float projY1 = cam.getProjectionMatrix().at(1, 1);

//   std::cout << "  Camera pos:    (" << vp[0] << ", " << vp[1] << ", " << vp[2] << ")\n";
//   std::cout << "  Object centre: (" << center[0] << ", " << center[1] << ", " << center[2] << ")\n";
//   std::cout << "  Object radius: " << radius << "\n";

//   // ------------------------------------------------------------------
//   // 5. Allocate simulation buffers
//   // ------------------------------------------------------------------
//   std::cout << "[4] Allocating simulation buffers...\n";

//   auto makeStorageBuf = [&](const char *name, uint64_t size) -> rendering::Buffer
//   {
//     return rhi->createBuffer(
//         rendering::BufferInfo{
//           .name = name,
//           .scratch = false,
//           .size = size,
//           .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_Pull,
//         });
//   };

//   rendering::Buffer countersBuffer = makeStorageBuf("SimCounters", sizeof(CullingCounters));
//   rendering::Buffer hierarchyQueueRead = makeStorageBuf("HierarchyQueueRead", MAX_QUEUE_ELEMENTS * sizeof(QueueElement));
//   rendering::Buffer hierarchyQueueWrite = makeStorageBuf("HierarchyQueueWrite", MAX_QUEUE_ELEMENTS * sizeof(QueueElement));
//   rendering::Buffer clusterQueueBuf = makeStorageBuf("ClusterQueue", MAX_QUEUE_ELEMENTS * sizeof(QueueElement));
//   rendering::Buffer visibleClustersBuf = makeStorageBuf("VisibleClusters", MAX_VISIBLE_CLUSTERS * sizeof(QueueElement));

//   // ------------------------------------------------------------------
//   // 6. Init
//   // ------------------------------------------------------------------
//   {
//     CullingCounters zero{0, 0, 0, 0};
//     rhi->bufferWrite(countersBuffer, 0, sizeof(CullingCounters), &zero);
//     std::vector<uint32_t> zeroPri(pageCount, 0u);
//     rhi->bufferWrite(scene.pagePriorityBuffer, 0, pageCount * sizeof(uint32_t), zeroPri.data());
//   }

//   // ------------------------------------------------------------------
//   // 7. Enqueue root node
//   // ------------------------------------------------------------------
//   {
//     QueueElement root{hierarchyStartOffset, 0u, SENTINEL_VALUE, 0u};
//     rhi->bufferWrite(hierarchyQueueWrite, 0, sizeof(QueueElement), &root);
//     CullingCounters c{1u, 0u, 0u, 0u};
//     rhi->bufferWrite(countersBuffer, 0, sizeof(CullingCounters), &c);
//   }
//   std::swap(hierarchyQueueRead, hierarchyQueueWrite);

//   // ------------------------------------------------------------------
//   // 8. Main culling loop
//   // ------------------------------------------------------------------
//   std::cout << "[5] Culling loop...\n";

//   constexpr uint32_t MAX_ITERS = 64u;
//   uint32_t iteration = 0u;

//   std::vector<QueueElement> readQHost(MAX_QUEUE_ELEMENTS);
//   std::vector<QueueElement> writeQHost(MAX_QUEUE_ELEMENTS);
//   std::vector<QueueElement> clusterQHost;
//   std::vector<PageTableEntry> pageTableHost(pageCount);

//   auto refreshPageTable = [&]()
//   {
//     rhi->bufferRead(
//         scene.pageTableBuffer,
//         0,
//         pageCount * sizeof(PageTableEntry),
//         [&](const void *d)
//         {
//           std::memcpy(pageTableHost.data(), d, pageCount * sizeof(PageTableEntry));
//         });
//   };
//   refreshPageTable();

//   while (true)
//   {
//     CullingCounters ctr{};
//     rhi->bufferRead(
//         countersBuffer,
//         0,
//         sizeof(CullingCounters),
//         [&](const void *d)
//         {
//           std::memcpy(&ctr, d, sizeof(ctr));
//         });

//     uint32_t readSize = ctr.hierarchyQueueSize;
//     if (readSize == 0u)
//     {
//       std::cout << "  Hierarchy queue drained after " << iteration << " iteration(s)\n";
//       break;
//     }
//     if (iteration >= MAX_ITERS)
//     {
//       std::cout << "  WARNING: hit MAX_ITERS\n";
//       break;
//     }

//     std::cout << "  Iter " << iteration << "  hqueue=" << readSize << "  cqueue=" << ctr.clusterQueueSize << "  visible=" << ctr.visibleClusterCount << "\n";

//     rhi->bufferRead(
//         hierarchyQueueRead,
//         0,
//         readSize * sizeof(QueueElement),
//         [&](const void *d)
//         {
//           std::memcpy(readQHost.data(), d, readSize * sizeof(QueueElement));
//         });
//     rhi->bufferRead(
//         scene.hierarchyBuffer,
//         0,
//         hierarchySize * sizeof(VirtualGeometryHierarchy),
//         [&](const void *d)
//         {
//           std::memcpy(cpuHierarchy.data(), d, hierarchySize * sizeof(VirtualGeometryHierarchy));
//         });

//     uint32_t newHQueueSize = 0u;
//     uint32_t newCQueueSize = ctr.clusterQueueSize;
//     std::vector<uint32_t> iterPriorities(pageCount, 0u);

//     for (uint32_t gid = 0; gid < readSize; ++gid)
//     {
//       const QueueElement &elem = readQHost[gid];
//       if (elem.index == SENTINEL_VALUE || elem.instanceIndex == SENTINEL_VALUE)
//         continue;
//       if (elem.index >= cpuHierarchy.size())
//         continue;

//       const VirtualGeometryHierarchy &node = cpuHierarchy[elem.index];
//       uint32_t pageIndex = node.pageIndex;

//       if (pageIndex != SENTINEL_VALUE && pageIndex < pageCount && pageTableHost[pageIndex].isInstalled)
//         iterPriorities[pageIndex] += 1u;

//       if (!shouldRenderAtLOD(node, modelMatrix, vp, projY1, kVH, kNear, kError))
//         continue;

//       bool isLeaf = (node.flags & 1u) != 0u;
//       if (isLeaf)
//       {
//         bool installed = pageIndex != SENTINEL_VALUE && pageIndex < pageCount && pageTableHost[pageIndex].isInstalled;
//         if (!installed)
//         {
//           if (pageIndex != SENTINEL_VALUE && pageIndex < pageCount)
//             iterPriorities[pageIndex] += 10u;
//           continue;
//         }
//         if (node.child_start == SENTINEL_VALUE)
//           continue;
//         uint32_t base = newCQueueSize;
//         newCQueueSize += node.child_count;
//         if (newCQueueSize > clusterQHost.size())
//           clusterQHost.resize(newCQueueSize);
//         for (uint32_t ci = 0; ci < node.child_count; ++ci)
//           clusterQHost[base + ci] = QueueElement{node.child_start + ci, elem.instanceIndex, pageIndex, 0u};
//       }
//       else
//       {
//         uint32_t base = newHQueueSize;
//         newHQueueSize += node.child_count;
//         if (newHQueueSize > writeQHost.size())
//           writeQHost.resize(newHQueueSize);
//         for (uint32_t ci = 0; ci < node.child_count; ++ci)
//           writeQHost[base + ci] = QueueElement{hierarchyStartOffset + node.child_start + ci, elem.instanceIndex, SENTINEL_VALUE, 0u};
//       }
//     }

//     if (newHQueueSize > 0u)
//       rhi->bufferWrite(hierarchyQueueWrite, 0, newHQueueSize * sizeof(QueueElement), writeQHost.data());

//     {
//       uint32_t prev = ctr.clusterQueueSize;
//       uint32_t added = newCQueueSize - prev;
//       if (added > 0u)
//         rhi->bufferWrite(clusterQueueBuf, prev * sizeof(QueueElement), added * sizeof(QueueElement), clusterQHost.data() + prev);
//     }

//     {
//       std::vector<uint32_t> gpuPri(pageCount, 0u);
//       rhi->bufferRead(
//           scene.pagePriorityBuffer,
//           0,
//           pageCount * sizeof(uint32_t),
//           [&](const void *d)
//           {
//             std::memcpy(gpuPri.data(), d, pageCount * sizeof(uint32_t));
//           });
//       for (uint32_t pi = 0; pi < pageCount; ++pi)
//         gpuPri[pi] += iterPriorities[pi];
//       rhi->bufferWrite(scene.pagePriorityBuffer, 0, pageCount * sizeof(uint32_t), gpuPri.data());

//       bool missingPages = false;
//       for (uint32_t pi = 0; pi < pageCount; ++pi)
//         if (gpuPri[pi] > 0u && !pageTableHost[pi].isInstalled)
//         {
//           missingPages = true;
//           break;
//         }

//       if (missingPages)
//       {
//         std::cout << "  → Missing pages, calling updatePageStreaming\n";
//         scene.updatePageStreaming(objName);

//         if (!validateInstalledPages(rhi, scene, objName, vgPath, unitScale, "after updatePageStreaming (iter " + std::to_string(iteration) + ")"))
//           result.success = false;

//         refreshPageTable();
//         rhi->bufferRead(
//             scene.hierarchyBuffer,
//             0,
//             hierarchySize * sizeof(VirtualGeometryHierarchy),
//             [&](const void *d)
//             {
//               std::memcpy(cpuHierarchy.data(), d, hierarchySize * sizeof(VirtualGeometryHierarchy));
//             });

//         clusterQHost.clear();
//         newCQueueSize = 0u;
//         QueueElement root{hierarchyStartOffset, 0u, SENTINEL_VALUE, 0u};
//         rhi->bufferWrite(hierarchyQueueRead, 0, sizeof(QueueElement), &root);
//         CullingCounters restart{1u, 0u, 0u, 0u};
//         rhi->bufferWrite(countersBuffer, 0, sizeof(CullingCounters), &restart);
//         std::vector<uint32_t> zeroPri(pageCount, 0u);
//         rhi->bufferWrite(scene.pagePriorityBuffer, 0, pageCount * sizeof(uint32_t), zeroPri.data());
//         std::cout << "  [restart] Restarting hierarchy traversal\n";
//         iteration = 0u;
//         continue;
//       }

//       std::vector<uint32_t> zeroPri(pageCount, 0u);
//       rhi->bufferWrite(scene.pagePriorityBuffer, 0, pageCount * sizeof(uint32_t), zeroPri.data());
//     }

//     CullingCounters nc{newHQueueSize, newCQueueSize, ctr.visibleClusterCount, ctr.iteration + 1u};
//     rhi->bufferWrite(countersBuffer, 0, sizeof(CullingCounters), &nc);
//     std::swap(hierarchyQueueRead, hierarchyQueueWrite);
//     ++iteration;
//   }

//   // ------------------------------------------------------------------
//   // 8b. Final validation — all pages that ended up installed
//   // ------------------------------------------------------------------
//   if (!validateInstalledPages(rhi, scene, objName, vgPath, unitScale, "final (all installed pages)"))
//     result.success = false;

//   // ------------------------------------------------------------------
//   // 9. processClusters
//   // ------------------------------------------------------------------
//   std::cout << "[6] processClusters...\n";

//   CullingCounters finalCtr{};
//   rhi->bufferRead(
//       countersBuffer,
//       0,
//       sizeof(CullingCounters),
//       [&](const void *d)
//       {
//         std::memcpy(&finalCtr, d, sizeof(finalCtr));
//       });

//   std::vector<QueueElement> finalCQ(finalCtr.clusterQueueSize);
//   if (finalCtr.clusterQueueSize > 0u)
//     rhi->bufferRead(
//         clusterQueueBuf,
//         0,
//         finalCtr.clusterQueueSize * sizeof(QueueElement),
//         [&](const void *d)
//         {
//           std::memcpy(finalCQ.data(), d, finalCtr.clusterQueueSize * sizeof(QueueElement));
//         });

//   std::vector<QueueElement> visibleList;
//   visibleList.reserve(finalCQ.size());
//   for (const auto &e : finalCQ)
//     if (e.index != SENTINEL_VALUE && e.instanceIndex != SENTINEL_VALUE && e.pageIndex != SENTINEL_VALUE)
//       visibleList.push_back(e);

//   uint32_t visibleCount = static_cast<uint32_t>(visibleList.size());
//   if (visibleCount > 0u)
//     rhi->bufferWrite(visibleClustersBuf, 0, visibleCount * sizeof(QueueElement), visibleList.data());
//   finalCtr.visibleClusterCount = visibleCount;
//   rhi->bufferWrite(countersBuffer, 0, sizeof(CullingCounters), &finalCtr);

//   // ------------------------------------------------------------------
//   // 10. Final report
//   // ------------------------------------------------------------------
//   refreshPageTable();
//   uint32_t pagesInstalled = 0u;
//   for (uint32_t pi = 0; pi < pageCount; ++pi)
//     if (pageTableHost[pi].isInstalled)
//       ++pagesInstalled;

//   std::cout << "\n--- Results for " << objName << " ---\n";
//   std::cout << "  Visible clusters    : " << visibleCount << "\n";
//   std::cout << "  Pages installed     : " << pagesInstalled << " / " << pageCount << "\n";
//   std::cout << "  Hierarchy iterations: " << iteration << "\n";

//   for (const auto &vc : visibleList)
//   {
//     if (vc.pageIndex >= pageCount || !pageTableHost[vc.pageIndex].isInstalled)
//     {
//       std::cout << "  ERROR: visible cluster refs uninstalled page " << vc.pageIndex << "\n";
//       result.success = false;
//       break;
//     }
//   }

//   if (result.success && visibleCount > 0u)
//     std::cout << "  ✓ All visible clusters are valid\n";
//   else if (visibleCount == 0u)
//     std::cout << "  WARNING: no clusters visible (check error threshold / LOD)\n";

//   result.visibleClusters = visibleCount;
//   result.iterations = iteration;
//   result.pagesInstalled = pagesInstalled;
//   return result;
// }

// // ============================================================================
// // main
// // ============================================================================
// int main()
// {
//   const std::vector<std::string> test_meshes = {
//     "assets/meshes/obj/suzanne.obj", "assets/meshes/obj/teapot.obj",
//     // "assets/meshes/obj/armadillo.obj",
//   };

//   std::cout << "Virtual Geometry Scene – CPU Simulation Test\n";
//   std::cout << "=============================================\n\n";

//   rendering::DeviceRequiredLimits limits{0, 0, 0};
//   rendering::DeviceFeatures features = rendering::DeviceFeatures::DeviceFeatures_Compute | rendering::DeviceFeatures::DeviceFeatures_Subgroup_Basic |
//                                        rendering::DeviceFeatures::DeviceFeatures_Subgroup_Shuffle | rendering::DeviceFeatures::DeviceFeatures_Timestamp;

//   rendering::backend::vulkan::VulkanRHI *rhi = new rendering::backend::vulkan::VulkanRHI(rendering::backend::vulkan::Vulkan_1_2, limits, features, {});
//   auto surfaces = std::vector<VkSurfaceKHR>();
//   rhi->init(surfaces);

//   std::string exeDir = utils::getExecutableDirectory();
//   std::vector<TestResult> results;

//   for (const auto &meshRel : test_meshes)
//   {
//     std::string fullPath = exeDir + "/" + meshRel;
//     FILE *f = fopen(fullPath.c_str(), "rb");
//     if (!f)
//     {
//       std::cout << "Skipping " << meshRel << " (not found)\n";
//       continue;
//     }
//     fclose(f);

//     std::string name = meshRel;
//     {
//       auto p = name.rfind('/');
//       if (p != std::string::npos)
//         name = name.substr(p + 1);
//     }
//     {
//       auto p = name.rfind('.');
//       if (p != std::string::npos)
//         name = name.substr(0, p);
//     }

//     try
//     {
//       results.push_back(runTest(rhi, fullPath, name));
//     }
//     catch (const std::exception &e)
//     {
//       std::cout << "EXCEPTION: " << e.what() << "\n";
//       TestResult r;
//       r.meshName = name;
//       r.success = false;
//       results.push_back(r);
//     }
//   }

//   std::cout << "\n=============================================\n";
//   std::cout << "Summary\n";
//   std::cout << "=============================================\n";
//   std::cout << "  Mesh\tVisible\tPages\tIters\tOK?\n";
//   std::cout << "  ----\t-------\t-----\t-----\t---\n";
//   bool allOk = true;
//   for (const auto &r : results)
//   {
//     std::cout << "  " << r.meshName << "\t" << r.visibleClusters << "\t" << r.pagesInstalled << "\t" << r.iterations << "\t" << (r.success ? "✓" : "✗") << "\n";
//     if (!r.success)
//       allOk = false;
//   }
//   std::cout << "\n" << (allOk && !results.empty() ? "✓ ALL TESTS PASSED\n" : "✗ SOME TESTS FAILED\n");
//   std::cout << "=============================================\n";

//   delete rhi;
//   return allOk ? 0 : 1;
// }

int main() {
  return 0;
}