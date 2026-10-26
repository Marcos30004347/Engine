// Cardinality-aware planning, and the runner's pre-flight check.
//
// A traversal seeded from a common edge type starts with a table that size, and
// every later step multiplies out of it. Starting from the rarest type instead
// is the single biggest lever on how much work a match does.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"
#include "graphrewrite/runtime/PlanCheck.hpp"

#include <algorithm>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

// `rare` has very few edges; `common` has many. A rule matching both should be
// seeded from `rare`.
const char *twoTypeRule()
{
  return "let A common B.\n"
         "let A rare B.\n"
         "rule r: when A common B, B rare A then A common B.\n";
}

struct Built
{
  compiler::CompiledProgram program;
  compiler::UsageValues usage;
  DiagnosticSink sink;
  bool ok = false;
};

void build(const std::string &source, const std::vector<uint32_t> &counts, Built &out)
{
  compiler::CompileOptions options;

  options.edgeCounts = counts;

  out.ok = compiler::compile(source, "t.graphwrite", options, out.sink, out.program, out.usage);
}

compiler::EdgeTypeId typeOf(const Built &built, const char *pattern)
{
  compiler::EdgeTypeId id = compiler::invalidEdgeType;

  built.program.edgeTypes.find(pattern, id);

  return id;
}

const compiler::Instruction *seedOf(const compiler::CompiledProgram &program, const uint32_t rule)
{
  for (const compiler::Instruction &instruction : program.instructions)
  {
    if (instruction.op != compiler::Op::Select)
    {
      continue;
    }

    if (std::find(instruction.rules.begin(), instruction.rules.end(), rule) != instruction.rules.end())
    {
      return &instruction;
    }
  }

  return nullptr;
}

void withoutStatisticsPlanningIsShareOrdered()
{
  Built built;

  build(twoTypeRule(), {}, built);

  check(built.ok, "compiles without statistics");

  const compiler::Instruction *seed = seedOf(built.program, 0u);

  check(seed != nullptr, "the rule has a seed");
  checkEqual(built.program.edgeTypes[0].assumedCount, 0u, "and no cardinality was assumed");
}

void statisticsMoveTheSeedToTheRarestType()
{
  Built built;

  const compiler::EdgeTypeId commonType = 0u; // declaration order
  const compiler::EdgeTypeId rareType = 1u;

  // `common` has a million edges, `rare` has ten.
  build(twoTypeRule(), {1000000u, 10u}, built);

  check(built.ok, "compiles with statistics");

  const compiler::Instruction *seed = seedOf(built.program, 0u);

  check(seed != nullptr, "the rule has a seed");
  checkEqual(seed->label, rareType, "and it is seeded from the RARE type, not the common one");
  check(seed->label != commonType, "so the first table is ten rows rather than a million");

  checkEqual(built.program.edgeTypes[commonType].assumedCount, 1000000u, "the plan records what it assumed for the common type");
  checkEqual(built.program.edgeTypes[rareType].assumedCount, 10u, "and for the rare one");
}

void reversingTheStatisticsReversesTheSeed()
{
  // The ordering must follow the data, not the declaration order.
  Built built;

  build(twoTypeRule(), {10u, 1000000u}, built);

  const compiler::Instruction *seed = seedOf(built.program, 0u);

  checkEqual(seed->label, compiler::EdgeTypeId(0u), "with the counts swapped, the seed swaps too");
}

void planCheckFlagsASuboptimalSeed()
{
  // Plan against one distribution, then run against the opposite one.
  Built built;

  build(twoTypeRule(), {10u, 1000000u}, built);

  const compiler::EdgeTypeId planned = seedOf(built.program, 0u)->label;

  EdgePageStore store;

  store.configure(built.program.edgeTypes);

  // Reality: the type it was seeded from is now the common one.
  for (uint32_t i = 0u; i < 500u; ++i)
  {
    store.addEdge(planned, Edge{i, i + 1u});
  }

  store.addEdge(planned == 0u ? 1u : 0u, Edge{1u, 2u});

  CpuProgramRunner runner(built.program, built.usage, store, CpuProgramRunner::Settings());

  const PlanCheck check1 = runner.checkPlan();

  check(!check1.optimal(), "the check notices the seed is no longer the rarest type");
  checkEqual(check1.suboptimalSeeds.size(), size_t(1), "for the one rule");

  const PlanCheck::Mismatch &mismatch = check1.suboptimalSeeds[0];

  checkEqual(mismatch.plannedSeed, planned, "naming the type it was planned to seed from");
  checkEqual(mismatch.plannedSeedCount, 500u, "how many edges that now has");
  check(mismatch.rarestCount < mismatch.plannedSeedCount, "and that something rarer is available");

  check(check1.plannedSeedRows > check1.bestSeedRows, "the check quantifies the cost: planned seed rows exceed the best achievable");

  std::cout << "    plan check: seeds " << check1.plannedSeedRows << " rows, best achievable " << check1.bestSeedRows << "\n";
}

void planCheckIsQuietWhenThePlanFits()
{
  Built built;

  build(twoTypeRule(), {500u, 1u}, built);

  EdgePageStore store;

  store.configure(built.program.edgeTypes);

  for (uint32_t i = 0u; i < 500u; ++i)
  {
    store.addEdge(0u, Edge{i, i + 1u});
  }

  store.addEdge(1u, Edge{1u, 2u});

  CpuProgramRunner runner(built.program, built.usage, store, CpuProgramRunner::Settings());

  const PlanCheck check2 = runner.checkPlan();

  check(check2.optimal(), "a plan that matches the data reports no suboptimal seed");
  checkEqual(check2.staleStatistics.size(), size_t(0), "and no stale statistics");
}

void planCheckReportsStaleStatistics()
{
  Built built;

  build(twoTypeRule(), {500u, 1u}, built);

  EdgePageStore store;

  store.configure(built.program.edgeTypes);

  // Nothing seeded at all: every assumed count is now wrong.
  CpuProgramRunner runner(built.program, built.usage, store, CpuProgramRunner::Settings());

  const PlanCheck check3 = runner.checkPlan();

  check(!check3.staleStatistics.empty(), "an empty graph makes every assumed count stale, and the check says so");
}

void seedingFromTheRarestTypeReallyMatchesLess()
{
  // The point of all of it: fewer rows seeded means less work, with the same
  // answer.
  const char *source = "let A common B.\n"
                       "let A rare B.\n"
                       "rule r: when A common B, B rare A then A common B.\n";

  std::vector<Fact> facts;

  for (uint32_t i = 0u; i < 400u; ++i)
  {
    facts.emplace_back(i, EdgeTypeId(0u), i + 1u); // common
  }

  for (uint32_t i = 0u; i < 3u; ++i)
  {
    facts.emplace_back(i + 1u, EdgeTypeId(1u), i); // rare, and matching
  }

  uint32_t rowsCommonSeeded = 0u;
  uint32_t rowsRareSeeded = 0u;
  uint32_t matchesCommon = 0u;
  uint32_t matchesRare = 0u;

  for (uint32_t variant = 0u; variant < 2u; ++variant)
  {
    Built built;

    // variant 0: plan says `common` is rare (wrong). variant 1: the truth.
    build(source, variant == 0u ? std::vector<uint32_t>{1u, 400u} : std::vector<uint32_t>{400u, 3u}, built);

    EdgePageStore store;

    store.configure(built.program.edgeTypes);

    CpuProgramRunner runner(built.program, built.usage, store, CpuProgramRunner::Settings());

    runner.seed(facts);
    runner.runToCompletion();

    const compiler::Instruction *seed = seedOf(built.program, 0u);
    const uint32_t seededRows = runner.tableForRegister(seed->reg) == nullptr ? 0u : runner.tableForRegister(seed->reg)->rows();

    if (variant == 0u)
    {
      rowsCommonSeeded = seededRows;
      matchesCommon = runner.resultForRule(0u).rows;
    }
    else
    {
      rowsRareSeeded = seededRows;
      matchesRare = runner.resultForRule(0u).rows;
    }
  }

  checkEqual(matchesCommon, matchesRare, "both plans find the same matches - the ordering changes cost, not the answer");
  check(matchesRare > 0u, "and there really were matches to find");
  check(rowsRareSeeded < rowsCommonSeeded, "but seeding from the rare type starts with far fewer rows");

  std::cout << "    seeded rows: " << rowsCommonSeeded << " from the common type vs " << rowsRareSeeded
            << " from the rare one, same " << matchesRare << " matches\n";
}

} // namespace

int main()
{
  withoutStatisticsPlanningIsShareOrdered();
  statisticsMoveTheSeedToTheRarestType();
  reversingTheStatisticsReversesTheSeed();
  planCheckFlagsASuboptimalSeed();
  planCheckIsQuietWhenThePlanFits();
  planCheckReportsStaleStatistics();
  seedingFromTheRarestTypeReallyMatchesLess();

  return report("GraphRewritePlanningTests");
}
