#pragma once

// Joining rule graphs so one traversal serves many rules.
//
// This is the reason the compiler exists. Two rules that both begin
// `C at L, D at L` should match those two edges ONCE, not twice. The merge
// finds the largest overlap between a rule's graph and the graph built so far,
// fuses them along it, and records on every shared edge and vertex which rules
// it now stands for - the "colours" the emitter later uses to stop a rule asking
// for an edge some shared query has already covered.
//
// Rules that overlap nothing start a new merged graph ("thread").

#include "graphrewrite/compiler/RuleGraph.hpp"

#include <string>
#include <vector>

namespace graphrewrite::compiler {

struct MergedGraph
{
  std::vector<PatternEdge> edges;

  // Parallel to `edges`: which rules each edge belongs to.
  std::vector<std::vector<uint32_t>> edgeRules;

  // Per merged vertex: which rules it stands for, and that rule's own vertex id.
  std::vector<std::vector<uint32_t>> vertexRules;
  std::vector<std::vector<uint32_t>> vertexIndices;

  uint32_t vertexCount() const
  {
    return static_cast<uint32_t>(vertexRules.size());
  }

  // Rule -> its vertex mapping into this graph, for the rules merged here.
  std::vector<uint32_t> rules;
};

std::vector<MergedGraph> mergeRuleGraphs(const std::vector<RuleGraph> &graphs);

std::string dumpMergedGraph(const MergedGraph &graph, const EdgeTypeTable &table);

} // namespace graphrewrite::compiler
