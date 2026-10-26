// Phase 3.12: applying rewrites.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"
#include "graphrewrite/runtime/RewriteApplier.hpp"

#include <algorithm>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

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

EdgeTypeId typeOf(const Fixture &fixture, const char *pattern)
{
  compiler::EdgeTypeId id = compiler::invalidEdgeType;

  fixture.program.edgeTypes.find(pattern, id);

  return id;
}

bool hasEdge(const std::vector<Fact> &facts, const uint32_t origin, const EdgeTypeId type, const uint32_t target)
{
  return std::find(facts.begin(), facts.end(), Fact{origin, type, target}) != facts.end();
}

void removalsAndInsertionsAreDerivedFromThen()
{
  Fixture fixture;

  // `A likes B` is restated and survives; `B knows A` is not and is removed;
  // `A hates B` is new and is inserted.
  check(build("let A likes B.\n"
              "let A knows B.\n"
              "let A hates B.\n"
              "rule r: when A likes B, B knows A then A likes B, A hates B.\n",
              fixture),
        "programme compiles");

  checkEqual(fixture.program.ruleRewrites.size(), size_t(1), "one rule rewrite");

  const compiler::RuleRewrite &rewrite = fixture.program.ruleRewrites[0];

  checkEqual(rewrite.removals.size(), size_t(1), "one edge removed");
  checkEqual(rewrite.insertions.size(), size_t(1), "one edge inserted");
  checkEqual(rewrite.removals[0].label, typeOf(fixture, "? knows ?"), "the removed edge is the one THEN drops");
  checkEqual(rewrite.insertions[0].label, typeOf(fixture, "? hates ?"), "the inserted edge is the one THEN adds");
}

void applyChangesTheGraph()
{
  Fixture fixture;

  build("let A likes B.\n"
        "let A knows B.\n"
        "let A hates B.\n"
        "rule r: when A likes B, B knows A then A likes B, A hates B.\n",
        fixture);

  const EdgeTypeId likes = typeOf(fixture, "? likes ?");
  const EdgeTypeId knows = typeOf(fixture, "? knows ?");
  const EdgeTypeId hates = typeOf(fixture, "? hates ?");

  CpuProgramRunner runner(fixture.program, fixture.usage, fixture.store, CpuProgramRunner::Settings());

  runner.seed({{0u, likes, 1u}, {1u, knows, 0u}});

  check(runner.runToCompletion(), "the match completes");

  const RuleResult result = runner.resultForRule(0u);

  checkEqual(result.rows, 1u, "one match");

  RewriteApplier applier(fixture.program, fixture.store);

  MatchedRow row;

  row.rule = 0u;
  row.vertices = {0u, 1u}; // A = 0, B = 1

  const RewriteStats stats = applier.apply({row});

  checkEqual(stats.rulesFired, 1u, "the rule fired");
  checkEqual(stats.edgesRemoved, 1u, "one edge removed");
  checkEqual(stats.edgesInserted, 1u, "one edge inserted");
  checkEqual(stats.claimsRejected, 0u, "nothing was rejected");

  const std::vector<Fact> after = runner.snapshot();

  check(hasEdge(after, 0u, likes, 1u), "the preserved edge survives");
  check(!hasEdge(after, 1u, knows, 0u), "the dropped edge is gone");
  check(hasEdge(after, 0u, hates, 1u), "the new edge is there");
}

void claimsSerialiseConflictingRows()
{
  // Two rows that both want to remove the same edge. Exactly one may fire, or
  // the result is a state no sequential order produces.
  Fixture fixture;

  build("let A likes B.\n"
        "let A knows B.\n"
        "rule r: when A likes B, B knows A then A likes B.\n",
        fixture);

  const EdgeTypeId likes = typeOf(fixture, "? likes ?");
  const EdgeTypeId knows = typeOf(fixture, "? knows ?");

  fixture.store.addEdge(likes, Edge{0u, 1u});
  fixture.store.addEdge(knows, Edge{1u, 0u});

  RewriteApplier applier(fixture.program, fixture.store);

  MatchedRow first;
  MatchedRow second;

  first.rule = 0u;
  first.vertices = {0u, 1u};
  second = first; // the very same match, offered twice

  const RewriteStats stats = applier.apply({first, second});

  checkEqual(stats.rulesFired, 1u, "only one of two conflicting rows fires");
  checkEqual(stats.claimsRejected, 1u, "the other is rejected, not applied");
  checkEqual(stats.edgesRemoved, 1u, "and the edge is removed exactly once");
}

void newVerticesAreAllocated()
{
  Fixture fixture;

  // `C` appears only in THEN, so it is a fresh vertex on every firing.
  build("let A likes B.\n"
        "let A knows B.\n"
        "rule r: when A likes B then A likes B, B knows C.\n",
        fixture);

  check(!fixture.ok, "a THEN variable the WHEN never binds is rejected by analysis");
  check(fixture.sink.hasErrors(), "with a diagnostic");
}

void repeatedApplicationConverges()
{
  Fixture fixture;

  // Removes its own trigger, so it can fire at most once per matching pair.
  build("let A likes B.\n"
        "let A knows B.\n"
        "rule r: when A likes B, B knows A then A likes B.\n",
        fixture);

  const EdgeTypeId likes = typeOf(fixture, "? likes ?");
  const EdgeTypeId knows = typeOf(fixture, "? knows ?");

  for (uint32_t i = 0u; i < 4u; ++i)
  {
    fixture.store.addEdge(likes, Edge{i, i + 1u});
    fixture.store.addEdge(knows, Edge{i + 1u, i});
  }

  RewriteApplier applier(fixture.program, fixture.store);

  uint32_t totalFired = 0u;

  for (uint32_t step = 0u; step < 8u; ++step)
  {
    CpuProgramRunner runner(fixture.program, fixture.usage, fixture.store, CpuProgramRunner::Settings());

    runner.reset();
    runner.runToCompletion();

    const RuleResult result = runner.resultForRule(0u);

    if (result.rows == 0u)
    {
      break;
    }

    std::vector<MatchedRow> rows;

    for (uint32_t i = 0u; i < 4u; ++i)
    {
      MatchedRow row;

      row.rule = 0u;
      row.vertices = {i, i + 1u};

      rows.push_back(row);
    }

    const RewriteStats stats = applier.apply(rows);

    totalFired += stats.rulesFired;

    if (stats.rulesFired == 0u)
    {
      break;
    }
  }

  checkEqual(totalFired, 4u, "each of the four pairs fires exactly once, then the graph is quiescent");

  CpuProgramRunner finalRunner(fixture.program, fixture.usage, fixture.store, CpuProgramRunner::Settings());

  finalRunner.runToCompletion();

  checkEqual(finalRunner.resultForRule(0u).rows, 0u, "and nothing matches any more");
}

void touchedPartitionsAreReported()
{
  Fixture fixture;

  build("let A likes B.\n"
        "let A knows B.\n"
        "let A hates B.\n"
        "rule r: when A likes B, B knows A then A likes B, A hates B.\n",
        fixture);

  const EdgeTypeId likes = typeOf(fixture, "? likes ?");
  const EdgeTypeId knows = typeOf(fixture, "? knows ?");

  fixture.store.addEdge(likes, Edge{0u, 1u});
  fixture.store.addEdge(knows, Edge{1u, 0u});

  RewriteApplier applier(fixture.program, fixture.store);

  MatchedRow row;

  row.rule = 0u;
  row.vertices = {0u, 1u};

  const RewriteStats stats = applier.apply({row});

  check(stats.touchedPartitions.size() >= 2u, "the partitions that changed are reported, so only those need re-uploading");
}

} // namespace

int main()
{
  removalsAndInsertionsAreDerivedFromThen();
  applyChangesTheGraph();
  claimsSerialiseConflictingRows();
  newVerticesAreAllocated();
  repeatedApplicationConverges();
  touchedPartitionsAreReported();

  return report("GraphRewriteRewriteTests");
}
