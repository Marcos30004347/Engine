#include "procedural/world/hierarchy/Composer.hpp"

#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Offset.hpp"
#include "procedural/world/WorldGenerationContracts.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::hierarchy {

namespace {

bool fail(std::string *error, const std::string &text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

uint64_t hashOf(const std::string &text)
{
  uint64_t value = 1469598103934665603ull;

  for (const char c : text)
  {
    value ^= uint64_t(uint8_t(c));
    value *= 1099511628211ull;
  }

  return value;
}

} // namespace

const char *coverName(const Cover cover)
{
  switch (cover)
  {
  case Cover::Open:
    return "open";
  case Cover::Forest:
    return "forest";
  case Cover::Rock:
    return "rock";
  case Cover::Water:
    return "water";
  case Cover::Urban:
    return "urban";
  case Cover::Road:
    return "road";
  case Cover::Building:
    return "building";
  }

  return "open";
}

const char *edgeRoleName(const EdgeRole role)
{
  switch (role)
  {
  case EdgeRole::RegionBoundary:
    return "region-boundary";
  case EdgeRole::Road:
    return "road";
  case EdgeRole::River:
    return "river";
  case EdgeRole::Coast:
    return "coast";
  case EdgeRole::Wall:
    return "wall";
  case EdgeRole::Rail:
    return "rail";
  case EdgeRole::Administrative:
    return "administrative";
  case EdgeRole::LotBoundary:
    return "lot-boundary";
  }

  return "region-boundary";
}

const char *roleName(const Role role)
{
  switch (role)
  {
  case Role::WorldPartition:
    return "world-partition";
  case Role::SettlementSites:
    return "settlement-sites";
  case Role::Districts:
    return "districts";
  case Role::Streets:
    return "streets";
  case Role::Blocks:
    return "blocks";
  case Role::Lots:
    return "lots";
  case Role::Buildings:
    return "buildings";
  case Role::Fields:
    return "fields";
  case Role::Interiors:
    return "interiors";
  case Role::Terrain:
    return "terrain";
  }

  return "lots";
}

double Edge::halfWidthMetres() const
{
  double widest = 0;

  for (const auto &band : bands)
  {
    widest = std::max(widest, std::max(std::abs(band.fromMetres), std::abs(band.toMetres)));
  }

  return widest;
}

bool Edge::hasRole(const EdgeRole role) const
{
  for (const auto &entry : roles)
  {
    if (entry.role == role)
    {
      return true;
    }
  }

  return false;
}

double Region::areaMetres() const
{
  return geom2::areaMetres(boundary);
}

Region makeRegion(const RegionId id, const RegionId parent, Polygon boundary, const Frame &frame)
{
  Region region;
  region.id = id;
  region.parent = parent;
  region.frame = frame;
  region.convex = boundary.holes.empty() && geom2::convex(boundary.outer);
  region.boundary = std::move(boundary);

  return region;
}

RegionId RegionTree::add(Region region)
{
  const RegionId id = region.id;
  regions_.push_back(std::move(region));

  return id;
}

const Region *RegionTree::find(const RegionId id) const
{
  for (const auto &region : regions_)
  {
    if (region.id == id)
    {
      return &region;
    }
  }

  return nullptr;
}

Region *RegionTree::findMutable(const RegionId id)
{
  for (auto &region : regions_)
  {
    if (region.id == id)
    {
      return &region;
    }
  }

  return nullptr;
}

std::vector<RegionId> RegionTree::childrenOf(const RegionId id) const
{
  std::vector<RegionId> out;

  for (const auto &region : regions_)
  {
    if (region.parent == id)
    {
      out.push_back(region.id);
    }
  }

  return out;
}

EdgeId RegionTree::addEdge(Edge edge)
{
  const EdgeId id = edge.id;
  edges_.push_back(std::move(edge));

  return id;
}

const Edge *RegionTree::findEdge(const EdgeId id) const
{
  for (const auto &edge : edges_)
  {
    if (edge.id == id)
    {
      return &edge;
    }
  }

  return nullptr;
}

double Context::unitRandom(const std::string &generator, const RegionId region, const uint64_t purpose, const uint64_t sample) const
{
  // Purpose-addressed, through the same feature-seed rule the rest of the
  // world generator uses. Keyed by what the draw is *for* rather than by when
  // it happened, so shuffling the work list cannot change the result.
  const uint64_t seed = rewrite::featureSeed(worldSeed, hashOf(generator), region, purpose, sample);

  return double(seed >> 11) / double(1ull << 53);
}

bool Registry::bind(GeneratorPtr generator, std::string *error)
{
  if (!generator)
  {
    return fail(error, "cannot bind a null generator");
  }

  const Descriptor descriptor = generator->descriptor();

  if (descriptor.id.empty())
  {
    return fail(error, "a generator needs a stable id");
  }

  if (byId_.count(descriptor.id) != 0)
  {
    return fail(error, "generator already bound: " + descriptor.id);
  }

  byRole_[descriptor.role].push_back(descriptor.id);
  byId_.emplace(descriptor.id, std::move(generator));

  return true;
}

GeneratorBase *Registry::find(const std::string &id) const
{
  const auto it = byId_.find(id);

  return it == byId_.end() ? nullptr : it->second.get();
}

std::vector<std::string> Registry::boundTo(const Role role) const
{
  const auto it = byRole_.find(role);

  return it == byRole_.end() ? std::vector<std::string>{} : it->second;
}

GeneratorBase *Registry::choose(const Role role, const Region &region, const Policy &policy, std::string *because) const
{
  for (const auto &rule : policy.rules)
  {
    if (rule.role != role || (rule.when && !rule.when(region)))
    {
      continue;
    }

    GeneratorBase *chosen = find(rule.generator);

    if (chosen != nullptr)
    {
      if (because != nullptr)
      {
        *because = rule.because.empty() ? rule.generator : rule.because;
      }

      return chosen;
    }
  }

  const auto fallback = policy.defaults.find(role);

  if (fallback != policy.defaults.end())
  {
    if (because != nullptr)
    {
      *because = "default";
    }

    return find(fallback->second);
  }

  return nullptr;
}

bool Composer::subdivide(const Region &parent, const std::vector<Edge> &edges, RegionTree &tree, const std::string &producer, std::vector<RegionId> &children, Ledger &ledger, std::string *error)
{
  children.clear();

  // Widthed edges become ribbons of their own. A road is not a line through a
  // district; it is a strip of ground that the district no longer holds, and
  // making it a region rather than a hole is what lets anything stand on it.
  std::vector<geom2::Operand> operands{geom2::toOperand(parent.boundary)};
  std::vector<size_t> ribbonOfOperand;
  std::vector<geom2::InputRing> rings;

  for (const auto &ring : geom2::toOperand(parent.boundary).rings)
  {
    rings.push_back({ring, 0, false});
  }

  uint32_t nextInput = 1;
  std::vector<std::pair<uint32_t, const Edge *>> ribbons;

  for (const auto &edge : edges)
  {
    if (edge.path.size() < 2)
    {
      ledger.add(LedgerKind::Rejected, parent.id, producer + ": edge with fewer than two points");

      continue;
    }

    const double halfWidth = edge.halfWidthMetres();

    if (halfWidth <= 0.0)
    {
      // A zero-width edge cuts without consuming. It enters as an open path so
      // that it divides the faces either side even though it winds nothing.
      rings.push_back({edge.path, nextInput, true});

      continue;
    }

    std::vector<Polygon> ribbon;

    if (!geom2::offsetPolyline(edge.path, halfWidth, {}, ribbon, error))
    {
      return false;
    }

    for (const auto &polygon : ribbon)
    {
      for (const auto &ring : geom2::toOperand(polygon).rings)
      {
        rings.push_back({ring, nextInput, false});
      }
    }

    ribbons.emplace_back(nextInput, &edge);
    ++nextInput;
  }

  geom2::Arrangement arrangement;

  if (!geom2::buildArrangement(rings, {}, arrangement, error))
  {
    return false;
  }

  // The ribbons first, each its own child with the edge's cover.
  //
  // A face covered by several ribbons - a crossroads - goes to the first that
  // claims it, so the children remain an exact tiling of the parent rather
  // than double-counting every junction. Making a junction a region in its own
  // right is the natural refinement; claiming it twice is simply wrong, and
  // the area-conservation test is what says so.
  for (const auto &[input, edge] : ribbons)
  {
    std::vector<Polygon> pieces;

    if (!geom2::extractRegions(
            arrangement,
            [input = input](const std::vector<int32_t> &winding) {
              if (winding[0] == 0 || winding[input] == 0)
              {
                return false;
              }

              for (uint32_t earlier = 1; earlier < input; ++earlier)
              {
                if (winding[earlier] != 0)
                {
                  return false;
                }
              }

              return true;
            },
            pieces,
            error))
    {
      return false;
    }

    for (auto &piece : pieces)
    {
      Region child = makeRegion(tree.nextRegionId(), parent.id, std::move(piece), parent.frame);
      child.cover = edge->bands.empty() ? Cover::Road : edge->bands.front().cover;
      child.attributes = parent.attributes;
      child.producer = producer;
      child.sources.push_back(parent.id);

      children.push_back(tree.add(std::move(child)));
    }
  }

  // Then the land the ribbons left behind.
  std::vector<Polygon> remainder;

  if (!geom2::extractRegions(
          arrangement,
          [count = nextInput](const std::vector<int32_t> &winding) {
            if (winding[0] == 0)
            {
              return false;
            }

            for (uint32_t i = 1; i < count; ++i)
            {
              if (winding[i] != 0)
              {
                return false;
              }
            }

            return true;
          },
          remainder,
          error))
  {
    return false;
  }

  for (auto &piece : remainder)
  {
    Region child = makeRegion(tree.nextRegionId(), parent.id, std::move(piece), parent.frame);
    child.cover = parent.cover;
    child.attributes = parent.attributes;
    child.producer = producer;
    child.sources.push_back(parent.id);

    children.push_back(tree.add(std::move(child)));
  }

  for (const auto &edge : edges)
  {
    Edge stored = edge;
    stored.id = tree.nextEdgeId();
    tree.addEdge(std::move(stored));
  }

  return true;
}

bool Composer::run(const Plan &plan, RegionTree &tree, const RegionId root, Ledger &ledger, RunStats &stats, std::string *error)
{
  stats = {};
  objects_.clear();

  if (tree.find(root) == nullptr)
  {
    return fail(error, "root region is not in the tree");
  }

  std::vector<RegionId> front{root};
  stats.regionsIn = 1;

  for (const auto &step : plan.steps)
  {
    std::vector<RegionId> produced;
    ++stats.stepsRun;

    for (const RegionId id : front)
    {
      const Region *region = tree.find(id);

      if (region == nullptr)
      {
        continue;
      }

      if (step.when && !step.when(*region))
      {
        // Not this step's business: the region passes through to the next one
        // rather than being dropped, so a plan can skip a level for some
        // branches without losing them.
        produced.push_back(id);

        continue;
      }

      if (region->areaMetres() < step.minimumChildAreaMetres)
      {
        produced.push_back(id);

        continue;
      }

      std::string because;
      GeneratorBase *chosen = registry_.choose(step.role, *region, plan.policy, &because);

      if (chosen == nullptr)
      {
        ledger.add(LedgerKind::Omitted, id, std::string("no generator bound for role ") + roleName(step.role));
        produced.push_back(id);

        continue;
      }

      const Descriptor descriptor = chosen->descriptor();
      stats.decisions.push_back(std::string(roleName(step.role)) + ": " + descriptor.id + " (" + because + ") on region " + std::to_string(id));

      const Request request{*region, tree, context_};

      if (auto *edgeGenerator = dynamic_cast<EdgeGenerator *>(chosen); edgeGenerator != nullptr)
      {
        std::vector<Edge> edges;

        if (!edgeGenerator->propose(request, edges, ledger, error))
        {
          return false;
        }

        stats.edgesProposed += uint32_t(edges.size());

        if (edges.empty())
        {
          produced.push_back(id);

          continue;
        }

        // `region` is a pointer into the tree's storage, which subdivide grows.
        const Region snapshot = *region;
        std::vector<RegionId> children;

        if (!subdivide(snapshot, edges, tree, descriptor.id, children, ledger, error))
        {
          return false;
        }

        if (children.empty())
        {
          ledger.add(LedgerKind::Repaired, id, descriptor.id + ": produced no children, region kept whole");
          produced.push_back(id);

          continue;
        }

        produced.insert(produced.end(), children.begin(), children.end());

        continue;
      }

      if (auto *populator = dynamic_cast<RegionPopulator *>(chosen); populator != nullptr)
      {
        std::vector<Object> objects;

        if (!populator->populate(request, objects, ledger, error))
        {
          return false;
        }

        for (auto &object : objects)
        {
          object.region = id;
          objects_.push_back(std::move(object));
        }

        stats.objectsPlaced += uint32_t(objects.size());
        produced.push_back(id);

        continue;
      }

      if (auto *field = dynamic_cast<FieldGenerator *>(chosen); field != nullptr)
      {
        std::vector<Region *> targets{tree.findMutable(id)};

        if (!field->evaluate(tree, context_, targets, ledger, error))
        {
          return false;
        }

        produced.push_back(id);

        continue;
      }

      return fail(error, "generator " + descriptor.id + " implements none of the three interfaces");
    }

    front = std::move(produced);
  }

  stats.regionsOut = uint32_t(front.size());

  return true;
}

} // namespace procedural::world::hierarchy
