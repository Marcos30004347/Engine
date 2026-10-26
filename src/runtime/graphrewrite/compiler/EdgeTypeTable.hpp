#pragma once

// The set of edge types a programme uses, and their dense ids.
//
// A pattern string such as "? at ?" is an edge type's identity. Ids are assigned
// in declaration order and that order is the on-disk contract with the runtime,
// so it must not be made to depend on anything else.

#include "graphrewrite/frontend/Diagnostics.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace graphrewrite::compiler {

using EdgeTypeId = uint32_t;

constexpr EdgeTypeId invalidEdgeType = ~EdgeTypeId(0);

struct EdgeType
{
  std::string pattern;
  uint32_t arity = 2u; // 1 (a self-loop) or 2

  // Filled in during emission (phase 2.5). The runtime builds one page
  // partition per direction actually traversed, so a type only ever walked
  // forward costs half the storage of one walked both ways.
  bool usedForward = false;
  bool usedBackward = false;

  // How many edges of this type the programme was PLANNED against. Zero means
  // no statistics were supplied and the plan is share-ordered only.
  //
  // This is what lets a traversal start from the rarest type: seeding from a
  // type with ten edges instead of a million is the difference between a
  // ten-row table and a million-row one, and every later step multiplies out of
  // whatever the first one produced.
  uint32_t assumedCount = 0u;
};

class EdgeTypeTable
{
public:
  EdgeTypeId intern(const std::string &pattern, uint32_t arity);

  bool find(const std::string &pattern, EdgeTypeId &out) const;

  const EdgeType &operator[](EdgeTypeId id) const;
  EdgeType &mutableType(EdgeTypeId id);

  uint32_t size() const;

  const std::vector<EdgeType> &all() const;

private:
  std::vector<EdgeType> types;
  std::map<std::string, EdgeTypeId> byPattern;
};

} // namespace graphrewrite::compiler
