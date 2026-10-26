// Phase 1.4 [P2, P3, P4, P12]: sentence lowering, `let` and `fact`.
//
// The sentence's pattern string is the edge type's identity, so the tests that
// matter most here are the ones about how it is built - in particular that two
// different declarations cannot collapse into the same pattern.

#include "TestHarness.hpp"
#include "graphrewrite/frontend/Parser.hpp"

using namespace graphrewrite;
using namespace graphrewrite::testing;

namespace {

const Sentence *firstLet(const Program &program)
{
  for (const std::unique_ptr<Declaration> &declaration : program.declarations())
  {
    if (declaration->kind() == NodeKind::LetDecl)
    {
      return &static_cast<const LetDecl &>(*declaration).sentence();
    }
  }

  return nullptr;
}

void lowering(const std::string &source, const std::string &wantPattern, const std::string &wantOriginal, const size_t wantVariables,
              const std::string &label)
{
  DiagnosticSink sink;
  Parser parser(source, "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);
  const Sentence *sentence = firstLet(*program);

  if (sentence == nullptr)
  {
    check(false, label + " (no let parsed)");

    return;
  }

  checkEqual(sentence->pattern(), wantPattern, label + " pattern");
  checkEqual(sentence->original(), wantOriginal, label + " original");
  checkEqual(sentence->variables().size(), wantVariables, label + " variable count");
  checkEqual(sink.count(), 0u, label + " parses cleanly");
}

void sampleShapes()
{
  lowering("let Character at Location.", "? at ?", "Character at Location", 2u, "binary sentence");
  lowering("let Character is dead.", "? is dead", "Character is dead", 1u, "unary sentence");
  lowering("let Location is house of Character.", "? is house of ?", "Location is house of Character", 2u, "four-word sentence");
  lowering("let C' anger towards C''.", "? anger towards ?", "C' anger towards C''", 2u, "primed variables");
}

void patternSeparatorPreventsCollision()
{
  DiagnosticSink sink;
  Parser parser("let A b c D.\nlet A bc D.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);

  checkEqual(program->declarations().size(), size_t(2), "both declarations parsed");

  const Sentence &first = static_cast<const LetDecl &>(*program->declarations()[0]).sentence();
  const Sentence &second = static_cast<const LetDecl &>(*program->declarations()[1]).sentence();

  checkEqual(first.pattern(), std::string("? b c ?"), "spaced words keep their separator");
  checkEqual(second.pattern(), std::string("? bc ?"), "joined word is distinct");
  check(first.pattern() != second.pattern(), "'A b c D' and 'A bc D' are different edge types");
}

void variablesAreRecognisedByCase()
{
  DiagnosticSink sink;
  Parser parser("let Character at location.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);
  const Sentence *sentence = firstLet(*program);

  checkEqual(sentence->variables().size(), size_t(1), "only the uppercase word is a variable");
  checkEqual(sentence->variables()[0], std::string("Character"), "and it is the right one");
  checkEqual(sentence->pattern(), std::string("? at location"), "the lowercase word stays literal");
}

void annotationsDoNotDisturbThePattern()
{
  DiagnosticSink sink;
  Parser parser("let Character(weight=3) at Location.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);
  const Sentence *sentence = firstLet(*program);

  checkEqual(sink.count(), 0u, "annotated sentence parses cleanly");
  checkEqual(sentence->pattern(), std::string("? at ?"), "annotation is not part of the pattern");
  checkEqual(sentence->variables().size(), size_t(2), "annotation does not add a variable");
  checkEqual(sentence->annotations().size(), size_t(1), "annotation is retained");
  checkEqual(sentence->annotations()[0].key, std::string("weight"), "annotation key");
  checkEqual(sentence->annotations()[0].value, std::string("3"), "annotation value");
}

void multipleAnnotations()
{
  DiagnosticSink sink;
  Parser parser("let Character(a=1, b=two) at Location.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);
  const Sentence *sentence = firstLet(*program);

  checkEqual(sink.count(), 0u, "multiple annotations parse");
  checkEqual(sentence->annotations().size(), size_t(2), "both retained");
  checkEqual(sentence->annotations()[1].value, std::string("two"), "word-valued annotation");
}

void arityIsEnforced()
{
  {
    DiagnosticSink sink;
    Parser parser("let nothing here.", "t.graphwrite", sink);

    Program::parse(parser);

    checkEqual(sink.errorCount(), 1u, "a sentence with no variables is rejected");
    check(sink[0].message.find("0 variable") != std::string::npos, "and says how many it found");
  }

  {
    DiagnosticSink sink;
    Parser parser("let A b C d E.", "t.graphwrite", sink);

    Program::parse(parser);

    checkEqual(sink.errorCount(), 1u, "a sentence with three variables is rejected");
  }
}

void recoveryKeepsLaterDeclarations()
{
  DiagnosticSink sink;
  Parser parser("let Character at Location.\nlet broken.\nlet Character is dead.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);

  checkEqual(sink.errorCount(), 1u, "one bad declaration reports once");
  checkEqual(program->declarations().size(), size_t(2), "the two good declarations still land");
}

void spansPointAtTheDeclaration()
{
  DiagnosticSink sink;
  Parser parser("let A at B.\nlet C at D.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);

  checkEqual(program->declarations()[0]->span.line, 1u, "first declaration on line 1");
  checkEqual(program->declarations()[1]->span.line, 2u, "second declaration on line 2");
}

} // namespace

int main()
{
  sampleShapes();
  patternSeparatorPreventsCollision();
  variablesAreRecognisedByCase();
  annotationsDoNotDisturbThePattern();
  multipleAnnotations();
  arityIsEnforced();
  recoveryKeepsLaterDeclarations();
  spansPointAtTheDeclaration();

  return report("GraphRewriteSentenceTests");
}
