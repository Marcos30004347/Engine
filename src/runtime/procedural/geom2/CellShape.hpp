#pragma once

#include "procedural/geom2/CellGrid.hpp"

namespace procedural::geom2 {

// Measures on a set of cells.
//
// Everything the subdivision scores with, and `narrow` in particular: it is the
// one habitability primitive the whole generator is built on - corridor
// detection, room validation, lighting, door typing and where a stair may stand
// all derive from it - so it lives here once rather than as five thresholds in
// five places.

struct CellBounds
{
  uint32_t i0 = 0, j0 = 0, i1 = 0, j1 = 0; // Inclusive.

  uint32_t rows() const
  {
    return i1 >= i0 ? i1 - i0 + 1 : 0;
  }

  uint32_t columns() const
  {
    return j1 >= j0 ? j1 - j0 + 1 : 0;
  }

  bool empty() const
  {
    return rows() == 0 || columns() == 0;
  }
};

CellBounds bounds(const Board &);

// Cells over the area of the bounding box. One for a rectangle, and the number
// the per-floor setback maximises when it decides which room to give away.
double compactness(const Board &);

// Every cell of the area that cannot fit a two-by-two block of the area around
// it, computed for the whole board at once.
//
// Watabou asks this one cell at a time, eight membership tests each. Written as
// bit arithmetic it is a dozen word operations for the entire floor, which
// matters because it is asked at every level of the subdivision recursion and
// then again by every room validator.
Board narrow(const CellGrid &, const Board &);

// Whether the cell can be taken out without splitting the area in two.
bool removalKeepsConnected(const CellGrid &, const Board &, uint32_t cell);

// The largest axis-aligned rectangle of cells wholly inside the area.
//
// The right anchor for a label in an L-shaped room, and the reason it is here
// rather than in the renderer. Computed by the histogram stack rather than by
// trying every rectangle, so it costs the area and not its square.
bool largestRectangle(const CellGrid &, const Board &, CellBounds &out);

} // namespace procedural::geom2
