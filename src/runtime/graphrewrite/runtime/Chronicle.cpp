#include "graphrewrite/runtime/Chronicle.hpp"

#include <algorithm>

namespace graphrewrite::runtime {

void Chronicle::add(ChronicleEvent event)
{
  ++offered;

  if (!remembered.empty() && remembered.count(event.title) == 0u)
  {
    return;
  }

  const size_t index = events.size();

  // A node may be bound twice in one firing (a self-loop, a rule that names
  // the same person under two letters); index it once.
  std::vector<uint32_t> seen;

  for (const uint32_t node : event.bound)
  {
    if (node >= nodeCeiling || std::find(seen.begin(), seen.end(), node) != seen.end())
    {
      continue;
    }

    seen.push_back(node);

    std::vector<size_t> &list = byPerson[node];

    if (list.empty())
    {
      order.push_back(node);
    }

    list.push_back(index);
  }

  events.push_back(std::move(event));
}

void Chronicle::forgetBefore(const uint32_t tick)
{
  // Events arrive in tick order, so what is too old is a prefix.
  size_t first = 0u;

  while (first < events.size() && events[first].tick < tick)
  {
    ++first;
  }

  if (first == 0u)
  {
    return;
  }

  if (first == events.size())
  {
    events.clear();
    byPerson.clear();
    order.clear();

    return;
  }

  events.erase(events.begin(), events.begin() + static_cast<long>(first));

  // Every surviving index shifts down by the same amount, and a person left
  // with nothing leaves the cast.
  std::vector<uint32_t> survivors;

  for (const uint32_t person : order)
  {
    std::vector<size_t> &list = byPerson[person];
    std::vector<size_t> kept;

    for (const size_t index : list)
    {
      if (index >= first)
      {
        kept.push_back(index - first);
      }
    }

    if (kept.empty())
    {
      byPerson.erase(person);

      continue;
    }

    list.swap(kept);
    survivors.push_back(person);
  }

  order.swap(survivors);
}

void Chronicle::build(const std::vector<FiredEvent> &log, const compiler::CompiledProgram &program, const Narrator &narrator)
{
  events.clear();
  byPerson.clear();
  order.clear();
  offered = 0u;
  events.reserve(remembered.empty() ? log.size() : 0u);

  for (size_t i = 0u; i < log.size(); ++i)
  {
    const FiredEvent &fired = log[i];

    ChronicleEvent event;

    event.tick = fired.tick;
    event.rule = fired.rule;
    event.kind = fired.kind;
    event.bound = fired.bound;
    event.source = i;
    event.title = fired.rule < program.ruleNames.size() ? ruleTitleOf(program.ruleNames[fired.rule]) : std::string();
    event.category = narrator.categoryOf(event.title);
    event.weight = narrator.weightOf(event.title);

    add(std::move(event));
  }
}

const std::vector<size_t> &Chronicle::involving(const uint32_t person) const
{
  static const std::vector<size_t> none;

  const auto found = byPerson.find(person);

  return found == byPerson.end() ? none : found->second;
}

bool Chronicle::inWindow(const ChronicleEvent &event, const uint32_t before, const Window window) const
{
  if (event.tick >= before)
  {
    return false;
  }

  return window.unbounded() || before - event.tick <= window.weeks;
}

bool Chronicle::took(const uint32_t person, const std::string &title, const uint32_t before, const Window window) const
{
  return last(person, title, before, window) != ~size_t(0);
}

size_t Chronicle::last(const uint32_t person, const std::string &title, const uint32_t before, const Window window) const
{
  const std::vector<size_t> &list = involving(person);

  // Earliest first, so walk back from the end and stop at the first hit.
  for (size_t i = list.size(); i-- > 0u;)
  {
    const ChronicleEvent &event = events[list[i]];

    if (event.tick >= before)
    {
      continue;
    }

    if (!window.unbounded() && before - event.tick > window.weeks)
    {
      break;
    }

    if (event.title == title)
    {
      return list[i];
    }
  }

  return ~size_t(0);
}

bool Chronicle::tookTogether(const uint32_t a, const uint32_t b, const std::string &title, const uint32_t before, const Window window) const
{
  if (a == b)
  {
    return took(a, title, before, window);
  }

  // Walk the shorter list and test membership of the other.
  const std::vector<size_t> &first = involving(a);
  const std::vector<size_t> &second = involving(b);
  const std::vector<size_t> &shorter = first.size() <= second.size() ? first : second;
  const uint32_t other = first.size() <= second.size() ? b : a;

  for (size_t i = shorter.size(); i-- > 0u;)
  {
    const ChronicleEvent &event = events[shorter[i]];

    if (event.tick >= before)
    {
      continue;
    }

    if (!window.unbounded() && before - event.tick > window.weeks)
    {
      break;
    }

    if (event.title == title && std::find(event.bound.begin(), event.bound.end(), other) != event.bound.end())
    {
      return true;
    }
  }

  return false;
}

uint32_t Chronicle::count(const uint32_t person, const std::string &title, const uint32_t before, const Window window) const
{
  uint32_t total = 0u;

  for (const size_t index : involving(person))
  {
    const ChronicleEvent &event = events[index];

    if (inWindow(event, before, window) && event.title == title)
    {
      ++total;
    }
  }

  return total;
}

} // namespace graphrewrite::runtime
