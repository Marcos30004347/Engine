#pragma once

#include "graphrewrite/frontend/Diagnostics.hpp"


namespace graphrewrite {

enum class TokenKind
{
  Eof,
  Word,
  Number,

  KwLet,
  KwWhen,
  KwThen,
  KwRule,
  KwOr,
  KwAlways,
  KwMaybe,
  KwSet,

  LParen,
  RParen,
  Dot,
  Quote,
  Equal,
  Comma,
  Hash,
  Colon,
  Plus,
};

const char *tokenKindName(TokenKind kind);

// True for the keyword kinds, which is how the parser tells a reserved word from
// an identifier without comparing strings.
bool isKeyword(TokenKind kind);

// A token carries no pointer into the source. Its span already holds offset and
// length, so the text is recovered with Lexer::textOf when it is actually
// needed. Keeping a string_view here instead made it possible to outlive the
// Lexer that owned the buffer, which is exactly what happened the first time
// these tests ran.
struct Token
{
  TokenKind kind = TokenKind::Eof;
  SourceSpan span;
};

} // namespace graphrewrite
