#pragma once
#include "WorldGenerationContracts.hpp"

namespace procedural::world::rewrite
{
enum class DistrictUse : uint8_t
{
  Castle,
  Harbor,
  Civic,
  Merchant,
  Military,
  Prison,
  Manor,
  Residential,
  Suburb,
  Craftsmen,
  Guild,
  Temple,
  Church,
  Citadel,
  WealthyResidential,
  WorkerHousing,
  Count
};
// Convex, non-overlapping pieces from upstream suitability/terrain reservations.
// Flags are authoritative upstream predicates, not inferred from a nearby blue pixel.
struct CityPlanCell
{
  uint64_t id = 0;
  std::vector<Position> boundary;
  double slope = 0, floodRisk = 0, defense = 0, fertility = 0;
  bool buildable = true, navigableShore = false, secure = false, outsideOldWall = false;
  bool allocatable = true; // Circulation cells remain traversable without accepting a district.
};
struct DistrictRequest
{
  uint64_t id = 0;
  DistrictUse use = DistrictUse::Residential;
  double minimumArea = 300, targetArea = 1500;
  bool required = true;
};
struct CityPlanSettings
{
  uint64_t seed = 7, revision = 1;
  double maximumSlope = .25, maximumFloodRisk = .2, minimumPortalWidth = 4;
  uint32_t maximumCells = 512;
};
struct CityPlanDistrict
{
  uint64_t id = 0, anchorCell = 0;
  DistrictUse use = DistrictUse::Residential;
  std::vector<uint64_t> cells;
  // Convex components are retained; their internal seams are NOT streets.
  std::vector<std::vector<Position>> pieces;
  // Gate cell center -> shared-edge portals -> anchor center. Planning access,
  // not an engineered road or a promise of full swept-width clearance.
  std::vector<Position> access;
  std::vector<uint64_t> accessCells;
  double area = 0, unmetTargetArea = 0;
};
struct CityPlanOmission
{
  uint64_t request = 0;
  std::string reason;
};
struct CityMasterPlan
{
  uint64_t revision = 0, inputFingerprint = 0;
  std::vector<std::vector<Position>> gateConnections;
  std::vector<CityPlanDistrict> districts;
  std::vector<CityPlanOmission> omissions;
  std::vector<uint64_t> unassignedCells;
  double assignedArea = 0, unassignedArea = 0, excludedArea = 0;
};
// Deterministic under input permutation. Gate IDs name traversable cells.
// Merchant/civic anchors minimize the farthest gate travel cost; peripheral uses
// prefer nearby access, with defense/fertility and hard eligibility preserved.
// Bounded allocation may fail even if another allocation could succeed; failure
// is explicit and atomic. Optional requests never displace required districts.
bool generateCityMasterPlan(const std::vector<CityPlanCell> &, const std::vector<DistrictRequest> &, const std::vector<uint64_t> &gateCells, const CityPlanSettings &, CityMasterPlan &, std::string *error = nullptr);
// Compound reservations can be passed to generateCityLayout before frontage.
// Merchant/residential/suburban/craftsmen areas remain available for later local subdivision.
std::vector<std::vector<Position>> cityLandmarkReservations(const CityMasterPlan &);
const char *districtUseName(DistrictUse);
} // namespace procedural::world::rewrite
