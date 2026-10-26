#include "procedural/world/WorldBrief.hpp"

#include <algorithm>

namespace procedural::world {

namespace {

// FNV-1a. Hashes scalar bit patterns rather than object layout so captured
// baselines survive a compiler change.
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
  // Normalize the two zeroes so -0.0 and 0.0 cannot produce different worlds.
  const double normalized = value == 0.0 ? 0.0 : value;
  hashBytes(hash, &normalized, sizeof(normalized));
}

} // namespace

const char *tagName(const Tag tag)
{
  switch (tag)
  {
  case Tag::Island:
    return "island";
  case Tag::Archipelago:
    return "archipelago";
  case Tag::Coast:
    return "coast";
  case Tag::Bay:
    return "bay";
  case Tag::Fjord:
    return "fjord";
  case Tag::Headland:
    return "headland";
  case Tag::Lake:
    return "lake";
  case Tag::Landlocked:
    return "landlocked";
  case Tag::Lowland:
    return "lowland";
  case Tag::Highland:
    return "highland";
  case Tag::Barren:
    return "barren";
  case Tag::Verdant:
    return "verdant";
  case Tag::Arid:
    return "arid";
  case Tag::Frozen:
    return "frozen";
  default:
    return "unknown";
  }
}

bool tagIsShape(const Tag tag)
{
  return tag <= Tag::Landlocked;
}

const char *cellTypeName(const CellType type)
{
  switch (type)
  {
  case CellType::Ocean:
    return "ocean";
  case CellType::Lake:
    return "lake";
  case CellType::Coast:
    return "coast";
  case CellType::Plain:
    return "plain";
  case CellType::Hill:
    return "hill";
  case CellType::Mountain:
    return "mountain";
  case CellType::Plateau:
    return "plateau";
  case CellType::Valley:
    return "valley";
  case CellType::Wood:
    return "wood";
  case CellType::Swamp:
    return "swamp";
  case CellType::Desert:
    return "desert";
  case CellType::Rocks:
    return "rocks";
  case CellType::Cliff:
    return "cliff";
  case CellType::Canyon:
    return "canyon";
  default:
    return "unknown";
  }
}

bool cellTypeIsWater(const CellType type)
{
  return type == CellType::Ocean || type == CellType::Lake;
}

bool WorldBrief::valid() const
{
  return extentMetres > 0.0 && reliefMetres > 0.0 && std::isfinite(extentMetres) && std::isfinite(seaLevelMetres) && std::isfinite(reliefMetres);
}

void resolveTags(std::vector<Tag> &tags)
{
  std::sort(tags.begin(), tags.end());
  tags.erase(std::unique(tags.begin(), tags.end()), tags.end());

  // Exactly one shape tag. Keeping the first in canonical order rather than
  // rejecting the brief means a UI that lets both be ticked still generates
  // something, and generates it deterministically.
  bool seenShape = false;
  std::vector<Tag> resolved;
  resolved.reserve(tags.size());
  for (const Tag tag : tags)
  {
    if (tagIsShape(tag))
    {
      if (seenShape)
      {
        continue;
      }
      seenShape = true;
    }
    resolved.push_back(tag);
  }
  if (!seenShape)
  {
    resolved.insert(resolved.begin(), Tag::Island);
  }
  tags = std::move(resolved);
}

Tag shapeTag(const std::vector<Tag> &tags)
{
  for (const Tag tag : tags)
  {
    if (tagIsShape(tag))
    {
      return tag;
    }
  }
  return Tag::Island;
}

uint64_t worldBriefHash(const WorldBrief &brief)
{
  uint64_t hash = 14695981039346656037ull;
  hashBytes(hash, &brief.seed, sizeof(brief.seed));
  std::vector<Tag> tags = brief.tags;
  resolveTags(tags);
  for (const Tag tag : tags)
  {
    const uint8_t value = static_cast<uint8_t>(tag);
    hashBytes(hash, &value, sizeof(value));
  }
  hashDouble(hash, brief.extentMetres);
  hashDouble(hash, brief.seaLevelMetres);
  hashDouble(hash, brief.reliefMetres);
  return hash;
}

std::string worldBriefDescription(const WorldBrief &brief)
{
  std::vector<Tag> tags = brief.tags;
  resolveTags(tags);
  std::string text = "seed=" + std::to_string(brief.seed) + " tags=";
  for (size_t index = 0u; index < tags.size(); ++index)
  {
    if (index != 0u)
    {
      text += ",";
    }
    text += tagName(tags[index]);
  }
  text += " extent=" + std::to_string(brief.extentMetres / 1000.0) + "km";
  return text;
}

} // namespace procedural::world
