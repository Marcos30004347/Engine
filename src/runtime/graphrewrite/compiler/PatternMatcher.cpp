#include "graphrewrite/compiler/PatternMatcher.hpp"

#include <algorithm>

namespace graphrewrite::compiler {
namespace {

// Lower bound over an index array ordered by (label, key).
uint32_t lowerBound(const std::vector<GraphEdge> &edges, const std::vector<uint32_t> &order, const EdgeTypeId label, const uint32_t key,
                    const bool byOrigin)
{
  // i64 bounds on purpose: the implementation this replaces walked these with
  // an unsigned counter and ran off the front of the array at m == 0.
  int64_t low = 0;
  int64_t high = static_cast<int64_t>(order.size()) - 1;
  int64_t found = static_cast<int64_t>(order.size());

  while (low <= high)
  {
    const int64_t mid = low + (high - low) / 2;
    const GraphEdge &edge = edges[order[static_cast<size_t>(mid)]];
    const uint32_t value = byOrigin ? edge.origin : edge.target;

    if (edge.label > label || (edge.label == label && value >= key))
    {
      found = mid;
      high = mid - 1;
    }
    else
    {
      low = mid + 1;
    }
  }

  return static_cast<uint32_t>(found);
}

} // namespace

void LabelledGraph::add(const uint32_t origin, const EdgeTypeId label, const uint32_t target, const uint32_t index)
{
  GraphEdge edge;

  edge.origin = origin;
  edge.label = label;
  edge.target = target;
  edge.index = index;

  edgeList.push_back(edge);

  vertices = std::max(vertices, std::max(origin, target) + 1u);
}

void LabelledGraph::build()
{
  forwardOrder.resize(edgeList.size());
  backwardOrder.resize(edgeList.size());

  for (uint32_t i = 0u; i < static_cast<uint32_t>(edgeList.size()); ++i)
  {
    forwardOrder[i] = i;
    backwardOrder[i] = i;
  }

  // std::sort rather than the hand-rolled radix sort of the original, which
  // used an unsigned loop counter running to -1 and passed its output buffers
  // in as scratch. At compile-time graph sizes there is nothing to win by
  // reimplementing it.
  std::sort(forwardOrder.begin(), forwardOrder.end(), [this](const uint32_t a, const uint32_t b) {
    const GraphEdge &ea = edgeList[a];
    const GraphEdge &eb = edgeList[b];

    if (ea.label != eb.label)
    {
      return ea.label < eb.label;
    }

    if (ea.origin != eb.origin)
    {
      return ea.origin < eb.origin;
    }

    return ea.index < eb.index;
  });

  std::sort(backwardOrder.begin(), backwardOrder.end(), [this](const uint32_t a, const uint32_t b) {
    const GraphEdge &ea = edgeList[a];
    const GraphEdge &eb = edgeList[b];

    if (ea.label != eb.label)
    {
      return ea.label < eb.label;
    }

    if (ea.target != eb.target)
    {
      return ea.target < eb.target;
    }

    return ea.index < eb.index;
  });
}

uint32_t LabelledGraph::vertexCount() const
{
  return vertices;
}

uint32_t LabelledGraph::edgeCount() const
{
  return static_cast<uint32_t>(edgeList.size());
}

const std::vector<GraphEdge> &LabelledGraph::edges() const
{
  return edgeList;
}

void LabelledGraph::forwardRange(const EdgeTypeId label, const uint32_t origin, uint32_t &first, uint32_t &last) const
{
  first = lowerBound(edgeList, forwardOrder, label, origin, true);
  last = first;

  while (last < forwardOrder.size())
  {
    const GraphEdge &edge = edgeList[forwardOrder[last]];

    if (edge.label != label || edge.origin != origin)
    {
      break;
    }

    ++last;
  }
}

void LabelledGraph::backwardRange(const EdgeTypeId label, const uint32_t target, uint32_t &first, uint32_t &last) const
{
  first = lowerBound(edgeList, backwardOrder, label, target, false);
  last = first;

  while (last < backwardOrder.size())
  {
    const GraphEdge &edge = edgeList[backwardOrder[last]];

    if (edge.label != label || edge.target != target)
    {
      break;
    }

    ++last;
  }
}

uint32_t LabelledGraph::forwardEdge(const uint32_t slot) const
{
  return forwardOrder[slot];
}

uint32_t LabelledGraph::backwardEdge(const uint32_t slot) const
{
  return backwardOrder[slot];
}

bool buildMatchOrder(const LabelledGraph &pattern, const uint32_t startVertex, std::vector<MatchStep> &out, uint32_t &columnCount)
{
  out.clear();

  const uint32_t edgeCount = pattern.edgeCount();

  if (edgeCount == 0u)
  {
    columnCount = 0u;

    return false;
  }

  std::vector<bool> visited(edgeCount, false);
  std::vector<uint32_t> column(pattern.vertexCount(), noColumn);

  uint32_t columns = 0u;

  // Seed with an edge touching the requested start vertex when there is one, so
  // that trying every start vertex actually explores different orders.
  uint32_t seed = 0u;

  for (uint32_t i = 0u; i < edgeCount; ++i)
  {
    const GraphEdge &edge = pattern.edges()[i];

    if (edge.origin == startVertex || edge.target == startVertex)
    {
      seed = i;

      break;
    }
  }

  {
    const GraphEdge &edge = pattern.edges()[seed];

    MatchStep first;

    column[edge.origin] = columns++;

    first.patternEdge = seed;
    first.label = edge.label;
    first.direction = StepDirection::Forward;
    first.fromColumn = column[edge.origin];

    if (column[edge.target] == noColumn)
    {
      column[edge.target] = columns++;
    }

    first.writeColumn = column[edge.target];

    // A self loop binds one vertex, not two.
    if (edge.origin == edge.target)
    {
      first.direction = StepDirection::Cyclic;
    }

    visited[seed] = true;
    out.push_back(first);
  }

  bool progressed = true;

  while (progressed)
  {
    progressed = false;

    for (uint32_t i = 0u; i < edgeCount; ++i)
    {
      if (visited[i])
      {
        continue;
      }

      const GraphEdge &edge = pattern.edges()[i];

      const bool knownOrigin = column[edge.origin] != noColumn;
      const bool knownTarget = column[edge.target] != noColumn;

      MatchStep next;

      next.patternEdge = i;
      next.label = edge.label;

      if (knownOrigin && knownTarget)
      {
        next.direction = StepDirection::Cyclic;
        next.fromColumn = column[edge.origin];
        next.writeColumn = column[edge.target];
      }
      else if (knownOrigin)
      {
        next.direction = StepDirection::Forward;
        next.fromColumn = column[edge.origin];
        column[edge.target] = columns++;
        next.writeColumn = column[edge.target];
      }
      else if (knownTarget)
      {
        next.direction = StepDirection::Backward;
        next.fromColumn = column[edge.target];
        column[edge.origin] = columns++;
        next.writeColumn = column[edge.origin];
      }
      else
      {
        continue;
      }

      visited[i] = true;
      out.push_back(next);
      progressed = true;
    }
  }

  columnCount = columns;

  for (const bool seen : visited)
  {
    if (!seen)
    {
      return false; // disconnected pattern
    }
  }

  return true;
}

TablePatternMatcher::TablePatternMatcher(const LabelledGraph &dataGraph, const LabelledGraph &patternGraph, const uint32_t startVertex)
    : data(dataGraph), pattern(patternGraph)
{
  orderValid = buildMatchOrder(pattern, startVertex, steps, columns);

  columnToVertex.assign(columns == 0u ? 1u : columns, noColumn);
  vertexToColumn.assign(pattern.vertexCount(), noColumn);
}

bool TablePatternMatcher::valid() const
{
  return orderValid;
}

uint32_t TablePatternMatcher::rowCount() const
{
  return rows;
}

uint32_t TablePatternMatcher::columnCount() const
{
  return step == 0u ? 0u : columns;
}

void TablePatternMatcher::initiate()
{
  table.clear();
  rows = 0u;
  step = 0u;

  if (!orderValid || steps.empty())
  {
    return;
  }

  const MatchStep &first = steps[0];
  const GraphEdge &patternEdge = pattern.edges()[first.patternEdge];

  vertexToColumn[patternEdge.origin] = first.fromColumn;
  columnToVertex[first.fromColumn] = patternEdge.origin;
  vertexToColumn[patternEdge.target] = first.writeColumn;
  columnToVertex[first.writeColumn] = patternEdge.target;

  const bool selfLoop = patternEdge.origin == patternEdge.target;

  for (const GraphEdge &edge : data.edges())
  {
    if (edge.label != first.label)
    {
      continue;
    }

    if (selfLoop && edge.origin != edge.target)
    {
      continue;
    }

    if (!selfLoop && edge.origin == edge.target)
    {
      continue; // injectivity: two pattern vertices cannot share a data vertex
    }

    table.resize(table.size() + columns, 0u);

    const size_t base = table.size() - columns;

    table[base + first.fromColumn] = edge.origin;
    table[base + first.writeColumn] = edge.target;

    ++rows;
  }

  step = 1u;
}

void TablePatternMatcher::expand()
{
  if (finished() || rows == 0u)
  {
    step = static_cast<uint32_t>(steps.size());

    return;
  }

  const MatchStep &current = steps[step];
  const GraphEdge &patternEdge = pattern.edges()[current.patternEdge];

  std::vector<uint32_t> next;

  next.reserve(table.size());

  const bool cyclic = current.direction == StepDirection::Cyclic;
  const bool forward = current.direction != StepDirection::Backward;

  for (uint32_t row = 0u; row < rows; ++row)
  {
    const uint32_t *cells = &table[static_cast<size_t>(row) * columns];
    const uint32_t bound = cells[current.fromColumn];

    uint32_t first = 0u;
    uint32_t last = 0u;

    if (forward)
    {
      data.forwardRange(current.label, bound, first, last);
    }
    else
    {
      data.backwardRange(current.label, bound, first, last);
    }

    for (uint32_t slot = first; slot < last; ++slot)
    {
      const GraphEdge &candidate = data.edges()[forward ? data.forwardEdge(slot) : data.backwardEdge(slot)];
      const uint32_t other = forward ? candidate.target : candidate.origin;

      if (cyclic)
      {
        // Filters only: both endpoints are already bound, so the candidate has
        // to agree with what the row already says.
        if (cells[current.writeColumn] != other)
        {
          continue;
        }
      }
      else
      {
        bool duplicate = false;

        for (uint32_t col = 0u; col < columns; ++col)
        {
          if (columnToVertex[col] != noColumn && cells[col] == other)
          {
            duplicate = true;

            break;
          }
        }

        if (duplicate)
        {
          continue; // injectivity
        }
      }

      next.insert(next.end(), cells, cells + columns);

      if (!cyclic)
      {
        next[next.size() - columns + current.writeColumn] = other;
      }
    }
  }

  if (!cyclic)
  {
    vertexToColumn[forward ? patternEdge.target : patternEdge.origin] = current.writeColumn;
    columnToVertex[current.writeColumn] = forward ? patternEdge.target : patternEdge.origin;
  }

  table.swap(next);
  rows = columns == 0u ? 0u : static_cast<uint32_t>(table.size() / columns);

  ++step;
}

void TablePatternMatcher::removeNonInjectiveRows()
{
  if (rows == 0u)
  {
    return;
  }

  std::vector<uint32_t> kept;

  kept.reserve(table.size());

  for (uint32_t row = 0u; row < rows; ++row)
  {
    const uint32_t *cells = &table[static_cast<size_t>(row) * columns];

    bool injective = true;

    for (uint32_t a = 0u; injective && a < columns; ++a)
    {
      if (columnToVertex[a] == noColumn)
      {
        continue;
      }

      for (uint32_t b = a + 1u; b < columns; ++b)
      {
        if (columnToVertex[b] == noColumn)
        {
          continue;
        }

        if (cells[a] == cells[b])
        {
          injective = false;

          break;
        }
      }
    }

    if (injective)
    {
      kept.insert(kept.end(), cells, cells + columns);
    }
  }

  table.swap(kept);
  rows = columns == 0u ? 0u : static_cast<uint32_t>(table.size() / columns);
}

bool TablePatternMatcher::finished() const
{
  return !orderValid || rows == 0u || step >= static_cast<uint32_t>(steps.size());
}

MatchTable TablePatternMatcher::result() const
{
  MatchTable out;

  out.cells = table;
  out.rows = rows;
  out.cols = columns;
  out.columnToVertex = columnToVertex;
  out.vertexToColumn = vertexToColumn;

  for (uint32_t i = 0u; i < step && i < static_cast<uint32_t>(steps.size()); ++i)
  {
    out.matchedEdges.push_back(steps[i].patternEdge);
  }

  return out;
}

} // namespace graphrewrite::compiler
