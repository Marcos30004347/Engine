// Phase 2.4 [G3, G5, G6]: the overlap join.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Ir.hpp"
#include "graphrewrite/compiler/PatternMerger.hpp"
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
  std::unique_ptr<Program> program;
  AnalysedProgram analysed;
  EdgeTypeTable table;
  std::vector<RuleGraph> graphs;
  std::vector<MergedGraph> threads;
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

  out.threads = mergeRuleGraphs(out.graphs);
}

uint32_t sharedEdges(const MergedGraph &graph)
{
  uint32_t count = 0u;

  for (const std::vector<uint32_t> &rules : graph.edgeRules)
  {
    if (rules.size() > 1u)
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

void sharedPrefixMergesIntoOneThread()
{
  Compiled compiled;

  build(twoRuleSource(), compiled);

  checkEqual(compiled.sink.count(), 0u, "clean");
  checkEqual(compiled.graphs.size(), size_t(2), "two rule variants");
  checkEqual(compiled.threads.size(), size_t(1), "they merge into ONE thread");

  const MergedGraph &merged = compiled.threads[0];

  checkEqual(merged.edges.size(), size_t(4), "four merged edges");
  checkEqual(sharedEdges(merged), 2u, "two of them serve both rules");

  uint32_t soleOwner = 0u;

  for (const std::vector<uint32_t> &rules : merged.edgeRules)
  {
    if (rules.size() == 1u)
    {
      ++soleOwner;
    }
  }

  checkEqual(soleOwner, 2u, "and two are rule-specific");
  checkEqual(merged.rules.size(), size_t(2), "both rules recorded on the thread");
}

void disjointRulesStayApart()
{
  Compiled compiled;

  build("let A at B.\n"
        "let C owns D.\n"
        "rule one: when A at B then A at B.\n"
        "rule two: when C owns D then C owns D.\n",
        compiled);

  checkEqual(compiled.threads.size(), size_t(2), "rules sharing nothing produce two threads");
  checkEqual(compiled.threads[0].edges.size(), size_t(1), "each carries its own edge");
  checkEqual(compiled.threads[1].edges.size(), size_t(1), "including the second");
}

void identicalRulesFullyOverlap()
{
  Compiled compiled;

  build("let A at B.\n"
        "rule one: when A at B then A at B.\n"
        "rule two: when A at B then A at B.\n",
        compiled);

  checkEqual(compiled.threads.size(), size_t(1), "identical patterns merge");
  checkEqual(compiled.threads[0].edges.size(), size_t(1), "into a single edge");
  checkEqual(compiled.threads[0].edgeRules[0].size(), size_t(2), "serving both rules");
}

void vertexProvenanceIsRecorded()
{
  Compiled compiled;

  build(twoRuleSource(), compiled);

  const MergedGraph &merged = compiled.threads[0];

  bool sawShared = false;

  for (uint32_t vertex = 0u; vertex < merged.vertexCount(); ++vertex)
  {
    if (merged.vertexRules[vertex].size() > 1u)
    {
      sawShared = true;

      checkEqual(merged.vertexIndices[vertex].size(), merged.vertexRules[vertex].size(),
                 "a shared vertex records one source index per rule");
    }
  }

  check(sawShared, "at least one vertex is shared between the two rules");
}

void determinism()
{
  Compiled first;
  Compiled second;

  build(twoRuleSource(), first);
  build(twoRuleSource(), second);

  checkEqual(first.threads.size(), second.threads.size(), "same thread count");

  std::string firstDump;
  std::string secondDump;

  for (const MergedGraph &graph : first.threads)
  {
    firstDump += dumpMergedGraph(graph, first.table);
  }

  for (const MergedGraph &graph : second.threads)
  {
    secondDump += dumpMergedGraph(graph, second.table);
  }

  checkEqual(firstDump, secondDump, "compiling twice produces an identical merged graph");
}

void corpusJoins()
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

  checkEqual(compiled.sink.count(), 0u, "corpus joins cleanly");
  checkEqual(compiled.graphs.size(), size_t(15), "15 rule variants");

  uint32_t mergedEdges = 0u;
  uint32_t shared = 0u;

  for (const MergedGraph &graph : compiled.threads)
  {
    mergedEdges += static_cast<uint32_t>(graph.edges.size());
    shared += sharedEdges(graph);
  }

  uint32_t unjoined = 0u;

  for (const RuleGraph &graph : compiled.graphs)
  {
    unjoined += static_cast<uint32_t>(graph.when.size());
  }

  checkEqual(unjoined, 47u, "47 match steps without the join");

  check(mergedEdges < unjoined, "the join strictly reduces the number of match steps");
  check(shared > 0u, "and edges really are shared between rules");

  // Every rule's WHEN must be fully covered by the thread it landed in, or that
  // rule is silently never matched.
  uint32_t covered = 0u;

  for (const MergedGraph &graph : compiled.threads)
  {
    for (const uint32_t rule : graph.rules)
    {
      uint32_t edgesForRule = 0u;

      for (const std::vector<uint32_t> &rules : graph.edgeRules)
      {
        if (std::find(rules.begin(), rules.end(), rule) != rules.end())
        {
          ++edgesForRule;
        }
      }

      if (edgesForRule >= compiled.graphs[rule].when.size())
      {
        ++covered;
      }
      else
      {
        std::cerr << "    rule " << compiled.graphs[rule].name << " needs " << compiled.graphs[rule].when.size() << " edges, merged covers "
                  << edgesForRule << "\n";
      }
    }
  }

  checkEqual(covered, 15u, "every rule's WHEN is fully covered by its merged graph");

  std::cout << "    corpus: " << compiled.threads.size() << " threads, " << mergedEdges << " merged edges, " << shared
            << " shared, vs " << unjoined << " unjoined\n";
}

} // namespace

int main()
{
  sharedPrefixMergesIntoOneThread();
  disjointRulesStayApart();
  identicalRulesFullyOverlap();
  vertexProvenanceIsRecorded();
  determinism();
  corpusJoins();

  return report("GraphRewriteJoinTests");
}
