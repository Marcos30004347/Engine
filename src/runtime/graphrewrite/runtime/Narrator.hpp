#pragma once

// Turns rewrite firings into prose.
//
// The narration file pairs rule titles with sentence templates:
//
//   they-marry: {A} and {B} were married.
//
// Placeholders name the rule's OWN variables. `{A}` is the name; `{A:he}`,
// `{A:him}`, `{A:his}`, `{A:man}`, `{A:son}`, `{A:husband}` pick a word by the
// node's gender, read from the graph at narration time. A capitalised form -
// `{A:He}` - capitalises the word.
//
// Rules that fire close together and touch the same person are usually one
// event told several times. A CHAIN says so:
//
//   chain he-kills-the-husband + the-widow-blames-the-killer within 3:
//     {1.A} killed {1.C} for love of {1.B}, and {2.B} came to hate {1.A:him}.
//
// Any number of rules, joined by `+`. Within a window of ticks from the first
// firing, each later rule's firing must share a node with one already in the
// chain; the whole thing is told once, with the chain's sentence, at the tick
// the last part landed. `{1.A}` is the first rule's A, `{3.C}` the third's.
//
// A row that was scheduled, released when its time came, or cancelled then, is
// told through `scheduled:<rule>`, `released:<rule>` and `cancel:<rule>`
// templates; without one it is silent.
//
// A rule with no template falls back to reading out the edges it inserted -
// "Cenric killed Aldric." - except for types listed as silent:
//
//   silent ? is alive
//
// which is how bookkeeping like `is alive` and `lives in` stays out of the text.
//
// Four more line kinds serve the CHRONICLER, which composes months rather
// than listing weeks (see Chronicler.hpp):
//
//   weight 30 the-vows-are-said death-comes    how much a firing counts when
//                                              a month has more than it can
//                                              tell; default 10 for an
//                                              `important` rule, else 1
//   category death: death-comes old-age        what a dropped firing is
//                                              counted as
//   summary death: a death / {n} deaths        how those counts read
//   sub anger: fury | a cold rage              `<anger>` in any sentence
//                                              expands to one of these
//
// They cost nothing to a caller that only wants sentences: the narrator's
// own output is unchanged except that `<subs>` in a template expand.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/frontend/Diagnostics.hpp"
#include "graphrewrite/runtime/Grammar.hpp"
#include "graphrewrite/runtime/NameGenerator.hpp"
#include "graphrewrite/runtime/RewriteEvent.hpp"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace graphrewrite::runtime {

struct FiredEvent
{
  uint32_t tick = 0u;
  uint32_t rule = 0u;
  std::vector<uint32_t> bound; // matched vertices, then the ones THEN created
  RewriteEventKind kind = RewriteEventKind::Applied;
};

// One rendered sentence and the firings that went into it - one for a plain
// rule, several for a chain. A composer that wants to group, rank or answer
// back in dialogue needs the events, not only the prose.
struct NarratedSentence
{
  std::string text;
  std::vector<size_t> events; // indices into the vector passed to narrate()
};

struct NarratedTick
{
  uint32_t tick = 0u;
  std::vector<std::string> sentences;

  // The same sentences, in the same order, with their events.
  std::vector<NarratedSentence> detail;
};

class Narrator
{
public:
  using GenderLookup = std::function<Gender(uint32_t id)>;

  bool load(const std::string &text, const std::string &path, DiagnosticSink &sink);
  bool loadFile(const std::string &path, DiagnosticSink &sink);

  // Renders every event. Chains are resolved first, then the rest are told one
  // by one; output is grouped by tick, in tick order.
  std::vector<NarratedTick> narrate(const std::vector<FiredEvent> &events, const compiler::CompiledProgram &program,
                                    NameTable &names, const GenderLookup &genderOf) const;

  // Incremental: renders every chain and event whose telling lands at or
  // before `uptoTick`, marks those events consumed, and leaves the rest for a
  // later call. Events after uptoTick take part in chain matching (a chain is
  // told at its LAST part's tick), which is why the caller holds back a
  // window of ticks: nothing told before uptoTick can change once every event
  // within the longest chain window after it is known.
  std::vector<NarratedTick> narrateUpTo(const std::vector<FiredEvent> &events, uint32_t uptoTick,
                                        const compiler::CompiledProgram &program, NameTable &names, const GenderLookup &genderOf,
                                        std::vector<bool> &consumed) const;

  // The longest `within N` of any chain; the hold-back a live narrator needs.
  uint32_t longestChainWindow() const;

  // Tell only the rules the narration file lists as `important <rule>` (a
  // released/cancelled schedule counts under its rule). Everything else is
  // dropped before chains are matched, so a chain of important rules still
  // forms and one with an unimportant part does not.
  void setOnlyImportant(bool only)
  {
    onlyImportant = only;
  }

  bool isImportant(const std::string &ruleTitle) const
  {
    return important.count(ruleTitle) != 0u;
  }

  // How much a firing of this rule counts when a month has more events than
  // it can tell. `weight N <rule>...` sets it; without a line, an important
  // rule is worth `importantWeight` and everything else 1.
  uint32_t weightOf(const std::string &ruleTitle) const;

  // What `category <name>: <rule>...` files the rule under, or empty.
  std::string categoryOf(const std::string &ruleTitle) const;

  // "two deaths" - how `summary <category>: one / many` reads `n` of them.
  // Empty when the category has no summary line, which is how a category is
  // left out of the month's closing sentence.
  std::string summaryPhrase(const std::string &category, uint32_t n) const;

  // The `<name>` substitutions the file's `sub` lines declared. A dialogue
  // book shares this object, so a sub written in either file is available to
  // both and there is one vocabulary for the whole story.
  Grammar &grammar()
  {
    return subs;
  }

  const Grammar &grammar() const
  {
    return subs;
  }

  // The key the grammar is rolled with, so a run is reproducible.
  void setGrammarSeed(const uint64_t seed)
  {
    grammarSeed = seed;
  }

  // Edge types the file marks `incidental <pattern>`: real edges that are
  // nonetheless not a reason to tell a reader one thing led to another.
  // `keeps the peace` comes back to a man the moment his plot resolves,
  // which links every killing to every later plot in the village.
  std::vector<compiler::EdgeTypeId> incidentalTypes(const compiler::CompiledProgram &program) const;

  // Every rule title the file names - in a template, a chain, an `important`,
  // a `weight` or a `category`. A `scheduled:`/`released:`/`cancel:` prefix
  // is left on. Used to check the file against the compiled programme: a
  // misspelled rule here never fires and nothing reports it.
  std::set<std::string> namedRules() const;

  size_t importantCount() const
  {
    return important.size();
  }

  // One event, on its own. Used by the tests and by anything that wants live
  // output rather than a post-pass.
  std::string tell(const FiredEvent &event, const compiler::CompiledProgram &program, NameTable &names,
                   const GenderLookup &genderOf) const;

  size_t templateCount() const
  {
    return templates.size();
  }

  size_t chainCount() const
  {
    return chains.size();
  }

private:
  struct Chain
  {
    std::vector<std::string> rules; // in order; two or more
    uint32_t window = 1u;
    std::string sentence;
  };

  std::string render(const std::string &sentence, const std::vector<const FiredEvent *> &parts,
                     const compiler::CompiledProgram &program, NameTable &names, const GenderLookup &genderOf) const;

  std::string fallback(const FiredEvent &event, const compiler::CompiledProgram &program, NameTable &names) const;

  // Runs the grammar over a rendered sentence, keyed off the firing.
  std::string substituted(const std::string &text, const std::vector<const FiredEvent *> &parts) const;

  std::map<std::string, std::string> templates; // rule title -> sentence
  std::vector<Chain> chains;
  std::set<std::string> silent;     // edge type patterns never read out
  std::set<std::string> incidental; // edge type patterns that are not a cause
  std::set<std::string> important; // rule titles the `important` lines name
  std::map<std::string, uint32_t> weights;    // rule title -> weight
  std::map<std::string, std::string> categories; // rule title -> category
  std::map<std::string, std::pair<std::string, std::string>> summaries; // category -> (one, many)
  Grammar subs;
  uint64_t grammarSeed = 1u;
  uint32_t importantWeight = 10u;
  bool onlyImportant = false;
};

// "they-marry#2" -> "they-marry".
std::string ruleTitleOf(const std::string &variantName);

} // namespace graphrewrite::runtime
