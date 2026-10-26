#include "procedural/world/MapGraph.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world {

namespace {

void hashBytes(uint64_t &hash, const void *data, const size_t size)
{
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0u; index < size; ++index)
  {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
}

void hashDouble(uint64_t &hash, const double value)
{
  const double normalized = value == 0.0 ? 0.0 : value;
  hashBytes(hash, &normalized, sizeof(normalized));
}

void hashU32(uint64_t &hash, const uint32_t value)
{
  hashBytes(hash, &value, sizeof(value));
}

// Even-odd crossing test. Exact enough for picking; the polygons here are
// simple Voronoi cells and a boundary-exact answer is not needed to select a
// cell under a cursor.
bool polygonContains(const std::vector<Point2> &polygon, const Point2 &point)
{
  if (polygon.size() < 3u)
  {
    return false;
  }
  bool inside = false;
  for (size_t i = 0u, j = polygon.size() - 1u; i < polygon.size(); j = i++)
  {
    const Point2 &a = polygon[i];
    const Point2 &b = polygon[j];
    if ((a.y > point.y) != (b.y > point.y))
    {
      const double t = (point.y - a.y) / (b.y - a.y);
      if (point.x < a.x + t * (b.x - a.x))
      {
        inside = !inside;
      }
    }
  }
  return inside;
}

} // namespace

uint32_t MapGraph::addNode(MapNode node)
{
  const uint32_t id = static_cast<uint32_t>(nodes_.size());
  node.id = id;
  nodes_.push_back(std::move(node));
  return id;
}

std::vector<uint32_t> MapGraph::nodesAtLevel(const uint8_t level) const
{
  std::vector<uint32_t> result;
  for (const MapNode &node : nodes_)
  {
    if (node.level == level)
    {
      result.push_back(node.id);
    }
  }
  return result;
}

uint32_t MapGraph::nodeAt(const Point2 position) const
{
  // Walk down from level 0 rather than scanning every node: a refined world has
  // far more leaves than roots, and descending is what keeps picking cheap as
  // the hierarchy deepens.
  uint32_t found = kInvalidNode;
  for (const MapNode &node : nodes_)
  {
    if (node.level != 0u)
    {
      continue;
    }
    if (polygonContains(node.area, position))
    {
      found = node.id;
      break;
    }
  }
  while (found != kInvalidNode && nodes_[found].refined())
  {
    uint32_t next = kInvalidNode;
    for (const uint32_t child : nodes_[found].children)
    {
      if (polygonContains(nodes_[child].area, position))
      {
        next = child;
        break;
      }
    }
    if (next == kInvalidNode)
    {
      break;
    }
    found = next;
  }
  return found;
}

void MapGraph::clear()
{
  nodes_.clear();
}

uint64_t MapGraph::signature() const
{
  uint64_t hash = worldBriefHash(brief_);
  for (const MapNode &node : nodes_)
  {
    hashU32(hash, node.id);
    hashU32(hash, node.parent);
    hashBytes(hash, &node.level, sizeof(node.level));
    const uint8_t type = static_cast<uint8_t>(node.type);
    hashBytes(hash, &type, sizeof(type));
    const uint8_t content = static_cast<uint8_t>(node.content);
    hashBytes(hash, &content, sizeof(content));
    hashU32(hash, node.region);
    hashBytes(hash, &node.biome, sizeof(node.biome));
    hashBytes(hash, &node.craton, sizeof(node.craton));
    const uint8_t flags = static_cast<uint8_t>((node.land ? 1u : 0u) | (node.coastal ? 2u : 0u) | (node.shallow ? 4u : 0u));
    hashBytes(hash, &flags, sizeof(flags));
    hashDouble(hash, node.intent.elevationMetres);
    hashDouble(hash, node.intent.upliftRate);
    hashDouble(hash, node.intent.reliefAmplitude);
    hashDouble(hash, node.intent.roughness);
    hashDouble(hash, node.site.x);
    hashDouble(hash, node.site.y);
    for (const Point2 &vertex : node.area)
    {
      hashDouble(hash, vertex.x);
      hashDouble(hash, vertex.y);
    }
    for (const uint32_t neighbour : node.neighbours)
    {
      hashU32(hash, neighbour);
    }
    for (const uint32_t child : node.children)
    {
      hashU32(hash, child);
    }
  }
  return hash;
}

bool MapGraph::validate(std::string *error) const
{
  const auto fail = [&](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }
    return false;
  };

  for (const MapNode &node : nodes_)
  {
    if (node.id >= nodes_.size())
    {
      return fail("node id out of range");
    }
    if (nodes_[node.id].id != node.id)
    {
      return fail("node id does not match its slot");
    }

    // A cell is land or water, never both. The two flags are derived from the
    // same classification and a disagreement means a stage wrote one and not
    // the other.
    if (node.land && cellTypeIsWater(node.type))
    {
      return fail("node " + std::to_string(node.id) + " is land but has water type " + cellTypeName(node.type));
    }
    if (!node.land && !cellTypeIsWater(node.type))
    {
      return fail("node " + std::to_string(node.id) + " is water but has land type " + cellTypeName(node.type));
    }

    // Content answers a different question from landform, but it cannot
    // disagree about whether there is water here.
    if (node.land && cellTypeIsWater(node.content))
    {
      return fail("node " + std::to_string(node.id) + " is land but has water content " + cellTypeName(node.content));
    }
    if (!node.land && !cellTypeIsWater(node.content))
    {
      return fail("node " + std::to_string(node.id) + " is water but has land content " + cellTypeName(node.content));
    }

    if (node.parent != kInvalidNode)
    {
      if (node.parent >= nodes_.size())
      {
        return fail("node parent out of range");
      }
      const MapNode &parent = nodes_[node.parent];
      if (node.level != parent.level + 1u)
      {
        return fail("node " + std::to_string(node.id) + " level does not follow its parent");
      }
      if (std::find(parent.children.begin(), parent.children.end(), node.id) == parent.children.end())
      {
        return fail("node " + std::to_string(node.id) + " is not listed by its parent");
      }
    }
    else if (node.level != 0u)
    {
      return fail("node " + std::to_string(node.id) + " has no parent but is not level 0");
    }

    for (const uint32_t child : node.children)
    {
      if (child >= nodes_.size())
      {
        return fail("node child out of range");
      }
      if (nodes_[child].parent != node.id)
      {
        return fail("child does not point back at its parent");
      }
    }

    // Neighbour links must be symmetric or blending and boundary contracts
    // silently apply to one side only.
    for (const uint32_t neighbour : node.neighbours)
    {
      if (neighbour >= nodes_.size())
      {
        return fail("node neighbour out of range");
      }
      const auto &others = nodes_[neighbour].neighbours;
      if (std::find(others.begin(), others.end(), node.id) == others.end())
      {
        return fail("neighbour link from " + std::to_string(node.id) + " to " + std::to_string(neighbour) + " is not symmetric");
      }
    }

    if (!node.area.empty() && node.area.size() < 3u)
    {
      return fail("node " + std::to_string(node.id) + " has a degenerate polygon");
    }
  }
  return true;
}

} // namespace procedural::world
