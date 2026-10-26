// Phase 2.6: per-rule column mappings and edge provenance.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/ColumnMapping.hpp"
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
  assignRuleColumns(out.threads, out.graphs, out.program, out.sink);
}

void everyQueryMapsEveryRuleItServes()
{
  Compiled compiled;

  build("let C at L.\n"
        "let C anger towards D.\n"
        "let C philia towards D.\n"
        "rule insult: when C at L, D at L, C anger towards D then C at L.\n"
        "rule compliment: when C at L, D at L, C philia towards D then C at L.\n",
        compiled);

  checkEqual(compiled.sink.count(), 0u, "clean");

  for (const Instruction &instruction : compiled.program.instructions)
  {
    if (instruction.op != Op::Select && instruction.op != Op::Where)
    {
      continue;
    }

    checkEqual(instruction.perRule.size(), instruction.rules.size(), "one column mapping per rule served");

    for (const RuleColumns &columns : instruction.perRule)
    {
      check(std::find(instruction.rules.begin(), instruction.rules.end(), columns.ruleId) != instruction.rules.end(),
            "the mapping names a rule the instruction actually serves");
    }
  }
}

void columnsAreInTheRulesOwnNumbering()
{
  Compiled compiled;

  build("let C at L.\n"
        "let C anger towards D.\n"
        "let C philia towards D.\n"
        "rule insult: when C at L, D at L, C anger towards D then C at L.\n"
        "rule compliment: when C at L, D at L, C philia towards D then C at L.\n",
        compiled);

  for (const Instruction &instruction : compiled.program.instructions)
  {
    for (const RuleColumns &columns : instruction.perRule)
    {
      const RuleGraph &graph = compiled.graphs[columns.ruleId];

      check(columns.fromColumn < graph.vertexCount(), "fromColumn is a vertex of that rule");
      check(columns.writeColumn < graph.vertexCount(), "writeColumn is a vertex of that rule");
    }
  }
}

void provenanceForRemovedEdgesOnly()
{
  Compiled compiled;

  // `A likes B` is restated by THEN and so survives; `B is dead` is not and so
  // must be removed, which means its provenance has to be recorded.
  build("let A likes B.\n"
        "let A is dead.\n"
        "rule r: when A likes B, B is dead then A likes B.\n",
        compiled);

  checkEqual(compiled.sink.count(), 0u, "clean");

  uint32_t withProvenance = 0u;
  uint32_t withoutProvenance = 0u;

  for (const Instruction &instruction : compiled.program.instructions)
  {
    for (const RuleColumns &columns : instruction.perRule)
    {
      if (columns.edgeColumn == noRuleColumn)
      {
        ++withoutProvenance;
      }
      else
      {
        ++withProvenance;
      }
    }
  }

  checkEqual(withoutProvenance, 1u, "the preserved edge needs no provenance column");
  checkEqual(withProvenance, 1u, "the removed edge gets one");
}

void everythingPreservedMeansNoProvenance()
{
  Compiled compiled;

  build("let A likes B.\n"
        "rule r: when A likes B then A likes B.\n",
        compiled);

  for (const Instruction &instruction : compiled.program.instructions)
  {
    for (const RuleColumns &columns : instruction.perRule)
    {
      checkEqual(columns.edgeColumn, noRuleColumn, "a rule that preserves everything allocates no provenance");
    }
  }

  checkEqual(compiled.program.ruleColumnCounts[0], compiled.graphs[0].vertexCount(),
             "and its table is exactly as wide as it has vertices");
}

void columnCountCoversVerticesAndProvenance()
{
  Compiled compiled;

  build("let A likes B.\n"
        "let A is dead.\n"
        "rule r: when A likes B, B is dead then A likes B.\n",
        compiled);

  checkEqual(compiled.program.ruleColumnCounts[0], compiled.graphs[0].vertexCount() + 1u,
             "the table is one column wider than the rule has vertices");

  // Provenance columns must sit above the vertex columns and be unique.
  std::vector<uint32_t> seen;

  for (const Instruction &instruction : compiled.program.instructions)
  {
    for (const RuleColumns &columns : instruction.perRule)
    {
      if (columns.edgeColumn == noRuleColumn)
      {
        continue;
      }

      check(columns.edgeColumn >= compiled.graphs[columns.ruleId].vertexCount(), "provenance sits above the vertex columns");
      check(std::find(seen.begin(), seen.end(), columns.edgeColumn) == seen.end(), "provenance columns are unique");

      seen.push_back(columns.edgeColumn);
    }
  }
}

void corpusColumnsAreConsistent()
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

  checkEqual(compiled.sink.count(), 0u, "corpus assigns columns cleanly");
  checkEqual(compiled.program.ruleColumnCounts.size(), size_t(15), "one column count per rule");

  uint32_t mappings = 0u;

  for (const Instruction &instruction : compiled.program.instructions)
  {
    mappings += static_cast<uint32_t>(instruction.perRule.size());

    for (const RuleColumns &columns : instruction.perRule)
    {
      check(columns.fromColumn < compiled.program.ruleColumnCounts[columns.ruleId], "fromColumn within the rule's table");
      check(columns.writeColumn < compiled.program.ruleColumnCounts[columns.ruleId], "writeColumn within the rule's table");
    }
  }

  // 15 queries; the shared ones carry a mapping per rule, so there are more
  // mappings than instructions.
  check(mappings > 15u, "shared queries carry a mapping for each rule they serve");

  std::cout << "    corpus: " << mappings << " per-rule column mappings across 15 queries\n";
}

} // namespace

int main()
{
  everyQueryMapsEveryRuleItServes();
  columnsAreInTheRulesOwnNumbering();
  provenanceForRemovedEdgesOnly();
  everythingPreservedMeansNoProvenance();
  columnCountCoversVerticesAndProvenance();
  corpusColumnsAreConsistent();

  return report("GraphRewriteColumnsTests");
}
