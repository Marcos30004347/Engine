#include "WorldGeneratorDirector.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <future>
#include <queue>
#include <set>

namespace procedural::world::rewrite {
bool planGenerators(const std::vector<GenerationTask> &tasks, const GenerationSnapshot &inputs, GenerationPlan &out, std::string *error)
{
  auto fail = [&](const std::string &s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (tasks.size() > 4096)
  {
    return fail("generator task budget exceeded");
  }
  std::set<std::pair<std::string, uint64_t>> identities;
  std::map<GenerationKey, std::pair<size_t, GenerationPort>> producers;
  for (size_t i = 0; i < tasks.size(); ++i)
  {
    auto &d = tasks[i].descriptor;
    if (d.id.empty() || !d.domain || !d.version || !tasks[i].generate || !identities.emplace(d.id, d.domain).second)
    {
      return fail("invalid or duplicate generator identity");
    }
    if (d.reads.size() + d.writes.size() > 4096)
    {
      return fail("generator port budget exceeded");
    }
    for (auto &p : d.writes)
    {
      if (p.key.product.empty() || !p.key.domain || !p.schema || p.type == typeid(void) || inputs.count(p.key) || !producers.emplace(p.key, std::make_pair(i, p)).second)
      {
        return fail("invalid port, duplicate writer or input override");
      }
    }
  }
  std::vector<std::set<size_t>> children(tasks.size());
  std::vector<size_t> degree(tasks.size()), level(tasks.size());
  for (size_t i = 0; i < tasks.size(); ++i)
  {
    std::set<GenerationKey> reads;
    for (auto &p : tasks[i].descriptor.reads)
    {
      if (p.key.product.empty() || !p.key.domain || !p.schema || p.type == typeid(void) || !reads.insert(p.key).second)
      {
        return fail("invalid or duplicate read port");
      }
      auto source = producers.find(p.key);
      if (source != producers.end())
      {
        if (source->second.second.type != p.type || source->second.second.schema != p.schema)
        {
          return fail("producer schema/type mismatch");
        }
        if (children[source->second.first].insert(i).second)
        {
          ++degree[i];
        }
      }
      else
      {
        auto value = inputs.find(p.key);
        if (value == inputs.end() || value->second.schema != p.schema || std::type_index(value->second.value.type()) != p.type)
        {
          return fail("missing or incompatible input");
        }
      }
    }
  }
  auto compare = [&](size_t a, size_t b) {
    auto &x = tasks[a].descriptor;
    auto &y = tasks[b].descriptor;
    return std::make_pair(x.id, x.domain) > std::make_pair(y.id, y.domain);
  };
  std::priority_queue<size_t, std::vector<size_t>, decltype(compare)> ready(compare);
  for (size_t i = 0; i < tasks.size(); ++i)
  {
    if (!degree[i])
    {
      ready.push(i);
    }
  }
  GenerationPlan plan;
  while (!ready.empty())
  {
    auto i = ready.top();
    ready.pop();
    plan.order.push_back(i);
    if (plan.levels.size() <= level[i])
    {
      plan.levels.resize(level[i] + 1);
    }
    plan.levels[level[i]].push_back(i);
    for (auto c : children[i])
    {
      level[c] = std::max(level[c], level[i] + 1);
      if (!--degree[c])
      {
        ready.push(c);
      }
    }
  }
  if (plan.order.size() != tasks.size())
  {
    return fail("cyclic generator dependencies");
  }
  out = std::move(plan);
  if (error)
  {
    error->clear();
  }
  return true;
}

bool affectedGenerators(const std::vector<GenerationTask> &tasks, const GenerationSnapshot &inputs, const std::vector<GenerationKey> &changed, std::vector<size_t> &out, std::string *error)
{
  GenerationPlan plan;
  if (!planGenerators(tasks, inputs, plan, error))
  {
    return false;
  }
  std::set<GenerationKey> dirty(changed.begin(), changed.end());
  std::vector<size_t> result;
  for (auto i : plan.order)
  {
    const auto &d = tasks[i].descriptor;
    bool affected = false;
    for (auto &port : d.reads)
    {
      affected |= dirty.count(port.key) != 0;
    }
    if (!affected)
    {
      continue;
    }
    result.push_back(i);
    for (auto &port : d.writes)
    {
      dirty.insert(port.key);
    }
  }
  out = std::move(result);
  return true;
}

static bool executeGeneratorDag(
    const std::vector<GenerationTask> &tasks,
    const GenerationSnapshot &inputs,
    uint64_t seed,
    GenerationSnapshot &output,
    uint32_t maximumWorkers,
    const std::function<bool()> &cancelled,
    std::string *error,
    const std::vector<GenerationKey> *retainedProducts)
{
  auto fail = [&](const std::string &s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (maximumWorkers == 0 || maximumWorkers > 32)
  {
    return fail("invalid generator worker budget");
  }
  try
  {
    GenerationPlan plan;
    if (!planGenerators(tasks, inputs, plan, error))
    {
      return false;
    }
    GenerationSnapshot candidate = inputs;
    std::set<GenerationKey> retain;
    std::map<GenerationKey, size_t> readers;
    if (retainedProducts)
    {
      retain.insert(retainedProducts->begin(), retainedProducts->end());
      for (auto &t : tasks)
      {
        for (auto &p : t.descriptor.reads)
        {
          ++readers[p.key];
        }
      }
    }

    struct Result
    {
      bool ok = false;
      GenerationSnapshot delta;
      std::string error;
    };

    auto execute = [&](size_t index) {
      Result result;
      try
      {
        if (cancelled && cancelled())
        {
          result.error = "generation cancelled";
          return result;
        }
        const auto &task = tasks[index];
        const auto &d = task.descriptor;
        GenerationReadView view(candidate, d.reads);
        uint64_t identity = 14695981039346656037ULL;
        for (unsigned char c : d.id)
        {
          identity ^= c;
          identity *= 1099511628211ULL;
        }
        GenerationRequest request{view, featureSeed(seed, d.version, d.domain, identity), cancelled};
        const auto started = std::chrono::steady_clock::now();
        const bool ran = task.generate(request, result.delta, &result.error);

        // Which generator the time actually went into.
        //
        // Sampling a running chain names whichever stage happens to be
        // executing; it cannot tell you that one stage is half the run. This
        // can, and it is one line per generator.
        if (std::getenv("GENERATOR_TIMING") != nullptr)
        {
          const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

          if (ms > 50.)
          {
            std::fprintf(stderr, "generator %s: %.0f ms\n", d.id.c_str(), ms);
          }
        }

        if (!ran)
        {
          result.error=d.id+": "+result.error;
          return result;
        }
        if (result.delta.size() != d.writes.size())
        {
          result.error = "generator output count mismatch: " + d.id;
          return result;
        }
        for (auto &port : d.writes)
        {
          auto it = result.delta.find(port.key);
          if (it == result.delta.end() || it->second.schema != port.schema || std::type_index(it->second.value.type()) != port.type)
          {
            result.error = "generator output schema/type mismatch: " + d.id;
            return result;
          }
        }
        result.ok = true;
      }
      catch (const std::exception &e)
      {
        result.error = e.what();
      }
      catch (...)
      {
        result.error = "unknown generator exception";
      }
      return result;
    };
    for (const auto &level : plan.levels)
    {
      for (size_t start = 0; start < level.size();)
      {
        size_t end = start + 1;
        if (tasks[level[start]].parallelSafe)
        {
          while (end < level.size() && end - start < maximumWorkers && tasks[level[end]].parallelSafe)
          {
            ++end;
          }
        }
        std::vector<Result> results;
        if (end - start == 1)
        {
          results.push_back(execute(level[start]));
        }
        else
        {
          std::vector<std::future<Result>> jobs;
          for (size_t i = start; i < end; ++i)
          {
            jobs.push_back(std::async(std::launch::async, execute, level[i]));
          }
          for (auto &job : jobs)
          {
            results.push_back(job.get());
          }
        }
        for (auto &r : results)
        {
          if (!r.ok)
          {
            return fail(r.error.empty() ? "generator failed" : r.error);
          }
        }
        if (cancelled && cancelled())
        {
          return fail("generation cancelled");
        }
        for (auto &r : results)
        {
          candidate.insert(std::make_move_iterator(r.delta.begin()), std::make_move_iterator(r.delta.end()));
        }
        if (retainedProducts)
        {
          for (size_t i = start; i < end; ++i)
          {
            for (auto &p : tasks[level[i]].descriptor.reads)
            {
              --readers[p.key];
            }
          }
          for (auto it = candidate.begin(); it != candidate.end();)
          {
            if (!readers[it->first] && !retain.count(it->first))
            {
              it = candidate.erase(it);
            }
            else
            {
              ++it;
            }
          }
        }
        start = end;
      }
    }
    if (cancelled && cancelled())
    {
      return fail("generation cancelled");
    }
    if (retainedProducts)
    {
      for (auto &key : retain)
      {
        if (!candidate.count(key))
        {
          return fail("requested product has no producer");
        }
      }
      for (auto it = candidate.begin(); it != candidate.end();)
      {
        if (!retain.count(it->first))
        {
          it = candidate.erase(it);
        }
        else
        {
          ++it;
        }
      }
    }
    output = std::move(candidate);
    if (error)
    {
      error->clear();
    }
    return true;
  }
  catch (const std::exception &e)
  {
    return fail(e.what());
  }
  catch (...)
  {
    return fail("unknown generator execution exception");
  }
}

bool runGeneratorsParallel(const std::vector<GenerationTask> &tasks, const GenerationSnapshot &inputs, uint64_t seed, GenerationSnapshot &out, uint32_t workers, const std::function<bool()> &cancelled, std::string *error)
{
  return executeGeneratorDag(tasks, inputs, seed, out, workers, cancelled, error, nullptr);
}

bool runGeneratorsProjected(
    const std::vector<GenerationTask> &tasks,
    const GenerationSnapshot &inputs,
    uint64_t seed,
    GenerationSnapshot &out,
    const std::vector<GenerationKey> &retained,
    uint32_t workers,
    const std::function<bool()> &cancelled,
    std::string *error)
{
  return executeGeneratorDag(tasks, inputs, seed, out, workers, cancelled, error, &retained);
}

bool runGenerators(const std::vector<GenerationTask> &tasks, const GenerationSnapshot &inputs, uint64_t seed, GenerationSnapshot &output, const std::function<bool()> &cancelled, std::string *error)
{
  return runGeneratorsParallel(tasks, inputs, seed, output, 1, cancelled, error);
}
} // namespace procedural::world::rewrite
