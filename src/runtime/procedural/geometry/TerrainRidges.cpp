#include "procedural/geometry/TerrainRidges.hpp"
#include "procedural/geometry/TerrainOrogeny.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace procedural::geometry
{
namespace
{
uint64_t hash(uint64_t x) { x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 27; x *= 0x94d049bb133111ebull; return x ^ (x >> 31); }
double random(uint64_t x) { return double(hash(x) >> 11) / double(uint64_t(1) << 53); }
double length(Point2 a, Point2 b) { return std::hypot(a.x-b.x, a.y-b.y); }
double smooth(double t) { t = std::clamp(t, 0.0, 1.0); return t*t*(3-2*t); }
}
static double ridgeProfile(Point2 p, const RidgeNode &a, const RidgeNode &b, bool uplift)
{
  const double dx=b.position.x-a.position.x, dy=b.position.y-a.position.y;
  const double span=std::hypot(dx,dy);
  if(span<1e-8)return 0;
  const double t=std::clamp(((p.x-a.position.x)*dx+(p.y-a.position.y)*dy)/(span*span),0.0,1.0);
  const double d=std::hypot(p.x-a.position.x-t*dx,p.y-a.position.y-t*dy);
  const double signedDistance=((p.x-a.position.x)*dy-(p.y-a.position.y)*dx)/span;
  // A hard side test creates a height jump along the infinite extension of
  // every segment. Fade asymmetry around end caps instead of creating seams.
  const double side=signedDistance/std::max(d,1.0);
  const double width=(a.widthMetres+(b.widthMetres-a.widthMetres)*t)*(1.1+.3*side);
  if(d>=width || width<=0)return 0;
  return (a.riseMetres+(b.riseMetres-a.riseMetres)*t)*(uplift ? std::pow(1-d*d/(width*width),3.0) : std::pow(1-d/width,1.65));
}
double ridgeProfileHeight(Point2 p,const RidgeNode &a,const RidgeNode &b){return ridgeProfile(p,a,b,false);}
double ridgeUpliftHeight(Point2 p,const RidgeNode &a,const RidgeNode &b){return ridgeProfile(p,a,b,true);}
MountainSkeleton buildMountainSkeleton(const world::MapGraph &graph,
    const world::MapEdgeSet &edges, const std::function<double(Point2)> &height,
    const std::function<double(Point2)> &relief, double seaLevel, uint64_t seed)
{
  // Rivers first, then mountains - the ordering Genevaux et al. use, and the
  // reason this produces a range system rather than a range.
  //
  // Drawing one crest line through a landmass can only ever make one mountain,
  // however well that line is shaped: a quarter of the island was a single
  // ridge because a single ridge is what a single spine means. A real range
  // system is not a line, it is the complement of the drainage: every pair of
  // neighbouring catchments is separated by a divide, so the ridges are the
  // boundaries of the river network's own partition of the land, and there are
  // as many of them as there are catchments. That also settles their height
  // without a free parameter, because a divide stands above the rivers on
  // either side of it by the slope of the valley wall times the distance to
  // them - which is Genevaux's slope constraint, and which makes a ridge deep
  // inland tall and broad and one near the coast low and narrow, for the same
  // reason it does in the world.
  MountainSkeleton result;
  const size_t count = graph.nodeCount();
  std::vector<std::vector<uint32_t>> adjacency(count);
  std::vector<double> potential(count), elevation(count), amplitude(count);
  std::vector<uint8_t> eligible(count);
  const double extent = graph.brief().extentMetres;
  size_t coarse = 0, land = 0;
  for (const auto &node : graph.nodes())
  {
    if (node.refined()) continue;
    ++coarse;
    if (node.land && node.type != world::CellType::Lake) ++land;
  }
  const double landAreaSquareKm = coarse == 0 ? 0.0
      : double(land) / double(coarse) * extent * extent / 1e6;
  const double summitMetres = naturalSummitMetres(landAreaSquareKm);
  for (const auto &node : graph.nodes())
  {
    if (node.refined() || !node.land || node.type == world::CellType::Lake) continue;
    eligible[node.id] = 1;
    elevation[node.id] = height(node.site);
    amplitude[node.id] = relief(node.site);
    potential[node.id] = smooth((elevation[node.id]-seaLevel-150)/650) * (.3+.7*smooth((amplitude[node.id]-30)/200));
  }
  for (const auto &edge : edges.edges)
  {
    if (edge.cellA == world::kInvalidNode || edge.cellB == world::kInvalidNode ||
        !eligible[edge.cellA] || !eligible[edge.cellB]) continue;
    adjacency[edge.cellA].push_back(edge.cellB); adjacency[edge.cellB].push_back(edge.cellA);
  }
  for (const auto &node : graph.nodes())
    if (eligible[node.id]) for (const auto next : node.neighbours)
      if (next < count && eligible[next])
      { adjacency[node.id].push_back(next); adjacency[next].push_back(node.id); }
  for (auto &list : adjacency) { std::sort(list.begin(),list.end()); list.erase(std::unique(list.begin(),list.end()),list.end()); }

  // --- the river network ---------------------------------------------------
  // Steepest descent on the base field. A node with no lower neighbour, or one
  // that touches water, is an outlet; everything else drains to one neighbour,
  // so the land comes out partitioned into catchments by construction.
  std::vector<uint32_t> receiver(count, world::kInvalidNode), catchment(count, world::kInvalidNode);
  std::vector<uint32_t> order;
  order.reserve(count);
  for (uint32_t id = 0; id < count; ++id)
  {
    if (!eligible[id]) continue;
    order.push_back(id);
    bool coastal = false;
    double steepest = 0;
    for (const auto next : adjacency[id])
    {
      const double span = std::max(1.0, length(graph.node(id).site, graph.node(next).site));
      const double fall = (elevation[id] - elevation[next]) / span;
      if (fall > steepest) { steepest = fall; receiver[id] = next; }
    }
    // Touching the sea makes a node an outlet whatever its neighbours do.
    for (const auto next : graph.node(id).neighbours)
      if (next < count && !eligible[next]) coastal = true;
    if (coastal) receiver[id] = world::kInvalidNode;
  }
  std::sort(order.begin(), order.end(),
      [&](uint32_t a, uint32_t b) { return elevation[a] > elevation[b]; });
  // Catchment identity: follow the receivers down to an outlet.
  for (uint32_t id : order)
  {
    uint32_t at = id, guard = 0;
    while (receiver[at] != world::kInvalidNode && guard++ < count)
    {
      if (catchment[at] != world::kInvalidNode) break;
      at = receiver[at];
    }
    catchment[id] = catchment[at] != world::kInvalidNode ? catchment[at] : at;
  }
  // Upstream count, accumulated down the receiver tree in descending order of
  // elevation, so a node is counted before the one it drains into.
  std::vector<uint32_t> upstream(count, 1u);
  for (uint32_t id : order)
    if (receiver[id] != world::kInvalidNode) upstream[receiver[id]] += upstream[id];
  // A channel is a node carrying enough of its catchment to be a river rather
  // than a hillside. This is the network the ridges are measured against.
  //
  // How much of its catchment a node has to carry is what sets the spacing of
  // the whole range system: at four cells almost every hillside is a "river",
  // no divide is ever far from one, and the ridges come out as 600 m hills. A
  // channel has to mean a river the landscape is organised around, so the
  // threshold scales with how many cells the landmass has.
  std::vector<uint8_t> channel(count, 0u);
  const uint32_t channelMinimum = std::max<uint32_t>(24u, uint32_t(land / 12));
  for (uint32_t id = 0; id < count; ++id)
    if (eligible[id] && (upstream[id] >= channelMinimum || receiver[id] == world::kInvalidNode))
      channel[id] = 1u;

  // Distance from every node to the nearest channel, over the graph. This is
  // the half-width of the valley the node sits in, and therefore - through the
  // flank slope - the height the divide above it can stand.
  std::vector<double> toChannel(count, std::numeric_limits<double>::infinity());
  {
    using Entry = std::pair<double, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    for (uint32_t id = 0; id < count; ++id)
      if (channel[id]) { toChannel[id] = 0; queue.push({0, id}); }
    while (!queue.empty())
    {
      const auto [distance, id] = queue.top(); queue.pop();
      if (distance != toChannel[id]) continue;
      for (const auto next : adjacency[id])
      {
        const double step = distance + length(graph.node(id).site, graph.node(next).site);
        if (step < toChannel[next]) { toChannel[next] = step; queue.push({step, next}); }
      }
    }
  }

  // --- the divides ---------------------------------------------------------
  // Where two catchments meet, there is a ridge. Both sides of the boundary are
  // taken, so the crest has width rather than being a one-cell line.
  std::vector<uint8_t> onDivide(count, 0u);
  for (uint32_t id = 0; id < count; ++id)
  {
    if (!eligible[id]) continue;
    for (const auto next : adjacency[id])
      if (eligible[next] && catchment[id] != catchment[next])
      { onDivide[id] = 1u; onDivide[next] = 1u; }
  }
  // A divide that never gets far from a river is a field boundary, not a
  // mountain. Keeping them all would put a ridge between every pair of brooks.
  const double lowest = std::max(extent * .004, 300.0);
  for (uint32_t id = 0; id < count; ++id)
    if (onDivide[id] && toChannel[id] < lowest) onDivide[id] = 0u;

  // Divides have a hierarchy, and it is the drainage's hierarchy: the watershed
  // between two great basins is a range, and the one between two brooks in the
  // same valley is a bank between two fields. Rank each divide by the smaller
  // of the two catchments it separates - the smaller, because a ridge is only
  // as important as the lesser of the two countries it divides.
  //
  // Without this every divide on the map is a mountain, which is worse than
  // the single spine it replaced: 66% of the island came out steeper than a
  // village can be built on, and the map had no cities left on it at all.
  std::vector<uint32_t> catchmentSize(count, 0u);
  for (uint32_t id = 0; id < count; ++id)
    if (eligible[id] && catchment[id] < count) ++catchmentSize[catchment[id]];
  uint32_t largestCatchment = 1u;
  for (uint32_t id = 0; id < count; ++id) largestCatchment = std::max(largestCatchment, catchmentSize[id]);
  std::vector<uint32_t> divideRank(count, 0u);
  for (uint32_t id = 0; id < count; ++id)
  {
    if (!onDivide[id]) continue;
    for (const auto next : adjacency[id])
      if (eligible[next] && catchment[id] != catchment[next])
        divideRank[id] = std::max(divideRank[id],
            std::min(catchmentSize[catchment[id]], catchmentSize[catchment[next]]));
  }

  // The deepest any divide gets from a river. The slope constraint gives the
  // *relative* heights correctly - inland ridges above coastal ones - but it
  // says nothing about the absolute scale, which belongs to the landmass. So
  // the widest catchment on the map is the one that reaches the ceiling, and
  // every other divide is placed below it by its own distance.
  double deepestValley = 0;
  for (uint32_t id = 0; id < count; ++id)
    if (onDivide[id] && std::isfinite(toChannel[id]))
      deepestValley = std::max(deepestValley, toChannel[id]);

  std::vector<uint32_t> skeletonId(count, world::kInvalidNode);
  std::vector<uint8_t> visited(count, 0u);
  for (uint32_t root = 0; root < count; ++root)
  {
    if (!onDivide[root] || visited[root]) continue;
    // One connected run of divide, which is one range.
    std::vector<uint32_t> ridge{root}; visited[root] = 1;
    for (size_t i = 0; i < ridge.size(); ++i)
      for (const auto next : adjacency[ridge[i]])
        if (onDivide[next] && !visited[next]) { visited[next] = 1; ridge.push_back(next); }
    if (ridge.size() < 3u) continue;
    // Its extent, and how far along it each node lies - the crest profile is a
    // function of that, so the summits recur along the ridge rather than with
    // the node numbering.
    double highest = 0, reach = 0;
    uint32_t crown = ridge.front();
    for (const auto id : ridge)
      if (toChannel[id] > highest) { highest = toChannel[id]; crown = id; }
    for (const auto id : ridge)
      reach = std::max(reach, length(graph.node(crown).site, graph.node(id).site));
    if (reach < extent * .02) continue;
    double maximumPotential = 0, province = 0;
    size_t seams = 0;
    for (const auto id : ridge)
    {
      maximumPotential = std::max(maximumPotential, potential[id]);
      province += terrainSlopeProvince(graph.node(id).site, extent, seed);
      for (const auto next : adjacency[id])
        if (onDivide[next] && graph.node(id).craton != graph.node(next).craton) { ++seams; break; }
    }
    province /= double(ridge.size());
    const double seamFraction = double(seams) / double(ridge.size());
    const double spineLength = reach * 2.0;
    // What kind of country this is comes from the province first and the
    // geology second. A divide in a low-lambda province is a bank between two
    // valleys however long it is; the same divide in a high-lambda province is
    // a range. This is what puts several different mountains on one map
    // instead of one mountain everywhere the divides happen to run.
    OrogenyKind chosen = OrogenyKind::Massif;
    const OrogenyKind candidates[] = {
        province > .62 ? (seamFraction > .14 ? OrogenyKind::Cordillera : OrogenyKind::Volcanic)
                       : OrogenyKind::FoldBelt,
        province > .62 ? (seamFraction > .14 ? OrogenyKind::Volcanic : OrogenyKind::Cordillera)
                       : OrogenyKind::Massif,
        OrogenyKind::FoldBelt, OrogenyKind::Massif};
    for (const auto candidate : candidates)
    {
      const auto trial = makeOrogeny(candidate);
      if (trial->admits(maximumPotential, spineLength, extent)) { chosen = candidate; break; }
    }
    const auto orogeny = makeOrogeny(chosen);
    ++result.rangeCount;
    result.rangeKinds.push_back(uint8_t(chosen));
    const uint64_t ridgeSeed = seed ^ (uint64_t(crown + 1u) * 0x9E3779B97F4A7C15ull);
    for (const auto id : ridge)
    {
      // Genevaux's slope constraint: the crest stands above the rivers beside
      // it by the valley-wall slope times the distance to them. The landmass
      // ceiling still applies, because a small island cannot carry an Alp
      // however wide its catchments are.
      const double valley = toChannel[id];
      const double along = length(graph.node(crown).site, graph.node(id).site) + reach * .5;
      const double shape = orogeny->crestVariation(along, spineLength, ridgeSeed);
      // Lambda here, not for the ridge as a whole: a range rises out of its
      // foreland, so the same crest stands tall where it crosses a mountainous
      // province and dies where it leaves one.
      // The province decides whether this is mountain country; it must not also
      // be charged against the slope, or the two multiply and the range comes
      // out a third of the height the same province already asked for.
      const double lambda = orogeny->profile().flankSlope
          * (.62 + .38 * terrainSlopeProvince(graph.node(id).site, extent, seed));
      const double ceiling = summitMetres * orogeny->profile().heightGain
          * (.30 + .70 * province);
      // Where this divide sits in the drainage hierarchy, and how far it stands
      // from the rivers it separates. The first decides whether it is a range
      // at all; the second shapes it along its length.
      const double importance = std::pow(
          std::clamp(double(divideRank[id]) / double(largestCatchment), 0.0, 1.0), .55);
      const double share = deepestValley > 1.0
          ? std::pow(std::clamp(valley / deepestValley, 0.0, 1.0), .5) : 0.0;
      // Held under the valley wall the ground could actually support, which is
      // Genevaux's slope constraint and the reason a ridge near the coast is
      // low however important the watershed behind it is.
      const double rise = std::min(ceiling * importance * (.35 + .65 * share), lambda * valley)
          * shape * (.62 + .38 * potential[id]);
      if (!(rise > 1.0)) continue;
      const auto index = uint32_t(result.nodes.size());
      skeletonId[id] = index;
      // The flank runs from the crest down to the river it drains to, so the
      // half-width is that distance - not a multiple of the height. Scaling
      // width with height instead is what merged every ridge into one massif.
      result.nodes.push_back({graph.node(id).site, id, rise,
          std::max(valley * .85, rise / std::max(.05, lambda) * .35)});
    }
    for (const auto id : ridge)
    {
      if (skeletonId[id] == world::kInvalidNode) continue;
      for (const auto next : adjacency[id])
        if (next > id && skeletonId[next] != world::kInvalidNode && onDivide[next])
          result.edges.push_back({skeletonId[id], skeletonId[next], 0});
    }
    // The passes: the lowest points along the crest are where a road crosses.
    uint32_t saddle = world::kInvalidNode; double lowestRise = 1e30;
    for (const auto id : ridge)
      if (skeletonId[id] != world::kInvalidNode && result.nodes[skeletonId[id]].riseMetres < lowestRise)
      { lowestRise = result.nodes[skeletonId[id]].riseMetres; saddle = skeletonId[id]; }
    if (saddle != world::kInvalidNode) result.passes.push_back(saddle);
  }
  // One normalisation at the end, so the tallest range on the map is the one
  // the landmass can support.
  //
  // Everything above this point decides *relative* height - which province is
  // mountainous, where a divide sits in the drainage hierarchy, how far it
  // stands from its rivers - and each of those is a factor below one, so they
  // multiply and the best range on the map ends up at a fraction of the
  // ceiling it was measured against. The island came out at 540 m where its
  // area supports 1,740. Scaling the finished skeleton keeps every one of
  // those relationships and fixes only the scale, which is the one thing none
  // of them was entitled to decide.
  double tallest = 0;
  for (const auto &node : result.nodes) tallest = std::max(tallest, node.riseMetres);
  if (tallest > 1.0)
  {
    double gain = 1.0;
    for (const auto kind : result.rangeKinds)
      gain = std::max(gain, orogenyProfile(OrogenyKind(kind)).heightGain);
    // Not the full ceiling: the tallest range is a range, not the theoretical
    // maximum the landmass could ever carry.
    const double target = summitMetres * gain * .85;
    // Bounded, because scaling rise and width together preserves the flank
    // slope but widens every mountain with it: an unbounded correction took the
    // upland from 15% of the island to 31% and left nowhere to put a city. If
    // the skeleton is far short of the target the answer is that the rivers are
    // too closely spaced - `channelMinimum` - not that the ridges want
    // stretching.
    const double scale = std::clamp(target / tallest, 1.0, 1.6);
    for (auto &node : result.nodes)
    { node.riseMetres *= scale; node.widthMetres *= scale; }
  }
  return result;
}
}
