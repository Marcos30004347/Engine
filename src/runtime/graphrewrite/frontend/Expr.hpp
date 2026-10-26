#pragma once

// A call expression attached to a sentence, in parentheses after it:
//
//   C is a church in V (availableAt(C, now() + 5))     a GUARD, on a WHEN edge
//   A wed in C (occurAt(now() + 5))                     a SCHEDULE, on a THEN edge
//   A is engaged to B (now())                          the default schedule
//   A weds on D' (D' = day(now() + 4))                 a BIND: the new node D'
//                                                      IS the value, not a fresh id
//
// The matcher never evaluates these. A guard is assumed true while matching and
// checked on the CPU, by a handler registered under the call's name, before the
// row fires; a schedule is evaluated the same way and says WHEN the edge is
// applied - an edge scheduled past the current time waits in a queue and is
// re-validated when its time comes.
//
// There is no `-`: every rule title is hyphenated, so `-` cannot be a token.

#include <cstdint>
#include <string>
#include <vector>

namespace graphrewrite {

struct Expr
{
  enum class Kind
  {
    Number,   // 5
    Variable, // C - a rule variable; `vertex` is filled in by the compiler
    Word,     // Days - a bare word, resolved by the runtime as a named unit
    Call,     // now(), availableAt(C, ...), time.now(Days)
    Add,      // a + b; args are the two operands
    Bind,     // D' = expr; args are the variable and the expression
  };

  Kind kind = Kind::Number;
  uint32_t number = 0u;
  std::string name; // Variable / Word / Call
  uint32_t vertex = ~uint32_t(0); // Variable, once lowered
  std::vector<Expr> args;

  static Expr call(std::string id)
  {
    Expr expr;

    expr.kind = Kind::Call;
    expr.name = std::move(id);

    return expr;
  }

  static Expr variable(std::string id)
  {
    Expr expr;

    expr.kind = Kind::Variable;
    expr.name = std::move(id);

    return expr;
  }

  // Every variable name mentioned anywhere in the expression.
  void collectVariables(std::vector<std::string> &out) const;

  // Renames variables in place; used by set expansion.
  void rename(const std::vector<std::pair<std::string, std::string>> &renaming);
};

// Source form: `availableAt(C, now() + 5)`.
std::string printExpr(const Expr &expr);

} // namespace graphrewrite
