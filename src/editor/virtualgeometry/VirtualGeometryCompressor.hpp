// VirtualGeometryCompressor.hpp
#pragma once
#include <cstdint>
#include <vector>
#include "VirtualGeometryBuilder.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"

namespace virtualgeometry
{

/**
 * @brief Vertex attribute encoder / decoder.
 *
 * Encodes vertex data using quantization and compression techniques to
 * minimise memory usage while maintaining visual quality.
 *
 * Each meshlet now also carries:
 *   • self / parent LODBounds  — copied verbatim from the source cluster.
 *   • bone weights             — (float weight, uint32_t boneIndex) pairs,
 *                                uniform count per vertex within the meshlet.
 */
class VirtualGeometryCompressor
{
public:
  /**
   * @brief Encode VirtualGeometryBuildData into compressed page-based format.
   *
   * Process (per page, per cluster):
   *  1. Copy self/parent LODBounds into the Meshlet descriptor.
   *  2. Quantise positions → per-channel bit-packed bitstream.
   *  3. Encode normals via octahedral projection + pack2x16snorm.
   *  4. Store UVs as raw floats.
   *  5. Store indices as uint8_t (max 255 vertices per meshlet).
   *  6. Encode bone weights as interleaved (weight-bits, boneIndex) uint32
   *     pairs into VertexBuffers::boneWeights.
   */
  static VirtualGeometryEncodedData encode(
      const VirtualGeometryBuildData &build_data,
      const QuantizationConfig       &config = QuantizationConfig());

  // ── Decode helpers (CPU-side validation / testing) ───────────────────────

  /** Decode vertex positions for one meshlet from a page. */
  static void decodePositions(
      const VirtualGeometryPage &page,
      uint32_t                   meshlet_index,
      std::vector<float>        &out_positions,
      const QuantizationConfig  &config);

  /** Decode vertex normals for one meshlet from a page. */
  static void decodeNormals(
      const VirtualGeometryPage &page,
      uint32_t                   meshlet_index,
      std::vector<float>        &out_normals);

  /** Decode UVs for one meshlet from a page. */
  static void decodeUVs(
      const VirtualGeometryPage &page,
      uint32_t                   meshlet_index,
      std::vector<float>        &out_uvs);

  /** Decode triangle indices for one meshlet from a page. */
  static void decodeIndices(
      const VirtualGeometryPage &page,
      uint32_t                   meshlet_index,
      std::vector<uint8_t>      &out_indices);

  /**
   * @brief Decode bone weights for one meshlet from a page.
   *
   * Output is a flat array of BoneWeight structs ordered
   * [vertex0_inf0, vertex0_inf1, ..., vertex1_inf0, ...].
   * The number of influences per vertex equals
   * page.meshlets[meshlet_index].boneWeightsPerVertex.
   */
  static void decodeBoneWeights(
      const VirtualGeometryPage &page,
      uint32_t                   meshlet_index,
      std::vector<BoneWeight>   &out_bone_weights);

private:
  /** Encode positions for a single meshlet into the page bitstream. */
  static void encodeMeshletPositions(
      const std::vector<Vertex> &vertices,
      const QuantizationConfig  &config,
      Meshlet                   &meshlet,
      std::vector<uint32_t>     &position_buffer);

  /** Encode normals for a single meshlet into the page normal buffer. */
  static void encodeMeshletNormals(
      const std::vector<Vertex> &vertices,
      std::vector<uint32_t>     &normal_buffer);

  /**
   * Encode bone weights for a single meshlet.
   *
   * All vertices must have the same number of influences.  If the vertex
   * list is empty or vertices carry no bone weights the function is a no-op
   * (boneWeightsPerVertex stays 0).
   *
   * Encoding: each influence is two consecutive uint32 words:
   *   word 0 — float weight reinterpreted as uint32
   *   word 1 — boneIndex (uint32)
   */
  static void encodeMeshletBoneWeights(
      const std::vector<Vertex> &vertices,
      Meshlet                   &meshlet,
      std::vector<uint32_t>     &bone_weight_buffer);

  // ── Math helpers ─────────────────────────────────────────────────────────
  static void     octahedralEncode(const float n[3], float &ox, float &oy);
  static void     octahedralDecode(float ox, float oy, float out[3]);
  static uint32_t pack2x16Snorm(float x, float y);
  static void     unpack2x16Snorm(uint32_t packed, float &ox, float &oy);
};

} // namespace virtualgeometry
