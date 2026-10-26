#pragma once
#include "WorldLandProduct.hpp"

namespace procedural::world::rewrite {
struct CoastDetailSettings
{
  uint32_t subdivisions = 8;
  double amplitude = 0.3; // Fraction of the incident-cell triangle corridor.
};

// Input must be the convex planning partition. Each physical coast is refined
// once and shared by its land/ocean patches. World-border edges remain fixed.
// Cell identity and land membership survive; boundary segment IDs are extended.
bool detailLandCoasts(const LandProduct &, const CoastDetailSettings &, LandProduct &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
