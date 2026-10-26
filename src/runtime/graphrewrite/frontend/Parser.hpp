#pragma once

// The parsing context: owns the source, the lexer and the diagnostic sink, and
// exposes the small set of verbs every AST node parses through.

#include "graphrewrite/frontend/Ast.hpp"
#include "graphrewrite/frontend/Diagnostics.hpp"
#include "graphrewrite/frontend/Lexer.hpp"

#include <memory>
#include <map>
#include <string>
#include <string_view>

namespace graphrewrite {

class SetDecl;

// Thrown by expect/error and caught at the declaration boundary, where it turns
// into a diagnostic followed by a resynchronise. It never escapes the module.
class ParseError
{
};

class Parser
{
public:
  Parser(std::string source, std::string path, DiagnosticSink &sink);

  std::unique_ptr<Program> parseProgram();

  // --- the verbs -----------------------------------------------------------
  const Token &peek(uint32_t ahead = 0u) const;
  bool peekIs(TokenKind kind, uint32_t ahead = 0u) const;
  bool accept(TokenKind kind);
  Token expect(TokenKind kind);
  [[noreturn]] void error(std::string message);
  [[noreturn]] void errorAt(SourceSpan span, std::string message);

  std::string_view textOf(const Token &token) const;
  SourceSpan spanFrom(const Token &start) const;

  // Skip to just past the next '.', so one bad declaration does not cascade.
  void recover();

  const std::string &path() const;
  DiagnosticSink &diagnostics();

  // Declared sets, by name. The parser has to know these to expand a call site,
  // and holding them here - rather than in a later pass - is what keeps set
  // expansion invisible to everything downstream.
  void addSet(const SetDecl *declaration);
  const SetDecl *findSet(const std::string &name) const;

  // A stable id for one set CALL - name plus arguments - within the declaration
  // being parsed. `together(A, B)` in a WHEN and in a THEN is the same call and
  // must expand to the same locals, or restating it would not restate anything.
  // `together(C, D)` is a different call and gets different locals.
  uint32_t localScopeFor(const std::string &signature);
  void resetLocalScopes();

private:
  Lexer lexer; // private on purpose: nodes reach tokens only through the verbs
  std::string sourcePath;
  DiagnosticSink &sink;
  std::map<std::string, const SetDecl *> setTable;
  std::map<std::string, uint32_t> callScopes;
  uint32_t nextScope = 0u;
};

} // namespace graphrewrite
