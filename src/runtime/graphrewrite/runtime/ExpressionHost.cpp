#include "graphrewrite/runtime/ExpressionHost.hpp"

#include <algorithm>

namespace graphrewrite::runtime {

void ExpressionHost::registerHandler(std::string id, Handler handler, void *user)
{
  handlers[std::move(id)] = Entry{std::move(handler), user};
}

void ExpressionHost::setUnit(std::string word, const uint32_t value)
{
  units[std::move(word)] = value;
}

bool ExpressionHost::has(const std::string &id) const
{
  return handlers.count(id) != 0u;
}

bool ExpressionHost::evaluate(const Expr &expr, const CallContext &context, uint32_t &out) const
{
  switch (expr.kind)
  {
  case Expr::Kind::Number:
    out = expr.number;

    return true;

  case Expr::Kind::Variable:
    if (context.bound == nullptr || expr.vertex >= context.bound->size())
    {
      return false;
    }

    out = (*context.bound)[expr.vertex];

    return true;

  case Expr::Kind::Word:
  {
    const auto unit = units.find(expr.name);

    if (unit == units.end())
    {
      if (std::find(missingIds.begin(), missingIds.end(), expr.name) == missingIds.end())
      {
        missingIds.push_back(expr.name);
      }

      return false;
    }

    out = unit->second;

    return true;
  }

  case Expr::Kind::Add:
  {
    uint32_t left = 0u;
    uint32_t right = 0u;

    if (expr.args.size() != 2u || !evaluate(expr.args[0], context, left) || !evaluate(expr.args[1], context, right))
    {
      return false;
    }

    out = left + right;

    return true;
  }

  case Expr::Kind::Bind:
    // The value the bound variable takes. Who it is assigned to is the
    // applier's business; here it is just the right-hand side.
    return expr.args.size() == 2u && evaluate(expr.args[1], context, out);

  case Expr::Kind::Call:
    break;
  }

  // --- calls ---------------------------------------------------------------

  if (expr.name == "now")
  {
    out = context.now;

    return true;
  }

  std::vector<uint32_t> args;
  std::vector<std::string> words;

  for (const Expr &arg : expr.args)
  {
    uint32_t value = 0u;

    if (!evaluate(arg, context, value))
    {
      return false;
    }

    args.push_back(value);

    if (arg.kind == Expr::Kind::Word)
    {
      words.push_back(arg.name);
    }
  }

  if (expr.name == "occurAt")
  {
    // occurAt(x) is x in ticks; occurAt(x, Unit) is x units, each `Unit` ticks.
    if (args.empty())
    {
      return false;
    }

    out = args.size() >= 2u ? args[0] * args[1] : args[0];

    return true;
  }

  const auto entry = handlers.find(expr.name);

  if (entry == handlers.end())
  {
    if (std::find(missingIds.begin(), missingIds.end(), expr.name) == missingIds.end())
    {
      missingIds.push_back(expr.name);
    }

    return false;
  }

  return entry->second.handler(args, words, context, entry->second.user, out);
}

} // namespace graphrewrite::runtime
