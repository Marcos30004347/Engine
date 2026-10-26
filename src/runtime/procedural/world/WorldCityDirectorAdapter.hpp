#pragma once
#include "WorldCityLayout.hpp"
#include "WorldSettlementDirector.hpp"

namespace procedural::world::rewrite {
// Build an immutable walking graph and programme candidate sites from accepted
// city shells. Split street intersections and wall/tower obstructions; include
// the actual entrance-to-street links. New institutions still need prepared
// institutional sites: ordinary house shells are not silently church precincts.
// policy.sites supplies explicit per-object terrain/water eligibility; missing
// site permissions are omitted, rather than assuming potable water exists.
bool prepareCityDirector(const CityLayout &, const SettlementDirectorInput &policy, SettlementDirectorInput &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
