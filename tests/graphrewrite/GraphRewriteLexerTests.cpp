// Phase 1.2 [P1]: keyword recognition requires a whole-word match.
//
// The suite leads with the negative cases because that is where the ported
// implementation was wrong: matching a keyword by prefix turned `letter` into
// `let` + `ter`, which then failed somewhere else entirely.

#include "TestHarness.hpp"
#include "graphrewrite/frontend/Lexer.hpp"

#include <vector>

using namespace graphrewrite;
using namespace graphrewrite::testing;

namespace {

// The lexer is passed in, not created here: tokens are only meaningful while the
// Lexer that produced them is alive.
std::vector<Token> lexAll(Lexer &lexer)
{
  std::vector<Token> out;

  while (true)
  {
    const Token token = lexer.next();

    out.push_back(token);

    if (token.kind == TokenKind::Eof)
    {
      break;
    }
  }

  return out;
}

TokenKind firstKind(const std::string &source)
{
  DiagnosticSink sink;
  Lexer lexer(source, sink);

  return lexer.peek().kind;
}

void keywordsNeedAWholeWord()
{
  checkEqual(std::string(tokenKindName(firstKind("letter"))), std::string("a word"), "letter is a word");
  checkEqual(std::string(tokenKindName(firstKind("whence"))), std::string("a word"), "whence is a word");
  checkEqual(std::string(tokenKindName(firstKind("thence"))), std::string("a word"), "thence is a word");
  checkEqual(std::string(tokenKindName(firstKind("ruler"))), std::string("a word"), "ruler is a word");
  checkEqual(std::string(tokenKindName(firstKind("factory"))), std::string("a word"), "factory is a word");
  checkEqual(std::string(tokenKindName(firstKind("orbit"))), std::string("a word"), "orbit is a word");

  check(firstKind("let") == TokenKind::KwLet, "let is a keyword");
  check(firstKind("when") == TokenKind::KwWhen, "when is a keyword");
  check(firstKind("then") == TokenKind::KwThen, "then is a keyword");
  check(firstKind("rule") == TokenKind::KwRule, "rule is a keyword");
  check(firstKind("set") == TokenKind::KwSet, "set is a keyword");
  check(firstKind("or") == TokenKind::KwOr, "or is a keyword");
}

void apostropheIsPartOfAWord()
{
  DiagnosticSink sink;

  Lexer lexer("C' anger towards C''", sink);

  const std::vector<Token> tokens = lexAll(lexer);

  checkEqual(tokens.size(), size_t(5), "four words and EOF");
  checkEqual(std::string(lexer.textOf(tokens[0])), std::string("C'"), "C' is one word");
  checkEqual(std::string(lexer.textOf(tokens[3])), std::string("C''"), "C'' is one word");
  check(tokens[0].kind == TokenKind::Word, "C' lexes as a word");
}

void punctuationAndNumbers()
{
  DiagnosticSink sink;

  Lexer lexer("a(b=3),c:d.", sink);

  const std::vector<Token> tokens = lexAll(lexer);

  const std::vector<TokenKind> want = {
      TokenKind::Word,  TokenKind::LParen, TokenKind::Word, TokenKind::Equal, TokenKind::Number,
      TokenKind::RParen, TokenKind::Comma, TokenKind::Word, TokenKind::Colon, TokenKind::Word,
      TokenKind::Dot,   TokenKind::Eof};

  checkEqual(tokens.size(), want.size(), "token count for punctuation sample");

  bool same = tokens.size() == want.size();

  for (size_t i = 0u; same && i < want.size(); ++i)
  {
    same = tokens[i].kind == want[i];
  }

  check(same, "punctuation and number kinds");
}

void spansTrackLineAndColumn()
{
  DiagnosticSink sink;

  Lexer lexer("let A at B.\nlet C is dead.", sink);

  const std::vector<Token> tokens = lexAll(lexer);

  checkEqual(tokens[0].span.line, 1u, "first token on line 1");
  checkEqual(tokens[0].span.column, 1u, "first token at column 1");

  // token 5 is the `let` that opens the second line
  checkEqual(tokens[5].span.line, 2u, "second declaration on line 2");
  checkEqual(tokens[5].span.column, 1u, "second declaration at column 1");
}

void hyphensStayInsideWords()
{
  DiagnosticSink sink;

  Lexer lexer("do-form-opinion-like:", sink);

  const std::vector<Token> tokens = lexAll(lexer);

  checkEqual(std::string(lexer.textOf(tokens[0])), std::string("do-form-opinion-like"), "a hyphenated rule title is one word");
  check(tokens[1].kind == TokenKind::Colon, "the colon after a title is punctuation");
}

void peekDoesNotConsume()
{
  DiagnosticSink sink;
  Lexer lexer("a b c", sink);

  check(lexer.textOf(lexer.peek(0u)) == "a", "peek 0");
  check(lexer.textOf(lexer.peek(1u)) == "b", "peek 1");
  check(lexer.textOf(lexer.peek(2u)) == "c", "peek 2");
  check(lexer.textOf(lexer.peek(0u)) == "a", "peek did not advance");
  check(lexer.peek(99u).kind == TokenKind::Eof, "peek past the end clamps to EOF");

  lexer.next();

  check(lexer.textOf(lexer.peek(0u)) == "b", "next advanced by one");
}

void overLongWordIsADiagnostic()
{
  DiagnosticSink sink;

  const std::string huge(Lexer::maxWordLength + 10u, 'x');

  Lexer lexer(huge, sink);

  checkEqual(sink.errorCount(), 1u, "an over-long word reports once");
  check(lexer.peek().kind == TokenKind::Word, "and still produces a token");
}

void emptySource()
{
  DiagnosticSink sink;
  Lexer lexer("", sink);

  check(lexer.atEnd(), "empty source is immediately at end");
  checkEqual(sink.count(), 0u, "empty source is not an error");
}

void realCorpusTokenises()
{
  DiagnosticSink sink;

  const std::string source = "let Character at Location.\n"
                             "let Character is dead.\n"
                             "rule do-wake-up:\n"
                             "when C is sleeping\n"
                             "then C is awake, C is hungry.\n"
                             "fact JennyHouse is house of Jenny, Jenny at JennyHouse.\n";

  Lexer lexer(source, sink);

  const std::vector<Token> tokens = lexAll(lexer);

  checkEqual(sink.count(), 0u, "corpus sample lexes without diagnostics");

  uint32_t keywordCount = 0u;

  for (const Token &token : tokens)
  {
    if (isKeyword(token.kind))
    {
      ++keywordCount;
    }
  }

  // let, let, rule, when, then, fact
  checkEqual(keywordCount, 6u, "six keywords in the sample");
}

} // namespace

int main()
{
  keywordsNeedAWholeWord();
  apostropheIsPartOfAWord();
  punctuationAndNumbers();
  spansTrackLineAndColumn();
  hyphensStayInsideWords();
  peekDoesNotConsume();
  overLongWordIsADiagnostic();
  emptySource();
  realCorpusTokenises();

  return report("GraphRewriteLexerTests");
}
