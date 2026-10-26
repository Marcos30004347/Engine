#include "procedural/world/MapPlacement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace procedural::world
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool finiteRange(const std::array<double, 2> &range)
{
  return std::isfinite(range[0]) && std::isfinite(range[1]) && range[0] <= range[1];
}

uint64_t mix64(uint64_t value)
{
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}

uint64_t siteHash(const uint64_t seed, const int64_t x, const int64_t y, const uint64_t salt)
{
  uint64_t value = mix64(seed ^ salt);
  value ^= mix64(static_cast<uint64_t>(x) ^ 0x6a09e667f3bcc909ull);
  value ^= mix64(static_cast<uint64_t>(y) ^ 0xbb67ae8584caa73bull);
  return mix64(value);
}

double unitHash(const uint64_t value)
{
  return static_cast<double>(value >> 11u) * (1.0 / 9007199254740992.0);
}

bool inside(const double value, const std::array<double, 2> &range)
{
  return value >= range[0] && value <= range[1];
}

double biomeAffinity(const PlacementAssetRule &rule, const MapExportBiomeSample &sample)
{
  if (rule.biomes.empty())
    return 1.0;
  double affinity = 0.0;
  for (uint32_t component = 0u; component < sample.count; ++component)
    for (const PlacementBiomeAffinity &entry : rule.biomes)
      if (entry.biome == static_cast<Biome>(sample.ids[component]))
        affinity += sample.weights[component] * entry.weight;
  return affinity;
}

bool acceptsSubstrate(const PlacementAssetRule &rule, const uint8_t substrate)
{
  return rule.substrates.empty() || std::find(rule.substrates.begin(), rule.substrates.end(), substrate) != rule.substrates.end();
}

struct EvaluatedSite
{
  enum class Rejection : uint8_t
  {
    None,
    OutsideWorld,
    Water,
    Exclusion,
    Density,
  };

  int64_t x = 0;
  int64_t y = 0;
  Point2 position{};
  uint64_t priority = 0u;
  bool acceptedDensity = false;
  bool usedAncestor = false;
  Rejection rejection = Rejection::OutsideWorld;
  PlacementCandidate candidate{};
};

struct EligibleAsset
{
  uint32_t assetIndex = 0u;
  double selectionWeight = 0.0;
  double densityAffinity = 0.0;
};

const MapExportField *availableField(const MapExportBundle &bundle, const char *name)
{
  const MapExportField *field = bundle.field(name);
  return field != nullptr && field->available ? field : nullptr;
}

double componentOrZero(const MapExportFieldSample &sample, const uint32_t component)
{
  return component < sample.componentCount ? sample.values[component] : 0.0;
}

} // namespace

bool PlacementPalette::validate(std::string *error) const
{
  if (version == 0u || assets.empty())
    return fail(error, "placement palette has no versioned asset rules");
  std::set<std::string> keys;
  for (const PlacementAssetRule &asset : assets)
  {
    if (asset.assetKey.empty() || asset.collectionKey.empty() || asset.resourceKey.empty() || !keys.insert(asset.assetKey).second)
      return fail(error, "placement palette asset keys and resource references must be non-empty and unique");
    if (static_cast<uint32_t>(asset.kind) > static_cast<uint32_t>(PlacementAssetKind::Mesh) || static_cast<uint32_t>(asset.potential) > static_cast<uint32_t>(PlacementPotentialChannel::ExposedRock) ||
        !std::isfinite(asset.selectionWeight) || asset.selectionWeight <= 0.0 || !std::isfinite(asset.densityMultiplier) || asset.densityMultiplier < 0.0 || !std::isfinite(asset.footprintRadiusMetres) ||
        asset.footprintRadiusMetres < 0.0 || !std::isfinite(asset.minimumSpacingMetres) || asset.minimumSpacingMetres < 0.0 || !finiteRange(asset.scaleRange) || asset.scaleRange[0] <= 0.0 ||
        !finiteRange(asset.altitudeRangeMetres) || !finiteRange(asset.slopeRangeDegrees) || asset.slopeRangeDegrees[0] < 0.0 || asset.slopeRangeDegrees[1] > 90.0 || !finiteRange(asset.moistureRange) ||
        asset.moistureRange[0] < 0.0 || asset.moistureRange[1] > 1.0 || !finiteRange(asset.soilDepthRangeMetres) || asset.soilDepthRangeMetres[0] < 0.0 || !finiteRange(asset.riverDistanceRangeMetres) ||
        asset.riverDistanceRangeMetres[0] < 0.0)
      return fail(error, "placement palette contains an invalid asset rule");
    std::set<uint8_t> biomes;
    for (const PlacementBiomeAffinity &affinity : asset.biomes)
      if (static_cast<uint8_t>(affinity.biome) >= static_cast<uint8_t>(Biome::Count) || !std::isfinite(affinity.weight) || affinity.weight < 0.0 || !biomes.insert(static_cast<uint8_t>(affinity.biome)).second)
        return fail(error, "placement palette contains an invalid biome affinity");
    std::set<uint8_t> substrates;
    for (const uint8_t substrate : asset.substrates)
      if (!substrates.insert(substrate).second)
        return fail(error, "placement palette repeats a substrate ID");
  }
  return true;
}

bool buildPlacementCandidates(const MapExportBundle &bundle, const PlacementPalette &palette, const PlacementTileRequest &request, PlacementCandidateSet &result, std::string *error)
{
  result = {};
  if (!palette.validate(error))
    return false;
  if (!(bundle.extentMetres > 0.0) || !std::isfinite(bundle.extentMetres) || bundle.resolution == 0u || request.requestedLevel > bundle.maximumPageLevel || !(request.candidateCellMetres > 0.0) ||
      !std::isfinite(request.candidateCellMetres) || request.maximumEvaluatedSites == 0u || request.maximumCandidates == 0u)
    return fail(error, "placement tile request or map extent is invalid");
  for (const double coordinate : request.normalizedBounds)
    if (!std::isfinite(coordinate))
      return fail(error, "placement tile bounds are not finite");

  const MapExportField *placementField = availableField(bundle, "placement_potential");
  const MapExportField *vegetationPotentialField = availableField(bundle, "vegetation_potential");
  const MapExportField *rockPotentialField = availableField(bundle, "rock_potential");
  const MapExportField *exposedBedrockField = availableField(bundle, "exposed_bedrock");
  const MapExportField *canopyDensityField = availableField(bundle, "canopy_density");
  const MapExportField *groundDensityField = availableField(bundle, "ground_density");
  const MapExportField *riparianMaskField = availableField(bundle, "riparian_mask");
  const MapExportField *waterField = availableField(bundle, "water_type");
  const MapExportField *climateField = availableField(bundle, "climate");
  const MapExportField *substrateField = availableField(bundle, "substrate_id");
  const MapExportField *soilField = availableField(bundle, "soil_depth");
  const MapExportField *riverField = availableField(bundle, "river");
  const MapExportField *elevationField = availableField(bundle, "refined_elevation");
  if (elevationField == nullptr || !elevationField->available)
    elevationField = availableField(bundle, "base_elevation");
  if ((placementField == nullptr && vegetationPotentialField == nullptr && rockPotentialField == nullptr && exposedBedrockField == nullptr) || waterField == nullptr || climateField == nullptr ||
      substrateField == nullptr || soilField == nullptr || riverField == nullptr || elevationField == nullptr)
    return fail(error, "placement planning requires placement or rock/vegetation potential plus water, climate, elevation, river, substrate and soil fields");
  const MapExportField *exclusionField = availableField(bundle, "object_exclusion");

  const double minimumU = std::clamp(std::min(request.normalizedBounds[0], request.normalizedBounds[2]), 0.0, 1.0);
  const double minimumV = std::clamp(std::min(request.normalizedBounds[1], request.normalizedBounds[3]), 0.0, 1.0);
  const double maximumU = std::clamp(std::max(request.normalizedBounds[0], request.normalizedBounds[2]), 0.0, 1.0);
  const double maximumV = std::clamp(std::max(request.normalizedBounds[1], request.normalizedBounds[3]), 0.0, 1.0);
  if (!(maximumU > minimumU) || !(maximumV > minimumV))
    return fail(error, "placement tile bounds have no area");
  const double minimumX = minimumU * bundle.extentMetres;
  const double minimumY = minimumV * bundle.extentMetres;
  const double maximumX = maximumU * bundle.extentMetres;
  const double maximumY = maximumV * bundle.extentMetres;

  double maximumConflictDistance = 0.0;
  for (const PlacementAssetRule &asset : palette.assets)
    maximumConflictDistance = std::max(maximumConflictDistance, std::max(asset.minimumSpacingMetres, asset.footprintRadiusMetres * 2.0));
  const double worldCellCount = std::ceil(bundle.extentMetres / request.candidateCellMetres);
  if (!std::isfinite(worldCellCount) || worldCellCount > static_cast<double>(std::numeric_limits<int64_t>::max()))
    return fail(error, "placement candidate lattice is too large");
  const int64_t worldCells = std::max<int64_t>(1, static_cast<int64_t>(worldCellCount));
  const double conflictCellCount = std::ceil(maximumConflictDistance / request.candidateCellMetres) + 1.0;
  const int64_t conflictReach = !std::isfinite(conflictCellCount) || conflictCellCount >= static_cast<double>(worldCells) ? worldCells : static_cast<int64_t>(conflictCellCount);
  const int64_t firstX = std::max<int64_t>(0, static_cast<int64_t>(std::floor(minimumX / request.candidateCellMetres)) - conflictReach);
  const int64_t firstY = std::max<int64_t>(0, static_cast<int64_t>(std::floor(minimumY / request.candidateCellMetres)) - conflictReach);
  const int64_t lastX = std::min<int64_t>(worldCells - 1, static_cast<int64_t>(std::floor(maximumX / request.candidateCellMetres)) + conflictReach);
  const int64_t lastY = std::min<int64_t>(worldCells - 1, static_cast<int64_t>(std::floor(maximumY / request.candidateCellMetres)) + conflictReach);
  if (lastX < firstX || lastY < firstY)
    return fail(error, "placement tile does not intersect the candidate lattice");
  const size_t width = static_cast<size_t>(lastX - firstX + 1);
  const size_t height = static_cast<size_t>(lastY - firstY + 1);
  if (width == 0u || height == 0u || width > request.maximumEvaluatedSites / height)
    return fail(error, "placement tile exceeds its evaluated-site budget");
  result.evaluatedSites = width * height;
  std::vector<EvaluatedSite> sites(result.evaluatedSites);

  const auto sampleField = [&](const std::string &name, const Point2 &position, MapExportFieldSample &sample) -> bool
  {
    return sampleMapExportFieldAtWorld(bundle, name, position, request.requestedLevel, sample, error);
  };
  const double gradientStep = std::max(request.candidateCellMetres * 0.5, bundle.extentMetres / static_cast<double>(bundle.resolution));
  for (int64_t gridY = firstY; gridY <= lastY; ++gridY)
    for (int64_t gridX = firstX; gridX <= lastX; ++gridX)
    {
      EvaluatedSite &site = sites[static_cast<size_t>(gridY - firstY) * width + static_cast<size_t>(gridX - firstX)];
      site.x = gridX;
      site.y = gridY;
      const double jitterX = (unitHash(siteHash(bundle.seed, gridX, gridY, 0x243f6a8885a308d3ull)) - 0.5) * 0.8;
      const double jitterY = (unitHash(siteHash(bundle.seed, gridX, gridY, 0x13198a2e03707344ull)) - 0.5) * 0.8;
      const Point2 position((static_cast<double>(gridX) + 0.5 + jitterX) * request.candidateCellMetres, (static_cast<double>(gridY) + 0.5 + jitterY) * request.candidateCellMetres);
      site.position = position;
      if (position.x < 0.0 || position.y < 0.0 || position.x >= bundle.extentMetres || position.y >= bundle.extentMetres)
        continue;

      MapExportFieldSample water{};
      if (!sampleField("water_type", position, water))
        return false;
      site.usedAncestor = water.usedAncestor();
      if (water.values[0] >= 0.5)
      {
        site.rejection = EvaluatedSite::Rejection::Water;
        continue;
      }
      if (exclusionField != nullptr)
      {
        MapExportFieldSample exclusion{};
        if (!sampleField("object_exclusion", position, exclusion))
          return false;
        site.usedAncestor = site.usedAncestor || exclusion.usedAncestor();
        if (exclusion.values[0] >= 0.5)
        {
          site.rejection = EvaluatedSite::Rejection::Exclusion;
          continue;
        }
      }

      MapExportFieldSample potential{};
      MapExportFieldSample vegetationPotential{};
      MapExportFieldSample rockPotential{};
      MapExportFieldSample exposedBedrock{};
      MapExportFieldSample canopyDensity{};
      MapExportFieldSample groundDensity{};
      MapExportFieldSample riparianMask{};
      MapExportFieldSample elevation{};
      MapExportFieldSample climate{};
      MapExportFieldSample substrate{};
      MapExportFieldSample soil{};
      MapExportFieldSample river{};
      MapExportBiomeSample biome{};
      if ((placementField != nullptr && !sampleField("placement_potential", position, potential)) || (vegetationPotentialField != nullptr && !sampleField("vegetation_potential", position, vegetationPotential)) ||
          (rockPotentialField != nullptr && !sampleField("rock_potential", position, rockPotential)) || (exposedBedrockField != nullptr && !sampleField("exposed_bedrock", position, exposedBedrock)) ||
          (canopyDensityField != nullptr && !sampleField("canopy_density", position, canopyDensity)) || (groundDensityField != nullptr && !sampleField("ground_density", position, groundDensity)) ||
          (riparianMaskField != nullptr && !sampleField("riparian_mask", position, riparianMask)) || !sampleField(elevationField->name, position, elevation) || !sampleField("climate", position, climate) ||
          !sampleField("substrate_id", position, substrate) || !sampleField("soil_depth", position, soil) || !sampleField("river", position, river) ||
          !sampleMapExportBiomeAtWorld(bundle, position, request.requestedLevel, biome, error))
        return false;
      site.usedAncestor = site.usedAncestor || potential.usedAncestor() || vegetationPotential.usedAncestor() || rockPotential.usedAncestor() || exposedBedrock.usedAncestor() || canopyDensity.usedAncestor() ||
          groundDensity.usedAncestor() || riparianMask.usedAncestor() || elevation.usedAncestor() || climate.usedAncestor() || substrate.usedAncestor() || soil.usedAncestor() || river.usedAncestor() ||
          biome.usedAncestor();
      const Point2 left(std::max(0.0, position.x - gradientStep), position.y);
      const Point2 right(std::min(bundle.extentMetres, position.x + gradientStep), position.y);
      const Point2 down(position.x, std::max(0.0, position.y - gradientStep));
      const Point2 up(position.x, std::min(bundle.extentMetres, position.y + gradientStep));
      MapExportFieldSample leftElevation{};
      MapExportFieldSample rightElevation{};
      MapExportFieldSample downElevation{};
      MapExportFieldSample upElevation{};
      if (!sampleField(elevationField->name, left, leftElevation) || !sampleField(elevationField->name, right, rightElevation) || !sampleField(elevationField->name, down, downElevation) ||
          !sampleField(elevationField->name, up, upElevation))
        return false;
      site.usedAncestor = site.usedAncestor || leftElevation.usedAncestor() || rightElevation.usedAncestor() || downElevation.usedAncestor() || upElevation.usedAncestor();
      const double slopeX = (rightElevation.values[0] - leftElevation.values[0]) / std::max(1.0, right.x - left.x);
      const double slopeY = (upElevation.values[0] - downElevation.values[0]) / std::max(1.0, up.y - down.y);
      const double slopeDegrees = std::atan(std::hypot(slopeX, slopeY)) * 180.0 / kPi;
      const uint8_t substrateId = static_cast<uint8_t>(std::clamp(std::llround(substrate.values[0]), 0ll, 255ll));

      std::vector<EligibleAsset> eligible;
      double totalWeight = 0.0;
      for (uint32_t assetIndex = 0u; assetIndex < palette.assets.size(); ++assetIndex)
      {
        const PlacementAssetRule &asset = palette.assets[assetIndex];
        const double affinity = biomeAffinity(asset, biome);
        if (!(affinity > 0.0) || !inside(elevation.values[0], asset.altitudeRangeMetres) || !inside(slopeDegrees, asset.slopeRangeDegrees) || !inside(climate.values[1], asset.moistureRange) ||
            !inside(soil.values[0], asset.soilDepthRangeMetres) || !inside(std::max(0.0, river.values[0]), asset.riverDistanceRangeMetres) || !acceptsSubstrate(asset, substrateId))
          continue;
        const double weight = asset.selectionWeight * affinity;
        eligible.push_back({assetIndex, weight, affinity});
        totalWeight += weight;
      }
      if (eligible.empty() || !(totalWeight > 0.0))
      {
        site.rejection = EvaluatedSite::Rejection::Density;
        continue;
      }
      double selection = unitHash(siteHash(bundle.seed, gridX, gridY, 0xa4093822299f31d0ull)) * totalWeight;
      const EligibleAsset *selected = &eligible.back();
      for (const EligibleAsset &candidate : eligible)
      {
        if (selection <= candidate.selectionWeight)
        {
          selected = &candidate;
          break;
        }
        selection -= candidate.selectionWeight;
      }
      const PlacementAssetRule &asset = palette.assets[selected->assetIndex];
      // Biome affinity is a density layer as well as a weighted asset choice.
      // This keeps a species' occupancy proportional through ecotones instead
      // of allowing a tiny biome contributor to place at full channel density.
      double authoredPotential = placementField != nullptr ? componentOrZero(potential, static_cast<uint32_t>(asset.potential)) : 0.0;
      if (asset.potential == PlacementPotentialChannel::Trees && vegetationPotentialField != nullptr)
      {
        authoredPotential = std::max(authoredPotential, componentOrZero(vegetationPotential, 0u) * std::max(0.0, componentOrZero(canopyDensity, 0u)));
        authoredPotential = std::max(authoredPotential, componentOrZero(vegetationPotential, 3u) * std::max(0.0, componentOrZero(riparianMask, 0u)));
      }
      else if (asset.potential == PlacementPotentialChannel::Shrubs && vegetationPotentialField != nullptr)
      {
        authoredPotential = std::max(authoredPotential, componentOrZero(vegetationPotential, 1u));
        authoredPotential = std::max(authoredPotential, componentOrZero(vegetationPotential, 3u) * std::max(0.0, componentOrZero(riparianMask, 0u)));
      }
      else if (asset.potential == PlacementPotentialChannel::Groundcover && vegetationPotentialField != nullptr)
      {
        authoredPotential = std::max(authoredPotential, componentOrZero(vegetationPotential, 2u) * std::max(0.0, componentOrZero(groundDensity, 0u)));
      }
      else if (asset.potential == PlacementPotentialChannel::ExposedRock)
      {
        if (rockPotentialField != nullptr)
          authoredPotential = std::max({authoredPotential, componentOrZero(rockPotential, 0u), componentOrZero(rockPotential, 1u), componentOrZero(rockPotential, 2u), componentOrZero(rockPotential, 3u)});
        if (exposedBedrockField != nullptr)
          authoredPotential = std::max(authoredPotential, componentOrZero(exposedBedrock, 0u));
      }
      const double density = std::clamp(authoredPotential * asset.densityMultiplier * selected->densityAffinity, 0.0, 1.0);
      if (unitHash(siteHash(bundle.seed, gridX, gridY, 0x082efa98ec4e6c89ull)) >= density)
      {
        site.rejection = EvaluatedSite::Rejection::Density;
        continue;
      }
      const double scaleT = unitHash(siteHash(bundle.seed, gridX, gridY, 0x452821e638d01377ull));
      site.priority = siteHash(bundle.seed, gridX, gridY, 0xbe5466cf34e90c6cull);
      site.acceptedDensity = true;
      site.rejection = EvaluatedSite::Rejection::None;
      site.candidate = {
        siteHash(bundle.seed, gridX, gridY, 0xc0ac29b7c97c50ddull),
        selected->assetIndex,
        position,
        unitHash(siteHash(bundle.seed, gridX, gridY, 0x3f84d5b5b5470917ull)) * 2.0 * kPi,
        asset.scaleRange[0] + (asset.scaleRange[1] - asset.scaleRange[0]) * scaleT,
        density,
        elevation.values[0],
        slopeDegrees,
        climate.values[1],
        soil.values[0],
        std::max(0.0, river.values[0]),
        substrateId,
        static_cast<Biome>(biome.dominantBiome()),
        placementField != nullptr ? potential.resolvedPage
                                  : (vegetationPotentialField != nullptr ? vegetationPotential.resolvedPage
                                                                         : (rockPotentialField != nullptr ? rockPotential.resolvedPage : exposedBedrock.resolvedPage)),
      };
    }

  const auto siteAt = [&](const int64_t x, const int64_t y) -> const EvaluatedSite *
  {
    if (x < firstX || x > lastX || y < firstY || y > lastY)
      return nullptr;
    return &sites[static_cast<size_t>(y - firstY) * width + static_cast<size_t>(x - firstX)];
  };
  for (const EvaluatedSite &site : sites)
  {
    const Point2 position = site.position;
    const bool inRequest = position.x >= minimumX && position.y >= minimumY && position.x < maximumX && position.y < maximumY;
    if (!inRequest)
      continue;
    if (site.usedAncestor)
      ++result.ancestorSampledSites;
    if (!site.acceptedDensity)
    {
      if (site.rejection == EvaluatedSite::Rejection::Water)
        ++result.waterRejected;
      else if (site.rejection == EvaluatedSite::Rejection::Exclusion)
        ++result.exclusionRejected;
      else
        ++result.densityRejected;
      continue;
    }
    const PlacementAssetRule &asset = palette.assets[site.candidate.assetIndex];
    bool blocked = false;
    for (int64_t offsetY = -conflictReach; offsetY <= conflictReach && !blocked; ++offsetY)
      for (int64_t offsetX = -conflictReach; offsetX <= conflictReach; ++offsetX)
      {
        const EvaluatedSite *other = siteAt(site.x + offsetX, site.y + offsetY);
        if (other == nullptr || other == &site || !other->acceptedDensity)
          continue;
        const PlacementAssetRule &otherAsset = palette.assets[other->candidate.assetIndex];
        const double separation = std::max({asset.minimumSpacingMetres, otherAsset.minimumSpacingMetres, asset.footprintRadiusMetres + otherAsset.footprintRadiusMetres});
        const double dx = site.candidate.position.x - other->candidate.position.x;
        const double dy = site.candidate.position.y - other->candidate.position.y;
        if (dx * dx + dy * dy < separation * separation && (other->priority > site.priority || (other->priority == site.priority && other->candidate.stableId < site.candidate.stableId)))
        {
          blocked = true;
          break;
        }
      }
    if (blocked)
    {
      ++result.spacingRejected;
      continue;
    }
    result.candidates.push_back(site.candidate);
    if (result.candidates.size() > request.maximumCandidates)
    {
      result = {};
      return fail(error, "placement tile exceeds its candidate budget");
    }
  }
  std::sort(
      result.candidates.begin(),
      result.candidates.end(),
      [](const PlacementCandidate &left, const PlacementCandidate &right)
      {
        return left.stableId < right.stableId;
      });
  return true;
}

} // namespace procedural::world
