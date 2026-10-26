#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace virtualgeometry
{
// Camera-to-receiver limits; never use these to truncate caster rays.
struct ShadowDistancePolicy
{
  float fadeStart = std::numeric_limits<float>::max();
  float fadeEnd = std::numeric_limits<float>::max();
  void validate() const
  {
    if (!std::isfinite(fadeStart) || !std::isfinite(fadeEnd) || fadeStart < 0 || fadeEnd < fadeStart)
      throw std::invalid_argument("Shadow distances must be finite and 0 <= start <= end");
  }
  float weight(float distance) const
  {
    if (distance >= fadeEnd || fadeEnd == 0) return 0;
    if (distance <= fadeStart || fadeStart == fadeEnd) return 1;
    const float t = (distance-fadeStart)/(fadeEnd-fadeStart);
    return 1-t*t*(3-2*t);
  }
};

inline float shadowFirstClipmapExtent(uint32_t pagesPerAxis, uint32_t texelsPerPage,
    uint32_t screenHeight, float verticalFov, float referenceDepth, float targetPixels=1)
{
  if (!pagesPerAxis || !texelsPerPage || !screenHeight || !std::isfinite(verticalFov) ||
      verticalFov<=0 || verticalFov>=3.14159265f || !std::isfinite(referenceDepth) || referenceDepth<=0 ||
      !std::isfinite(targetPixels) || targetPixels<=0)
    throw std::invalid_argument("Invalid shadow resolution policy");
  const double extent=double(pagesPerAxis)*texelsPerPage*targetPixels*2*referenceDepth*std::tan(verticalFov*.5)/screenHeight;
  if (!std::isfinite(extent) || extent>std::numeric_limits<float>::max())
    throw std::invalid_argument("Shadow resolution extent overflows");
  return static_cast<float>(extent);
}

// Pure policy shared with configuration/tests. Extent is the FULL square side.
inline uint32_t shadowClipmapLevels(float firstExtent, float receiverRadius,
    uint32_t pagesPerAxis, uint32_t maximumLevels, float filterMargin = 0)
{
  if (!std::isfinite(firstExtent) || firstExtent <= 0 || !std::isfinite(receiverRadius) || receiverRadius < 0 ||
      !std::isfinite(filterMargin) || filterMargin < 0 || pagesPerAxis < 4 || maximumLevels == 0)
    throw std::invalid_argument("Invalid shadow clipmap coverage policy");
  double extent = firstExtent;
  for (uint32_t levels=1; levels<=maximumLevels; ++levels,extent*=2)
    if (extent*.5 - extent/pagesPerAxis - filterMargin >= receiverRadius) return levels;
  throw std::invalid_argument("Shadow distance exceeds configured clipmap capacity");
}
}
