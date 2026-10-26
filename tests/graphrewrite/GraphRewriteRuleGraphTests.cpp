// Phase 2.2 [G2]: rule graphs, and the unary self-loop.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Ir.hpp"
#include "graphrewrite/compiler/RuleGraph.hpp"
#include "graphrewrite/frontend/Parser.hpp"

#include <fstream>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::compiler;
using namespace graphrewrite::testing;

namespace {

struct Compiled
{
  std::unique_ptr<Program> program;
  AnalysedProgram analysed;
  EdgeTypeTable table;
  std::vector<RuleGraph> graphs;
  DiagnosticSink sink;
};

void build(const std::string &source, Compiled &out)
{
  Parser parser(source, "t.graphwrite", out.sink);

  out.program = Program::parse(parser);

  analyse(*out.program, OrExpansionOptions(), out.sink, out.analysed);
  buildEdgeTypeTable(out.analysed, out.table, out.sink);

  for (const RuleVariant &variant : out.analysed.variants)
  {
    RuleGraph graph;

    buildRuleGraph(variant, out.table, out.sink, graph);
    out.graphs.push_back(std::move(graph));
  }
}

void unarySentenceIsASelfLoop()
{
  Compiled compiled;

  build("let A likes B.\n"
        "let A is dead.\n"
        "rule r: when A likes B, B is dead then A is dead.",
        compiled);

  checkEqual(compiled.sink.count(), 0u, "clean");
  checkEqual(compiled.graphs.size(), size_t(1), "one rule graph");

  const RuleGraph &graph = compiled.graphs[0];

  checkEqual(graph.vertexCount(), 2u, "two vertices");
  checkEqual(graph.vertexNames[0], std::string("A"), "vertex 0 is A");
  checkEqual(graph.vertexNames[1], std::string("B"), "vertex 1 is B");

  checkEqual(graph.when.size(), size_t(2), "two WHEN edges");

  checkEqual(graph.when[0].origin, 0u, "likes edge origin");
  checkEqual(graph.when[0].target, 1u, "likes edge target");

  checkEqual(graph.when[1].origin, 1u, "is-dead edge origin is B");
  checkEqual(graph.when[1].target, 1u, "is-dead edge target is B - a self loop, not vertex 1 by accident");
  check(graph.when[1].origin == graph.when[1].target, "unary sentence produces a genuine self loop");
}

void selfLoopDoesNotCollideWithVertexOne()
{
  // The ported bug was `target = variables.size()`, i.e. the constant 1. With a
  // single variable that is NOT vertex 1, this test would have passed anyway;
  // the case that exposes it is a unary sentence on a vertex that is not v1.
  Compiled compiled;

  build("let A likes B.\n"
        "let A is dead.\n"
        "rule r: when A likes B, A is dead then A likes B.",
        compiled);

  const RuleGraph &graph = compiled.graphs[0];

  checkEqual(graph.when[1].origin, 0u, "unary sentence on A binds vertex 0");
  checkEqual(graph.when[1].target, 0u, "and loops to vertex 0, not vertex 1");
}

void sharedVariablesAreOneVertex()
{
  Compiled compiled;

  build("let A likes B.\n"
        "rule r: when A likes B then B likes A.",
        compiled);

  const RuleGraph &graph = compiled.graphs[0];

  checkEqual(graph.vertexCount(), 2u, "THEN reuses the WHEN vertices");
  checkEqual(graph.then[0].origin, 1u, "THEN edge origin is B");
  checkEqual(graph.then[0].target, 0u, "THEN edge target is A");
}

void thenOnlyVariableGetsANewVertex()
{
  Compiled compiled;

  build("let A likes B.\n"
        "rule r: when A likes B then A likes B, B likes C.",
        compiled);

  // `C` is unbound, so analysis rejects it - but the graph builder must still
  // number it rather than crash.
  check(compiled.sink.errorCount() >= 1u, "an unbound THEN variable is reported");
}

void edgeIndicesAreClauseLocal()
{
  Compiled compiled;

  build("let A likes B.\n"
        "rule r: when A likes B then A likes B.",
        compiled);

  const RuleGraph &graph = compiled.graphs[0];

  checkEqual(graph.when[0].index, 0u, "WHEN edge index starts at 0");
  checkEqual(graph.then[0].index, 0u, "THEN edge index also starts at 0");
}

void everyCorpusRuleLowers()
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

  checkEqual(compiled.sink.count(), 0u, "corpus lowers cleanly");
  checkEqual(compiled.graphs.size(), size_t(15), "15 rule graphs");

  uint32_t selfLoops = 0u;
  uint32_t totalWhenEdges = 0u;

  for (const RuleGraph &graph : compiled.graphs)
  {
    totalWhenEdges += static_cast<uint32_t>(graph.when.size());

    for (const PatternEdge &edge : graph.when)
    {
      if (edge.origin == edge.target)
      {
        ++selfLoops;
      }
    }
  }

  // The corpus's unary sentences: `C is sleeping`, `C is awake`, `C is hungry`,
  // `C is working` appear in WHEN clauses.
  check(selfLoops > 0u, "the corpus exercises unary self loops");
  checkEqual(totalWhenEdges, 47u, "47 WHEN edges across all rules - the unjoined match step count");
}

} // namespace

int main()
{
  unarySentenceIsASelfLoop();
  selfLoopDoesNotCollideWithVertexOne();
  sharedVariablesAreOneVertex();
  thenOnlyVariableGetsANewVertex();
  edgeIndicesAreClauseLocal();
  everyCorpusRuleLowers();

  return report("GraphRewriteRuleGraphTests");
}
