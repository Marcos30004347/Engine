#pragma once

// Month by month, in paragraphs, with people speaking.
//
// The narrator produces one sentence per event and the runner prints them
// under a heading, semicolon-separated, in the order they fired. That reads
// as a changelog, and in a world of hundreds it is unreadable: a month is
// hundreds of sentences with nothing relating them.
//
// The chronicler composes instead. For each month:
//
//   1. take the month's narrated sentences, chains already folded;
//   2. rank them by the weight the narration file gives their rule and keep a
//      budget of them; everything dropped is counted into its category and
//      becomes the month's closing sentence - "Elsewhere: two deaths and four
//      quarrels" - so nothing vanishes, it stops being told at length;
//   3. BLOCK the kept ones. Sharing a person is not enough to be one
//      narrative: a man's confession, his daughter's birth and the plot
//      against him are three things that happen to share him, and run
//      together they read as one rambling paragraph. A sentence joins a
//      block when it is CAUSALLY linked to something already in it, or when
//      it shares at least two people with it; one person in common and no
//      cause starts a new block;
//   4. order the blocks - the ones continuing a story the reader saw last
//      month first, then by weight - and keep blocks that share people
//      ADJACENT, so a man's several strands sit together without being run
//      into one another. Within a block, tick order, joined with `<then>`
//      or, where the rules say one thing brought the other about,
//      `<therefore>`;
//   5. hang dialogue off the heaviest events that have a line for it.
//
// Step 4 asks one more thing of a pair of sentences than "are these the same
// people": whether the programme itself says the later rule can be brought
// about by the earlier one (see ConsequenceGraph). When it can, the sentence
// is joined with a consequence - "and so", "which is why" - rather than a
// plain "Then", and a paragraph stops being a list of things that happened
// to one man and becomes a run of cause. Nothing about that is declared; it
// is read off the rules' own edges.
//
// Dialogue conditions look at the whole chronicle, not the month, which is
// the point of keeping one: a line in year six can answer a wedding in year
// one.
//
// There are two drivers over one algorithm. `Chronicler` is the batch one:
// hand it a whole run's log and it gives you every month. `MonthStream` is
// the streaming one, for a world too large to hold: weeks arrive, months
// come out behind them, and nothing but the budget is ever buffered - a
// month keeps only the `eventsPerMonth` heaviest sentences it has seen so
// far and a count of everything it let go, which is exactly the set the
// batch driver would have ended with.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/runtime/Chronicle.hpp"
#include "graphrewrite/runtime/ConsequenceGraph.hpp"
#include "graphrewrite/runtime/DialogueBook.hpp"
#include "graphrewrite/runtime/Narrator.hpp"

#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace graphrewrite::runtime {

struct ChronicleSettings
{
  uint32_t seed = 1u;

  // The week the story's calendar starts at, as StorySettings spells it.
  uint32_t startWeek = 1000u * 52u;

  // Ids at or above this are not people (week nodes).
  uint32_t nodeCeiling = 1u << 24u;

  // How many events a month tells at length before the rest become counts.
  // Zero tells all of them.
  uint32_t eventsPerMonth = 8u;

  // At most this many of any ONE rule in a month. Weight alone is the wrong
  // rank at scale: a world of two hundred thousand has thousands of sudden
  // illnesses in a month, they are the heaviest rule there is, and a month
  // composed on weight alone was twenty identical sentences about twenty
  // strangers dying. A cap buys variety at no cost to the summary, which
  // still counts every one of them. Zero lifts it, and so does an
  // eventsPerMonth of zero - "tell everything" means everything.
  uint32_t perRuleCap = 2u;

  // How many of a month's events may be answered with dialogue.
  uint32_t dialoguePerMonth = 2u;

  // A paragraph beyond this many sentences is split, so one runaway thread in
  // a busy month does not become a wall.
  uint32_t sentencesPerParagraph = 5u;

  // How many rules a chain of cause may run through before the connective
  // stops claiming one. One is "that rule directly produces an edge this
  // rule matches"; past three the relation is dense enough to be worthless.
  uint32_t causeSteps = 1u;

  // People in common needed to put two sentences in one block when there is
  // no causal link between them. One is too few - everybody in a village
  // shares somebody - so the default is two.
  uint32_t sharedToBlock = 2u;

  // How many months a person stays "recent", for deciding that a block
  // continues a story the reader is already following. Zero turns it off.
  uint32_t continuityMonths = 2u;
};

// One narrated sentence, resolved away from the narrator's per-call indices
// so it can outlive the batch it came from.
struct TellingSentence
{
  uint32_t tick = 0u;
  std::string text;
  uint32_t rule = 0u;   // the heaviest part's rule, for the cause graph
  std::string title;    // the rule it came from, for the per-rule cap
  std::string category; // what it is counted as when it is dropped
  std::vector<FiredEvent> events; // the firings that went into it
  std::vector<uint32_t> people;
  uint32_t weight = 1u;
  uint64_t order = 0u; // narration order, the tie-break that keeps runs stable
};

// One run of sentences that are about the same thing. A month is a handful
// of these; unrelated people never share one.
struct ChronicledBlock
{
  std::vector<std::string> paragraphs; // usually one; split when very long
  std::vector<uint32_t> cast;
  uint32_t weight = 0u;
  uint32_t week = 0u;

  // Shares a person with something told in a recent month: the reader is
  // already following this, so it leads.
  bool continues = false;
};

// One month, composed.
struct ChronicledMonth
{
  uint32_t key = 0u; // year * 12 + month, as StoryRunner::monthPhrase returns
  uint32_t week = 0u;
  std::string heading;

  // Every block's paragraphs in order, with the closing count last: the flat
  // view the text writer uses.
  std::vector<std::string> paragraphs;

  std::vector<ChronicledBlock> blocks;
  std::string summary; // "Elsewhere that month: ..."; empty if nothing was dropped

  uint32_t told = 0u;
  uint32_t summarised = 0u;
};

// A month being assembled, before it is composed.
struct PendingMonth
{
  uint32_t key = 0u;
  uint32_t week = 0u;
  std::string heading;

  // At most `eventsPerMonth`, the heaviest seen so far. Dropping the lightest
  // when the budget is full gives exactly the set a batch pass would keep,
  // because the ranking is (weight, order) and both are known on arrival.
  std::vector<TellingSentence> kept;

  std::map<std::string, uint32_t> dropped;    // category -> count
  std::map<std::string, uint32_t> keptOfRule; // rule title -> how many held
  uint32_t droppedOther = 0u;
  uint32_t summarised = 0u;
};

// The streaming driver. Feed it weeks in order; take months off the back.
class MonthStream
{
public:
  MonthStream(const Narrator &narrator, const compiler::CompiledProgram &program, ChronicleSettings settings);

  void setDialogue(const DialogueBook *book);

  // The long-term memory the dialogue conditions read. Bound it before the
  // first week arrives.
  Chronicle &chronicle()
  {
    return memory;
  }

  // Every firing of a week, told or not, into the chronicle. Call this for a
  // week BEFORE offering that week's narration, and never after: a condition
  // asks strictly before its own tick, so same-week events cannot be
  // evidence for each other either way.
  void remember(const std::vector<FiredEvent> &events);

  // One narrated tick. `pool` is the vector the narrator was handed, which
  // `tick.detail` indexes.
  void offer(const NarratedTick &tick, const std::vector<FiredEvent> &pool);

  // Months that can no longer change - every month wholly before the week
  // `safeTick`. Composing is what renders dialogue, so this is where the
  // names and genders are needed.
  std::vector<ChronicledMonth> takeCompleted(uint32_t safeTick, NameTable &names, const Narrator::GenderLookup &genderOf);

  // Everything still held, at the end of a run.
  std::vector<ChronicledMonth> takeAll(NameTable &names, const Narrator::GenderLookup &genderOf);

private:
  static bool heavier(const TellingSentence &a, const TellingSentence &b);

  // Offer one sentence to a month: held, or held in place of a lighter one,
  // or counted into its category.
  void admit(PendingMonth &month, TellingSentence sentence) const;
  void hold(PendingMonth &month, TellingSentence sentence) const;
  void drop(PendingMonth &month, const TellingSentence &sentence) const;

  ChronicledMonth composeMonth(PendingMonth &month, NameTable &names, const Narrator::GenderLookup &genderOf);

  const Narrator &narrator;
  const compiler::CompiledProgram &program;
  const DialogueBook *dialogue = nullptr;
  ChronicleSettings settings;

  Chronicle memory;
  ConsequenceGraph causes;
  std::map<uint32_t, PendingMonth> months; // key -> the month being assembled

  // Who was told about lately, and in which month, for the continuity test.
  std::map<uint32_t, uint32_t> lastToldIn;

  uint64_t order = 0u;
  uint32_t forgetHorizon = 0u; // 0: never forget
};

class Chronicler
{
public:
  Chronicler(const Narrator &narrator, const compiler::CompiledProgram &program, ChronicleSettings settings);

  // Optional. Without one, nobody speaks.
  void setDialogue(const DialogueBook *book)
  {
    dialogue = book;
  }

  // Composes every event into months. `names` and `genderOf` are the same
  // ones the narrator would use.
  std::vector<ChronicledMonth> compose(const std::vector<FiredEvent> &events, NameTable &names,
                                       const Narrator::GenderLookup &genderOf) const;

  // The months as text, headings and blank lines included.
  std::string text(const std::vector<FiredEvent> &events, NameTable &names, const Narrator::GenderLookup &genderOf) const;

  static std::string joinParagraph(const std::vector<std::string> &sentences);

  // One month, headed and spaced, the way text() writes it.
  static void appendMonth(std::ostream &out, const ChronicledMonth &month);

private:
  const Narrator &narrator;
  const compiler::CompiledProgram &program;
  const DialogueBook *dialogue = nullptr;
  ChronicleSettings settings;
};

} // namespace graphrewrite::runtime
