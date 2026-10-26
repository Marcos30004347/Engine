#include "graphrewrite/runtime/MatchTable.hpp"

namespace graphrewrite::runtime {

namespace {

// The first chunk laid out. Small tables - which is most of them - never grow.
constexpr uint32_t firstChunkRows = 1024u;

} // namespace

void MatchTable::reset(const uint32_t columns, const uint32_t capacity)
{
  columnCount = columns;
  rowCapacity = capacity;
  currentStride = capacity < firstChunkRows ? capacity : firstChunkRows;
  rowCount = 0u;

  cells.assign(static_cast<size_t>(columns) * currentStride, 0u);
}

uint32_t MatchTable::appendRow()
{
  if (rowCount >= rowCapacity)
  {
    return notResident;
  }

  if (rowCount >= currentStride)
  {
    // Double, capped at capacity, and re-lay every column at the new stride.
    // Columns are copied last-to-first so an in-place copy never reads a cell
    // it has already overwritten.
    const uint32_t stride = currentStride * 2u < rowCapacity ? currentStride * 2u : rowCapacity;

    cells.resize(static_cast<size_t>(columnCount) * stride, 0u);

    for (uint32_t column = columnCount; column-- > 1u;)
    {
      const size_t from = static_cast<size_t>(column) * currentStride;
      const size_t to = static_cast<size_t>(column) * stride;

      for (uint32_t row = currentStride; row-- > 0u;)
      {
        cells[to + row] = cells[from + row];
      }
    }

    currentStride = stride;
  }

  return rowCount++;
}

void MatchTable::setRowCount(const uint32_t count)
{
  // Callers that set a count directly get the full layout at once.
  const uint32_t rows = count < rowCapacity ? count : rowCapacity;

  if (rows > currentStride)
  {
    std::vector<uint32_t> grown(static_cast<size_t>(columnCount) * rowCapacity, 0u);

    for (uint32_t column = 0u; column < columnCount; ++column)
    {
      for (uint32_t row = 0u; row < rowCount && row < currentStride; ++row)
      {
        grown[static_cast<size_t>(column) * rowCapacity + row] = cells[static_cast<size_t>(column) * currentStride + row];
      }
    }

    cells.swap(grown);
    currentStride = rowCapacity;
  }

  rowCount = rows;
}

void MatchTable::repackInto(MatchTable &destination) const
{
  destination.columnCount = columnCount;
  destination.rowCount = rowCount;
  destination.rowCapacity = rowCount;
  destination.currentStride = rowCount;

  destination.cells.assign(static_cast<size_t>(columnCount) * rowCount, 0u);

  for (uint32_t column = 0u; column < columnCount; ++column)
  {
    for (uint32_t row = 0u; row < rowCount; ++row)
    {
      destination.cells[static_cast<size_t>(column) * rowCount + row] = cells[static_cast<size_t>(column) * currentStride + row];
    }
  }
}

} // namespace graphrewrite::runtime
