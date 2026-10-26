#pragma once
#include "WorldGenerationContracts.hpp"
#include <any>
#include <functional>
#include <map>
#include <string>
#include <typeindex>
#include <vector>

namespace procedural::world::rewrite {
// Stable product identities are namespaced by a persistent domain, never geometry.
struct GenerationKey
{
  std::string product;
  uint64_t domain = 0;

  bool operator<(const GenerationKey &b) const
  {
    return product != b.product ? product < b.product : domain < b.domain;
  }
};

struct GenerationPort
{
  GenerationKey key;
  std::type_index type{typeid(void)};
  uint32_t schema = 1;
};

struct GenerationProduct
{
  std::any value; // Value-owned payloads; adapters must not embed mutable shared aliases.
  uint32_t schema = 1;
  uint64_t revision = 1; // External payload changes must advance this token.
};

using GenerationSnapshot = std::map<GenerationKey, GenerationProduct>;
enum class GenerationScope
{
  Patch,
  Neighbourhood,
  Watershed,
  Network,
  World
};

struct GenerationDescriptor
{
  std::string id;
  uint64_t domain = 1, version = 1;
  GenerationScope scope = GenerationScope::World;
  std::vector<GenerationPort> reads, writes;
};

class GenerationReadView
{
public:
  GenerationReadView(const GenerationSnapshot &snapshot, const std::vector<GenerationPort> &ports) : snapshot_(snapshot), ports_(ports)
  {
  }

  template <class T> const T *get(const GenerationKey &key) const
  {
    for (const auto &port : ports_)
    {
      if (!(key < port.key) && !(port.key < key) && port.type == typeid(T))
      {
        auto it = snapshot_.find(key);
        if (it != snapshot_.end() && it->second.schema == port.schema)
        {
          return std::any_cast<T>(&it->second.value);
        }
      }
    }
    return nullptr;
  }

private:
  const GenerationSnapshot &snapshot_;
  const std::vector<GenerationPort> &ports_;
};

struct GenerationRequest
{
  const GenerationReadView &context;
  uint64_t seed;
  std::function<bool()> cancelled;
};

struct GenerationTask
{
  GenerationDescriptor descriptor;
  std::function<bool(const GenerationRequest &, GenerationSnapshot &delta, std::string *)> generate;
  bool parallelSafe = false; // Explicit opt-in: immutable payloads/captures and thread-safe cancellation.
};

struct GenerationPlan
{
  std::vector<size_t> order;
  std::vector<std::vector<size_t>> levels;
};

// Dependency edges are inferred from typed product ports. No duplicate writers,
// implicit input overrides, undeclared reads/writes, or missing producers.
bool planGenerators(const std::vector<GenerationTask> &, const GenerationSnapshot &inputs, GenerationPlan &, std::string *error = nullptr);
// Conservative forward invalidation for declared product reads. Changed products
// are supplied by the editor/producer; geometric/spatial queries are not yet indexed.
// Does not execute or remove products. Results are in dependency order.
bool affectedGenerators(const std::vector<GenerationTask> &, const GenerationSnapshot &inputs, const std::vector<GenerationKey> &changed, std::vector<size_t> &, std::string *error = nullptr);
// Bounded parallel execution of explicitly safe tasks. Unsafe tasks run alone;
// all deltas commit in canonical order and publication remains atomic.
bool runGeneratorsParallel(
    const std::vector<GenerationTask> &,
    const GenerationSnapshot &,
    uint64_t,
    GenerationSnapshot &,
    uint32_t maximumWorkers,
    const std::function<bool()> &cancelled = {},
    std::string *error = nullptr);
// One-shot builds retain only requested products and release consumed intermediates.
// Session builds keep all products for reuse and do not use this projection.
bool runGeneratorsProjected(
    const std::vector<GenerationTask> &,
    const GenerationSnapshot &,
    uint64_t,
    GenerationSnapshot &,
    const std::vector<GenerationKey> &retainedProducts,
    uint32_t maximumWorkers = 1,
    const std::function<bool()> &cancelled = {},
    std::string *error = nullptr);
// Conservative serial executor. Stage deltas are validated before they enter the
// candidate; the caller's output is replaced only after every stage succeeds.
// No cache/local rebuild or parallel-safety promise in this first implementation.
bool runGenerators(const std::vector<GenerationTask> &, const GenerationSnapshot &inputs, uint64_t worldSeed, GenerationSnapshot &output, const std::function<bool()> &cancelled = {}, std::string *error = nullptr);
} // namespace procedural::world::rewrite
