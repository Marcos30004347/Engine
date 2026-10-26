#include "graphrewrite/frontend/Lexer.hpp"

namespace graphrewrite {
namespace {

struct KeywordEntry
{
  const char *text;
  TokenKind kind;
};

// Matched against the whole word, never a prefix.
constexpr KeywordEntry keywords[] = {
    {"let", TokenKind::KwLet},
    {"when", TokenKind::KwWhen},
    {"then", TokenKind::KwThen},
    {"rule", TokenKind::KwRule},
    {"always", TokenKind::KwAlways},
    {"maybe", TokenKind::KwMaybe},
    {"set", TokenKind::KwSet},
    {"or", TokenKind::KwOr},
};

TokenKind classifyWord(const std::string_view word)
{
  for (const KeywordEntry &entry : keywords)
  {
    if (word == entry.text)
    {
      return entry.kind;
    }
  }

  bool allDigits = !word.empty();

  for (const char c : word)
  {
    if (c < '0' || c > '9')
    {
      allDigits = false;

      break;
    }
  }

  return allDigits ? TokenKind::Number : TokenKind::Word;
}

TokenKind punctuationKind(const char c)
{
  switch (c)
  {
  case '(':
    return TokenKind::LParen;
  case ')':
    return TokenKind::RParen;
  case '.':
    return TokenKind::Dot;
  case '"':
    return TokenKind::Quote;
  case '=':
    return TokenKind::Equal;
  case ',':
    return TokenKind::Comma;
  case '#':
    return TokenKind::Hash;
  case ':':
    return TokenKind::Colon;
  case '+':
    return TokenKind::Plus;
  default:
    return TokenKind::Eof;
  }
}

} // namespace

bool Lexer::isStopCharacter(const char c)
{
  // Note the absence of '\'': `C'` and `C''` are single words in the corpus.
  switch (c)
  {
  case '"':
  case '#':
  case '.':
  case ',':
  case ':':
  case '=':
  case '+':
  case '(':
  case ')':
  case ' ':
  case '\t':
  case '\r':
  case '\n':
  case '\0':
    return true;
  default:
    return false;
  }
}

Lexer::Lexer(std::string src, DiagnosticSink &sink) : source(std::move(src))
{
  tokenise(sink);
}

void Lexer::tokenise(DiagnosticSink &sink)
{
  uint32_t head = 0u;
  uint32_t line = 1u;
  uint32_t column = 1u;

  const uint32_t size = static_cast<uint32_t>(source.size());

  while (head < size)
  {
    const char c = source[head];

    if (c == ' ' || c == '\t' || c == '\r')
    {
      ++head;
      ++column;

      continue;
    }

    // `#` to end of line is a comment. The grammar never consumed the Hash
    // token, so before this it could only ever produce a parse error - which
    // meant a rule file had no way to explain itself.
    if (c == '#')
    {
      while (head < size && source[head] != '\n')
      {
        ++head;
        ++column;
      }

      continue;
    }

    if (c == '\n')
    {
      ++head;
      ++line;
      column = 1u;

      continue;
    }

    SourceSpan span;

    span.offset = head;
    span.line = line;
    span.column = column;

    const TokenKind punctuation = punctuationKind(c);

    if (punctuation != TokenKind::Eof)
    {
      span.length = 1u;

      tokens.push_back(Token{punctuation, span});

      ++head;
      ++column;

      continue;
    }

    uint32_t end = head;

    while (end < size && !isStopCharacter(source[end]))
    {
      ++end;
    }

    const uint32_t length = end - head;

    span.length = length;

    const std::string_view word = std::string_view(source).substr(head, length);

    if (length > maxWordLength)
    {
      sink.error(span, "word is longer than " + std::to_string(maxWordLength) + " characters");
    }

    tokens.push_back(Token{classifyWord(word), span});

    column += length;
    head = end;
  }

  SourceSpan endSpan;

  endSpan.offset = size;
  endSpan.length = 0u;
  endSpan.line = line;
  endSpan.column = column;

  tokens.push_back(Token{TokenKind::Eof, endSpan});
}

Token Lexer::next()
{
  const Token &token = peek();

  if (position + 1u < static_cast<uint32_t>(tokens.size()))
  {
    ++position;
  }

  return token;
}

const Token &Lexer::peek(const uint32_t ahead) const
{
  const uint32_t last = static_cast<uint32_t>(tokens.size()) - 1u;
  const uint32_t index = position + ahead;

  return tokens[index < last ? index : last];
}

bool Lexer::atEnd() const
{
  return peek().kind == TokenKind::Eof;
}

const std::string &Lexer::text() const
{
  return source;
}

std::string_view Lexer::textOf(const Token &token) const
{
  return std::string_view(source).substr(token.span.offset, token.span.length);
}

} // namespace graphrewrite
