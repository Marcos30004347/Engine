#include "graphrewrite/frontend/Token.hpp"

namespace graphrewrite {

const char *tokenKindName(const TokenKind kind)
{
  switch (kind)
  {
  case TokenKind::Eof:
    return "end of file";
  case TokenKind::Word:
    return "a word";
  case TokenKind::Number:
    return "a number";
  case TokenKind::KwLet:
    return "'let'";
  case TokenKind::KwWhen:
    return "'when'";
  case TokenKind::KwThen:
    return "'then'";
  case TokenKind::KwRule:
    return "'rule'";
    return "'fact'";
  case TokenKind::KwOr:
    return "'or'";
  case TokenKind::KwAlways:
    return "always";
  case TokenKind::KwMaybe:
    return "maybe";
  case TokenKind::KwSet:
    return "set";
  case TokenKind::LParen:
    return "'('";
  case TokenKind::RParen:
    return "')'";
  case TokenKind::Dot:
    return "'.'";
  case TokenKind::Quote:
    return "'\"'";
  case TokenKind::Equal:
    return "'='";
  case TokenKind::Comma:
    return "','";
  case TokenKind::Hash:
    return "'#'";
  case TokenKind::Colon:
    return "':'";
  case TokenKind::Plus:
    return "+";
  }

  return "<unknown>";
}

bool isKeyword(const TokenKind kind)
{
  switch (kind)
  {
  case TokenKind::KwLet:
  case TokenKind::KwWhen:
  case TokenKind::KwThen:
  case TokenKind::KwRule:
  case TokenKind::KwOr:
  case TokenKind::KwAlways:
  case TokenKind::KwMaybe:
  case TokenKind::KwSet:
    return true;
  default:
    return false;
  }
}

} // namespace graphrewrite
