// Phase 2.5 [G3]: instruction emission.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Ir.hpp"
#include "graphrewrite/compiler/ProgramBuilder.hpp"
#include "graphrewrite/frontend/Parser.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::compiler;
using namespace graphrewrite::testing;

namespace {

struct Compiled
{
  std::unique_ptr<graphrewrite::Program> ast;
  AnalysedProgram analysed;
  EdgeTypeTable table;
  std::vector<RuleGraph> graphs;
  std::vector<MergedGraph> threads;
  CompiledProgram program;
  DiagnosticSink sink;
};

void build(const std::string &source, Compiled &out)
{
  Parser parser(source, "t.graphwrite", out.sink);

  out.ast = graphrewrite::Program::parse(parser);

  analyse(*out.ast, OrExpansionOptions(), out.sink, out.analysed);
  buildEdgeTypeTable(out.analysed, out.table, out.sink);

  for (const RuleVariant &variant : out.analysed.variants)
  {
    RuleGraph graph;

    buildRuleGraph(variant, out.table, out.sink, graph);
    out.graphs.push_back(std::move(graph));
  }

  out.threads = mergeRuleGraphs(out.graphs);

  buildProgram(out.threads, out.graphs, out.table, out.sink, out.program);
}

uint32_t queryCount(const CompiledProgram &program)
{
  uint32_t count = 0u;

  for (const Instruction &instruction : program.instructions)
  {
    if (instruction.op == Op::Select || instruction.op == Op::Where)
    {
      ++count;
    }
  }

  return count;
}

const char *twoRuleSource()
{
  return "let C at L.\n"
         "let C anger towards D.\n"
         "let C philia towards D.\n"
         "rule insult: when C at L, D at L, C anger towards D then C at L.\n"
         "rule compliment: when C at L, D at L, C philia towards D then C at L.\n";
}

void sharedTraversal()
{
  Compiled compiled;

  build(twoRuleSource(), compiled);

  checkEqual(compiled.sink.count(), 0u, "clean");
  checkEqual(queryCount(compiled.program), 4u, "four queries for four merged edges");

  const Instruction &first = compiled.program.instructions[0];

  check(first.op == Op::Select, "the programme opens with a SELECT");
  checkEqual(compiled.program.edgeTypes[first.label].pattern, std::string("? at ?"), "on the most shared edge type");
  checkEqual(first.rules.size(), size_t(2), "serving both rules");
  checkEqual(first.fromColumn, 0u, "seed binds column 0");
  checkEqual(first.writeColumn, 1u, "and column 1");

  // The second query must also serve both rules: it is the other `? at ?` edge.
  uint32_t sharedQueries = 0u;
  uint32_t soleQueries = 0u;

  for (const Instruction &instruction : compiled.program.instructions)
  {
    if (instruction.op != Op::Select && instruction.op != Op::Where)
    {
      continue;
    }

    if (instruction.rules.size() > 1u)
    {
      ++sharedQueries;
    }
    else
    {
      ++soleQueries;
    }
  }

  checkEqual(sharedQueries, 2u, "two queries are shared between the rules");
  checkEqual(soleQueries, 2u, "and two diverge, one per rule");
}

void divergentEdgesAreCyclic()
{
  Compiled compiled;

  build(twoRuleSource(), compiled);

  // By the time the anger/philia edges are reached, both their endpoints are
  // already bound, so they filter rather than widen the table.
  for (const Instruction &instruction : compiled.program.instructions)
  {
    if (instruction.op != Op::Where || instruction.rules.size() != 1u)
    {
      continue;
    }

    check(instruction.direction == StepDirection::Cyclic, "a rule-specific edge closes a cycle and adds no column");
  }
}

void everyRuleIsFullyCovered()
{
  // Emission builds a trie over each rule's edge sequence, so a merged edge
  // reached through two different prefixes is genuinely two instructions. What
  // must hold is per RULE: exactly as many queries as it has WHEN edges, on one
  // table lineage. A rule short of a query is silently never matched.
  Compiled compiled;

  build(twoRuleSource(), compiled);

  std::vector<uint32_t> served(compiled.program.ruleNames.size(), 0u);

  for (const Instruction &instruction : compiled.program.instructions)
  {
    if (instruction.op != Op::Select && instruction.op != Op::Where)
    {
      continue;
    }

    for (const uint32_t rule : instruction.rules)
    {
      ++served[rule];
    }
  }

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(served.size()); ++rule)
  {
    checkEqual(served[rule], compiled.program.ruleEdgeCounts[rule],
               "rule " + compiled.program.ruleNames[rule] + " is served by one query per WHEN edge");
  }
}

void noInstructionServesNobody()
{
  Compiled compiled;

  build(twoRuleSource(), compiled);

  bool orphan = false;

  for (const Instruction &instruction : compiled.program.instructions)
  {
    if ((instruction.op == Op::Select || instruction.op == Op::Where) && instruction.rules.empty())
    {
      orphan = true;
    }
  }

  check(!orphan, "every query serves at least one rule");
}

void directionUsageIsRecorded()
{
  Compiled compiled;

  build(twoRuleSource(), compiled);

  bool anyForward = false;

  for (EdgeTypeId id = 0u; id < compiled.program.edgeTypes.size(); ++id)
  {
    anyForward = anyForward || compiled.program.edgeTypes[id].usedForward;
  }

  check(anyForward, "emission records which directions each edge type is traversed in");
}

void everyRuleIsReportedExactlyOnce()
{
  Compiled compiled;

  build(twoRuleSource(), compiled);

  std::vector<uint32_t> reports(compiled.program.ruleNames.size(), 0u);

  for (const Instruction &instruction : compiled.program.instructions)
  {
    if (instruction.op == Op::ReportRewrite)
    {
      ++reports[instruction.rule];
    }
  }

  for (size_t rule = 0u; rule < reports.size(); ++rule)
  {
    checkEqual(reports[rule], 1u, "rule " + compiled.program.ruleNames[rule] + " is reported exactly once");
  }
}

void registersAreFreed()
{
  Compiled compiled;

  build(twoRuleSource(), compiled);

  uint32_t frees = 0u;

  for (const Instruction &instruction : compiled.program.instructions)
  {
    if (instruction.op == Op::FreeTable)
    {
      ++frees;
    }
  }

  check(frees > 0u, "registers are freed after their last use");
}

void corpusEmits()
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/basic.graphwrite");

  if (!file)
  {
    check(false, "corpus asset found");

    return;
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  Compiled compiled;

  build(buffer.str(), compiled);

  checkEqual(compiled.sink.count(), 0u, "corpus emits cleanly");
  checkEqual(queryCount(compiled.program), 24u, "24 queries, against 47 unjoined match steps");
  checkEqual(compiled.program.threadStarts.size(), size_t(2), "two threads");
  checkEqual(compiled.program.ruleNames.size(), size_t(15), "15 rules named");

  std::vector<uint32_t> reports(15u, 0u);

  for (const Instruction &instruction : compiled.program.instructions)
  {
    if (instruction.op == Op::ReportRewrite)
    {
      ++reports[instruction.rule];
    }
  }

  const bool allOnce = std::all_of(reports.begin(), reports.end(), [](const uint32_t n) { return n == 1u; });

  check(allOnce, "every corpus rule is reported exactly once");

  std::cout << "    corpus programme: " << queryCount(compiled.program) << " queries, " << compiled.program.instructions.size()
            << " instructions total\n";
}

// An edge written twice in one clause is one edge, and an edge matched AND
// restated is left alone. Both are easy to get wrong in the direction of doing
// redundant work rather than producing a wrong answer, which is why they need a
// test rather than a glance at the output.
void duplicatesAreCollapsed()
{
  Compiled compiled;

  build("let Person is alive.\n"
        "let Person is man.\n"
        "let Person0 likes Person1.\n"
        "set living(A) = A is alive.\n"
        "set living-man(A) = A is alive, A is man.\n"
        "rule duplicated:\n"
        "when living(A), living-man(A), A likes B\n"
        "then living(A), living-man(A), A likes B.\n",
        compiled);

  checkEqual(compiled.sink.errorCount(), 0u, "a rule with a duplicated edge compiles");

  // `A is alive` appears twice in each clause. Without collapsing it that is a
  // fourth query - a whole extra join over an entire edge type.
  checkEqual(queryCount(compiled.program), 3u, "the duplicated edge does not emit a second query");
  checkEqual(compiled.graphs[0].when.size(), size_t(3), "and the rule graph holds three WHEN edges, not four");

  // Every edge here is in both clauses, so the rule should do nothing at all to
  // the graph - no remove, no re-add.
  check(!compiled.program.ruleRewrites.empty(), "the rule produced a rewrite entry");
  checkEqual(compiled.program.ruleRewrites[0].removals.size(), size_t(0), "a restated edge is never removed");
  checkEqual(compiled.program.ruleRewrites[0].insertions.size(), size_t(0), "and never re-inserted");
}

void restatingKeepsOnlyWhatChanged()
{
  Compiled compiled;

  build("let Person is alive.\n"
        "let Person0 likes Person1.\n"
        "let Person0 hates Person1.\n"
        "rule turns:\n"
        "when A is alive, A likes B\n"
        "then A is alive, A hates B.\n",
        compiled);

  checkEqual(compiled.sink.errorCount(), 0u, "the rule compiles");
  checkEqual(compiled.program.ruleRewrites[0].removals.size(), size_t(1), "only the dropped edge is removed");
  checkEqual(compiled.program.ruleRewrites[0].insertions.size(), size_t(1), "only the new edge is inserted");
}

// `set living(A...)` repeats its body once per argument.
void variadicSets()
{
  Compiled compiled;

  build("let Person is alive.\n"
        "let Person0 likes Person1.\n"
        "let Person0 hates Person1.\n"
        "set living(A...) = A is alive.\n"
        "set admires(A, B...) = A likes B.\n"
        "rule fan:\n"
        "when admires(A, B, C), living(A, B, C)\n"
        "then admires(A, B, C), living(A, B, C), A hates B.\n",
        compiled);

  checkEqual(compiled.sink.errorCount(), 0u, "a variadic set compiles");

  // admires(A,B,C) is two `likes` edges; living(A,B,C) is three self-loops.
  checkEqual(queryCount(compiled.program), 5u, "the body is emitted once per variadic argument");
  checkEqual(compiled.graphs[0].when.size(), size_t(5), "and the rule graph holds all five edges");

  // A variadic repeat that lands on an argument already covered collapses, so
  // `living(A, A)` is not two queries.
  Compiled repeated;

  build("let Person is alive.\n"
        "let Person0 likes Person1.\n"
        "set living(A...) = A is alive.\n"
        "rule same:\n"
        "when living(A, A), A likes B\n"
        "then living(A, A), A likes B.\n",
        repeated);

  checkEqual(repeated.sink.errorCount(), 0u, "a repeated argument compiles");
  checkEqual(queryCount(repeated.program), 2u, "and collapses to one query for the repeat");
}

} // namespace

int main()
{
  sharedTraversal();
  divergentEdgesAreCyclic();
  everyRuleIsFullyCovered();
  noInstructionServesNobody();
  directionUsageIsRecorded();
  everyRuleIsReportedExactlyOnce();
  registersAreFreed();
  corpusEmits();
  duplicatesAreCollapsed();
  restatingKeepsOnlyWhatChanged();
  variadicSets();

  return report("GraphRewriteEmitTests");
}
