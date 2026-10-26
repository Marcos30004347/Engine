// Phase 0 exit tests for the FSR 3.1 temporal upscaling plan.
//
// These check a property that is invisible in a screenshot and was silently
// wrong: the jitter offsets must span a whole pixel. A temporal accumulator can
// only anti-alias the part of the pixel footprint the renderer sampled, so an
// amplitude of a quarter leaves fifteen sixteenths of each pixel's area never
// sampled - the image is then aliased no matter which upscaler consumes it.
//
// Failures are reported by return code, never by `assert`. The test binaries
// are built in the same Release configuration as everything else, where NDEBUG
// erases both the assertion and the call inside it, so an assert-based check
// here would pass without executing anything.

#include "os/Logger.hpp"
#include "rendering/TemporalJitter.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

uint32_t failures = 0u;

void check(const bool condition, const std::string &what)
{
  if (condition) return;
  os::Logger::logf("FAIL %s", what.c_str());
  ++failures;
}

// T0.1: the offsets span a whole pixel, and no further.
void runSpanTest()
{
  constexpr uint32_t kPhases = 32u;
  float minimumX = 1.0e9f;
  float maximumX = -1.0e9f;
  float minimumY = 1.0e9f;
  float maximumY = -1.0e9f;
  for (uint32_t frame = 0u; frame < kPhases; ++frame)
  {
    float x = 0.0f;
    float y = 0.0f;
    ::rendering::temporalJitterOffset(frame, kPhases, 1.0f, x, y);
    minimumX = std::min(minimumX, x);
    maximumX = std::max(maximumX, x);
    minimumY = std::min(minimumY, y);
    maximumY = std::max(maximumY, y);
  }
  // Inside the pixel, never outside it: an offset beyond half a pixel samples a
  // neighbour's footprint and shows up as a soft, permanently blurred image.
  check(minimumX >= -0.5f && maximumX <= 0.5f, "T0.1 x offsets inside the pixel");
  check(minimumY >= -0.5f && maximumY <= 0.5f, "T0.1 y offsets inside the pixel");
  // And covering most of it. Halton over 32 phases gets close to the full
  // range; well short of it is the defect this test exists for.
  check(maximumX - minimumX > 0.8f, "T0.1 x offsets span the pixel");
  check(maximumY - minimumY > 0.8f, "T0.1 y offsets span the pixel");

  // The scale dial must be linear, so turning it down is a demonstration rather
  // than a different sequence.
  float fullX = 0.0f;
  float fullY = 0.0f;
  float halfX = 0.0f;
  float halfY = 0.0f;
  ::rendering::temporalJitterOffset(7u, kPhases, 1.0f, fullX, fullY);
  ::rendering::temporalJitterOffset(7u, kPhases, 0.5f, halfX, halfY);
  check(std::abs(fullX * 0.5f - halfX) < 1.0e-6f, "T0.1 scale is linear in x");
  check(std::abs(fullY * 0.5f - halfY) < 1.0e-6f, "T0.1 scale is linear in y");
}

// T0.2: deterministic, and periodic on exactly the stated period.
void runSequenceTest()
{
  constexpr uint32_t kPhases = 16u;
  std::vector<float> firstX;
  std::vector<float> firstY;
  for (uint32_t frame = 0u; frame < kPhases; ++frame)
  {
    float x = 0.0f;
    float y = 0.0f;
    ::rendering::temporalJitterOffset(frame, kPhases, 1.0f, x, y);
    firstX.push_back(x);
    firstY.push_back(y);
  }
  for (uint32_t frame = 0u; frame < kPhases; ++frame)
  {
    float x = 0.0f;
    float y = 0.0f;
    ::rendering::temporalJitterOffset(frame + kPhases, kPhases, 1.0f, x, y);
    check(
        std::abs(x - firstX[frame]) < 1.0e-6f &&
            std::abs(y - firstY[frame]) < 1.0e-6f,
        "T0.2 sequence repeats on its period");
  }

  // No phase may be the un-jittered centre: that frame contributes nothing new
  // and biases a short accumulation toward the pixel centre.
  for (uint32_t frame = 0u; frame < kPhases; ++frame)
  {
    float x = 0.0f;
    float y = 0.0f;
    ::rendering::temporalJitterOffset(frame, kPhases, 1.0f, x, y);
    check(
        std::abs(x) > 1.0e-6f || std::abs(y) > 1.0e-6f,
        "T0.2 no phase sits exactly at the pixel centre");
  }

  // Distinct phases must be distinct samples, or the effective phase count is
  // lower than configured and the pixel is covered more coarsely than intended.
  uint32_t duplicates = 0u;
  for (uint32_t a = 0u; a < kPhases; ++a)
    for (uint32_t b = a + 1u; b < kPhases; ++b)
      if (std::abs(firstX[a] - firstX[b]) < 1.0e-6f &&
          std::abs(firstY[a] - firstY[b]) < 1.0e-6f)
        ++duplicates;
  check(duplicates == 0u, "T0.2 phases are distinct");
}

// Phase count follows the upscale *area* ratio, which is how many low
// resolution samples must cover one output pixel.
void runPhaseCountTest()
{
  check(
      ::rendering::temporalJitterPhaseCount(1920u, 1920u) == 8u,
      "T0.2 native resolution keeps the base phase count");
  const uint32_t threeQuarters =
      ::rendering::temporalJitterPhaseCount(1440u, 1920u);
  // (1920/1440)^2 * 8 = 14.2
  check(
      threeQuarters >= 13u && threeQuarters <= 15u,
      "T0.2 phase count follows the area ratio");
  check(
      ::rendering::temporalJitterPhaseCount(960u, 1920u) >= 32u,
      "T0.2 half resolution needs at least four times the phases");
}

} // namespace

int main(int argc, char **argv)
{
  os::Logger::start();
  const std::string gate = argc == 2 ? argv[1] : "--all";
  if (gate == "--span") runSpanTest();
  else if (gate == "--sequence") runSequenceTest();
  else if (gate == "--phases") runPhaseCountTest();
  else
  {
    runSpanTest();
    runSequenceTest();
    runPhaseCountTest();
  }
  if (failures == 0u)
    os::Logger::logf("Temporal jitter tests passed");
  else
    os::Logger::logf("Temporal jitter tests: %u failure(s)", failures);
  os::Logger::shutdown();
  return failures == 0u ? 0 : 1;
}
