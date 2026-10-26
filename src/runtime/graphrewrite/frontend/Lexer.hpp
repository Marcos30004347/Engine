#pragma once

// Tokeniser for the graph-rewrite rule language.
//
// Two details here are load-bearing and both were wrong in the system this is
// ported from:
//
//   - A keyword is recognised only when the WHOLE word matches. The original
//     matched a prefix, so `letter` lexed as `let` followed by `ter` and the
//     error surfaced much later as a parse failure on a sentence.
//
//   - An apostrophe is NOT a stop character. The rule corpus names its second
//     character `C'` and its third `C''`; treating `'` as punctuation splits
//     every one of them.

#include "graphrewrite/frontend/Diagnostics.hpp"
#include "graphrewrite/frontend/Token.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace graphrewrite {

class Lexer
{
public:
  // Tokenises eagerly: rule files are small, and holding the whole stream makes
  // arbitrary lookahead free for the `or` grammar.
  Lexer(std::string source, DiagnosticSink &sink);

  Token next();
  const Token &peek(uint32_t ahead = 0u) const;
  bool atEnd() const;

  const std::string &text() const;

  // The source text a token covers. Valid for as long as this Lexer lives.
  std::string_view textOf(const Token &token) const;

  // The longest word the language accepts. Longer is a diagnostic, not a crash.
  static constexpr uint32_t maxWordLength = 256u;

  static bool isStopCharacter(char c);

private:
  void tokenise(DiagnosticSink &sink);

  std::string source;
  std::vector<Token> tokens;
  uint32_t position = 0u;
};

} // namespace graphrewrite
