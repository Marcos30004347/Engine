// Phase 1.7 [P5, P7, P9, P10]: OR expansion.
//
// Expectations here are the ones the Python model printed, so the C++ and the
// specification cannot drift apart silently.

#include "TestHarness.hpp"
#include "graphrewrite/frontend/OrExpansion.hpp"
#include "graphrewrite/frontend/Parser.hpp"

#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::testing;

namespace {

struct Expansion
{
  std::unique_ptr<Program> program;
  std::vector<RuleVariant> variants;
  DiagnosticSink sink;
};

// The AST must outlive the variants, so the program is kept alive in the result.
void expand(const std::string &source, Expansion &out, const uint32_t cap = 64u)
{
  Parser parser(source, "t.graphwrite", out.sink);

  out.program = Program::parse(parser);

  for (const std::unique_ptr<Declaration> &declaration : out.program->declarations())
  {
    if (declaration->kind() == NodeKind::RuleDecl)
    {
      OrExpansionOptions options;

      options.maxVariantsPerRule = cap;

      out.variants = expandOr(static_cast<const RuleDecl &>(*declaration), options, out.sink);

      return;
    }
  }
}

std::string clauseText(const std::vector<const Sentence *> &clause)
{
  std::string out;

  for (const Sentence *sentence : clause)
  {
    if (!out.empty())
    {
      out += ", ";
    }

    out += sentence->original();
  }

  return out;
}

void briefExample()
{
  Expansion expansion;

  expand("rule r: when A likes B or A likes D, B likes D then A likes D.", expansion);

  checkEqual(expansion.sink.count(), 0u, "expansion is clean");
  checkEqual(expansion.variants.size(), size_t(2), "two variants");

  checkEqual(expansion.variants[0].name, std::string("r#0"), "first variant name");
  checkEqual(clauseText(expansion.variants[0].when), std::string("A likes B, B likes D"), "r#0 WHEN");
  checkEqual(clauseText(expansion.variants[0].then), std::string("A likes D"), "r#0 THEN");

  checkEqual(expansion.variants[1].name, std::string("r#1"), "second variant name");
  checkEqual(clauseText(expansion.variants[1].when), std::string("A likes D, B likes D"), "r#1 WHEN");
  checkEqual(clauseText(expansion.variants[1].then), std::string("A likes D"), "r#1 THEN");
}

void orInThenIsAProduct()
{
  Expansion expansion;

  expand("rule r: when A likes B then A likes B or A hates B.", expansion);

  checkEqual(expansion.variants.size(), size_t(2), "two variants from a THEN disjunction");
  checkEqual(clauseText(expansion.variants[0].when), std::string("A likes B"), "shared WHEN 0");
  checkEqual(clauseText(expansion.variants[0].then), std::string("A likes B"), "THEN 0");
  checkEqual(clauseText(expansion.variants[1].when), std::string("A likes B"), "shared WHEN 1");
  checkEqual(clauseText(expansion.variants[1].then), std::string("A hates B"), "THEN 1");
}

void whenTimesThen()
{
  Expansion expansion;

  expand("rule r: when A likes B or A likes C then A hates B or A hates C.", expansion);

  checkEqual(expansion.variants.size(), size_t(4), "2 x 2 = 4 variants");
}

void deduplication()
{
  Expansion expansion;

  expand("rule r: when A likes B or A likes B then A likes B.", expansion);

  checkEqual(expansion.variants.size(), size_t(1), "identical alternatives collapse to one");
  checkEqual(expansion.variants[0].name, std::string("r#0"), "and names stay dense");
}

void deduplicationKeepsDistinctVariables()
{
  Expansion expansion;

  // Same pattern, different variables: these are NOT duplicates.
  expand("rule r: when A likes B or B likes A then A likes B.", expansion);

  checkEqual(expansion.variants.size(), size_t(2), "same pattern with swapped variables is two variants");
}

void capIsEnforced()
{
  Expansion expansion;

  std::string alternatives;

  for (uint32_t i = 0u; i < 9u; ++i)
  {
    if (i != 0u)
    {
      alternatives += " or ";
    }

    alternatives += "A likes V" + std::to_string(i);
  }

  expand("rule r: when " + alternatives + ", " + alternatives + " then A likes B.", expansion, 64u);

  checkEqual(expansion.variants.size(), size_t(0), "over the cap, nothing is expanded");
  checkEqual(expansion.sink.errorCount(), 1u, "and it is reported once");
  check(expansion.sink[0].message.find("81 variants") != std::string::npos, "the diagnostic states the real count");
}

void capBoundaryIsInclusive()
{
  Expansion expansion;

  expand("rule r: when A likes B or A likes C then A likes B.", expansion, 2u);

  checkEqual(expansion.variants.size(), size_t(2), "exactly at the cap is allowed");
  checkEqual(expansion.sink.errorCount(), 0u, "and not reported");
}

void andOnlyRuleGivesOneVariant()
{
  Expansion expansion;

  expand("rule r: when A likes B, B likes C then A likes C.", expansion);

  checkEqual(expansion.variants.size(), size_t(1), "a rule with no `or` gives exactly one variant");
  checkEqual(expansion.variants[0].when.size(), size_t(2), "both conjuncts retained");
  checkEqual(expansion.variants[0].name, std::string("r#0"), "still gets a variant name");
}

void countVariantsSaturates()
{
  Expansion expansion;
  DiagnosticSink sink;

  std::string alternatives;

  for (uint32_t i = 0u; i < 5u; ++i)
  {
    if (i != 0u)
    {
      alternatives += " or ";
    }

    alternatives += "A likes V" + std::to_string(i);
  }

  Parser parser("rule r: when " + alternatives + " then A likes B.", "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);

  for (const std::unique_ptr<Declaration> &declaration : program->declarations())
  {
    if (declaration->kind() == NodeKind::RuleDecl)
    {
      checkEqual(countVariants(static_cast<const RuleDecl &>(*declaration)), uint64_t(5), "countVariants agrees with the expansion");
    }
  }
}

void corpusRulesAllExpandToOne()
{
  // Nothing in the shipped corpus uses `or`, so every rule must expand to
  // exactly one variant. This is the regression that catches an expansion bug
  // that only shows up on AND-only input.
  Expansion expansion;
  DiagnosticSink sink;

  const std::string source = "rule a: when A at B then A at B.\n"
                             "rule b: when A at B, B at C then A at C.\n";

  Parser parser(source, "t.graphwrite", sink);

  const std::unique_ptr<Program> program = Program::parse(parser);

  uint32_t rules = 0u;

  for (const std::unique_ptr<Declaration> &declaration : program->declarations())
  {
    if (declaration->kind() != NodeKind::RuleDecl)
    {
      continue;
    }

    ++rules;

    const std::vector<RuleVariant> variants = expandOr(static_cast<const RuleDecl &>(*declaration), OrExpansionOptions(), sink);

    checkEqual(variants.size(), size_t(1), "AND-only rule expands to one variant");
  }

  checkEqual(rules, 2u, "both rules examined");
}

} // namespace

int main()
{
  briefExample();
  orInThenIsAProduct();
  whenTimesThen();
  deduplication();
  deduplicationKeepsDistinctVariables();
  capIsEnforced();
  capBoundaryIsInclusive();
  andOnlyRuleGivesOneVariant();
  countVariantsSaturates();
  corpusRulesAllExpandToOne();

  return report("GraphRewriteOrExpansionTests");
}
