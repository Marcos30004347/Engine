#pragma once

#include <algorithm>
#include <cstdint>

namespace rendering
{

// Sub-pixel sample offsets for temporal anti-aliasing and upscaling.
//
// Extracted from the sample so it can be tested directly. The correctness
// property that matters is not visible from a screenshot and is easy to get
// silently wrong: the offsets must span a whole pixel. A temporal accumulator
// can only anti-alias the part of the pixel footprint the renderer actually
// sampled, so an amplitude of half the intended one leaves three quarters of
// each pixel's area never sampled and the accumulator with nothing to average.

// Radical inverse of `index` in `base`, in [0, 1).
inline float haltonSequence(uint32_t index, const uint32_t base)
{
  float result = 0.0f;
  float fraction = 1.0f / static_cast<float>(base);
  while (index != 0u)
  {
    result += static_cast<float>(index % base) * fraction;
    index /= base;
    fraction /= static_cast<float>(base);
  }
  return result;
}

// Offset in pixels for `frameIndex`, in [-0.5, 0.5] * scale on each axis.
//
// `scale` is a debugging dial, not a tuning one: one is the correct value and
// anything less samples proportionally less of the pixel. It is exposed only so
// the effect can be demonstrated by turning it down.
//
// Halton (2, 3) is the standard choice - it is low discrepancy, so any prefix
// of the sequence covers the pixel evenly rather than only doing so once the
// full period has elapsed, which is what lets the image converge quickly after
// a camera cut instead of over the whole period.
//
// The index is offset by one because Halton's first term is zero on every base,
// and a zero offset is the un-jittered sample - starting there wastes a frame
// and biases short accumulations toward the pixel centre.
inline void temporalJitterOffset(
    const uint64_t frameIndex, const uint32_t sampleCount, const float scale,
    float &outX, float &outY)
{
  const uint32_t period = std::max(1u, sampleCount);
  const uint32_t index =
      static_cast<uint32_t>(frameIndex % period) + 1u;
  outX = (haltonSequence(index, 2u) - 0.5f) * scale;
  outY = (haltonSequence(index, 3u) - 0.5f) * scale;
}

// Phases that cover the pixel evenly at a given upscale ratio.
//
// FSR's rule: the sample count scales with the *area* ratio, because that is
// how many low-resolution samples have to be distributed across one output
// pixel before every part of it has been covered. Too few and the sequence
// repeats before the pixel is covered, which reads as a stable but aliased
// image; far too many and convergence after a cut becomes visibly slow.
inline uint32_t temporalJitterPhaseCount(
    const uint32_t renderExtent, const uint32_t displayExtent,
    const uint32_t basePhases = 8u)
{
  const float ratio = static_cast<float>(std::max(displayExtent, 1u)) /
                      static_cast<float>(std::max(renderExtent, 1u));
  const float scaled =
      static_cast<float>(basePhases) * ratio * ratio;
  return std::clamp(static_cast<uint32_t>(scaled + 0.5f), basePhases, 128u);
}

} // namespace rendering
