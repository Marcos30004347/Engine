#include "graphrewrite/frontend/Parser.hpp"

namespace graphrewrite {

Parser::Parser(std::string source, std::string path, DiagnosticSink &diagnosticSink)
    : lexer(std::move(source), diagnosticSink), sourcePath(std::move(path)), sink(diagnosticSink)
{
}

const Token &Parser::peek(const uint32_t ahead) const
{
  return lexer.peek(ahead);
}

bool Parser::peekIs(const TokenKind kind, const uint32_t ahead) const
{
  return lexer.peek(ahead).kind == kind;
}

bool Parser::accept(const TokenKind kind)
{
  if (lexer.peek().kind != kind)
  {
    return false;
  }

  lexer.next();

  return true;
}

Token Parser::expect(const TokenKind kind)
{
  const Token token = lexer.peek();

  if (token.kind != kind)
  {
    errorAt(token.span, std::string("expected ") + tokenKindName(kind) + ", found " + tokenKindName(token.kind));
  }

  return lexer.next();
}

void Parser::error(std::string message)
{
  errorAt(lexer.peek().span, std::move(message));
}

void Parser::errorAt(const SourceSpan span, std::string message)
{
  sink.error(span, std::move(message));

  throw ParseError();
}

std::string_view Parser::textOf(const Token &token) const
{
  return lexer.textOf(token);
}

SourceSpan Parser::spanFrom(const Token &start) const
{
  const Token &current = lexer.peek();

  SourceSpan span = start.span;

  span.length = current.span.offset > start.span.offset ? current.span.offset - start.span.offset : start.span.length;

  return span;
}

void Parser::recover()
{
  while (!lexer.atEnd() && lexer.peek().kind != TokenKind::Dot)
  {
    lexer.next();
  }

  accept(TokenKind::Dot);
}

const std::string &Parser::path() const
{
  return sourcePath;
}

DiagnosticSink &Parser::diagnostics()
{
  return sink;
}

} // namespace graphrewrite

namespace graphrewrite {

void Parser::addSet(const SetDecl *declaration)
{
  setTable[declaration->name()] = declaration;
}

uint32_t Parser::localScopeFor(const std::string &signature)
{
  const auto entry = callScopes.find(signature);

  if (entry != callScopes.end())
  {
    return entry->second;
  }

  ++nextScope;
  callScopes[signature] = nextScope;

  return nextScope;
}

void Parser::resetLocalScopes()
{
  callScopes.clear();
}

const SetDecl *Parser::findSet(const std::string &name) const
{
  const auto entry = setTable.find(name);

  return entry == setTable.end() ? nullptr : entry->second;
}

} // namespace graphrewrite
