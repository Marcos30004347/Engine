#pragma once

// Evaluates the clauses a rule file attaches to its sentences - on the CPU,
// after matching, before a row fires.
//
// The rule file names calls it cannot define: `availableAt`, `nowInDays`,
// whatever the programme running the rules decides they mean. Those are
// HANDLERS, registered here by string id with a context pointer, so a church's
// booking table or a calendar lives with the host application and not in the
// graph. A guard's handler may mutate its context - booking the church is the
// whole point of asking - which is why guards run only for rows that will fire.
//
// Built in: `now()`, `+`, numbers, variables (the bound node id), and
// `occurAt(x [, unit])`, where a bare word is a unit registered with setUnit.

#include "graphrewrite/frontend/Expr.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace graphrewrite::runtime {

struct CallContext
{
  uint32_t now = 0u;
  uint32_t rule = 0u;
  const std::vector<uint32_t> *bound = nullptr; // rule vertex -> node id
};

class ExpressionHost
{
public:
  // Returns false to FAIL the call: a guard that fails does not fire; a
  // schedule that fails applies at `now`. `words` carries the bare-word
  // arguments by name, in argument order, for handlers that care which unit
  // was asked for.
  using Handler = std::function<bool(const std::vector<uint32_t> &args, const std::vector<std::string> &words,
                                     const CallContext &context, void *user, uint32_t &result)>;

  void registerHandler(std::string id, Handler handler, void *user = nullptr);
  void setUnit(std::string word, uint32_t value);

  bool has(const std::string &id) const;

  // False when the expression could not be evaluated: an unknown handler, a
  // handler that refused, an unbound variable, a word with no unit.
  bool evaluate(const Expr &expr, const CallContext &context, uint32_t &out) const;

  // For diagnostics: the ids the host has been asked for and could not answer.
  const std::vector<std::string> &missing() const
  {
    return missingIds;
  }

private:
  struct Entry
  {
    Handler handler;
    void *user = nullptr;
  };

  std::map<std::string, Entry> handlers;
  std::map<std::string, uint32_t> units;
  mutable std::vector<std::string> missingIds;
};

} // namespace graphrewrite::runtime
