#pragma once
#include "procedural/Geometry.hpp"
#include <cstdint>
#include <vector>

namespace procedural::geometry
{
// A per-position "how much detail does this ground deserve" field.
//
// Error-driven refinement answers "where is the surface far from its
// triangles", which is necessary and not sufficient: a smooth slope is
// faithfully flat and also dull, and a mesh that spends its budget purely on
// measured error looks evenly busy everywhere it is curved. This field is the
// other half - an authored demand that clusters detail into patches, so the
// terrain has grain at a scale the height function does not carry.
//
// It is kept as a field rather than folded into the tolerance so the same
// values can be baked per chunk and reused: the texturing work wants exactly
// this kind of multi-octave variation to drive material blending, and a
// real-time tessellator wants it to pick its own subdivision level.
struct DetailSettings
{
  bool enabled = true;
  uint64_t seed = 0u;
  // Wavelength of the coarsest octave, in metres. This is the size of a patch
  // of "interesting" ground.
  double wavelengthMetres = 2600.0;
  uint32_t octaves = 4u;
  double gain = .55;
  // How far the field is allowed to move the refinement budget. 0 disables,
  // 1 means a demanding patch refines twice as hard as a dull one.
  double strength = .65;
};

class TerrainDetailField
{
public:
  void build(double extentMetres, const DetailSettings &settings);
  // 0 dull, 1 demanding. Smooth, seamless and deterministic.
  double at(Point2 point) const;
  // Multiplier for a refinement tolerance: below 1 where detail is wanted.
  double toleranceScale(Point2 point) const;
  bool enabled() const { return settings_.enabled; }

  // Baked chunk, for reuse by texturing and by a real-time tessellator. Values
  // are the same `at()` the mesher used, so a later pass cannot disagree with
  // the geometry it is decorating.
  struct Chunk
  {
    Point2 origin{};
    double sizeMetres = 0.0;
    uint32_t side = 0u;
    std::vector<uint8_t> values;   // row major, 0..255 over `at()`
  };
  Chunk bake(Point2 origin, double sizeMetres, uint32_t side) const;

private:
  DetailSettings settings_{};
  double extent_ = 1.0;
};
}
