#include "graphrewrite/runtime/EdgePageStore.hpp"

#include <algorithm>

namespace graphrewrite::runtime {
namespace {

bool lessByKey(const Edge &a, const Edge &b, const KeyOrder order)
{
  const uint32_t ka = keyOf(a, order);
  const uint32_t kb = keyOf(b, order);

  if (ka != kb)
  {
    return ka < kb;
  }

  return otherOf(a, order) < otherOf(b, order);
}

} // namespace

Partition::Partition(const EdgeTypeId edgeType, const KeyOrder order, const uint32_t id)
    : type(edgeType), keyOrder(order), partitionId(id)
{
}

EdgeTypeId Partition::edgeType() const
{
  return type;
}

KeyOrder Partition::order() const
{
  return keyOrder;
}

uint32_t Partition::id() const
{
  return partitionId;
}

PageId Partition::allocatePageId()
{
  if (!freeList.empty())
  {
    const PageId id = freeList.back();

    freeList.pop_back();
    pages[id].clear();

    return id;
  }

  pages.emplace_back();

  return static_cast<PageId>(pages.size() - 1u);
}

void Partition::build(std::vector<Edge> edges)
{
  pages.clear();
  freeList.clear();
  directory.order.clear();
  directory.minKey.clear();
  directory.maxKey.clear();

  std::sort(edges.begin(), edges.end(), [this](const Edge &a, const Edge &b) { return lessByKey(a, b, keyOrder); });

  size_t at = 0u;

  do
  {
    const size_t take = std::min<size_t>(pageEdges, edges.size() - at);

    const PageId id = allocatePageId();

    pages[id].assign(edges.begin() + static_cast<long>(at), edges.begin() + static_cast<long>(at + take));

    directory.order.push_back(id);
    directory.minKey.push_back(pages[id].empty() ? 0u : keyOf(pages[id].front(), keyOrder));
    directory.maxKey.push_back(pages[id].empty() ? 0u : keyOf(pages[id].back(), keyOrder));

    at += take;
  } while (at < edges.size());
}

uint32_t Partition::pageCount() const
{
  return directory.size();
}

PageId Partition::pageIdAt(const uint32_t position) const
{
  return directory.order[position];
}

const std::vector<Edge> &Partition::pageAt(const uint32_t position) const
{
  return pages[directory.order[position]];
}

uint32_t Partition::minKeyAt(const uint32_t position) const
{
  return directory.minKey[position];
}

uint32_t Partition::maxKeyAt(const uint32_t position) const
{
  return directory.maxKey[position];
}

const std::vector<Edge> &Partition::pageById(const PageId id) const
{
  return pages[id];
}

uint32_t Partition::positionOf(const PageId id) const
{
  for (uint32_t i = 0u; i < directory.size(); ++i)
  {
    if (directory.order[i] == id)
    {
      return i;
    }
  }

  return notResident;
}

void Partition::refreshRange(const uint32_t position)
{
  const std::vector<Edge> &page = pages[directory.order[position]];

  if (page.empty())
  {
    // Keep the old range: an empty page that claims [0, 0] would swallow every
    // search for key 0.
    return;
  }

  directory.minKey[position] = keyOf(page.front(), keyOrder);
  directory.maxKey[position] = keyOf(page.back(), keyOrder);
}

void Partition::replacePage(const uint32_t position, std::vector<Edge> contents)
{
  pages[directory.order[position]] = std::move(contents);

  refreshRange(position);
}

uint32_t Partition::splitPage(const uint32_t position)
{
  std::vector<Edge> &source = pages[directory.order[position]];

  const size_t half = source.size() / 2u;

  const PageId id = allocatePageId();

  // allocatePageId may have grown `pages`, so re-take the reference.
  std::vector<Edge> &left = pages[directory.order[position]];
  std::vector<Edge> &right = pages[id];

  right.assign(left.begin() + static_cast<long>(half), left.end());
  left.erase(left.begin() + static_cast<long>(half), left.end());

  // A split inserts ONE entry into the directory. No existing PageId moves, so
  // nothing that already refers to a page by id has to be updated.
  directory.order.insert(directory.order.begin() + static_cast<long>(position) + 1, id);
  directory.minKey.insert(directory.minKey.begin() + static_cast<long>(position) + 1, right.empty() ? 0u : keyOf(right.front(), keyOrder));
  directory.maxKey.insert(directory.maxKey.begin() + static_cast<long>(position) + 1, right.empty() ? 0u : keyOf(right.back(), keyOrder));

  refreshRange(position);

  (void)source;

  return position + 1u;
}

uint32_t Partition::insert(const Edge &edge)
{
  const uint32_t key = keyOf(edge, keyOrder);

  // The page whose range covers the key, or the first page past it.
  uint32_t position = 0u;

  while (position + 1u < directory.size() && directory.maxKey[position] < key)
  {
    ++position;
  }

  std::vector<Edge> &page = pages[directory.order[position]];

  const auto at = std::lower_bound(page.begin(), page.end(), edge,
                                   [this](const Edge &a, const Edge &b) { return lessByKey(a, b, keyOrder); });

  page.insert(at, edge);

  refreshRange(position);

  if (page.size() > pageEdges)
  {
    const uint32_t right = splitPage(position);

    return keyOf(edge, keyOrder) >= directory.minKey[right] ? right : position;
  }

  return position;
}

bool Partition::remove(const Edge &edge)
{
  const uint32_t key = keyOf(edge, keyOrder);

  const PageRun run = rangeFor(key);

  for (uint32_t position = run.first; !run.empty() && position <= run.last; ++position)
  {
    std::vector<Edge> &page = pages[directory.order[position]];

    const auto found = std::find(page.begin(), page.end(), edge);

    if (found == page.end())
    {
      continue;
    }

    page.erase(found);

    refreshRange(position);

    return true;
  }

  return false;
}

bool Partition::contains(const Edge &edge) const
{
  const PageRun run = rangeFor(keyOf(edge, keyOrder));

  for (uint32_t position = run.first; !run.empty() && position <= run.last; ++position)
  {
    const std::vector<Edge> &page = pages[directory.order[position]];

    if (std::find(page.begin(), page.end(), edge) != page.end())
    {
      return true;
    }
  }

  return false;
}

PageRun Partition::rangeFor(const uint32_t key) const
{
  return directoryRange(directory, key);
}

const PartitionDirectory &Partition::directoryView() const
{
  return directory;
}

std::vector<Edge> Partition::snapshot() const
{
  std::vector<Edge> out;

  for (const PageId id : directory.order)
  {
    out.insert(out.end(), pages[id].begin(), pages[id].end());
  }

  return out;
}

void Partition::restore(std::vector<std::vector<Edge>> pagesById, PartitionDirectory restored)
{
  pages = std::move(pagesById);
  directory = std::move(restored);
  freeList.clear();

  // Any page id the directory does not reference is a free slot.
  std::vector<bool> used(pages.size(), false);

  for (const PageId id : directory.order)
  {
    if (id < used.size())
    {
      used[id] = true;
    }
  }

  for (PageId id = static_cast<PageId>(pages.size()); id-- > 0u;)
  {
    if (!used[id])
    {
      freeList.push_back(id);
    }
  }
}

const std::vector<std::vector<Edge>> &Partition::allPages() const
{
  return pages;
}

bool Partition::checkInvariants(std::string &problem) const
{
  if (directory.order.size() != directory.minKey.size() || directory.order.size() != directory.maxKey.size())
  {
    problem = "directory and key arrays disagree in length";

    return false;
  }

  for (uint32_t position = 0u; position < directory.size(); ++position)
  {
    const std::vector<Edge> &page = pages[directory.order[position]];

    if (page.size() > pageEdges)
    {
      problem = "page " + std::to_string(position) + " holds " + std::to_string(page.size()) + " edges";

      return false;
    }

    if (page.empty())
    {
      continue;
    }

    if (!std::is_sorted(page.begin(), page.end(), [this](const Edge &a, const Edge &b) { return lessByKey(a, b, keyOrder); }))
    {
      problem = "page " + std::to_string(position) + " is not sorted";

      return false;
    }

    if (directory.minKey[position] != keyOf(page.front(), keyOrder) || directory.maxKey[position] != keyOf(page.back(), keyOrder))
    {
      problem = "page " + std::to_string(position) + " key range disagrees with its contents";

      return false;
    }

    if (position > 0u && directory.minKey[position] < directory.minKey[position - 1u])
    {
      problem = "directory is out of order at position " + std::to_string(position);

      return false;
    }
  }

  return true;
}

// ---------------------------------------------------------------- EdgePageStore

int32_t EdgePageStore::indexOf(const EdgeTypeId type, const KeyOrder order) const
{
  const std::vector<int32_t> &table = order == KeyOrder::ByOrigin ? forwardIndex : backwardIndex;

  if (type >= table.size())
  {
    return -1;
  }

  return table[type];
}

void EdgePageStore::configure(const compiler::EdgeTypeTable &types)
{
  partitions.clear();

  forwardIndex.assign(types.size(), -1);
  backwardIndex.assign(types.size(), -1);

  for (EdgeTypeId type = 0u; type < types.size(); ++type)
  {
    // EVERY type gets an origin partition, whether or not a rule matches on it:
    // a rewrite can insert an edge of a type nothing traverses, and without
    // somewhere to put it that edge is silently dropped.
    forwardIndex[type] = static_cast<int32_t>(partitions.size());
    partitions.emplace_back(type, KeyOrder::ByOrigin, static_cast<uint32_t>(partitions.size()));

    // The saving is on the SECOND partition: a type never walked backward does
    // not pay for a target-ordered copy of its edges.
    if (types[type].usedBackward)
    {
      backwardIndex[type] = static_cast<int32_t>(partitions.size());
      partitions.emplace_back(type, KeyOrder::ByTarget, static_cast<uint32_t>(partitions.size()));
    }
  }

  for (Partition &partition : partitions)
  {
    partition.build({});
  }
}

uint32_t EdgePageStore::partitionCount() const
{
  return static_cast<uint32_t>(partitions.size());
}

bool EdgePageStore::has(const EdgeTypeId type, const KeyOrder order) const
{
  return indexOf(type, order) >= 0;
}

Partition &EdgePageStore::partition(const EdgeTypeId type, const KeyOrder order)
{
  return partitions[static_cast<size_t>(indexOf(type, order))];
}

const Partition &EdgePageStore::partition(const EdgeTypeId type, const KeyOrder order) const
{
  return partitions[static_cast<size_t>(indexOf(type, order))];
}

Partition &EdgePageStore::partitionAt(const uint32_t index)
{
  return partitions[index];
}

const Partition &EdgePageStore::partitionAt(const uint32_t index) const
{
  return partitions[index];
}

void EdgePageStore::addEdge(const EdgeTypeId type, const Edge &edge)
{
  for (const KeyOrder order : {KeyOrder::ByOrigin, KeyOrder::ByTarget})
  {
    if (has(type, order))
    {
      partition(type, order).insert(edge);
    }
  }

  reserveVertices(std::max(edge.origin, edge.target) + 1u);
}

bool EdgePageStore::removeEdge(const EdgeTypeId type, const Edge &edge)
{
  bool removed = false;

  for (const KeyOrder order : {KeyOrder::ByOrigin, KeyOrder::ByTarget})
  {
    if (has(type, order))
    {
      removed = partition(type, order).remove(edge) || removed;
    }
  }

  return removed;
}

bool EdgePageStore::containsEdge(const EdgeTypeId type, const Edge &edge) const
{
  for (const KeyOrder order : {KeyOrder::ByOrigin, KeyOrder::ByTarget})
  {
    if (has(type, order) && partition(type, order).contains(edge))
    {
      return true;
    }
  }

  return false;
}

uint32_t EdgePageStore::edgeCount(const EdgeTypeId type) const
{
  if (!has(type, KeyOrder::ByOrigin))
  {
    return 0u;
  }

  const Partition &source = partition(type, KeyOrder::ByOrigin);

  uint32_t total = 0u;

  for (uint32_t position = 0u; position < source.pageCount(); ++position)
  {
    total += static_cast<uint32_t>(source.pageAt(position).size());
  }

  return total;
}

std::vector<uint32_t> EdgePageStore::edgeCounts(const uint32_t typeCount) const
{
  std::vector<uint32_t> counts(typeCount, 0u);

  for (EdgeTypeId type = 0u; type < typeCount; ++type)
  {
    counts[type] = edgeCount(type);
  }

  return counts;
}

uint32_t EdgePageStore::allocateVertex()
{
  return nextVertex++;
}

uint32_t EdgePageStore::vertexCount() const
{
  return nextVertex;
}

void EdgePageStore::reserveVertices(const uint32_t count)
{
  if (count > vertexCeiling)
  {
    return;
  }

  nextVertex = std::max(nextVertex, count);
}

void EdgePageStore::setVertexCeiling(const uint32_t ceiling)
{
  vertexCeiling = ceiling;
}

} // namespace graphrewrite::runtime
