#pragma once
#include "WorldPhysicalTerrain.hpp"
#include "WorldRiverChannels.hpp"
namespace procedural::world::rewrite
{
enum SuitabilityReason : uint32_t
{
  Wet = 1,
  Flood = 2,
  Cliff = 4,
  Talus = 8,
  Brink = 16,
  Steep = 32,
  Rough = 64,
  Inaccessible = 128,
  Ramp = 256,
  PoorSoil = 512,
  Saturated = 1024,
  InsufficientArea = 2048,
  Narrow = 4096,
  ExpansionBlocked = 8192,
  Occupied = 16384,
  NoTown = 32768,
  Quota = 65536,
  TooClose = 131072
};
struct SuitabilitySettings
{
  double minX = 0, minZ = 0, maxX = 300, maxZ = 240, spacingMetres = 4;
  double maximumBuildGrade = .15, maximumTravelGrade = .35, maximumReliefMetres = 3;
  double brinkSetbackMetres = 6, waterSetbackMetres = 6, floodReachMetres = 25, floodRiseMetres = 2;
  double minimumSoil = .2, maximumMoisture = .95;
};
struct SuitabilityCell
{
  Position center;
  double slope = 0, relief = 0, soil = 0, moisture = 0, waterDistance = 1e6, score = 0;
  uint32_t reasons = 0, component = UINT32_MAX;
  bool walkable = false;
};
class WorldSuitability
{
public:
  using Field = std::function<double(double, double)>;
  bool build(
      const WorldPhysicalTerrain &,
      const WorldRiverChannels &,
      const Field &soil,
      const Field &moisture,
      const std::vector<Position> &accessSeeds,
      const SuitabilitySettings &,
      uint64_t dependencyRevision,
      std::string *error = nullptr,
      const std::function<bool(double, double)> &additionalWater = {});
  const std::vector<SuitabilityCell> &cells() const
  {
    return cells_;
  }
  const SuitabilitySettings &settings() const
  {
    return settings_;
  }
  uint32_t columns() const
  {
    return columns_;
  }
  uint32_t rows() const
  {
    return rows_;
  }
  uint64_t fingerprint() const
  {
    return fingerprint_;
  }
  size_t cellAt(double x, double z) const;
  std::vector<uint32_t> neighbors(uint32_t cell) const;

private:
  SuitabilitySettings settings_;
  uint32_t columns_ = 0, rows_ = 0;
  uint64_t fingerprint_ = 0;
  std::vector<SuitabilityCell> cells_;
};
struct SettlementSettings
{
  double townAreaMetres2 = 1000, villageAreaMetres2 = 400, expansionBandMetres = 8, minimumExpansionRatio = .3;
  double minimumCompactness = .3, maximumAspect = 3, maximumTownDistanceMetres = 600, minimumSettlementGapMetres = 40;
  uint32_t towns = 1, villages = 2, candidateStrideCells = 5, maximumCandidates = 512;
};
struct SettlementCandidate
{
  uint32_t seed = 0, reasons = 0;
  bool town = true;
  double score = 0;
  std::vector<uint32_t> core, expansion;
};
struct SettlementReservation
{
  uint64_t id = 0, parentTown = 0;
  bool town = true;
  Position center;
  double areaMetres2 = 0, score = 0;
  uint32_t component = 0;
  std::vector<uint32_t> core, expansion;
};
struct SettlementProduct
{
  uint64_t suitabilityFingerprint = 0;
  uint32_t unfilledTowns = 0, unfilledVillages = 0;
  std::vector<SettlementReservation> reservations;
  std::vector<SettlementCandidate> candidates;
  bool matches(const WorldSuitability &s) const
  {
    return suitabilityFingerprint != 0 && suitabilityFingerprint == s.fingerprint();
  }
};
bool evaluateSettlement(const WorldSuitability &, uint32_t seed, bool town, const SettlementSettings &, SettlementCandidate &, std::string *error = nullptr);
bool reserveSettlements(const WorldSuitability &, const SettlementSettings &, SettlementProduct &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
