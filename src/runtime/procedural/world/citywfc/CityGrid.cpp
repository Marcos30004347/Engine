#include "procedural/world/citywfc/CityGrid.hpp"

#include "procedural/geom2/Boolean.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace procedural::world::citywfc {
namespace {

using geom2::kNoSide;
using geom2::sideOf;
using geom2::toMetres;
using geom2::toUnit;

// A fault message with the offending cell's corners in it.
//
// `quadMeshFault` names a face index, which is the right answer for a library
// and useless in a pipeline: the index belongs to a mesh the caller no longer
// has by the time the message is read. Carrying the coordinates makes the
// difference between "face 13984 is self-intersecting" and a shape that can be
// looked at.
std::string describe(const geom2::QuadMesh &mesh, const std::string &fault)
{
  const size_t at = fault.find_first_of("0123456789");

  if (at == std::string::npos)
  {
    return fault;
  }

  const uint32_t face = uint32_t(std::stoul(fault.substr(at)));

  if (face >= mesh.faces())
  {
    return fault;
  }

  geom2::Point corner[4];
  mesh.corners(face, corner);

  std::string text = fault + " [";

  for (uint32_t i = 0; i < 4; ++i)
  {
    text += (i ? " " : "") + std::to_string(geom2::toMetres(corner[i].x)) + "," + std::to_string(geom2::toMetres(corner[i].y));
  }

  return text + "] area=" + std::to_string(mesh.areaMetres(face));
}

double percentile(std::vector<double> values, const double fraction)
{
  if (values.empty())
  {
    return 0;
  }

  std::sort(values.begin(), values.end());

  const double at = fraction * double(values.size() - 1);
  const size_t low = size_t(std::floor(at));
  const size_t high = std::min(low + 1, values.size() - 1);
  const double t = at - double(low);

  return values[low] * (1.0 - t) + values[high] * t;
}

double edgeLength(const geom2::Point &a, const geom2::Point &b)
{
  const double dx = toMetres(b.x - a.x), dy = toMetres(b.y - a.y);

  return std::sqrt(dx * dx + dy * dy);
}

// The smallest interior angle of a quad, in degrees. A measurement, not a
// construction: nothing downstream is built from it, so the transcendental here
// costs the determinism gate nothing.
double smallestAngle(const geom2::Point corner[4])
{
  double smallest = 360;

  for (uint32_t i = 0; i < 4; ++i)
  {
    const geom2::Point &here = corner[i];
    const geom2::Point &before = corner[(i + 3) % 4];
    const geom2::Point &after = corner[(i + 1) % 4];

    const double ax = toMetres(before.x - here.x), ay = toMetres(before.y - here.y);
    const double bx = toMetres(after.x - here.x), by = toMetres(after.y - here.y);

    const double dot = ax * bx + ay * by;
    const double cross = ax * by - ay * bx;
    const double angle = std::abs(std::atan2(cross, dot)) * 180.0 / 3.14159265358979323846;

    smallest = std::min(smallest, angle);
  }

  return smallest;
}

} // namespace

bool buildCityGrid(const std::vector<std::vector<Position>> &cells,
                   const CityGridSettings &settings,
                   CityGrid &out,
                   std::string *error)
{
  const auto fail = [&](const std::string &why) {
    if (error)
    {
      *error = why;
    }

    out = {};

    return false;
  };

  std::vector<geom2::Operand> operands;

  for (const std::vector<Position> &cell : cells)
  {
    if (cell.size() < 3)
    {
      continue;
    }

    geom2::Ring ring;
    ring.reserve(cell.size());

    // The plan plane is x-z, not x-y: y is elevation everywhere in this
    // generator, and a planning cell polygon leaves it at zero. Reading x and y
    // here would collapse every cell onto a line and the failure would look
    // like a lattice bug rather than a units bug.
    for (const Position &p : cell)
    {
      if (!geom2::representable(p.x) || !geom2::representable(p.z))
      {
        return fail("a planning cell vertex is outside the representable grid");
      }

      ring.push_back({toUnit(p.x), toUnit(p.z)});
    }

    if (!geom2::counterClockwise(ring))
    {
      geom2::reverse(ring);
    }

    if (!geom2::ringFault(ring).empty())
    {
      continue;
    }

    operands.push_back(geom2::toOperand(ring));
  }

  if (operands.empty())
  {
    return fail("no usable planning cell was supplied");
  }

  std::vector<geom2::Polygon> merged;
  std::string booleanError;

  if (!geom2::unionOf(operands, merged, &booleanError))
  {
    return fail("the planning cells would not union: " + booleanError);
  }

  if (merged.empty())
  {
    return fail("the planning cells union to nothing");
  }

  // The largest component is the city; the rest are recorded and left out,
  // because a grid over two disjoint pieces is two grids.
  size_t largest = 0;

  for (size_t i = 1; i < merged.size(); ++i)
  {
    if (geom2::areaMetres(merged[i]) > geom2::areaMetres(merged[largest]))
    {
      largest = i;
    }
  }

  const bool built = buildCityGridFromRegion(merged[largest], settings, out, error);

  if (built && merged.size() > 1)
  {
    out.ledger.push_back("omitted " + std::to_string(merged.size() - 1) + " planning component(s) disjoint from the largest");
  }

  return built;
}

bool buildCityGridFromRegion(const geom2::Polygon &region,
                             const CityGridSettings &settings,
                             CityGrid &out,
                             std::string *error)
{
  out = {};
  out.region = region;

  const auto fail = [&](const std::string &why) {
    if (error)
    {
      *error = why;
    }

    out = {};

    return false;
  };

  const std::string regionFault = geom2::polygonFault(region);

  if (!regionFault.empty())
  {
    return fail("the city region is not a sound polygon: " + regionFault);
  }

  if (settings.outlineSimplifyFraction > 0)
  {
    const size_t before = region.outer.size();
    size_t beforeHoles = 0;

    for (const geom2::Ring &hole : region.holes)
    {
      beforeHoles += hole.size();
    }

    geom2::simplifyPolygon(out.region,
                           settings.outlineSimplifyFraction * settings.lattice.targetCellEdgeMetres,
                           settings.outlineShortestEdgeFraction * settings.lattice.targetCellEdgeMetres);

    size_t afterHoles = 0;

    for (const geom2::Ring &hole : out.region.holes)
    {
      afterHoles += hole.size();
    }

    if (out.region.outer.size() != before || afterHoles != beforeHoles)
    {
      out.ledger.push_back("simplified the outline from " + std::to_string(before + beforeHoles) + " to " + std::to_string(out.region.outer.size() + afterHoles) + " vertices");
    }

    if (!geom2::polygonFault(out.region).empty())
    {
      out.region = region;
      out.ledger.push_back("kept the unsimplified outline: simplification broke it");
    }
  }

  const geom2::Polygon &outline = out.region;

  // 1. The lattice, as rhombi and leftover triangles.
  geom2::PolyMesh poly;
  std::vector<uint32_t> hexOfPoly;

  if (!buildCityLattice(outline, settings.lattice, poly, hexOfPoly, out.lattice, error))
  {
    return false;
  }

  // 2. Ortho subdivision: everything becomes a quad, whatever it was.
  std::vector<uint32_t> hexOfFace;

  for (size_t f = 0; f < poly.faces.size(); ++f)
  {
    for (size_t k = 0; k < poly.faces[f].size(); ++k)
    {
      hexOfFace.push_back(hexOfPoly[f]);
    }
  }

  if (!orthoSubdivide(poly, out.mesh, error))
  {
    return false;
  }

  if (out.mesh.faces() != hexOfFace.size())
  {
    return fail("the subdivision produced " + std::to_string(out.mesh.faces()) + " faces for " + std::to_string(hexOfFace.size()) + " provenance entries");
  }

  geom2::weld(out.mesh);

  {
    const std::string fault = geom2::quadMeshFault(out.mesh);

    if (!fault.empty())
    {
      return fail("the subdivided lattice is unsound: " + describe(out.mesh, fault));
    }
  }

  const double snapRadius = settings.snapRadiusFactor * settings.lattice.targetCellEdgeMetres;
  const double targetArea = settings.lattice.targetCellEdgeMetres * settings.lattice.targetCellEdgeMetres;
  const double minimumArea = settings.minimumAreaFraction * targetArea;

  // 3. Relax the whole lattice, halo included, with nothing holding its edge.
  //
  // Before any cutting, so that every cell which survives step 4 was relaxed
  // with neighbours on all four sides. Doing this after the cut is what makes
  // the outer row either slivers or parallelograms, whichever way the outline
  // constraint is written.
  {
    CityRelaxSettings free = settings.relax;
    free.snapRadiusMetres = 0;
    free.targetEdgeMetres = settings.lattice.targetCellEdgeMetres;

    if (!relaxQuadMesh(out.mesh, outline, free, out.relax, error))
    {
      return false;
    }

    const std::string fault = geom2::quadMeshFault(out.mesh);

    if (!fault.empty())
    {
      return fail("the free relaxation left the mesh unsound: " + describe(out.mesh, fault));
    }
  }

  // 4. Drop what is outside. A face is in the city when its centroid is.
  {
    std::vector<bool> keep(out.mesh.faces(), false);
    std::vector<uint32_t> keptHex;

    for (uint32_t f = 0; f < out.mesh.faces(); ++f)
    {
      keep[f] = geom2::contains(outline, out.mesh.centroid(f));

      if (keep[f])
      {
        keptHex.push_back(hexOfFace[f]);
      }
    }

    out.quality.droppedOutside = out.mesh.faces() - uint32_t(keptHex.size());

    if (keptHex.empty())
    {
      return fail("every lattice cell fell outside the city outline");
    }

    geom2::dropFaces(out.mesh, keep);
    hexOfFace.swap(keptHex);
  }

  // A face cut off from every neighbour is an island the relaxation cannot
  // help and a router could never reach.
  {
    const geom2::FaceAdjacency adjacency = geom2::faceAdjacency(out.mesh);
    std::vector<bool> keep(out.mesh.faces(), true);
    std::vector<uint32_t> keptHex;
    uint32_t islands = 0;

    for (uint32_t f = 0; f < out.mesh.faces(); ++f)
    {
      keep[f] = adjacency.offset[f + 1] > adjacency.offset[f];

      if (keep[f])
      {
        keptHex.push_back(hexOfFace[f]);
      }
      else
      {
        ++islands;
      }
    }

    if (islands != 0)
    {
      geom2::dropFaces(out.mesh, keep);
      hexOfFace.swap(keptHex);
      out.ledger.push_back("dropped " + std::to_string(islands) + " isolated cell(s) with no neighbour");
    }
  }


  // 5. Put the boundary on the outline.
  //
  // Locally, and only where a vertex is already close: a vertex further away
  // than the radius is a gap in the coverage, not a vertex to drag across the
  // city. The move guard inside the snap refuses anything that would fold a
  // cell, which is what makes a parameterisation of the outline unnecessary.
  {
    uint32_t refused = 0;
    snapBoundaryToRegion(out.mesh, outline, snapRadius, settings.settle.minimumAreaFraction * targetArea, &refused);

    if (refused != 0)
    {
      out.ledger.push_back(std::to_string(refused) + " boundary vertex move(s) refused by the fold guard");
    }
  }

  {
    const std::string fault = geom2::quadMeshFault(out.mesh);

    if (!fault.empty())
    {
      out.ledger.push_back("boundary fit left the mesh unsound: " + describe(out.mesh, fault));
    }
  }

  // 6. Settle the interior against the outline it now has.
  CityRelaxSettings settle = settings.settle;
  settle.targetEdgeMetres = settings.lattice.targetCellEdgeMetres;
  settle.snapRadiusMetres = snapRadius;

  CityRelaxReport settleReport;

  if (!relaxQuadMesh(out.mesh, outline, settle, settleReport, error))
  {
    return false;
  }

  out.settle = settleReport;

  {
    const std::string fault = geom2::quadMeshFault(out.mesh);

    if (!fault.empty())
    {
      return fail("the settling pass left the mesh unsound: " + describe(out.mesh, fault));
    }
  }

  // 7. Peel whatever the outline still leaves unusable, and settle again.
  //
  // Before the settling pass there is nothing to peel - the cells are only
  // squashed once they have been pulled onto the outline - so this has to come
  // after it rather than instead of it. Each pass can only remove cells the
  // previous pass made bad, which is why a small cap is enough.
  for (uint32_t pass = 0; settings.repairRim && pass < settings.rimRepairPasses; ++pass)
  {
    std::vector<bool> keep(out.mesh.faces(), true);
    std::vector<uint32_t> keptHex;
    uint32_t peeled = 0;

    for (uint32_t f = 0; f < out.mesh.faces(); ++f)
    {
      bool onRim = false;

      for (uint32_t i = 0; i < 4; ++i)
      {
        onRim = onRim || out.mesh.sides[sideOf(f, i)].twin == kNoSide;
      }

      if (onRim)
      {
        geom2::Point corner[4];
        out.mesh.corners(f, corner);

        keep[f] = out.mesh.areaMetres(f) >= minimumArea && smallestAngle(corner) >= settings.minimumAngleDegrees;
      }

      if (keep[f])
      {
        keptHex.push_back(hexOfFace[f]);
      }
      else
      {
        ++peeled;
      }
    }

    if (peeled == 0)
    {
      break;
    }

    if (keptHex.empty())
    {
      return fail("peeling the rim removed every cell");
    }

    geom2::dropFaces(out.mesh, keep);
    hexOfFace.swap(keptHex);
    out.quality.mergedRim += peeled;

    const geom2::FaceAdjacency adjacency = geom2::faceAdjacency(out.mesh);
    std::vector<bool> connected(out.mesh.faces(), true);
    std::vector<uint32_t> stillHex;
    uint32_t islands = 0;

    for (uint32_t f = 0; f < out.mesh.faces(); ++f)
    {
      connected[f] = adjacency.offset[f + 1] > adjacency.offset[f];

      if (connected[f])
      {
        stillHex.push_back(hexOfFace[f]);
      }
      else
      {
        ++islands;
      }
    }

    if (islands != 0)
    {
      geom2::dropFaces(out.mesh, connected);
      hexOfFace.swap(stillHex);
    }

    CityRelaxSettings again = settle;
    again.iterations = std::max(4u, settle.iterations / 2);

    CityRelaxReport ignored;

    if (!relaxQuadMesh(out.mesh, outline, again, ignored, error))
    {
      return false;
    }

    const std::string fault = geom2::quadMeshFault(out.mesh);

    if (!fault.empty())
    {
      return fail("the rim repair left the mesh unsound: " + describe(out.mesh, fault));
    }
  }

  if (out.quality.mergedRim != 0)
  {
    out.ledger.push_back("peeled " + std::to_string(out.quality.mergedRim) + " rim cell(s) below " + std::to_string(int(minimumArea)) + " m2 or " + std::to_string(int(settings.minimumAngleDegrees)) + " degrees");
  }

  // 5. Measure. Everything a caller could want to gate on, computed once.
  out.hexOfFace = hexOfFace;

  CityGridQuality &quality = out.quality;

  quality.faces = out.mesh.faces();
  quality.vertices = uint32_t(out.mesh.vertices.size());
  quality.triangles = out.lattice.triangles;
  quality.unpairedTriangles = out.lattice.unpaired;
  quality.targetEdgeMetres = settings.lattice.targetCellEdgeMetres;
  quality.targetAreaMetres = settings.lattice.targetCellEdgeMetres * settings.lattice.targetCellEdgeMetres;
  quality.regionAreaMetres = geom2::areaMetres(outline);
  quality.fitResidual = out.settle.finalResidual;
  quality.residualRatio = out.relax.initialResidual > 0 ? out.settle.finalResidual / out.relax.initialResidual : 0;

  std::vector<double> edges, areas, angles, aspects;
  std::vector<double> seamEdges, interiorEdges, seamAngles, interiorAngles;
  std::vector<double> rimAngles, coreAngles, rimAreas, coreAreas;

  quality.minEdge = std::numeric_limits<double>::max();
  quality.minInteriorAngle = 360;

  for (uint32_t f = 0; f < out.mesh.faces(); ++f)
  {
    geom2::Point corner[4];
    out.mesh.corners(f, corner);

    bool straddles = false;
    bool onRim = false;

    for (uint32_t i = 0; i < 4; ++i)
    {
      const uint32_t twin = out.mesh.sides[sideOf(f, i)].twin;

      onRim = onRim || twin == kNoSide;
      straddles = straddles || (twin != kNoSide && hexOfFace[geom2::faceOf(twin)] != hexOfFace[f]);
    }

    double shortest = std::numeric_limits<double>::max(), longest = 0;

    for (uint32_t i = 0; i < 4; ++i)
    {
      const double length = edgeLength(corner[i], corner[(i + 1) % 4]);
      shortest = std::min(shortest, length);
      longest = std::max(longest, length);

      // The rim is fitted to the outline and cannot also be square, so it is
      // kept out of the headline numbers and reported on its own.
      if (!onRim)
      {
        edges.push_back(length);
        quality.minEdge = std::min(quality.minEdge, length);
        quality.maxEdge = std::max(quality.maxEdge, length);
      }
    }

    const double area = out.mesh.areaMetres(f);
    quality.gridAreaMetres += area;

    const double angle = smallestAngle(corner);
    const double aspect = shortest > 0 ? longest / shortest : 0;

    if (!onRim)
    {
      areas.push_back(area);
      angles.push_back(angle);
      aspects.push_back(aspect);
      quality.minInteriorAngle = std::min(quality.minInteriorAngle, angle);
      quality.maxAspect = std::max(quality.maxAspect, aspect);
    }

    double mean = 0;

    for (uint32_t i = 0; i < 4; ++i)
    {
      mean += edgeLength(corner[i], corner[(i + 1) % 4]) * .25;
    }

    if (onRim)
    {
      ++quality.rimFaces;
      rimAngles.push_back(angle);
      rimAreas.push_back(area);
    }
    else
    {
      coreAngles.push_back(angle);
      coreAreas.push_back(area);

      // The seam comparison excludes the rim on purpose: whether a rim cell is
      // also a seam cell is an accident of where the patches fell, and letting
      // that noise into the seam metric hides the thing it is meant to catch.
      if (straddles)
      {
        ++quality.seamFaces;
        seamEdges.push_back(mean);
        seamAngles.push_back(angle);
      }
      else
      {
        interiorEdges.push_back(mean);
        interiorAngles.push_back(angle);
      }
    }
  }

  quality.p05Edge = percentile(edges, .05);
  quality.medianEdge = percentile(edges, .5);
  quality.p95Edge = percentile(edges, .95);
  quality.p05Area = percentile(areas, .05);
  quality.medianArea = percentile(areas, .5);
  quality.p95Area = percentile(areas, .95);
  quality.p05InteriorAngle = percentile(angles, .05);
  quality.p95Aspect = percentile(aspects, .95);

  quality.seamMedianEdge = percentile(seamEdges, .5);
  quality.interiorMedianEdge = percentile(interiorEdges, .5);
  quality.seamMedianAngle = percentile(seamAngles, .5);
  quality.interiorMedianAngle = percentile(interiorAngles, .5);

  quality.rimP05Angle = percentile(rimAngles, .05);
  quality.coreP05Angle = percentile(coreAngles, .05);
  quality.rimMinAngle = percentile(rimAngles, 0);
  quality.coreMinAngle = percentile(coreAngles, 0);
  quality.rimP05Area = percentile(rimAreas, .05);
  quality.coreP05Area = percentile(coreAreas, .05);

  quality.coverageRatio = quality.regionAreaMetres > 0 ? quality.gridAreaMetres / quality.regionAreaMetres : 0;

  // How far the mesh boundary strays from the outline it was fitted to.
  {
    const std::vector<std::vector<uint32_t>> cycles = geom2::boundaryCycles(out.mesh);

    std::vector<geom2::Ring> rings;
    rings.push_back(outline.outer);

    for (const geom2::Ring &hole : outline.holes)
    {
      rings.push_back(hole);
    }

    for (const std::vector<uint32_t> &cycle : cycles)
    {
      quality.boundaryVertices += uint32_t(cycle.size());

      for (const uint32_t v : cycle)
      {
        const double px = toMetres(out.mesh.vertices[v].x), py = toMetres(out.mesh.vertices[v].y);
        double best = std::numeric_limits<double>::max();

        for (const geom2::Ring &ring : rings)
        {
          for (size_t i = 0; i < ring.size(); ++i)
          {
            const geom2::Point &a = ring[i];
            const geom2::Point &b = ring[(i + 1) % ring.size()];

            const double ax = toMetres(a.x), ay = toMetres(a.y);
            const double dx = toMetres(b.x) - ax, dy = toMetres(b.y) - ay;
            const double denominator = dx * dx + dy * dy;

            double t = 0;

            if (denominator > 0)
            {
              t = std::clamp(((px - ax) * dx + (py - ay) * dy) / denominator, 0.0, 1.0);
            }

            const double ex = px - (ax + dx * t), ey = py - (ay + dy * t);
            best = std::min(best, ex * ex + ey * ey);
          }
        }

        quality.boundaryDeviationMetres = std::max(quality.boundaryDeviationMetres, std::sqrt(best));
      }
    }
  }

  return true;
}

} // namespace procedural::world::citywfc
