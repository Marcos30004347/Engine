#pragma once

// Expands `or` away.
//
// Everything downstream of this pass sees only WHEN / AND / THEN. A rule with
// alternatives becomes several AND-only variants, one per combination, and each
// is then analysed and compiled independently:
//
//   rule r: when A likes B or A likes D, B likes D then A likes D.
//     ->  r#0: when A likes B, B likes D  then A likes D
//         r#1: when A likes D, B likes D  then A likes D

#include "graphrewrite/frontend/Ast.hpp"
#include "graphrewrite/frontend/Diagnostics.hpp"

#include <string>
#include <vector>

namespace graphrewrite {

// Sentences are borrowed from the AST, which must outlive the variants.
struct RuleVariant
{
  std::string name; // "<rule>#<n>"
  std::vector<const Sentence *> when;
  std::vector<const Sentence *> then;
  SourceSpan span;

  // Every variant of a rule inherits that rule's policy: `or` splits the
  // pattern, it does not split the dice.
  RulePolicy policy;
};

struct OrExpansionOptions
{
  // The product of the alternative counts is bounded: a handful of wide
  // disjunctions multiply out fast, and silently compiling ten thousand
  // variants is worse than saying so.
  uint32_t maxVariantsPerRule = 64u;
};

std::vector<RuleVariant> expandOr(const RuleDecl &rule, const OrExpansionOptions &options, DiagnosticSink &sink);

// Number of variants `rule` would expand to, saturating rather than overflowing.
uint64_t countVariants(const RuleDecl &rule);

} // namespace graphrewrite
