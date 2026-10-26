#pragma once

// Subgraph matching over small labelled graphs.
//
// This is the compile-time engine: it finds how much of one rule's graph
// overlaps another so the two can share a traversal. It is also the shape the
// GPU runtime implements, which is why the algorithm is written as a sequence
// of passes over flat arrays with a prefix sum between them rather than as a
// recursive backtracking search.
//
// The state is a TABLE: one column per bound pattern vertex, one row per
// partial match. Each step takes the next pattern edge and either widens the
// table by a column (a new vertex is bound) or filters it (both endpoints were
// already bound - a cyclic edge).

#include "graphrewrite/compiler/EdgeTypeTable.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace graphrewrite::compiler {

constexpr uint32_t noColumn = ~uint32_t(0);

struct GraphEdge
{
  uint32_t origin = 0u;
  EdgeTypeId label = invalidEdgeType;
  uint32_t target = 0u;
  uint32_t index = 0u;
};

// An edge list plus the two orderings the matcher probes it by.
class LabelledGraph
{
public:
  void add(uint32_t origin, EdgeTypeId label, uint32_t target, uint32_t index);
  void build();

  uint32_t vertexCount() const;
  uint32_t edgeCount() const;

  const std::vector<GraphEdge> &edges() const;

  // Half-open ranges into forwardOrder / backwardOrder.
  void forwardRange(EdgeTypeId label, uint32_t origin, uint32_t &first, uint32_t &last) const;
  void backwardRange(EdgeTypeId label, uint32_t target, uint32_t &first, uint32_t &last) const;

  uint32_t forwardEdge(uint32_t slot) const;
  uint32_t backwardEdge(uint32_t slot) const;

private:
  std::vector<GraphEdge> edgeList;
  std::vector<uint32_t> forwardOrder;  // sorted by (label, origin)
  std::vector<uint32_t> backwardOrder; // sorted by (label, target)
  uint32_t vertices = 0u;
};

enum class StepDirection
{
  Backward = 0,
  Forward = 1,
  Cyclic = 2,
};

struct MatchStep
{
  uint32_t patternEdge = 0u;
  EdgeTypeId label = invalidEdgeType;
  StepDirection direction = StepDirection::Forward;
  uint32_t fromColumn = 0u;
  uint32_t writeColumn = 0u;
};

struct MatchTable
{
  std::vector<uint32_t> cells; // row-major: [row * cols + col]
  uint32_t rows = 0u;
  uint32_t cols = 0u;

  std::vector<uint32_t> columnToVertex;
  std::vector<uint32_t> vertexToColumn;
  std::vector<uint32_t> matchedEdges; // pattern edge indices consumed so far

  uint32_t at(const uint32_t row, const uint32_t col) const
  {
    return cells[row * cols + col];
  }
};

class IPatternMatcher
{
public:
  virtual ~IPatternMatcher() = default;

  virtual void initiate() = 0;
  virtual void expand() = 0;
  virtual void removeNonInjectiveRows() = 0;
  virtual bool finished() const = 0;
  virtual MatchTable result() const = 0;
};

// Orders the pattern's edges so every step after the first attaches to an
// already-bound vertex. Returns false if the pattern is disconnected.
bool buildMatchOrder(const LabelledGraph &pattern, uint32_t startVertex, std::vector<MatchStep> &out, uint32_t &columnCount);

class TablePatternMatcher : public IPatternMatcher
{
public:
  TablePatternMatcher(const LabelledGraph &data, const LabelledGraph &pattern, uint32_t startVertex);

  void initiate() override;
  void expand() override;
  void removeNonInjectiveRows() override;
  bool finished() const override;
  MatchTable result() const override;

  bool valid() const;
  uint32_t rowCount() const;
  uint32_t columnCount() const;

private:
  const LabelledGraph &data;
  const LabelledGraph &pattern;

  std::vector<MatchStep> steps;
  uint32_t step = 0u;
  uint32_t columns = 0u;
  bool orderValid = false;

  std::vector<uint32_t> table; // row-major, `columns` wide
  uint32_t rows = 0u;

  std::vector<uint32_t> columnToVertex;
  std::vector<uint32_t> vertexToColumn;
};

} // namespace graphrewrite::compiler
