#include "graphrewrite/frontend/Expr.hpp"

namespace graphrewrite {

void Expr::collectVariables(std::vector<std::string> &out) const
{
  if (kind == Kind::Variable)
  {
    bool seen = false;

    for (const std::string &name_ : out)
    {
      seen = seen || name_ == name;
    }

    if (!seen)
    {
      out.push_back(name);
    }
  }

  for (const Expr &arg : args)
  {
    arg.collectVariables(out);
  }
}

void Expr::rename(const std::vector<std::pair<std::string, std::string>> &renaming)
{
  if (kind == Kind::Variable)
  {
    for (const auto &entry : renaming)
    {
      if (entry.first == name)
      {
        name = entry.second;

        break;
      }
    }
  }

  for (Expr &arg : args)
  {
    arg.rename(renaming);
  }
}

std::string printExpr(const Expr &expr)
{
  switch (expr.kind)
  {
  case Expr::Kind::Number:
    return std::to_string(expr.number);
  case Expr::Kind::Variable:
  case Expr::Kind::Word:
    return expr.name;
  case Expr::Kind::Add:
    return printExpr(expr.args[0]) + " + " + printExpr(expr.args[1]);
  case Expr::Kind::Bind:
    return printExpr(expr.args[0]) + " = " + printExpr(expr.args[1]);
  case Expr::Kind::Call:
  {
    std::string out = expr.name + "(";

    for (size_t i = 0u; i < expr.args.size(); ++i)
    {
      out += (i == 0u ? "" : ", ") + printExpr(expr.args[i]);
    }

    return out + ")";
  }
  }

  return std::string();
}

} // namespace graphrewrite
