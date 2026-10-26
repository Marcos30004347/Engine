#pragma once

// Every firing, kept and indexed by who was in it.
//
// The narrator turns a firing into a sentence and forgets it. A line of
// dialogue cannot work that way: "You had her word before God" is only worth
// saying if the man saying it was at the wedding, and that wedding may be five
// years back. So the chronicle keeps them all - the ones that were told and
// the bookkeeping ones that were not - in tick order, with an index from each
// person to the events they took part in.
//
// The questions it answers are the ones a dialogue condition asks:
//
//   did A ever take part in a `the-vows-are-said`?           took()
//   did A and X take part in the SAME one?                   tookTogether()
//   how many `blood-for-blood` has A been in?                count()
//
// each optionally bounded to a window of weeks before the moment in question,
// because "I have not forgotten" means something different about last month
// than about a grudge from before the speaker's children were born.
//
// BOUNDING. A world of two hundred thousand fires a million and a half times
// a year, and keeping all of that indexed by person is not on. Two limits
// make it affordable, and neither changes an answer:
//
//   setRemembered(titles)  keep only firings of rules something might ask
//                          about - the dialogue book knows which, and it is
//                          a dozen out of a hundred and fifty
//   forgetBefore(tick)     drop what is older than the longest window any
//                          condition names; a question that cannot reach
//                          back that far cannot tell the difference
//
// Unset, both are off and the chronicle keeps everything, which is what a
// village of thirty wants.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/runtime/Narrator.hpp"
#include "graphrewrite/runtime/RewriteEvent.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace graphrewrite::runtime {

struct ChronicleEvent
{
  uint32_t tick = 0u;
  uint32_t rule = 0u;
  RewriteEventKind kind = RewriteEventKind::Applied;
  std::vector<uint32_t> bound;

  std::string title;    // the rule title, variant suffix stripped
  std::string category; // what the narration file files it under, or empty
  uint32_t weight = 1u;

  // Where this event sits in the caller's own event vector, so a composer can
  // map a narrated sentence back to the chronicle and the other way round.
  size_t source = 0u;
};

// A window of weeks before a moment. `any()` is "at any time before".
struct Window
{
  uint32_t weeks = 0u;

  static Window any()
  {
    return Window{0u};
  }

  bool unbounded() const
  {
    return weeks == 0u;
  }
};

class Chronicle
{
public:
  // Ids at or above this are not people - the story binds week nodes into a
  // firing the same way it binds a bride - so they are kept out of the
  // per-person index and out of the cast.
  void setNodeCeiling(const uint32_t ceiling)
  {
    nodeCeiling = ceiling;
  }

  // Only firings of these rule titles are kept. Empty (the default) keeps
  // every rule. Set it before anything is added.
  void setRemembered(std::set<std::string> titles)
  {
    remembered = std::move(titles);
  }

  // Drops every firing before `tick` and compacts the index. Indices handed
  // out by earlier calls are invalidated; nothing holds them across a forget.
  void forgetBefore(uint32_t tick);

  // What the chronicle was offered, before the limits, for a diagnostic.
  uint64_t seen() const
  {
    return offered;
  }

  // Adds one firing, unless its rule is not remembered. Events must arrive
  // in non-decreasing tick order, which is how both the runner's log and the
  // narration worker's batches come.
  void add(ChronicleEvent event);

  // Builds the chronicle from a run's log. Weight and category come from the
  // narration file through the narrator.
  void build(const std::vector<FiredEvent> &events, const compiler::CompiledProgram &program, const Narrator &narrator);

  size_t size() const
  {
    return events.size();
  }

  const ChronicleEvent &at(const size_t index) const
  {
    return events[index];
  }

  const std::vector<ChronicleEvent> &all() const
  {
    return events;
  }

  // The events `person` took part in, earliest first, as indices.
  const std::vector<size_t> &involving(uint32_t person) const;

  // Strictly before `before`, and (for a bounded window) no earlier than
  // `before - window.weeks`.
  bool took(uint32_t person, const std::string &title, uint32_t before, Window window = Window::any()) const;
  bool tookTogether(uint32_t a, uint32_t b, const std::string &title, uint32_t before, Window window = Window::any()) const;
  uint32_t count(uint32_t person, const std::string &title, uint32_t before, Window window = Window::any()) const;

  // The most recent event of that title `person` was in, or ~0u.
  size_t last(uint32_t person, const std::string &title, uint32_t before, Window window = Window::any()) const;

  // Everyone the chronicle has ever seen, in first-appearance order.
  const std::vector<uint32_t> &cast() const
  {
    return order;
  }

private:
  bool inWindow(const ChronicleEvent &event, uint32_t before, Window window) const;

  std::vector<ChronicleEvent> events;
  std::map<uint32_t, std::vector<size_t>> byPerson;
  std::vector<uint32_t> order;
  std::set<std::string> remembered;
  uint64_t offered = 0u;
  uint32_t nodeCeiling = ~uint32_t(0);
};

} // namespace graphrewrite::runtime
