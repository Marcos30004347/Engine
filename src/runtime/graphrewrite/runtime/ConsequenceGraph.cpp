#include "graphrewrite/runtime/ConsequenceGraph.hpp"

#include <algorithm>

namespace graphrewrite::runtime {
namespace {

// How far `leadsToWithin` is precomputed. Three is what a reader can still
// follow as one thread of cause: the throne falls empty, a man takes it, a
// rival plots against him.
constexpr uint32_t defaultSteps = 3u;

void sortUnique(std::vector<compiler::EdgeTypeId> &list)
{
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());
}

} // namespace

bool ConsequenceGraph::at(const std::vector<uint64_t> &rows, const uint32_t cause, const uint32_t effect) const
{
  if (cause >= rules || effect >= rules)
  {
    return false;
  }

  return (rows[static_cast<size_t>(cause) * words + effect / 64u] >> (effect % 64u)) & 1u;
}

void ConsequenceGraph::set(std::vector<uint64_t> &rows, const uint32_t cause, const uint32_t effect)
{
  rows[static_cast<size_t>(cause) * words + effect / 64u] |= uint64_t(1) << (effect % 64u);
}

void ConsequenceGraph::build(const compiler::CompiledProgram &program, const std::vector<compiler::EdgeTypeId> &incidental)
{
  std::vector<compiler::EdgeTypeId> ignored = incidental;

  std::sort(ignored.begin(), ignored.end());

  rules = static_cast<uint32_t>(program.ruleRewrites.size());
  words = (rules + 63u) / 64u;

  produced.assign(rules, {});
  consumed.assign(rules, {});

  for (uint32_t rule = 0u; rule < rules; ++rule)
  {
    const compiler::RuleRewrite &rewrite = program.ruleRewrites[rule];

    std::vector<compiler::EdgeTypeId> matched;

    for (const compiler::PatternEdge &edge : rewrite.when)
    {
      matched.push_back(edge.label);
    }

    sortUnique(matched);

    consumed[rule] = matched;

    // Produced: inserted and NOT matched. An edge a rule restates to keep is
    // not something it brought into the world.
    for (const compiler::PatternEdge &edge : rewrite.insertions)
    {
      if (!std::binary_search(matched.begin(), matched.end(), edge.label) &&
          !std::binary_search(ignored.begin(), ignored.end(), edge.label))
      {
        produced[rule].push_back(edge.label);
      }
    }

    sortUnique(produced[rule]);
  }

  direct.assign(static_cast<size_t>(rules) * words, 0u);

  for (uint32_t cause = 0u; cause < rules; ++cause)
  {
    if (produced[cause].empty())
    {
      continue;
    }

    for (uint32_t effect = 0u; effect < rules; ++effect)
    {
      if (cause == effect)
      {
        continue;
      }

      for (const compiler::EdgeTypeId label : produced[cause])
      {
        if (std::binary_search(consumed[effect].begin(), consumed[effect].end(), label))
        {
          set(direct, cause, effect);

          break;
        }
      }
    }
  }

  // Depth-limited reachability by repeated squaring of the step relation.
  closureSteps = defaultSteps;
  withinN = direct;

  std::vector<uint64_t> frontier = direct;

  for (uint32_t step = 1u; step < closureSteps; ++step)
  {
    std::vector<uint64_t> next(static_cast<size_t>(rules) * words, 0u);

    for (uint32_t cause = 0u; cause < rules; ++cause)
    {
      for (uint32_t middle = 0u; middle < rules; ++middle)
      {
        if (!at(frontier, cause, middle))
        {
          continue;
        }

        for (uint32_t word = 0u; word < words; ++word)
        {
          next[static_cast<size_t>(cause) * words + word] |= direct[static_cast<size_t>(middle) * words + word];
        }
      }
    }

    for (size_t i = 0u; i < withinN.size(); ++i)
    {
      withinN[i] |= next[i];
    }

    frontier.swap(next);
  }
}

bool ConsequenceGraph::leadsTo(const uint32_t cause, const uint32_t effect) const
{
  return at(direct, cause, effect);
}

bool ConsequenceGraph::leadsToWithin(const uint32_t cause, const uint32_t effect, const uint32_t maxSteps) const
{
  if (maxSteps <= 1u)
  {
    return at(direct, cause, effect);
  }

  // Only one depth is precomputed; a deeper question is answered with it.
  return at(withinN, cause, effect);
}

bool ConsequenceGraph::linked(const ChronicleEvent &cause, const ChronicleEvent &effect, const uint32_t nodeCeiling,
                              const uint32_t maxSteps) const
{
  if (cause.tick > effect.tick)
  {
    return false;
  }

  if (!leadsToWithin(cause.rule, effect.rule, maxSteps))
  {
    return false;
  }

  for (const uint32_t node : cause.bound)
  {
    if (node >= nodeCeiling)
    {
      continue;
    }

    if (std::find(effect.bound.begin(), effect.bound.end(), node) != effect.bound.end())
    {
      return true;
    }
  }

  return false;
}

std::vector<std::string> ConsequenceGraph::linkTypes(const uint32_t cause, const uint32_t effect,
                                                     const compiler::CompiledProgram &program) const
{
  std::vector<std::string> types;

  if (cause >= rules || effect >= rules)
  {
    return types;
  }

  for (const compiler::EdgeTypeId label : produced[cause])
  {
    if (std::binary_search(consumed[effect].begin(), consumed[effect].end(), label) && label < program.edgeTypes.size())
    {
      types.push_back(program.edgeTypes[label].pattern);
    }
  }

  return types;
}

size_t ConsequenceGraph::directLinks() const
{
  size_t total = 0u;

  for (const uint64_t word : direct)
  {
    total += static_cast<size_t>(__builtin_popcountll(word));
  }

  return total;
}

} // namespace graphrewrite::runtime
