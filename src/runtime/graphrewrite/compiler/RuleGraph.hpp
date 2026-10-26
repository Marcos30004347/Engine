#pragma once

// A rule variant as two small labelled graphs: what it matches, and what it
// leaves behind.

#include "graphrewrite/frontend/Ast.hpp"
#include "graphrewrite/compiler/EdgeTypeTable.hpp"
#include "graphrewrite/frontend/OrExpansion.hpp"

#include <string>
#include <vector>

namespace graphrewrite::compiler {

struct PatternEdge
{
  uint32_t origin = 0u;
  uint32_t target = 0u;
  EdgeTypeId label = invalidEdgeType;
  uint32_t index = 0u; // position within its own clause

  // The sentence's clause, with variables resolved to this rule's vertex ids.
  // On a WHEN edge it is a guard; on a THEN edge a schedule. Not part of the
  // edge's identity - see sameShapeAs.
  bool hasClause = false;
  Expr clause;

  bool sameShapeAs(const PatternEdge &other) const
  {
    return origin == other.origin && target == other.target && label == other.label;
  }
};

struct RuleGraph
{
  std::string name; // the variant name, e.g. "do-insult-private#0"

  // Whether a matched row actually rewrites. Carried from the source through
  // OR expansion, since every variant of a rule shares the rule's dice.
  RulePolicy policy;
  std::vector<PatternEdge> when;
  std::vector<PatternEdge> then;
  std::vector<std::string> vertexNames; // vertex id -> variable name

  uint32_t vertexCount() const
  {
    return static_cast<uint32_t>(vertexNames.size());
  }
};

bool buildRuleGraph(const RuleVariant &variant, const EdgeTypeTable &table, DiagnosticSink &sink, RuleGraph &out);

std::string dumpRuleGraph(const RuleGraph &graph, const EdgeTypeTable &table);

} // namespace graphrewrite::compiler
