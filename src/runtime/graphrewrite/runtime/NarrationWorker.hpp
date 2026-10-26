#pragma once

// Narrates on its own thread, a week behind the simulation.
//
// The GPU hands the CPU a batch of fired rows per week. Rendering them - chain
// matching, names, gender - is CPU work with nothing to say to the matcher, so
// it runs here while the next week is being simulated. Chains look across a
// window of weeks, so the worker holds back the newest `longestChainWindow`
// weeks and narrates everything older; finish() narrates the rest.
//
// Nothing here touches the live graph. Gender, which the narrator reads per
// node, arrives with each batch as a snapshot taken on the simulation thread.
//
// With `setChronicle` the worker composes months instead of listing weeks
// (see Chronicler.hpp): every week goes into a bounded chronicle, the
// narrated ticks go into a MonthStream, and a month is composed and appended
// once the clock is far enough past it that nothing can change it. The
// memory this holds is the month budget, not the month - which is what makes
// it usable on a world of two hundred thousand, where listing every week
// produced thirty megabytes of prose for one year.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/runtime/Chronicler.hpp"
#include "graphrewrite/runtime/DialogueBook.hpp"
#include "graphrewrite/runtime/NameGenerator.hpp"
#include "graphrewrite/runtime/Narrator.hpp"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace graphrewrite::runtime {

class NarrationWorker
{
public:
  NarrationWorker(const Narrator &narrator, const compiler::CompiledProgram &program, uint32_t nameSeed,
                  const std::vector<std::string> &worldNames, uint32_t dayBase, uint32_t startWeek, bool byMonth = false);
  ~NarrationWorker();

  // What the narrator needs to know about a node, since the thread never
  // touches the graph: who they are and which family they belong to.
  struct NodeFacts
  {
    uint32_t id = 0u;
    Gender gender = Gender::Unknown;
    uint32_t family = ~uint32_t(0);
  };

  // Compose months in paragraphs rather than listing weeks. `book` may be
  // null, in which case nobody speaks. Call before the first push.
  void setChronicle(ChronicleSettings settings, const DialogueBook *book);

  // One week's events, with those facts for every node they mention.
  void push(uint32_t tick, std::vector<FiredEvent> events, std::vector<NodeFacts> facts);

  // Narrates whatever is still held back and stops the thread.
  void finish();

  // The story so far (complete after finish()).
  std::string text();

  uint32_t weeksNarrated() const
  {
    return narratedWeeks;
  }

  double busySeconds() const
  {
    return seconds;
  }

private:
  struct Batch
  {
    uint32_t tick = 0u;
    std::vector<FiredEvent> events;
    std::vector<NodeFacts> facts;
  };

  void loop();
  void narrateUpTo(uint32_t uptoTick);

  Gender genderLookup(uint32_t id) const
  {
    const auto found = genders.find(id);

    return found == genders.end() ? Gender::Unknown : found->second;
  }

  uint32_t familyLookup(uint32_t id) const
  {
    const auto found = families.find(id);

    return found == families.end() ? ~uint32_t(0) : found->second;
  }

  const Narrator &narrator;
  const compiler::CompiledProgram &program;
  NameTable names;
  std::map<uint32_t, Gender> genders;
  std::map<uint32_t, uint32_t> families;
  uint32_t startWeek;
  uint32_t window;
  bool byMonth;
  uint32_t lastMonth = ~0u;

  std::unique_ptr<MonthStream> stream; // set by setChronicle
  std::vector<FiredEvent> held; // not yet told
  std::string out;
  uint32_t narratedWeeks = 0u;
  double seconds = 0.0;

  std::mutex mutex;
  std::condition_variable wake;
  std::vector<Batch> inbox;
  bool done = false;
  std::thread thread;
};

} // namespace graphrewrite::runtime
