// Phase 3.8: column-major tables, and why repack cannot run in place.

#include "TestHarness.hpp"
#include "graphrewrite/runtime/MatchTable.hpp"

#include <map>
#include <random>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

void addressingUsesTheRightStride()
{
  MatchTable table;

  table.reset(3u, 10u);

  checkEqual(table.stride(), 10u, "a table being written uses its CAPACITY as the stride");
  checkEqual(table.columns(), 3u, "with the requested columns");
  checkEqual(table.rows(), 0u, "and no rows yet");

  for (uint32_t row = 0u; row < 4u; ++row)
  {
    checkEqual(table.appendRow(), row, "appendRow hands out consecutive rows");
  }

  checkEqual(table.rows(), 4u, "four rows appended");
  check(!table.packed(), "and the table is not packed while capacity exceeds rows");
}

void appendReportsOverflowRatherThanCorrupting()
{
  MatchTable table;

  table.reset(2u, 3u);

  for (uint32_t i = 0u; i < 3u; ++i)
  {
    check(table.appendRow() != notResident, "row " + std::to_string(i) + " fits");
  }

  checkEqual(table.appendRow(), notResident, "the fourth row reports overflow");
  checkEqual(table.rows(), 3u, "and the table is left at its capacity");
}

void repackRoundTripPreservesEveryCell()
{
  std::mt19937 rng(7u);

  uint32_t checked = 0u;

  for (uint32_t trial = 0u; trial < 300u; ++trial)
  {
    const uint32_t columns = 1u + rng() % 6u;
    const uint32_t rows = rng() % 40u;
    const uint32_t capacity = rows + rng() % 20u + 1u;

    MatchTable table;

    table.reset(columns, capacity);

    std::map<std::pair<uint32_t, uint32_t>, uint32_t> reference;

    for (uint32_t row = 0u; row < rows; ++row)
    {
      table.appendRow();
    }

    for (uint32_t column = 0u; column < columns; ++column)
    {
      for (uint32_t row = 0u; row < rows; ++row)
      {
        const uint32_t value = rng();

        table.set(column, row, value);
        reference[{column, row}] = value;
      }
    }

    MatchTable packed;

    table.repackInto(packed);

    if (packed.stride() != rows)
    {
      check(false, "packed stride equals the row count");

      return;
    }

    if (packed.raw().size() != static_cast<size_t>(columns) * rows)
    {
      check(false, "packed table is tight");

      return;
    }

    for (const auto &entry : reference)
    {
      if (packed.at(entry.first.first, entry.first.second) != entry.second)
      {
        check(false, "repack preserved every cell");

        return;
      }

      ++checked;
    }
  }

  check(checked > 10000u, "many cells were checked");
  check(true, "repack round-trips over 300 random tables");
}

void inPlaceRepackWouldAlias()
{
  // Proof, not opinion. `writeRows <= writeCapacity` makes every destination
  // offset <= its source offset, which makes in-place look safe. It is not:
  // within a column, the write for row r1 lands on the SOURCE address of an
  // earlier row r2 = r1 - c * (capacity - rows), and a parallel dispatch has no
  // ordering between that write and that read.
  uint32_t aliasingPairs = 0u;

  for (uint32_t capacity = 1u; capacity <= 40u; ++capacity)
  {
    for (uint32_t rows = 0u; rows <= capacity; ++rows)
    {
      for (uint32_t column = 0u; column < 6u; ++column)
      {
        for (uint32_t r1 = 0u; r1 < rows; ++r1)
        {
          const uint32_t destination = column * rows + r1;

          // Is that the source address of a DIFFERENT row in the same column?
          if (destination < column * capacity)
          {
            continue;
          }

          const uint32_t r2 = destination - column * capacity;

          if (r2 < rows && r2 != r1)
          {
            ++aliasingPairs;
          }
        }
      }
    }
  }

  check(aliasingPairs > 10000u, "in-place repack aliases in thousands of shapes - it must write to a separate destination");

  std::cout << "    " << aliasingPairs << " aliasing write/read pairs for capacity <= 40\n";
}

void repackOfAnEmptyTableIsWellFormed()
{
  MatchTable table;

  table.reset(4u, 10u);

  MatchTable packed;

  table.repackInto(packed);

  checkEqual(packed.rows(), 0u, "an empty table packs to zero rows");
  checkEqual(packed.raw().size(), size_t(0), "and an empty buffer");
  checkEqual(packed.columns(), 4u, "keeping its column count");
}

void packedTableReadsBackAtTheTightStride()
{
  MatchTable table;

  table.reset(2u, 8u);

  for (uint32_t row = 0u; row < 3u; ++row)
  {
    table.appendRow();

    table.set(0u, row, 100u + row);
    table.set(1u, row, 200u + row);
  }

  MatchTable packed;

  table.repackInto(packed);

  check(packed.packed(), "the destination is packed");

  for (uint32_t row = 0u; row < 3u; ++row)
  {
    checkEqual(packed.at(0u, row), 100u + row, "column 0 row " + std::to_string(row));
    checkEqual(packed.at(1u, row), 200u + row, "column 1 row " + std::to_string(row));
  }

  // And the underlying layout really is column-major: column 1 starts at
  // offset rows, not at offset 1.
  checkEqual(packed.raw()[0], 100u, "column 0 begins the buffer");
  checkEqual(packed.raw()[3], 200u, "column 1 begins at offset `rows`, which is what makes a column read coalesce");
}

} // namespace

int main()
{
  addressingUsesTheRightStride();
  appendReportsOverflowRatherThanCorrupting();
  repackRoundTripPreservesEveryCell();
  inPlaceRepackWouldAlias();
  repackOfAnEmptyTableIsWellFormed();
  packedTableReadsBackAtTheTightStride();

  return report("GraphRewriteTableTests");
}
