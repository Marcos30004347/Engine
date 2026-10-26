// Phase 2.8 [G7]: the programme verifier.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/ColumnMapping.hpp"
#include "graphrewrite/compiler/Ir.hpp"
#include "graphrewrite/compiler/ProgramBuilder.hpp"
#include "graphrewrite/compiler/ProgramVerifier.hpp"
#include "graphrewrite/frontend/Parser.hpp"

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
  UsageValues usage;
  DiagnosticSink sink;
  bool verified = false;
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

  out.usage = computeUsageValues(out.program);
  out.verified = verifyProgram(out.program, out.usage, out.sink);
}

bool mentions(const DiagnosticSink &sink, const std::string &needle)
{
  for (const Diagnostic &diagnostic : sink.all())
  {
    if (diagnostic.message.find(needle) != std::string::npos)
    {
      return true;
    }
  }

  return false;
}

void wellFormedProgrammesVerify()
{
  Compiled compiled;

  build("let C at L.\n"
        "let C anger towards D.\n"
        "let C philia towards D.\n"
        "rule insult: when C at L, D at L, C anger towards D then C at L.\n"
        "rule compliment: when C at L, D at L, C philia towards D then C at L.\n",
        compiled);

  if (!compiled.verified)
  {
    for (const Diagnostic &diagnostic : compiled.sink.all())
    {
      std::cerr << "    " << diagnostic.message << "\n";
    }
  }

  check(compiled.verified, "a compiled two-rule programme verifies clean");
  checkEqual(compiled.sink.count(), 0u, "with no diagnostics at all");
}

void undefinedRegisterIsCaught()
{
  CompiledProgram program;

  program.edgeTypes.intern("? a ?", 2u);
  program.ruleNames.push_back("r");
  program.ruleColumnCounts.push_back(2u);

  Instruction where;

  where.op = Op::Where;
  where.reg = 0u;
  where.from = 7u; // never defined
  where.label = 0u;
  where.rules = {0u};
  where.perRule.push_back(RuleColumns{0u, 0u, 1u, noRuleColumn});

  program.instructions.push_back(where);

  DiagnosticSink sink;

  const bool ok = verifyProgram(program, computeUsageValues(program), sink);

  check(!ok, "an undefined register is rejected");
  check(mentions(sink, "WHERE reads undefined register 7"), "with a message naming the register");
}

void orphanInstructionIsCaught()
{
  CompiledProgram program;

  program.edgeTypes.intern("? a ?", 2u);

  Instruction select;

  select.op = Op::Select;
  select.reg = 0u;
  select.label = 0u; // no rules

  program.instructions.push_back(select);

  DiagnosticSink sink;

  const bool ok = verifyProgram(program, computeUsageValues(program), sink);

  check(!ok, "an instruction serving nobody is rejected");
  check(mentions(sink, "serves no rule"), "with the right message");
}

void missingReportIsCaught()
{
  CompiledProgram program;

  program.edgeTypes.intern("? a ?", 2u);
  program.ruleNames.push_back("lonely");
  program.ruleColumnCounts.push_back(2u);

  Instruction select;

  select.op = Op::Select;
  select.reg = 0u;
  select.label = 0u;
  select.rules = {0u};
  select.perRule.push_back(RuleColumns{0u, 0u, 1u, noRuleColumn});

  program.instructions.push_back(select);

  Instruction free;

  free.op = Op::FreeTable;
  free.from = 0u;

  program.instructions.push_back(free);

  DiagnosticSink sink;

  const bool ok = verifyProgram(program, computeUsageValues(program), sink);

  check(!ok, "a rule that is never reported is rejected");
  check(mentions(sink, "reported 0 times"), "naming how many times it was reported");
}

void ruleSplitAcrossTwoRootsIsCaught()
{
  // The failure this guards is total and silent: a rule whose edges sit under
  // two different roots has no single table to thread, so it can never match,
  // and every other check passes.
  CompiledProgram program;

  program.edgeTypes.intern("? a ?", 2u);
  program.ruleNames.push_back("split");
  program.ruleColumnCounts.push_back(4u);
  program.ruleEdgeCounts.push_back(2u);

  const auto root = [](const uint32_t reg) {
    Instruction instruction;

    instruction.op = Op::Select;
    instruction.reg = reg;
    instruction.label = 0u;
    instruction.rules = {0u};
    instruction.perRule.push_back(RuleColumns{0u, 0u, 1u, noRuleColumn});

    return instruction;
  };

  program.instructions.push_back(root(0u));
  program.instructions.push_back(root(1u)); // a SECOND root for the same rule

  Instruction report;

  report.op = Op::ReportRewrite;
  report.from = 1u;
  report.rule = 0u;

  program.instructions.push_back(report);

  for (const uint32_t reg : {0u, 1u})
  {
    Instruction free;

    free.op = Op::FreeTable;
    free.from = reg;

    program.instructions.push_back(free);
  }

  DiagnosticSink sink;

  const bool ok = verifyProgram(program, computeUsageValues(program), sink);

  check(!ok, "a rule served by two roots is rejected");
  check(mentions(sink, "roots"), "and the message says its edges must lie on one path");
}

void ruleMissingAQueryIsCaught()
{
  CompiledProgram program;

  program.edgeTypes.intern("? a ?", 2u);
  program.ruleNames.push_back("short");
  program.ruleColumnCounts.push_back(4u);
  program.ruleEdgeCounts.push_back(3u); // claims three WHEN edges

  Instruction select;

  select.op = Op::Select;
  select.reg = 0u;
  select.label = 0u;
  select.rules = {0u};
  select.perRule.push_back(RuleColumns{0u, 0u, 1u, noRuleColumn});

  program.instructions.push_back(select);

  Instruction report;

  report.op = Op::ReportRewrite;
  report.from = 0u;
  report.rule = 0u;

  program.instructions.push_back(report);

  Instruction free;

  free.op = Op::FreeTable;
  free.from = 0u;

  program.instructions.push_back(free);

  DiagnosticSink sink;

  check(!verifyProgram(program, computeUsageValues(program), sink), "a rule served by too few queries is rejected");
  check(mentions(sink, "WHEN edge(s) but is served by 1"), "naming the shortfall");
}

void outOfRangeColumnIsCaught()
{
  CompiledProgram program;

  program.edgeTypes.intern("? a ?", 2u);
  program.ruleNames.push_back("r");
  program.ruleColumnCounts.push_back(2u);

  Instruction select;

  select.op = Op::Select;
  select.reg = 0u;
  select.label = 0u;
  select.rules = {0u};
  select.perRule.push_back(RuleColumns{0u, 0u, 9u, noRuleColumn}); // column 9 of a 2-wide table

  program.instructions.push_back(select);

  Instruction report;

  report.op = Op::ReportRewrite;
  report.from = 0u;
  report.rule = 0u;

  program.instructions.push_back(report);

  Instruction free;

  free.op = Op::FreeTable;
  free.from = 0u;

  program.instructions.push_back(free);

  DiagnosticSink sink;

  const bool ok = verifyProgram(program, computeUsageValues(program), sink);

  check(!ok, "a column outside the rule's table is rejected");
  check(mentions(sink, "out of range"), "with the right message");
}

void everyViolationIsReported()
{
  CompiledProgram program;

  program.edgeTypes.intern("? a ?", 2u);
  program.ruleNames.push_back("r");
  program.ruleColumnCounts.push_back(2u);

  Instruction where;

  where.op = Op::Where;
  where.reg = 0u;
  where.from = 7u;
  where.label = 0u; // no rules either

  program.instructions.push_back(where);

  DiagnosticSink sink;

  verifyProgram(program, computeUsageValues(program), sink);

  check(sink.errorCount() >= 3u, "an undefined register, an orphan instruction and a missing report are all reported");
}

void corpusVerifies()
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

  if (!compiled.verified)
  {
    for (const Diagnostic &diagnostic : compiled.sink.all())
    {
      std::cerr << "    " << diagnostic.message << "\n";
    }
  }

  check(compiled.verified, "the shipped corpus compiles to a programme that verifies clean");
  checkEqual(compiled.sink.count(), 0u, "with no diagnostics");
}

} // namespace

int main()
{
  wellFormedProgrammesVerify();
  undefinedRegisterIsCaught();
  orphanInstructionIsCaught();
  missingReportIsCaught();
  ruleSplitAcrossTwoRootsIsCaught();
  ruleMissingAQueryIsCaught();
  outOfRangeColumnIsCaught();
  everyViolationIsReported();
  corpusVerifies();

  return report("GraphRewriteVerifierTests");
}
