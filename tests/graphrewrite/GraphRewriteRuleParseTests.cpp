// Phase 1.5: rules, conjunction, and a round trip over the real corpus.

#include "TestHarness.hpp"
#include "graphrewrite/frontend/AstPrinter.hpp"
#include "graphrewrite/frontend/Parser.hpp"

#include <fstream>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::testing;

namespace {

std::string readCorpus()
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/basic.graphwrite");

  if (!file)
  {
    return std::string();
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  return buffer.str();
}

void countsOf(const Program &program, size_t &lets, size_t &rules)
{
  lets = rules = 0u;

  for (const std::unique_ptr<Declaration> &declaration : program.declarations())
  {
    switch (declaration->kind())
    {
    case NodeKind::LetDecl:
      ++lets;
      break;
    case NodeKind::RuleDecl:
      ++rules;
      break;
    default:
      break;
    }
  }
}

void simpleRule()
{
  DiagnosticSink sink;
  Parser parser("rule do-wake-up:\nwhen C is sleeping\nthen C is awake, C is hungry.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);

  checkEqual(sink.count(), 0u, "rule parses cleanly");
  checkEqual(program->declarations().size(), size_t(1), "one declaration");

  const RuleDecl &rule = static_cast<const RuleDecl &>(*program->declarations()[0]);

  checkEqual(rule.title().original(), std::string("do-wake-up"), "title text");
  checkEqual(rule.title().variables().size(), size_t(0), "a title has no variables");
  checkEqual(rule.when().terms().size(), size_t(1), "one WHEN conjunct");
  checkEqual(rule.then().terms().size(), size_t(2), "two THEN conjuncts");
}

void titleWithVariablesIsRejected()
{
  DiagnosticSink sink;
  Parser parser("rule Bad:\nwhen C is sleeping\nthen C is awake.", "t.graphwrite", sink);

  Program::parse(parser);

  checkEqual(sink.errorCount(), 1u, "an uppercase title word is rejected");
}

void missingThenIsReported()
{
  DiagnosticSink sink;
  Parser parser("rule r:\nwhen C is sleeping.", "t.graphwrite", sink);

  Program::parse(parser);

  checkEqual(sink.errorCount(), 1u, "a rule without THEN reports once");
  check(sink[0].message.find("'then'") != std::string::npos, "and names the missing keyword");
}

void corpusParses()
{
  const std::string source = readCorpus();

  check(!source.empty(), "corpus asset was found");

  if (source.empty())
  {
    return;
  }

  DiagnosticSink sink;
  Parser parser(source, "basic.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);

  if (sink.count() != 0u)
  {
    for (const Diagnostic &diagnostic : sink.all())
    {
      std::cerr << "    " << diagnostic.format("basic.graphwrite") << "\n";
    }
  }

  checkEqual(sink.count(), 0u, "corpus parses with zero diagnostics");

  size_t lets = 0u;
  size_t rules = 0u;
  countsOf(*program, lets, rules);

  checkEqual(lets, size_t(19), "19 let declarations");
  checkEqual(rules, size_t(15), "15 rule declarations");
}

void roundTrip()
{
  const std::string source = readCorpus();

  if (source.empty())
  {
    return;
  }

  DiagnosticSink firstSink;
  Parser firstParser(source, "basic.graphwrite", firstSink);

  const std::unique_ptr<Program> first = Program::parse(firstParser);
  const std::string printed = printAst(*first);

  DiagnosticSink secondSink;
  Parser secondParser(printed, "reprinted", secondSink);

  const std::unique_ptr<Program> second = Program::parse(secondParser);

  checkEqual(secondSink.count(), 0u, "reprinted source parses cleanly");
  checkEqual(dumpAst(*second), dumpAst(*first), "reprint re-parses to a structurally identical tree");
}

} // namespace

int main()
{
  simpleRule();
  titleWithVariablesIsRejected();
  missingThenIsReported();
  corpusParses();
  roundTrip();

  return report("GraphRewriteRuleParseTests");
}
