#pragma once

// The starting graph, read from a `.graphfacts` file.
//
// One edge per line, written the way a rule would write it but with NAMES where
// a rule has variables:
//
//   Aldric is husband of Maerwyn
//   Aldric is alive
//
// Every capitalised word standing in a `?` position of some declared edge type
// is a name, and each distinct name gets a node id in order of first appearance.
// The same name anywhere in the file is the same node - that is what makes the
// graph connect up.
//
// This replaced the `fact` clause. A fact inside the rule file tied one starting
// world to one rule set; a separate file lets the same rules run over many
// worlds, and lets a world be generated without touching the rules.

#include "graphrewrite/compiler/EdgeTypeTable.hpp"
#include "graphrewrite/frontend/Diagnostics.hpp"
#include "graphrewrite/runtime/IProgramRunner.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace graphrewrite::runtime {

struct LoadedFacts
{
  std::vector<Fact> facts;

  // Node id -> the name the file gave it. Ids are dense from zero, so this is
  // also how many nodes the file declared, and the store's vertex allocator has
  // to start above it.
  std::vector<std::string> names;
  std::map<std::string, uint32_t> ids;

  // A bare NUMBER in a `?` position is a value, not a name: `Aldric born in
  // 965`. The caller says which node a value is (the story maps a year to its
  // week node above dayBase); without a resolver a number is an error. Value
  // nodes are not interned in `names`, so they never count as declared people.
  std::function<bool(uint32_t value, uint32_t &id)> valueResolver;
};

// Parses `text`. A line that matches no declared edge type is an ERROR, not a
// skipped edge: a typo here is otherwise invisible until a rule that should have
// fired never does.
bool loadFactsText(const std::string &text, const compiler::EdgeTypeTable &types, const std::string &path, DiagnosticSink &sink,
                   LoadedFacts &out);

bool loadFactsFile(const std::string &path, const compiler::EdgeTypeTable &types, DiagnosticSink &sink, LoadedFacts &out);

} // namespace graphrewrite::runtime
