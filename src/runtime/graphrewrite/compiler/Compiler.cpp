#include "graphrewrite/compiler/Compiler.hpp"

#include "graphrewrite/compiler/ColumnMapping.hpp"
#include "graphrewrite/compiler/Ir.hpp"
#include "graphrewrite/compiler/PatternMerger.hpp"
#include "graphrewrite/compiler/ProgramBuilder.hpp"
#include "graphrewrite/compiler/ProgramVerifier.hpp"
#include "graphrewrite/frontend/Parser.hpp"
#include "graphrewrite/frontend/SemanticAnalysis.hpp"

namespace graphrewrite::compiler {

bool compile(const std::string &source, const std::string &path, const CompileOptions &options, DiagnosticSink &sink,
             CompiledProgram &program, UsageValues &usage)
{
  Parser parser(source, path, sink);

  const std::unique_ptr<graphrewrite::Program> ast = graphrewrite::Program::parse(parser);

  if (sink.hasErrors())
  {
    return false;
  }

  AnalysedProgram analysed;

  if (!analyse(*ast, options.expansion, sink, analysed))
  {
    return false;
  }

  EdgeTypeTable table;

  if (!buildEdgeTypeTable(analysed, table, sink))
  {
    return false;
  }

  std::vector<RuleGraph> graphs;

  for (const RuleVariant &variant : analysed.variants)
  {
    RuleGraph graph;

    if (!buildRuleGraph(variant, table, sink, graph))
    {
      return false;
    }

    graphs.push_back(std::move(graph));
  }

  const std::vector<MergedGraph> threads = mergeRuleGraphs(graphs);

  if (!buildProgram(threads, graphs, table, sink, program, options.edgeCounts))
  {
    return false;
  }

  if (!assignRuleColumns(threads, graphs, program, sink))
  {
    return false;
  }

  usage = computeUsageValues(program);

  if (options.verify && !verifyProgram(program, usage, sink))
  {
    return false;
  }

  return true;
}

} // namespace graphrewrite::compiler
