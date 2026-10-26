#pragma once

// A pattern's match table.
//
// COLUMN-MAJOR, because every hot access is one column across many rows: reading
// the bound vertex of each row, and sweeping a column during the injectivity
// test. Consecutive rows then sit at consecutive addresses. Row-major would
// stride each of those reads by the column count.
//
// Two strides exist and confusing them is a silent corruption:
//
//   packed   addr(c, r) = c * rows     + r      (a finished table)
//   unpacked addr(c, r) = c * capacity + r      (one still being written)
//
// A table being filled must use a fixed stride, because an instruction that
// resumes across frames appends rows and the final count is not known until it
// completes. Repacking to the tight stride happens once, at the end.
//
// The CPU table GROWS: reset() lays out only the first chunk of its capacity,
// and appendRow() doubles the stride (re-laying every column out) when that
// fills. Zeroing the full capacity per instruction was the cost of a large
// world - a capacity of millions of rows, times ~300 instructions a tick,
// every tick, for tables that mostly hold a few hundred rows. The stride is
// still fixed while any caller holds a row index; growth happens only inside
// appendRow(), between rows.

#include "graphrewrite/runtime/Edge.hpp"

#include <cstdint>
#include <vector>

namespace graphrewrite::runtime {

class MatchTable
{
public:
  void reset(uint32_t columns, uint32_t rowCapacity);

  uint32_t rows() const
  {
    return rowCount;
  }

  uint32_t columns() const
  {
    return columnCount;
  }

  uint32_t capacity() const
  {
    return rowCapacity;
  }

  uint32_t stride() const
  {
    return currentStride;
  }

  bool packed() const
  {
    return currentStride == rowCount;
  }

  uint32_t at(const uint32_t column, const uint32_t row) const
  {
    return cells[static_cast<size_t>(column) * currentStride + row];
  }

  void set(const uint32_t column, const uint32_t row, const uint32_t value)
  {
    cells[static_cast<size_t>(column) * currentStride + row] = value;
  }

  // Appends one row and returns its index, or `notResident` when the table is
  // full. Overflow is a real state the caller must handle, not an assert.
  uint32_t appendRow();

  void setRowCount(uint32_t count);

  // Rows the buffer is laid out for right now (<= capacity()).
  uint32_t laidOut() const
  {
    return currentStride;
  }

  // Rewrites every cell at the tight stride. Writes into `destination` rather
  // than in place: within a column, the write for row r1 lands on the source
  // address of an EARLIER row r2 = r1 - c * (capacity - rows), and a parallel
  // implementation has no ordering between that write and that read.
  void repackInto(MatchTable &destination) const;

  const std::vector<uint32_t> &raw() const
  {
    return cells;
  }

private:
  std::vector<uint32_t> cells;
  uint32_t columnCount = 0u;
  uint32_t rowCount = 0u;
  uint32_t rowCapacity = 0u;
  uint32_t currentStride = 0u;
};

} // namespace graphrewrite::runtime
