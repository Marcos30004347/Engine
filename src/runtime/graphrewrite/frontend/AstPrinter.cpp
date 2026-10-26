#include "graphrewrite/frontend/AstPrinter.hpp"

namespace graphrewrite {
namespace {

void printSentence(const Sentence &sentence, std::string &out)
{
  bool first = true;

  for (const Sentence::Word &word : sentence.words())
  {
    if (!first)
    {
      out += ' ';
    }

    out += word.text;
    first = false;
  }

  if (sentence.clause() != nullptr)
  {
    out += " (" + printExpr(*sentence.clause()) + ")";
  }
}

void printConjunction(const Conjunction &conjunction, std::string &out)
{
  bool firstTerm = true;

  for (const std::unique_ptr<Disjunction> &term : conjunction.terms())
  {
    if (!firstTerm)
    {
      out += ", ";
    }

    bool firstAlternative = true;

    for (const std::unique_ptr<Sentence> &alternative : term->alternatives())
    {
      if (!firstAlternative)
      {
        out += " or ";
      }

      printSentence(*alternative, out);
      firstAlternative = false;
    }

    firstTerm = false;
  }
}

void dumpSentence(const Sentence &sentence, std::string &out)
{
  out += "(sentence \"";
  out += sentence.pattern();
  out += '"';

  for (const std::string &variable : sentence.variables())
  {
    out += ' ';
    out += variable;
  }

  for (const KeyValue &annotation : sentence.annotations())
  {
    out += " [";
    out += annotation.key;
    out += '=';
    out += annotation.value;
    out += ']';
  }

  out += ')';

  if (sentence.clause() != nullptr)
  {
    out += " (clause " + printExpr(*sentence.clause()) + ")";
  }
}

void dumpConjunction(const Conjunction &conjunction, std::string &out)
{
  out += "(and";

  for (const std::unique_ptr<Disjunction> &term : conjunction.terms())
  {
    out += " (or";

    for (const std::unique_ptr<Sentence> &alternative : term->alternatives())
    {
      out += ' ';
      dumpSentence(*alternative, out);
    }

    out += ')';
  }

  out += ')';
}

} // namespace

std::string printAst(const Program &program)
{
  std::string out;

  for (const std::unique_ptr<Declaration> &declaration : program.declarations())
  {
    switch (declaration->kind())
    {
    case NodeKind::LetDecl:
    {
      out += "let ";
      printSentence(static_cast<const LetDecl &>(*declaration).sentence(), out);
      out += ".\n";
      break;
    }
    case NodeKind::RuleDecl:
    {
      const RuleDecl &rule = static_cast<const RuleDecl &>(*declaration);

      out += "rule ";
      printSentence(rule.title(), out);

      // Reprinted source has to re-parse to the same tree, so a MAYBE rule must
      // say so - dropping it here would silently turn it into an ALWAYS rule.
      if (rule.policy().kind == FirePolicy::Maybe)
      {
        out += " maybe " + std::to_string(rule.policy().chance);

        if (rule.policy().denominator != maybeDenominator)
        {
          out += " of " + std::to_string(rule.policy().denominator);
        }
      }

      out += ":\nwhen ";
      printConjunction(rule.when(), out);
      out += "\nthen ";
      printConjunction(rule.then(), out);
      out += ".\n";
      break;
    }
    case NodeKind::SetDecl:
    {
      const SetDecl &set = static_cast<const SetDecl &>(*declaration);

      out += "set " + set.name() + "(";

      for (size_t i = 0u; i < set.parameters().size(); ++i)
      {
        out += (i == 0u ? "" : ", ") + set.parameters()[i];
      }

      out += ") = ";
      printConjunction(set.body(), out);
      out += ".\n";
      break;
    }
    default:
      break;
    }
  }

  return out;
}

std::string dumpAst(const Program &program)
{
  std::string out = "(program";

  for (const std::unique_ptr<Declaration> &declaration : program.declarations())
  {
    switch (declaration->kind())
    {
    case NodeKind::LetDecl:
      out += " (let ";
      dumpSentence(static_cast<const LetDecl &>(*declaration).sentence(), out);
      out += ')';
      break;
    case NodeKind::RuleDecl:
    {
      const RuleDecl &rule = static_cast<const RuleDecl &>(*declaration);

      out += " (rule ";
      dumpSentence(rule.title(), out);
      out += rule.policy().kind == FirePolicy::Maybe ? " maybe:" + std::to_string(rule.policy().chance) : " always";
      out += " (when ";
      dumpConjunction(rule.when(), out);
      out += ") (then ";
      dumpConjunction(rule.then(), out);
      out += "))";
      break;
    }
    case NodeKind::SetDecl:
    {
      const SetDecl &set = static_cast<const SetDecl &>(*declaration);

      out += " (set " + set.name();

      for (const std::string &parameter : set.parameters())
      {
        out += " " + parameter;
      }

      out += " ";
      dumpConjunction(set.body(), out);
      out += ")";
      break;
    }
    default:
      break;
    }
  }

  out += ')';

  return out;
}

} // namespace graphrewrite
