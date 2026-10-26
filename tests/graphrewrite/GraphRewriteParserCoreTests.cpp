// Phase 1.3: the parser's verbs, and error recovery.
//
// There is no grammar yet - the declaration parsers arrive in 1.4 and 1.5. What
// is being tested here is the contract every later node depends on: expect()
// reports exactly once and unwinds, recover() resynchronises at the next '.',
// and a file full of errors still terminates normally with every error recorded.

#include "TestHarness.hpp"
#include "graphrewrite/frontend/Parser.hpp"

using namespace graphrewrite;
using namespace graphrewrite::testing;

namespace {

void peekAndAccept()
{
  DiagnosticSink sink;
  Parser parser("let A at B.", "t.graphwrite", sink);

  check(parser.peekIs(TokenKind::KwLet), "peek sees the first token");
  check(parser.peekIs(TokenKind::Word, 1u), "peek can look ahead");
  check(!parser.accept(TokenKind::Dot), "accept of the wrong kind does not consume");
  check(parser.peekIs(TokenKind::KwLet), "and leaves the stream where it was");
  check(parser.accept(TokenKind::KwLet), "accept of the right kind consumes");
  check(parser.peekIs(TokenKind::Word), "the stream advanced");
  checkEqual(sink.count(), 0u, "accept never reports");
}

void expectReportsOnceAndUnwinds()
{
  DiagnosticSink sink;
  Parser parser("let A", "t.graphwrite", sink);

  bool threw = false;

  try
  {
    parser.expect(TokenKind::KwLet);
    parser.expect(TokenKind::Dot); // the next token is a Word
  }
  catch (const ParseError &)
  {
    threw = true;
  }

  check(threw, "expect throws on a mismatch");
  checkEqual(sink.errorCount(), 1u, "expect reports exactly once");
  check(sink[0].message.find("expected '.'") != std::string::npos, "the message names what was expected");
  check(sink[0].message.find("a word") != std::string::npos, "and what was found");
}

void errorCarriesPosition()
{
  DiagnosticSink sink;
  Parser parser("let A at B.\n  bogus", "t.graphwrite", sink);

  while (!parser.peekIs(TokenKind::Eof) && !parser.peekIs(TokenKind::Dot))
  {
    parser.accept(parser.peek().kind);
  }

  parser.accept(TokenKind::Dot);

  try
  {
    parser.error("something is wrong");
  }
  catch (const ParseError &)
  {
  }

  checkEqual(sink[0].span.line, 2u, "the diagnostic points at line 2");
  checkEqual(sink[0].span.column, 3u, "and at the right column");
  checkEqual(sink[0].format("t.graphwrite"), std::string("t.graphwrite:2:3: error: something is wrong"), "formatted with the path");
}

void recoverResynchronises()
{
  DiagnosticSink sink;
  Parser parser("a b c . let", "t.graphwrite", sink);

  parser.recover();

  check(parser.peekIs(TokenKind::KwLet), "recover stops just past the next '.'");
}

void recoverAtEndTerminates()
{
  DiagnosticSink sink;
  Parser parser("a b c", "t.graphwrite", sink);

  parser.recover();

  check(parser.peekIs(TokenKind::Eof), "recover with no '.' runs to end of file");
}

void programRecoversAndKeepsGoing()
{
  DiagnosticSink sink;

  // Nothing parses yet, so every declaration is an error. The point is that all
  // four are reported and the call returns.
  Parser parser("one. two. three. four.", "t.graphwrite", sink);

  std::unique_ptr<Program> program = Program::parse(parser);

  check(program != nullptr, "parseProgram returns a program even on failure");
  checkEqual(sink.errorCount(), 4u, "one diagnostic per bad declaration, not just the first");
}

void textOfReadsTheSource()
{
  DiagnosticSink sink;
  Parser parser("hello world", "t.graphwrite", sink);

  checkEqual(std::string(parser.textOf(parser.peek())), std::string("hello"), "textOf recovers a token's text");
  checkEqual(std::string(parser.textOf(parser.peek(1u))), std::string("world"), "textOf works through lookahead");
}

void spanFromCoversARange()
{
  DiagnosticSink sink;
  Parser parser("alpha beta gamma", "t.graphwrite", sink);

  const Token start = parser.peek();

  parser.accept(TokenKind::Word);
  parser.accept(TokenKind::Word);

  const SourceSpan span = parser.spanFrom(start);

  checkEqual(span.offset, 0u, "span starts at the opening token");
  check(span.length >= 11u, "span reaches the current position");
}

} // namespace

int main()
{
  peekAndAccept();
  expectReportsOnceAndUnwinds();
  errorCarriesPosition();
  recoverResynchronises();
  recoverAtEndTerminates();
  programRecoversAndKeepsGoing();
  textOfReadsTheSource();
  spanFromCoversARange();

  return report("GraphRewriteParserCoreTests");
}
