// Phase 3.5: the CPU reference, against a brute-force oracle.
//
// The oracle enumerates every injective map from a rule's vertices to data
// vertices under which all of that rule's WHEN edges exist. That is the
// definition of a match; the paged implementation is an optimisation of it, so
// any disagreement is the implementation's fault.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

using Fact = std::tuple<uint32_t, EdgeTypeId, uint32_t>;

struct Fixture
{
  compiler::CompiledProgram program;
  compiler::UsageValues usage;
  EdgePageStore store;
  DiagnosticSink sink;
  bool ok = false;
};

bool build(const std::string &source, Fixture &out)
{
  out.ok = compiler::compile(source, "t.graphwrite", compiler::CompileOptions(), out.sink, out.program, out.usage);

  if (out.ok)
  {
    out.store.configure(out.program.edgeTypes);
  }

  return out.ok;
}

// Which vertices a rule's WHEN clause binds, and which edges it needs, recovered
// from the compiled programme rather than re-derived.
struct RuleShape
{
  std::vector<std::tuple<uint32_t, EdgeTypeId, uint32_t>> edges; // in rule-vertex space
  uint32_t vertexCount = 0u;
};

RuleShape shapeOf(const compiler::CompiledProgram &program, const uint32_t rule)
{
  RuleShape shape;

  for (const compiler::Instruction &instruction : program.instructions)
  {
    if (instruction.op != compiler::Op::Select && instruction.op != compiler::Op::Where)
    {
      continue;
    }

    for (const compiler::RuleColumns &columns : instruction.perRule)
    {
      if (columns.ruleId != rule)
      {
        continue;
      }

      const bool backward = instruction.direction == compiler::StepDirection::Backward;

      const uint32_t origin = backward ? columns.writeColumn : columns.fromColumn;
      const uint32_t target = backward ? columns.fromColumn : columns.writeColumn;

      shape.edges.emplace_back(origin, instruction.label, target);
      shape.vertexCount = std::max(shape.vertexCount, std::max(origin, target) + 1u);
    }
  }

  return shape;
}

std::set<std::vector<uint32_t>> bruteForce(const RuleShape &shape, const std::vector<Fact> &facts, const uint32_t vertexCount)
{
  std::set<Fact> present(facts.begin(), facts.end());
  std::set<std::vector<uint32_t>> out;

  std::vector<uint32_t> assignment(shape.vertexCount, 0u);
  std::vector<bool> used(vertexCount, false);

  const std::function<void(uint32_t)> recurse = [&](const uint32_t vertex) {
    if (vertex == shape.vertexCount)
    {
      for (const auto &edge : shape.edges)
      {
        const uint32_t origin = assignment[std::get<0>(edge)];
        const uint32_t target = assignment[std::get<2>(edge)];

        if (present.count(std::make_tuple(origin, std::get<1>(edge), target)) == 0u)
        {
          return;
        }
      }

      out.insert(assignment);

      return;
    }

    for (uint32_t candidate = 0u; candidate < vertexCount; ++candidate)
    {
      if (used[candidate])
      {
        continue;
      }

      used[candidate] = true;
      assignment[vertex] = candidate;

      recurse(vertex + 1u);

      used[candidate] = false;
    }
  };

  if (shape.vertexCount > 0u && vertexCount > 0u)
  {
    recurse(0u);
  }

  return out;
}

uint32_t matchedRuleCount(const compiler::CompiledProgram &program, const CpuProgramRunner &runner)
{
  uint32_t count = 0u;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    const MatchTable *table = runner.finalTableForRule(rule);

    count += (table != nullptr && table->rows() != 0u) ? 1u : 0u;
  }

  return count;
}

uint32_t totalRowCount(const compiler::CompiledProgram &program, const CpuProgramRunner &runner)
{
  uint32_t total = 0u;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    const MatchTable *table = runner.finalTableForRule(rule);

    total += table == nullptr ? 0u : table->rows();
  }

  return total;
}

std::set<std::vector<uint32_t>> rowsOf(const MatchTable *table, const RuleShape &shape)
{
  std::set<std::vector<uint32_t>> out;

  if (table == nullptr)
  {
    return out;
  }

  for (uint32_t row = 0u; row < table->rows(); ++row)
  {
    std::vector<uint32_t> assignment(shape.vertexCount, 0u);

    for (uint32_t vertex = 0u; vertex < shape.vertexCount; ++vertex)
    {
      assignment[vertex] = table->at(vertex, row);
    }

    out.insert(assignment);
  }

  return out;
}

void singleRuleMatchesBruteForce()
{
  Fixture fixture;

  check(build("let A likes B.\n"
              "let A knows B.\n"
              "rule r: when A likes B, B knows A then A likes B.\n",
              fixture),
        "programme compiles");

  const std::vector<Fact> facts = {{0u, 0u, 1u}, {1u, 1u, 0u}, {0u, 0u, 2u}, {2u, 1u, 0u}, {3u, 0u, 4u}};

  CpuProgramRunner runner(fixture.program, fixture.usage, fixture.store, CpuProgramRunner::Settings());

  runner.seed(facts);
  runner.runToCompletion();

  const RuleShape shape = shapeOf(fixture.program, 0u);

  const std::set<std::vector<uint32_t>> got = rowsOf(runner.finalTableForRule(0u), shape);
  const std::set<std::vector<uint32_t>> want = bruteForce(shape, facts, 6u);

  checkEqual(got.size(), want.size(), "match count agrees with brute force");
  check(got == want, "and the matches themselves agree");
  checkEqual(got.size(), size_t(2), "two matches, as expected by hand");
}

void randomisedAgainstBruteForce()
{
  Fixture fixture;

  check(build("let A likes B.\n"
              "let A knows B.\n"
              "rule chain: when A likes B, B knows C then A likes B.\n"
              "rule mutual: when A likes B, B likes A then A likes B.\n",
              fixture),
        "two-rule programme compiles");

  std::mt19937 rng(20260919u);

  uint32_t compared = 0u;
  uint32_t mismatches = 0u;
  uint32_t nonEmpty = 0u;

  for (uint32_t trial = 0u; trial < 250u; ++trial)
  {
    const uint32_t vertices = 4u + rng() % 3u;

    std::set<Fact> factSet;

    const uint32_t edges = 3u + rng() % 8u;

    for (uint32_t i = 0u; i < edges; ++i)
    {
      const uint32_t origin = rng() % vertices;
      const uint32_t target = rng() % vertices;

      if (origin != target)
      {
        factSet.insert(std::make_tuple(origin, static_cast<EdgeTypeId>(rng() % 2u), target));
      }
    }

    const std::vector<Fact> facts(factSet.begin(), factSet.end());

    EdgePageStore store;

    store.configure(fixture.program.edgeTypes);

    CpuProgramRunner runner(fixture.program, fixture.usage, store, CpuProgramRunner::Settings());

    runner.seed(facts);
    runner.runToCompletion();

    for (uint32_t rule = 0u; rule < static_cast<uint32_t>(fixture.program.ruleNames.size()); ++rule)
    {
      const RuleShape shape = shapeOf(fixture.program, rule);

      if (shape.vertexCount == 0u)
      {
        continue;
      }

      const std::set<std::vector<uint32_t>> got = rowsOf(runner.finalTableForRule(rule), shape);
      const std::set<std::vector<uint32_t>> want = bruteForce(shape, facts, vertices);

      ++compared;

      if (!want.empty())
      {
        ++nonEmpty;
      }

      if (got != want)
      {
        ++mismatches;

        if (mismatches <= 3u)
        {
          std::cerr << "    trial " << trial << " rule " << fixture.program.ruleNames[rule] << ": got " << got.size() << " want "
                    << want.size() << "\n";
        }
      }
    }
  }

  check(compared > 300u, "enough rule/graph pairs were compared");
  check(nonEmpty > 100u, "and enough had a non-empty expected result");
  checkEqual(mismatches, 0u, "the paged CPU matcher agrees with brute force everywhere");
}

void resultIsIndependentOfPageSize()
{
  // The paging must not change WHAT is matched, only how it is reached. This is
  // the property the GPU's streaming will extend across frames.
  Fixture fixture;

  build("let A likes B.\n"
        "rule r: when A likes B then A likes B.\n",
        fixture);

  std::mt19937 rng(5u);

  std::set<Fact> factSet;

  for (uint32_t i = 0u; i < 900u; ++i)
  {
    const uint32_t origin = rng() % 30u;
    const uint32_t target = rng() % 30u;

    if (origin != target)
    {
      factSet.insert(std::make_tuple(origin, EdgeTypeId(0u), target));
    }
  }

  const std::vector<Fact> facts(factSet.begin(), factSet.end());

  EdgePageStore store;

  store.configure(fixture.program.edgeTypes);

  CpuProgramRunner runner(fixture.program, fixture.usage, store, CpuProgramRunner::Settings());

  runner.seed(facts);
  runner.runToCompletion();

  checkEqual((runner.finalTableForRule(0u) ? runner.finalTableForRule(0u)->rows() : 0u), static_cast<uint32_t>(facts.size()),
             "every edge of the type is a match for a one-edge rule, across many pages");

  check(store.partition(0u, KeyOrder::ByOrigin).pageCount() > 1u, "and the data really did span several pages");
}

void overflowIsReportedNotSilent()
{
  Fixture fixture;

  build("let A likes B.\n"
        "rule r: when A likes B then A likes B.\n",
        fixture);

  std::vector<Fact> facts;

  for (uint32_t i = 0u; i < 50u; ++i)
  {
    facts.emplace_back(i, EdgeTypeId(0u), i + 1u);
  }

  CpuProgramRunner::Settings small;

  small.rowCapacityPerTable = 10u;

  CpuProgramRunner runner(fixture.program, fixture.usage, fixture.store, small);

  runner.seed(facts);
  runner.runToCompletion();

  check(runner.stats().overflowed, "a table that cannot hold every match reports overflow");
  checkEqual((runner.finalTableForRule(0u) ? runner.finalTableForRule(0u)->rows() : 0u), 10u, "and stops at its capacity rather than corrupting memory");
}

void snapshotRoundTrips()
{
  Fixture fixture;

  build("let A likes B.\n"
        "let A knows B.\n"
        "rule r: when A likes B, B knows A then A likes B.\n",
        fixture);

  const std::vector<Fact> facts = {{0u, 0u, 1u}, {1u, 1u, 0u}, {5u, 0u, 6u}};

  CpuProgramRunner runner(fixture.program, fixture.usage, fixture.store, CpuProgramRunner::Settings());

  runner.seed(facts);

  std::vector<Fact> expected = facts;

  std::sort(expected.begin(), expected.end());

  check(runner.snapshot() == expected, "the store round-trips every seeded fact");
}

void corpusRunsWithoutDiagnostics()
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/basic.graphwrite");

  if (!file)
  {
    check(false, "corpus asset found");

    return;
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  Fixture fixture;

  check(build(buffer.str(), fixture), "corpus compiles");

  CpuProgramRunner runner(fixture.program, fixture.usage, fixture.store, CpuProgramRunner::Settings());

  // Seed facts a corpus rule actually wants. `do-cook-meal-at-home` needs four
  // edges at once, which makes it the useful one to exercise:
  //
  //   when C at L, L is house of C, C is awake, C is hungry
  //
  // The unary sentences are self loops, so `C is awake` is the edge (C, C).
  const auto typeOf = [&fixture](const char *pattern) {
    EdgeTypeId id = compiler::invalidEdgeType;

    fixture.program.edgeTypes.find(pattern, id);

    return id;
  };

  const EdgeTypeId at = typeOf("? at ?");
  const EdgeTypeId houseOf = typeOf("? is house of ?");
  const EdgeTypeId awake = typeOf("? is awake");
  const EdgeTypeId hungry = typeOf("? is hungry");

  check(at != compiler::invalidEdgeType, "the corpus declares '? at ?'");
  check(houseOf != compiler::invalidEdgeType, "and '? is house of ?'");
  check(awake != compiler::invalidEdgeType, "and '? is awake'");
  check(hungry != compiler::invalidEdgeType, "and '? is hungry'");

  const uint32_t jenny = 0u;
  const uint32_t house = 1u;

  runner.seed({
      {jenny, at, house},
      {house, houseOf, jenny},
      {jenny, awake, jenny},
      {jenny, hungry, jenny},
  });

  runner.runToCompletion();

  check(matchedRuleCount(fixture.program, runner) >= 1u, "a corpus rule matches once its facts are all present");

  // And name it, so a regression says which rule stopped matching.
  bool sawCookMeal = false;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(fixture.program.ruleNames.size()); ++rule)
  {
    if ((runner.finalTableForRule(rule) ? runner.finalTableForRule(rule)->rows() : 0u) == 0u)
    {
      continue;
    }

    if (fixture.program.ruleNames[rule].rfind("do-cook-meal-at-home", 0u) == 0u)
    {
      sawCookMeal = true;
    }

    std::cout << "    matched: " << fixture.program.ruleNames[rule] << " (" << (runner.finalTableForRule(rule) ? runner.finalTableForRule(rule)->rows() : 0u) << " rows)\n";
  }

  check(sawCookMeal, "do-cook-meal-at-home matches its four seeded edges");

  // Removing one of the four must stop it matching - otherwise the conjunction
  // is not being enforced.
  EdgePageStore partial;

  partial.configure(fixture.program.edgeTypes);

  CpuProgramRunner partialRunner(fixture.program, fixture.usage, partial, CpuProgramRunner::Settings());

  partialRunner.seed({{jenny, at, house}, {house, houseOf, jenny}, {jenny, awake, jenny}});
  partialRunner.runToCompletion();

  bool stillMatches = false;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(fixture.program.ruleNames.size()); ++rule)
  {
    if (fixture.program.ruleNames[rule].rfind("do-cook-meal-at-home", 0u) == 0u && (partialRunner.finalTableForRule(rule) ? partialRunner.finalTableForRule(rule)->rows() : 0u) != 0u)
    {
      stillMatches = true;
    }
  }

  check(!stillMatches, "dropping one required edge stops the rule matching");
}

} // namespace

int main()
{
  singleRuleMatchesBruteForce();
  randomisedAgainstBruteForce();
  resultIsIndependentOfPageSize();
  overflowIsReportedNotSilent();
  snapshotRoundTrips();
  corpusRunsWithoutDiagnostics();

  return report("GraphRewriteCpuOracleTests");
}
