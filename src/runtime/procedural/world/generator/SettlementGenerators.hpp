#pragma once

#include "Generator.hpp"
#include "procedural/world/WorldSettlementFabricAdapter.hpp"
#include "procedural/world/WorldSettlementSiteAdapter.hpp"
#include "procedural/world/WorldCityAgriculture.hpp"
#include "procedural/world/WorldCityCastle.hpp"
#include "procedural/world/WorldCityCompletion.hpp"
#include "procedural/world/WorldCityExterior.hpp"
#include "procedural/world/WorldCityGrowth.hpp"
#include "procedural/world/WorldCityModel.hpp"
#include "procedural/world/WorldCityRelief.hpp"
#include "procedural/world/WorldCitySkeleton.hpp"

#include <memory>
#include <string>
#include <vector>

namespace procedural::world::generator {

using rewrite::CityTerritory;
using rewrite::RuntimeBuildInput;
using rewrite::RuntimeBuildResult;
using rewrite::SettlementFabric;
using rewrite::SettlementFabricSettings;
using rewrite::SettlementRuntimeSettings;
using rewrite::TerritoryGenerationBrief;
using rewrite::WorldSettlementSite;
using rewrite::WorldSiteGenerationBrief;

// Parent: picks the site on the actual world and emits its planning cells
// and hinterland regions (inside WorldSettlementSite) for the children.
class SettlementSiteGenerator final : public Generator<WorldSiteGenerationBrief, WorldSettlementSite>
{
public:
  explicit SettlementSiteGenerator(uint64_t domain);

protected:
  bool run(const WorldSiteGenerationBrief &input, WorldSettlementSite &output, Ledger &ledger, std::string *error) override;
};

// Bridge: the site's composed territory brief as its own product.
class TerritoryBriefGenerator final : public Generator<WorldSettlementSite, TerritoryGenerationBrief>
{
public:
  explicit TerritoryBriefGenerator(uint64_t domain);

protected:
  bool run(const WorldSettlementSite &input, TerritoryGenerationBrief &output, Ledger &ledger, std::string *error) override;
};

// Child of the site: districts, farmland, fields and farmsteads inside the
// site's regions.
class TerritoryGenerator final : public Generator<TerritoryGenerationBrief, CityTerritory>
{
public:
  explicit TerritoryGenerator(uint64_t domain);

protected:
  bool run(const TerritoryGenerationBrief &input, CityTerritory &output, Ledger &ledger, std::string *error) override;
};

// Child of the territory: terrace, lanes, houses, residents and instances.
class FabricGenerator final : public GeneratorBase
{
public:
  explicit FabricGenerator(uint64_t domain);

  GenerationDescriptor descriptor() const override;

  bool generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error) override;

private:
  uint64_t domain_;
};

// City-scale structural composition. These three tasks retain the lineage of
// the world-map Voronoi hierarchy while exposing city-specific cross graphs.
class CitySkeletonGenerator final : public GeneratorBase
{
public:
  explicit CitySkeletonGenerator(uint64_t domain);
  GenerationDescriptor descriptor() const override;
  bool generate(const GenerationRequest &, GenerationSnapshot &, Ledger &, std::string *error) override;
private:
  uint64_t domain_;
};

class CityExteriorGenerator final : public GeneratorBase
{
public:
  explicit CityExteriorGenerator(uint64_t domain);
  GenerationDescriptor descriptor() const override;
  bool generate(const GenerationRequest &, GenerationSnapshot &, Ledger &, std::string *error) override;
private:
  uint64_t domain_;
};

// The hill, decided. The road network is graded against the settlement's
// surface, every lot terraces off the streets it fronts, and the resulting
// ground field is what the model and the runtime mesh read instead of the
// whole-town terrace.
class CityReliefGenerator final : public GeneratorBase
{
public:
  explicit CityReliefGenerator(uint64_t domain);
  GenerationDescriptor descriptor() const override;
  bool generate(const GenerationRequest &, GenerationSnapshot &, Ledger &, std::string *error) override;
private:
  uint64_t domain_;
};

class CityAgricultureGenerator final : public GeneratorBase
{
public:
  explicit CityAgricultureGenerator(uint64_t domain);
  GenerationDescriptor descriptor() const override;
  bool generate(const GenerationRequest &, GenerationSnapshot &, Ledger &, std::string *error) override;
private:
  uint64_t domain_;
};

class CityCastleGenerator final : public GeneratorBase
{
public:
  explicit CityCastleGenerator(uint64_t domain);
  GenerationDescriptor descriptor() const override;
  bool generate(const GenerationRequest &, GenerationSnapshot &, Ledger &, std::string *error) override;
private:
  uint64_t domain_;
};

class CityGrowthGenerator final : public GeneratorBase
{
public:
  explicit CityGrowthGenerator(uint64_t domain);
  GenerationDescriptor descriptor() const override;
  bool generate(const GenerationRequest &, GenerationSnapshot &, Ledger &, std::string *error) override;
private:
  uint64_t domain_;
};

class CityCompletionGenerator final : public GeneratorBase
{
public:
  explicit CityCompletionGenerator(uint64_t domain);
  GenerationDescriptor descriptor() const override;
  bool generate(const GenerationRequest &, GenerationSnapshot &, Ledger &, std::string *error) override;
private:
  uint64_t domain_;
};

class CityModelGenerator final : public GeneratorBase
{
public:
  explicit CityModelGenerator(uint64_t domain, rewrite::CityModelSettings settings = {});
  GenerationDescriptor descriptor() const override;
  bool generate(const GenerationRequest &, GenerationSnapshot &, Ledger &, std::string *error) override;
private:
  rewrite::CityModelSettings settings_;
  uint64_t domain_;
};

// One bounded runtime job. The batching policy issues one per tile of the
// hinterland, or one for the settlement core.
struct RuntimeJob
{
  std::string name;
  double minX = 0, minZ = 0, maxX = 0, maxZ = 0;
  RuntimeBuildInput input;
};

struct RuntimeJobSet
{
  std::vector<RuntimeJob> jobs;
};

struct RuntimeJobResult
{
  std::string name;
  double minX = 0, minZ = 0, maxX = 0, maxZ = 0;
  bool built = false;
  std::string error;
  RuntimeBuildResult result;
};

struct RuntimeJobResults
{
  std::vector<RuntimeJobResult> jobs;
  size_t built = 0;
};

class RuntimeJobGenerator final : public GeneratorBase
{
public:
  explicit RuntimeJobGenerator(uint64_t domain);

  GenerationDescriptor descriptor() const override;

  bool generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error) override;

private:
  uint64_t domain_;
};

// Builds every job through the existing runtime builder. A job that exceeds a
// budget is recorded as a capacity entry; the other jobs still build.
class RuntimeBuildGenerator final : public GeneratorBase
{
public:
  explicit RuntimeBuildGenerator(uint64_t domain);

  GenerationDescriptor descriptor() const override;

  bool generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error) override;

private:
  uint64_t domain_;
};

// The settlement chain as director tasks, sharing one ledger.
std::vector<GenerationTask> settlementGeneratorTasks(uint64_t domain, std::shared_ptr<Ledger> ledger, rewrite::CityModelSettings modelSettings = {});

} // namespace procedural::world::generator
