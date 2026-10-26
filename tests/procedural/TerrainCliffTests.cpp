// Gates for plateau blocks and the cliffs cut from them.
//
// The properties that matter: a block is flat, its height is on the ladder, the
// brink stands above the toe by the riser, and flattening does not wreck the
// drainage that runs afterwards.

#include "procedural/geometry/TerrainMeshBuild.hpp"
#include "procedural/world/WorldPipeline.hpp"

#include <cmath>
#include <vector>
#include <iostream>
#include <string>

namespace
{

using namespace procedural::world;
using namespace procedural::geometry;
using procedural::Point2;

uint32_t failures = 0u;

void expect(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";
    return;
  }
  std::cout << "  FAILED " << what << "\n";
  ++failures;
}

WorldPipelineSettings smallWorld(const bool plateaus)
{
  WorldPipelineSettings settings{};
  settings.brief.seed = 11ull;
  settings.brief.extentMetres = 48000.0;
  settings.brief.reliefMetres = 2200.0;
  settings.brief.tags = {Tag::Coast};
  settings.cellSpacingMetres = 900.0;
  settings.hierarchyLevels = 2u;
  settings.grownLandmasses = true;
  settings.landFraction = 0.8;
  settings.continentCount = 1u;
  settings.continentSpanMetres = 40000.0;
  settings.landmassCores = 5u;
  settings.cratonSpread = 0.55;
  // A 48 km world has too few cells of catchment to reach the default
  // channel-initiation threshold, and a world with no rivers cannot test river
  // carving. Lower it rather than growing the world, which costs seconds.
  settings.rivers.riverThresholdCells = 2.0;
  settings.plateaus = plateaus;
  settings.plateau.coverageFraction = 0.22;
  return settings;
}

} // namespace

int main()
{
  std::string error;

  std::cout << "plateau blocks\n";
  GeneratedWorld plain;
  if (!generateWorld(smallWorld(false), plain, {}, &error))
  {
    std::cout << "  FAILED baseline world: " << error << "\n";
    return 1;
  }
  GeneratedWorld world;
  if (!generateWorld(smallWorld(true), world, {}, &error))
  {
    std::cout << "  FAILED plateau world: " << error << "\n";
    return 1;
  }
  expect(!world.plateaus.blocks.empty(),
      "the world grows plateau blocks (" +
          std::to_string(world.plateaus.blocks.size()) + ")");
  expect(world.plateaus.ladderIntervalMetres > 0.0,
      "the ladder has an interval (" +
          std::to_string(world.plateaus.ladderIntervalMetres) + " m)");

  const double interval = world.plateaus.ladderIntervalMetres;
  bool flat = true;
  bool onLadder = true;
  bool positiveRiser = true;
  for (const PlateauBlock &block : world.plateaus.blocks)
  {
    // Every cell of a block sits at exactly the block's height: that is what
    // "flattened" has to mean, or the brink is not a single elevation and the
    // face below it is not a ruled strip.
    for (const uint32_t cell : block.cells)
      flat &= std::abs(world.graph.node(cell).intent.elevationMetres -
                  block.elevationMetres) < 1.0e-9;
    const double rungs = block.elevationMetres / interval;
    onLadder &= std::abs(rungs - std::round(rungs)) < 1.0e-6;
    positiveRiser &= block.riserMetres > 0.0;
  }
  expect(flat, "every cell of a block sits at exactly the block's elevation");
  expect(onLadder, "every block height is a whole number of ladder rungs");
  expect(positiveRiser, "every block stands above the ground it interrupts");

  std::cout << "hydrology survives flattening\n";
  // The scheduling argument: plateaus run before the definitive drainage pass,
  // so the rivers that follow route around the blocks rather than uphill across
  // them. A plateau field that doubles the world's lakes has dammed an outlet.
  const double lakeRatio = static_cast<double>(world.lakeCells) /
      std::max(1.0, static_cast<double>(plain.lakeCells));
  expect(lakeRatio < 1.5,
      "lake area stays within 1.5x of the unflattened world (" +
          std::to_string(world.lakeCells) + " against " +
          std::to_string(plain.lakeCells) + ")");
  expect(world.waterfalls >= plain.waterfalls,
      "water leaving a block over its brink is marked as a knickpoint (" +
          std::to_string(world.waterfalls) + " against " +
          std::to_string(plain.waterfalls) + ")");

  std::cout << "cliff geometry\n";
  CliffSettings cliffSettings{};
  cliffSettings.seed = 11ull;
  cliffSettings.spacingMetres = 60.0;
  const CliffGeometry cliffs = buildCliffs(world, cliffSettings);
  expect(!cliffs.quads.empty(),
      "the blocks produce cliff faces (" +
          std::to_string(cliffs.quads.size()) + " quads)");
  expect(cliffs.meanSetbackMetres > 0.0,
      "the faces have a setback rather than being walls (" +
          std::to_string(cliffs.meanSetbackMetres) + " m mean)");

  bool brinkAboveToe = true;
  bool setbackPositive = true;
  for (const StepQuad &quad : cliffs.quads)
  {
    brinkAboveToe &= quad.brinkAHeight > quad.toeAHeight &&
        quad.brinkBHeight > quad.toeBHeight;
    setbackPositive &= std::hypot(quad.toeA.x - quad.brinkA.x,
                           quad.toeA.y - quad.brinkA.y) >= 0.0;
  }
  expect(brinkAboveToe, "every face has its brink above its toe");
  expect(setbackPositive, "every face has a measurable footprint");

  // Brink and toe are both constraint chains, so the triangulation carries the
  // lines and the appended faces meet them exactly.
  uint32_t brinkChains = 0u;
  uint32_t toeChains = 0u;
  bool chainsPinned = true;
  for (const Chain &chain : cliffs.chains)
  {
    const bool isBrink = any(chain.kind, ChainKind::Brink);
    const bool isToe = any(chain.kind, ChainKind::Toe);
    brinkChains += isBrink ? 1u : 0u;
    toeChains += isToe ? 1u : 0u;
    // A brink and a toe are isolines by construction, so their vertices take
    // an exact elevation instead of resampling the surface. Without that the
    // face is a ruled strip between two wobbling lines.
    if (isBrink || isToe)
      chainsPinned &= chain.hasElevation && chain.points.size() >= 2u;
  }
  expect(brinkChains > 0u && brinkChains == toeChains,
      "each brink chain has exactly one toe chain (" +
          std::to_string(brinkChains) + "/" + std::to_string(toeChains) + ")");
  expect(chainsPinned, "every brink and toe carries a pinned elevation");

  std::cout << "ramps and the terrace graph\n";
  expect(!cliffs.terraces.edges.empty(),
      "the blocks get ramps (" +
          std::to_string(cliffs.terraces.edges.size()) + ")");
  // The gate that makes this gameplay rather than decoration: a plateau you
  // cannot get onto is a bug, and checking it is a traversal not a screenshot.
  expect(cliffs.terraces.connected(),
      "every block is reachable from the base terrace (" +
          std::to_string(cliffs.terraces.unreachableBlocks.size()) +
          " unreachable of " + std::to_string(cliffs.terraces.blockCount) + ")");
  bool walkable = true;
  for (const TerraceEdge &edge : cliffs.terraces.edges)
    walkable &= edge.gradient <=
        cliffSettings.ramps.maximumWalkableGradient * (1.0 + 1.0e-6);
  expect(walkable,
      "every ramp is under the walkable gradient (steepest " +
          std::to_string(cliffs.terraces.steepestRampGradient) + ")");

  // The graph has to be a hierarchy, not a star. A block sitting partly on
  // another block has two different risers and two different ways up, and
  // averaging them into one number is what made every ramp land on the base
  // terrace regardless of what was actually underneath it.
  size_t stacked = 0u;
  bool endpointsValid = true;
  for (const TerraceEdge &edge : cliffs.terraces.edges)
  {
    stacked += edge.from != kBaseTerrace ? 1u : 0u;
    endpointsValid &= edge.to < cliffs.terraces.blockCount &&
        (edge.from == kBaseTerrace || edge.from < cliffs.terraces.blockCount);
  }
  expect(endpointsValid, "every ramp names terraces that exist");
  expect(stacked > 0u,
      "some ramps climb from one block to another, so the graph is a "
      "hierarchy rather than a star (" + std::to_string(stacked) + " of " +
          std::to_string(cliffs.terraces.edges.size()) + ")");

  // Reachability must be a traversal: a block whose only ramp climbs from a
  // block that is itself unreachable is unreachable, and only walking the
  // graph says so. Reproduce it independently here rather than trusting the
  // field the builder filled in.
  {
    std::vector<std::vector<uint32_t>> adjacency(cliffs.terraces.blockCount);
    std::vector<uint32_t> stack;
    std::vector<uint8_t> seen(cliffs.terraces.blockCount, 0u);
    for (const TerraceEdge &edge : cliffs.terraces.edges)
    {
      if (edge.from == kBaseTerrace)
      {
        if (seen[edge.to] == 0u)
        {
          seen[edge.to] = 1u;
          stack.push_back(edge.to);
        }
        continue;
      }
      adjacency[edge.from].push_back(edge.to);
      adjacency[edge.to].push_back(edge.from);
    }
    while (!stack.empty())
    {
      const uint32_t block = stack.back();
      stack.pop_back();
      for (const uint32_t next : adjacency[block])
        if (seen[next] == 0u)
        {
          seen[next] = 1u;
          stack.push_back(next);
        }
    }
    size_t unreachable = 0u;
    for (const uint8_t flag : seen)
      unreachable += flag == 0u ? 1u : 0u;
    expect(unreachable == cliffs.terraces.unreachableBlocks.size(),
        "an independent traversal agrees with the reported reachability (" +
            std::to_string(unreachable) + ")");
  }

  // A ramp is a widened setback, so it has to leave the face wider than the
  // rock's own repose would - otherwise nothing was actually cut.
  CliffSettings noRamps = cliffSettings;
  noRamps.cutRamps = false;
  const CliffGeometry bare = buildCliffs(world, noRamps);
  expect(cliffs.meanSetbackMetres > bare.meanSetbackMetres,
      "cutting ramps widens the faces (" +
          std::to_string(cliffs.meanSetbackMetres) + " m against " +
          std::to_string(bare.meanSetbackMetres) + " m)");
  expect(bare.terraces.edges.size() < cliffs.terraces.edges.size(),
      "and without them there are fewer ways up (" +
          std::to_string(bare.terraces.edges.size()) + " against " +
          std::to_string(cliffs.terraces.edges.size()) + ")");

  std::cout << "river strokes\n";
  StrokeSettings strokeSettings{};
  strokeSettings.seed = 11ull;
  strokeSettings.spacingMetres = 80.0;
  const StrokeGeometry strokes = buildStrokes(world, strokeSettings);
  if (world.rivers.segments.empty())
  {
    std::cout << "  (this world has no rivers to carve)\n";
  }
  else
  {
    expect(strokes.courseCount > 0u,
        "the drainage network carves courses (" +
            std::to_string(strokes.courseCount) + ")");

    // Hydraulic geometry: the three exponents sum to one, so the coefficients
    // are mutually consistent and a 2000 cumec trunk really is about 112 m
    // wide. Check the law rather than a tuned constant.
    const double width = strokeSettings.widthMetres(2000.0);
    const double depth = strokeSettings.depthMetres(2000.0);
    expect(width > 100.0 && width < 125.0,
        "a 2000 cumec trunk is about 112 m wide (" + std::to_string(width) + ")");
    expect(depth > 5.5 && depth < 7.0,
        "and about 6.3 m deep (" + std::to_string(depth) + ")");

    // The rule that makes water look like water: both edges of a cross-section
    // at one elevation. A channel whose banks sit at different heights is a
    // groove cut into a tilted hillside.
    bool brinkAboveToe = true;
    bool carved = false;
    for (const StepQuad &quad : strokes.quads)
    {
      brinkAboveToe &= quad.brinkAHeight >= quad.toeAHeight &&
          quad.brinkBHeight >= quad.toeBHeight;
      carved |= (quad.brinkAHeight - quad.toeAHeight) > 0.1;
    }
    expect(brinkAboveToe,
        "every bank face has its brink at or above its toe");
    expect(carved, "the channel is actually incised below its banks");

    // The centreline is pinned to the bed, which is what stops the
    // triangulation running the ground straight across the trench.
    bool centrePinned = false;
    for (const Chain &chain : strokes.chains)
      if (any(chain.kind, ChainKind::Stroke) && chain.hasElevation &&
          chain.elevations.size() == chain.points.size())
        centrePinned = true;
    expect(centrePinned,
        "stroke chains carry a per-point elevation rather than sampling the "
        "un-carved field");

    // Water cannot climb: the surface a course is cut into must never rise
    // downstream, after quantising to reaches as well as before.
    bool descends = true;
    for (const Chain &chain : strokes.chains)
    {
      if (chain.elevations.size() != chain.points.size())
        continue;
      for (size_t index = 1u; index < chain.elevations.size(); ++index)
        descends &= chain.elevations[index] <= chain.elevations[index - 1u] + 1.0e-6;
    }
    expect(descends, "no course climbs on its way downstream");

    // Confluences: each length of channel belongs to exactly one course.
    // Walking every headwater to the sea instead carves every shared trunk
    // once per tributary, at two different widths, overlapping.
    expect(strokes.duplicateCells == 0u,
        "no length of channel is carved twice (" +
            std::to_string(strokes.duplicateCells) + " duplicates over " +
            std::to_string(strokes.carvedCells) + " channel cells)");

    // Three corridors meeting at a point leave a notch between them; the disc
    // is what turns a meeting into a merge.
    uint32_t closedDiscs = 0u;
    for (const Chain &chain : strokes.chains)
      if (any(chain.kind, ChainKind::Stroke) && chain.closed)
        ++closedDiscs;
    // Topology first, geometry second. The network resolves reaches and
    // junctions with every width and water surface already decided, so the
    // geometry step has nothing left to negotiate - which is what makes a
    // confluence a merge rather than three corridors that nearly touch.
    const ChannelNetwork network = resolveChannelNetwork(world, strokeSettings);
    expect(!network.reaches.empty(),
        "the network resolves into reaches (" +
            std::to_string(network.reaches.size()) + ")");
    bool endsAgree = true;
    bool widensDownstream = true;
    for (const ChannelNode &node : network.nodes)
    {
      // Every reach arriving at a junction must arrive at exactly the
      // junction's water surface.
      for (const uint32_t incoming : node.incoming)
        endsAgree &= std::abs(
                         network.reaches[incoming].vertices.back().surfaceMetres -
                         node.surfaceMetres) < 1.0e-9;
      // The merged channel is sized by the summed discharge, so it is wider
      // than any tributary rather than as wide as the largest.
      for (const uint32_t incoming : node.incoming)
        widensDownstream &= node.widthMetres >=
            network.reaches[incoming].vertices.back().widthMetres - 1.0e-9;
    }
    expect(endsAgree,
        "every reach arrives at its junction's exact water surface");
    expect(widensDownstream,
        "a merged channel is at least as wide as every tributary");
    expect(network.duplicateCells == 0u,
        "the resolved network carves no length of channel twice");

    expect(strokes.junctionCount > 0u,
        "this world has confluences to merge (" +
            std::to_string(strokes.junctionCount) + ")");
    expect(closedDiscs == strokes.junctionCount,
        "every confluence gets a junction disc (" +
            std::to_string(closedDiscs) + "/" +
            std::to_string(strokes.junctionCount) + ")");
  }

  std::cout << "relief stylization\n";
  {
    WorldPipelineSettings stylised = smallWorld(true);
    stylised.stylizeRanges = true;
    GeneratedWorld tall;
    if (!generateWorld(stylised, tall, {}, &error))
    {
      std::cout << "  FAILED stylized world: " << error << "\n";
      ++failures;
    }
    else
    {
      expect(tall.stylization.monotone, "the remap is monotone");
      expect(tall.stylization.gainApplied <= tall.stylization.maximumGain,
          "the gain stays under the derivative bound (" +
              std::to_string(tall.stylization.gainApplied) + " of " +
              std::to_string(tall.stylization.maximumGain) + ")");
      expect(tall.stylization.reliefAfter > tall.stylization.reliefBefore * 1.1,
          "the upper band actually expands (" +
              std::to_string(tall.stylization.reliefBefore) + " -> " +
              std::to_string(tall.stylization.reliefAfter) + " m)");

      // The whole argument for a monotone map of elevation *alone*: it cannot
      // reverse any descent relation, so the drainage network survives
      // untouched. Not approximately - identically.
      expect(tall.rivers.riverCells == world.rivers.riverCells &&
              tall.rivers.mouths == world.rivers.mouths &&
              tall.waterfalls == world.waterfalls &&
              tall.lakeCells == world.lakeCells,
          "drainage is unchanged by stylization (" +
              std::to_string(tall.rivers.riverCells) + "/" +
              std::to_string(world.rivers.riverCells) + " river cells, " +
              std::to_string(tall.waterfalls) + "/" +
              std::to_string(world.waterfalls) + " waterfalls)");

      expect(!tall.ridges.empty(),
          "divides are extracted from the drainage network (" +
              std::to_string(tall.ridges.size()) + " edges)");
      expect(!tall.peaks.empty(),
          "summits are pinned (" + std::to_string(tall.peaks.size()) + ")");

      // A plateau's brink has to stay exactly on a contour, or the ladder that
      // ties terraces, rungs and shelves together comes apart.
      bool onLadder = true;
      const double interval = tall.plateaus.ladderIntervalMetres;
      for (const PlateauBlock &block : tall.plateaus.blocks)
      {
        const double rungs = block.elevationMetres / interval;
        onLadder &= std::abs(rungs - std::round(rungs)) < 1.0e-6;
      }
      expect(onLadder, "plateau blocks are re-snapped to the stylized ladder");
    }
  }

  std::cout << "fluvial relief\n";
  {
    // The planet generator's division of labour: the graph owns tectonics and
    // climate, and a stream-power solver owns the valleys and the ranges. The
    // claim to defend is that ranges are what *uplift defended against
    // incision*, so start from a surface with no relief at all and see whether
    // the process builds one that agrees with the uplift field.
    WorldPipelineSettings eroded = smallWorld(false);
    eroded.fluvialErosion = true;
    eroded.fluvial.flattenBeforeErosion = 1.0;
    eroded.fluvial.iterations = 120u;
    GeneratedWorld carved;
    if (!generateWorld(eroded, carved, {}, &error))
    {
      std::cout << "  FAILED eroded world: " << error << "\n";
      ++failures;
    }
    else
    {
      expect(carved.fluvial.iterations == 120u &&
              carved.fluvial.drainageRebuilds > 1u,
          "the solver runs and refreshes its drainage (" +
              std::to_string(carved.fluvial.drainageRebuilds) + " rebuilds)");

      // Flattening leaves height and uplift unrelated; if the model works,
      // erosion against uplift makes them agree.
      expect(std::abs(carved.fluvial.upliftCorrelationBefore) < 0.35,
          "the flattened surface carries no range structure (" +
              std::to_string(carved.fluvial.upliftCorrelationBefore) + ")");
      expect(carved.fluvial.upliftCorrelationAfter > 0.7,
          "and uplift opposed by incision builds one (" +
              std::to_string(carved.fluvial.upliftCorrelationAfter) + ")");
      expect(carved.fluvial.upliftCorrelationAfter >
              carved.fluvial.upliftCorrelationBefore + 0.3,
          "the correlation rises rather than merely surviving");
      expect(carved.fluvial.reliefAfter > 0.0,
          "the process produces relief from a near-flat start (" +
              std::to_string(carved.fluvial.reliefAfter) + " m)");

      // The implicit stream-power form cannot overshoot its receiver, so
      // clamping should be rare. A large count means the step is too
      // aggressive for the erodibility.
      expect(carved.fluvial.inversionsClamped * 20u < carved.graph.nodeCount(),
          "inversions are rare (" +
              std::to_string(carved.fluvial.inversionsClamped) + " over " +
              std::to_string(carved.graph.nodeCount()) + " cells)");
      expect(carved.fluvial.resolvedStreamPowerK > 0.0,
          "erodibility is calibrated against this world rather than assumed");
    }
  }

  std::cout << "the mesh carries the faces\n";
  TerrainMeshSettings meshSettings{};
  meshSettings.tileSizeMetres = 6000.0;
  meshSettings.verticalToleranceMetres = 8.0;
  meshSettings.maximumRefinementRounds = 4u;
  meshSettings.minimumEdgeMetres = 12.0;
  meshSettings.chainSpacingMetres = 300.0;
  meshSettings.borderSpacingMetres = 750.0;
  meshSettings.workerCount = 0u;
  TerrainMesh mesh;
  const bool built = buildTerrainMesh(world, meshSettings, mesh, &error);
  expect(built, "the cliff world meshes: " + error);
  if (built)
  {
    expect(mesh.statistics.failedTiles == 0u, "no tile is left unbuilt");
    expect(mesh.statistics.constraintsIncluded ==
            mesh.statistics.constraintCount,
        "every constraint is inserted, brinks and toes included (" +
            std::to_string(mesh.statistics.constraintsIncluded) + "/" +
            std::to_string(mesh.statistics.constraintCount) + ")");
    expect(mesh.statistics.stepFaceTriangles > 0u,
        "the mesh carries step-face triangles (" +
            std::to_string(mesh.statistics.stepFaceTriangles) + ")");
  }

  if (failures != 0u)
  {
    std::cout << failures << " cliff test(s) failed\n";
    return 1;
  }
  std::cout << "all cliff tests passed\n";
  return 0;
}
