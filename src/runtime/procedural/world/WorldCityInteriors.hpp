#pragma once
#include "WorldCityLayout.hpp"
#include "WorldDwellingPlan.hpp"

namespace procedural::world::rewrite {
struct CityInteriorRequest
{
  uint64_t object = 0;
  DwellingUse use = DwellingUse::Cottage;
  uint32_t floors = 1;
};

struct CityInteriorRejection
{
  uint64_t object = 0;
  std::string reason;
};

struct CityInteriors
{
  uint64_t layoutRevision = 0;
  std::vector<DwellingPlan> dwellings;
  std::vector<CityInteriorRejection> unresolved;
};

// Explicit programme requests attach to accepted object IDs. This adapter does
// not reinterpret a roof height as occupancy, reshape wings or erase courts.
bool compileCityInteriors(const CityLayout &, const std::vector<CityInteriorRequest> &, uint64_t seed, CityInteriors &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
