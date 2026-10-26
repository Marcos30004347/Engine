// Phase 1.8 [P8, P11]: semantic analysis.

#include "TestHarness.hpp"
#include "graphrewrite/frontend/Parser.hpp"
#include "graphrewrite/frontend/SemanticAnalysis.hpp"

#include <fstream>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::testing;

namespace {

struct Analysis
{
  std::unique_ptr<Program> program;
  AnalysedProgram analysed;
  DiagnosticSink sink;
  bool ok = false;
};

void run(const std::string &source, Analysis &out)
{
  Parser parser(source, "t.graphwrite", out.sink);

  out.program = Program::parse(parser);
  out.ok = analyse(*out.program, OrExpansionOptions(), out.sink, out.analysed);
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

void cleanProgramAnalyses()
{
  Analysis analysis;

  run("let A likes B.\n"
      "let A is dead.\n"
      "rule r: when A likes B, B is dead then A is dead.",
      analysis);

  check(analysis.ok, "a well-formed program analyses cleanly");
  checkEqual(analysis.sink.count(), 0u, "with no diagnostics");
  checkEqual(analysis.analysed.declarations.size(), size_t(2), "two edge types");
  checkEqual(analysis.analysed.variants.size(), size_t(1), "one AND-only variant");
}

void unboundThenVariableNamesTheVariant()
{
  Analysis analysis;

  run("let A likes B.\n"
      "rule r: when A likes B or A likes D then A likes B.",
      analysis);

  check(!analysis.ok, "the program is rejected");
  checkEqual(analysis.sink.errorCount(), 1u, "exactly one variant is at fault");
  check(mentions(analysis.sink, "variant 'r#1'"), "the diagnostic names r#1, not the rule");
  check(mentions(analysis.sink, "unbound variable(s) B"), "and names the variable");
  check(!mentions(analysis.sink, "variant 'r#0'"), "r#0 is not implicated");
}

void duplicateLetIsReported()
{
  Analysis analysis;

  run("let A likes B.\n"
      "let C likes D.",
      analysis);

  check(!analysis.ok, "a redeclared pattern is rejected");
  checkEqual(analysis.sink.errorCount(), 1u, "reported once");
  check(mentions(analysis.sink, "already declared at line 1"), "and points at the first declaration");
  checkEqual(analysis.analysed.declarations.size(), size_t(1), "only the first declaration is kept");
}

void duplicateRuleTitleIsReported()
{
  Analysis analysis;

  run("let A likes B.\n"
      "rule r: when A likes B then A likes B.\n"
      "rule r: when A likes B then A likes B.",
      analysis);

  check(!analysis.ok, "a redeclared rule title is rejected");
  check(mentions(analysis.sink, "rule 'r' is already declared"), "with the right message");
}

void undeclaredPatternIsReported()
{
  Analysis analysis;

  run("let A likes B.\n"
      "rule r: when A hates B then A likes B.",
      analysis);

  check(!analysis.ok, "an undeclared pattern is rejected");
  check(mentions(analysis.sink, "no 'let' declares '? hates ?'"), "naming the missing pattern");
}

void arityMismatchIsReported()
{
  Analysis analysis;

  run("let A is dead.\n"
      "rule r: when A is dead then A is dead.\n",
      analysis);

  check(analysis.ok, "matching arity is accepted");

  Analysis bad;

  run("let A likes B.\n"
      "let A is dead.\n"
      "rule r: when A likes B then A is dead.\n",
      bad);

  check(bad.ok, "a correct program with two arities is fine");
}

void everyErrorIsReportedInOnePass()
{
  Analysis analysis;

  run("let A likes B.\n"
      "let C likes D.\n"                              // duplicate pattern
      "rule r: when A hates B then A likes B.\n"      // undeclared pattern
      "rule s: when A likes B then A likes Z.\n",     // unbound THEN variable
      analysis);

  check(!analysis.ok, "the program is rejected");
  checkEqual(analysis.sink.errorCount(), 3u, "all three problems are reported in one pass");
  check(mentions(analysis.sink, "already declared"), "duplicate reported");
  check(mentions(analysis.sink, "no 'let' declares"), "undeclared pattern reported");
  check(mentions(analysis.sink, "unbound variable(s) Z"), "unbound variable reported");
}

void parseErrorsDoNotPreventAnalysis()
{
  Analysis analysis;

  run("let A likes B.\n"
      "let broken.\n"
      "rule r: when A likes B then A likes B.",
      analysis);

  checkEqual(analysis.sink.errorCount(), 1u, "only the parse error is reported");
  checkEqual(analysis.analysed.variants.size(), size_t(1), "the good rule still analyses");
}

void corpusAnalysesCleanly()
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/basic.graphwrite");

  check(file.good(), "corpus asset found");

  if (!file)
  {
    return;
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  Analysis analysis;

  run(buffer.str(), analysis);

  if (!analysis.ok)
  {
    for (const Diagnostic &diagnostic : analysis.sink.all())
    {
      std::cerr << "    " << diagnostic.format("basic.graphwrite") << "\n";
    }
  }

  check(analysis.ok, "the shipped corpus analyses cleanly");
  checkEqual(analysis.analysed.declarations.size(), size_t(19), "19 edge types");
  checkEqual(analysis.analysed.variants.size(), size_t(15), "15 rule variants");
}

} // namespace

int main()
{
  cleanProgramAnalyses();
  unboundThenVariableNamesTheVariant();
  duplicateLetIsReported();
  duplicateRuleTitleIsReported();
  undeclaredPatternIsReported();
  arityMismatchIsReported();
  everyErrorIsReportedInOnePass();
  parseErrorsDoNotPreventAnalysis();
  corpusAnalysesCleanly();

  return report("GraphRewriteSemanticsTests");
}
