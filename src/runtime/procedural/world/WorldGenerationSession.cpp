#include "WorldGenerationSession.hpp"
#include <limits>
#include <set>
namespace procedural::world::rewrite
{
namespace
{
using Identity = std::pair<std::string, uint64_t>;
Identity identity(const GenerationDescriptor &d)
{
  return {d.id, d.domain};
}
bool portsEqual(const std::vector<GenerationPort> &a, const std::vector<GenerationPort> &b)
{
  if (a.size() != b.size())
    return false;
  std::map<GenerationKey, std::pair<std::type_index, uint32_t>> x, y;
  for (auto &p : a)
    x.emplace(p.key, std::make_pair(p.type, p.schema));
  for (auto &p : b)
    y.emplace(p.key, std::make_pair(p.type, p.schema));
  if (x.size() != y.size())
    return false;
  auto i = x.begin(), j = y.begin();
  for (; i != x.end(); ++i, ++j)
    if (i->first < j->first || j->first < i->first || i->second != j->second)
      return false;
  return true;
}
bool same(const GenerationDescriptor &a, const GenerationDescriptor &b)
{
  return a.version == b.version && a.scope == b.scope && portsEqual(a.reads, b.reads) && portsEqual(a.writes, b.writes);
}
} // namespace
bool updateGenerationSession(
    const std::vector<GenerationTask> &tasks,
    const GenerationSnapshot &inputs,
    uint64_t seed,
    uint64_t expected,
    const std::vector<Identity> &requested,
    GenerationSession &state,
    GenerationUpdateReport &report,
    const std::function<bool()> &cancelled,
    std::string *error,
    uint32_t maximumWorkers)
{
  auto fail = [&](const char *s)
  {
    if (error)
      *error = s;
    return false;
  };
  if (expected != state.revision || expected == std::numeric_limits<uint64_t>::max())
    return fail("stale or exhausted generation revision");
  try
  {
    GenerationPlan plan;
    if (!planGenerators(tasks, inputs, plan, error))
      return false;
    std::map<Identity, const GenerationDescriptor *> old, current;
    for (auto &d : state.descriptors)
      old.emplace(identity(d), &d);
    for (auto &t : tasks)
      current.emplace(identity(t.descriptor), &t.descriptor);
    std::set<Identity> dirty(requested.begin(), requested.end());
    for (auto id : dirty)
      if (!current.count(id))
        return fail("unknown dirty invocation");
    std::set<GenerationKey> changed;
    for (auto &[key, value] : inputs)
    {
      if (!value.revision || !value.schema || !value.value.has_value())
        return fail("invalid input revision or payload");
      auto prev = state.inputs.find(key);
      if (prev == state.inputs.end())
      {
        changed.insert(key);
        continue;
      }
      if (value.revision < prev->second.revision)
        return fail("input revision regressed");
      bool incompatible = value.schema != prev->second.schema || value.value.type() != prev->second.value.type();
      if (incompatible && value.revision == prev->second.revision)
        return fail("input schema changed without revision");
      if (incompatible || value.revision != prev->second.revision)
        changed.insert(key);
    }
    for (auto &[key, value] : state.inputs)
      if (!inputs.count(key))
        changed.insert(key);
    for (auto &[id, d] : old)
      if (!current.count(id) || !same(*d, *current.at(id)))
        for (auto &p : d->writes)
          changed.insert(p.key);
    GenerationSession candidate;
    candidate.revision = expected + 1;
    candidate.seed = seed;
    candidate.inputs = inputs;
    candidate.products = inputs;
    GenerationUpdateReport result;
    std::vector<GenerationTask> execute;
    for (auto i : plan.order)
    {
      if (cancelled && cancelled())
        return fail("generation cancelled");
      const auto &task = tasks[i];
      const auto &d = task.descriptor;
      auto id = identity(d);
      bool rebuild = !state.revision || seed != state.seed || dirty.count(id) || !old.count(id) || !same(d, *old.at(id));
      for (auto &p : d.reads)
        rebuild |= changed.count(p.key) != 0;
      for (auto &p : d.writes)
        rebuild |= !state.products.count(p.key);
      if (rebuild)
      {
        execute.push_back(task);
        result.executed.push_back(id);
        for (auto &p : d.writes)
          changed.insert(p.key);
      }
      else
      {
        for (auto &p : d.writes)
          candidate.products.emplace(p.key, state.products.at(p.key));
        result.reused.push_back(id);
      }
      candidate.descriptors.push_back(d);
    }
    GenerationSnapshot generated;
    if (!runGeneratorsParallel(execute, candidate.products, seed, generated, maximumWorkers, cancelled, error))
      return false;
    candidate.products = std::move(generated);
    for (auto &task : execute)
      for (auto &port : task.descriptor.writes)
        candidate.products.at(port.key).revision = candidate.revision;
    for (auto &[key, value] : state.products)
      if (!candidate.products.count(key))
        result.removed.push_back(key);
    if (cancelled && cancelled())
      return fail("generation cancelled");
    state = std::move(candidate);
    report = std::move(result);
    if (error)
      error->clear();
    return true;
  }
  catch (const std::exception &e)
  {
    if (error)
      *error = e.what();
    return false;
  }
}
} // namespace procedural::world::rewrite
