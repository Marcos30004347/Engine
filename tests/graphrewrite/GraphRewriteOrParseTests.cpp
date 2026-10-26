// Phase 1.6 [P6]: `or` in the grammar, and its precedence.
//
// The precedence assertion is made on the TREE, not on a reprint. Both
// precedences print back to the same text, so a round-trip test would pass
// against either reading and prove nothing.

#include "TestHarness.hpp"
#include "graphrewrite/frontend/AstPrinter.hpp"
#include "graphrewrite/frontend/Parser.hpp"

using namespace graphrewrite;
using namespace graphrewrite::testing;

namespace {

const RuleDecl *firstRule(const Program &program)
{
  for (const std::unique_ptr<Declaration> &declaration : program.declarations())
  {
    if (declaration->kind() == NodeKind::RuleDecl)
    {
      return &static_cast<const RuleDecl &>(*declaration);
    }
  }

  return nullptr;
}

void orBindsTighterThanComma()
{
  DiagnosticSink sink;
  Parser parser("rule r: when A likes B or A likes C, C likes D then A likes D.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);

  checkEqual(sink.count(), 0u, "parses cleanly");

  const RuleDecl *rule = firstRule(*program);

  checkEqual(rule->when().terms().size(), size_t(2), "two conjunction terms");
  checkEqual(rule->when().terms()[0]->alternatives().size(), size_t(2), "first term has two alternatives");
  checkEqual(rule->when().terms()[1]->alternatives().size(), size_t(1), "second term has one");
}

void longChainOfAlternatives()
{
  DiagnosticSink sink;
  Parser parser("rule r: when A likes B or A likes C or A likes D or A likes E then A likes B.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);
  const RuleDecl *rule = firstRule(*program);

  checkEqual(sink.count(), 0u, "a four-way disjunction parses");
  checkEqual(rule->when().terms().size(), size_t(1), "still one conjunct");
  checkEqual(rule->when().terms()[0]->alternatives().size(), size_t(4), "four alternatives");
}

void orInThenClause()
{
  DiagnosticSink sink;
  Parser parser("rule r: when A likes B then A likes B or A hates B.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);
  const RuleDecl *rule = firstRule(*program);

  checkEqual(sink.count(), 0u, "or in THEN parses");
  checkEqual(rule->then().terms()[0]->alternatives().size(), size_t(2), "two THEN alternatives");
}

void loneSentenceIsStillOneAlternative()
{
  DiagnosticSink sink;
  Parser parser("rule r: when A likes B, B likes C then A likes C.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);
  const RuleDecl *rule = firstRule(*program);

  checkEqual(rule->when().terms().size(), size_t(2), "two conjuncts");
  checkEqual(rule->when().terms()[0]->alternatives().size(), size_t(1), "each is a single alternative");
  checkEqual(rule->when().terms()[1]->alternatives().size(), size_t(1), "including the last");
}

void danglingOrIsReported()
{
  DiagnosticSink sink;
  Parser parser("rule r: when A likes B or then A likes B.", "t.graphwrite", sink);

  Program::parse(parser);

  checkEqual(sink.errorCount(), 1u, "an `or` with nothing after it reports once");
}

void orRoundTrips()
{
  const std::string source = "rule r:\nwhen A likes B or A likes C, C likes D\nthen A likes D.\n";

  DiagnosticSink firstSink;
  Parser firstParser(source, "t.graphwrite", firstSink);

  const std::unique_ptr<Program> first = Program::parse(firstParser);
  const std::string printed = printAst(*first);

  DiagnosticSink secondSink;
  Parser secondParser(printed, "reprinted", secondSink);

  const std::unique_ptr<Program> second = Program::parse(secondParser);

  checkEqual(secondSink.count(), 0u, "reprinted disjunction parses");
  checkEqual(dumpAst(*second), dumpAst(*first), "disjunction survives a round trip");
}

void orIsNotAWordAnywhereElse()
{
  DiagnosticSink sink;
  Parser parser("let Character orbit Location.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);

  checkEqual(sink.count(), 0u, "`orbit` is not the keyword");

  const Sentence &sentence = static_cast<const LetDecl &>(*program->declarations()[0]).sentence();

  checkEqual(sentence.pattern(), std::string("? orbit ?"), "and stays in the pattern");
}

} // namespace

int main()
{
  orBindsTighterThanComma();
  longChainOfAlternatives();
  orInThenClause();
  loneSentenceIsStillOneAlternative();
  danglingOrIsReported();
  orRoundTrips();
  orIsNotAWordAnywhereElse();

  return report("GraphRewriteOrParseTests");
}
