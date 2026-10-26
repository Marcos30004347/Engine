#include "graphrewrite/compiler/RuleGraph.hpp"

#include <algorithm>

#include <map>

namespace graphrewrite::compiler {
namespace {

uint32_t vertexFor(const std::string &name, std::map<std::string, uint32_t> &ids, RuleGraph &graph)
{
  const auto existing = ids.find(name);

  if (existing != ids.end())
  {
    return existing->second;
  }

  const uint32_t id = static_cast<uint32_t>(graph.vertexNames.size());

  ids.emplace(name, id);
  graph.vertexNames.push_back(name);

  return id;
}

bool lowerClause(const std::vector<const Sentence *> &clause, const EdgeTypeTable &table, std::map<std::string, uint32_t> &ids,
                 RuleGraph &graph, std::vector<PatternEdge> &out, DiagnosticSink &sink)
{
  bool ok = true;

  for (const Sentence *sentence : clause)
  {
    EdgeTypeId label = invalidEdgeType;

    if (!table.find(sentence->pattern(), label))
    {
      sink.error(sentence->span, "no edge type for '" + sentence->pattern() + "'");

      ok = false;

      continue;
    }

    const std::vector<std::string> &variables = sentence->variables();

    PatternEdge edge;

    edge.label = label;
    edge.index = static_cast<uint32_t>(out.size());

    if (variables.size() == 2u)
    {
      edge.origin = vertexFor(variables[0], ids, graph);
      edge.target = vertexFor(variables[1], ids, graph);
    }
    else if (variables.size() == 1u)
    {
      // A unary sentence is a SELF-LOOP. The implementation this is ported from
      // wrote `target = variables.size()`, which is the literal number 1, so
      // every unary fact attached itself to whatever vertex 1 happened to be.
      edge.origin = vertexFor(variables[0], ids, graph);
      edge.target = edge.origin;
    }
    else
    {
      sink.error(sentence->span, "sentence '" + sentence->original() + "' has no variables");

      ok = false;

      continue;
    }

    // The same edge written twice in one clause is one edge. It is easy to do by
    // accident now that sets compose - `living(A), living-man(A)` yields `A is
    // alive` twice - and the duplicate is not free: in the WHEN it emits a whole
    // extra WHERE, a redundant join over an entire edge type, and a tick is a GPU
    // submit round trip. In the THEN it would insert the edge twice.
    //
    // Two identical pattern edges cannot mean two DISTINCT graph edges: a pattern
    // edge is identified by its endpoints' variable names and its label, so there
    // is nothing for a second copy to denote.
    //
    // Dropped before `index` is taken from out.size(), so indices stay dense and
    // the edge columns a rewrite reads still line up.
    const bool already = std::any_of(out.begin(), out.end(),
                                     [&edge](const PatternEdge &other) { return other.sameShapeAs(edge); });

    if (already)
    {
      continue;
    }

    if (sentence->clause() != nullptr)
    {
      edge.hasClause = true;
      edge.clause = *sentence->clause();
    }

    out.push_back(edge);
  }

  return ok;
}

// Clauses refer to variables the sentences bound - possibly a THEN variable
// that is lowered after the sentence carrying the clause - so they are resolved
// once both clauses are in.
bool resolveClauseVariables(Expr &expr, const std::map<std::string, uint32_t> &ids, const SourceSpan &span, DiagnosticSink &sink)
{
  bool ok = true;

  if (expr.kind == Expr::Kind::Variable)
  {
    const auto entry = ids.find(expr.name);

    if (entry == ids.end())
    {
      sink.error(span, "clause refers to '" + expr.name + "', which the rule never binds");

      ok = false;
    }
    else
    {
      expr.vertex = entry->second;
    }
  }

  for (Expr &arg : expr.args)
  {
    ok = resolveClauseVariables(arg, ids, span, sink) && ok;
  }

  return ok;
}

} // namespace

bool buildRuleGraph(const RuleVariant &variant, const EdgeTypeTable &table, DiagnosticSink &sink, RuleGraph &out)
{
  out.name = variant.name;
  out.policy = variant.policy;

  std::map<std::string, uint32_t> ids;

  // WHEN first, then THEN: a variable shared between the clauses is one vertex,
  // and the numbering a rule's own table columns use starts from the match side.
  bool ok = lowerClause(variant.when, table, ids, out, out.when, sink);

  ok = lowerClause(variant.then, table, ids, out, out.then, sink) && ok;

  for (std::vector<PatternEdge> *clause : {&out.when, &out.then})
  {
    for (PatternEdge &edge : *clause)
    {
      if (edge.hasClause)
      {
        ok = resolveClauseVariables(edge.clause, ids, variant.span, sink) && ok;
      }
    }
  }

  return ok;
}

std::string dumpRuleGraph(const RuleGraph &graph, const EdgeTypeTable &table)
{
  std::string out = graph.name + "\n";

  for (uint32_t i = 0u; i < graph.vertexCount(); ++i)
  {
    out += "  v" + std::to_string(i) + " = " + graph.vertexNames[i] + "\n";
  }

  for (const std::vector<PatternEdge> *clause : {&graph.when, &graph.then})
  {
    out += clause == &graph.when ? "  when\n" : "  then\n";

    for (const PatternEdge &edge : *clause)
    {
      out += "    " + std::to_string(edge.origin) + " --(" + std::to_string(edge.label) + ": " + table[edge.label].pattern + ")--> " +
             std::to_string(edge.target) + "\n";
    }
  }

  return out;
}

} // namespace graphrewrite::compiler
