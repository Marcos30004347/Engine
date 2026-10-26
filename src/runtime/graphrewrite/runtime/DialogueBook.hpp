#pragma once

// What people say, chosen by what has already happened to them.
//
// A dialogue block hangs off a rule. When that rule fires, its clauses are
// tried in order and the first whose conditions hold against the chronicle
// supplies the lines:
//
//   dialogue the-jilted-man-plots-against-the-lover chance 60:
//     when B met X in the-vows-are-said:
//       B to X: "You had her word before God, {X}. You will not keep it long."
//     when B in the-jilted-nurse-a-grudge within 100:
//       B: "I have not forgotten. {X} will not have {A}."
//     else:
//       B: "It should have been me."
//
// The roles are the RULE's own variable names, so `{X}` in the line and `X` in
// the condition are the same person, and `{B:his}` works as it does in
// narration. `<subs>` from the shared grammar work too.
//
// A condition is one of
//
//   <Role> in <rule-title>              took part in one, ever, before now
//   <Role> met <Role> in <rule-title>   both in the SAME one
//   <Role> in <rule-title> twice        ... at least twice (or `N times`)
//
// each optionally followed by `within <N>` weeks, written as `when` or
// `unless`, and joined with `and`. `else:` always holds. `chance N` on the
// header is the percent of firings that say anything at all - deterministic
// per event, so a run repeats.
//
// A block may hang off a CHAIN instead of a single rule - the narrator has
// already folded those firings into one sentence, and the dialogue that
// belongs to a whole arc is not the dialogue that belongs to either half:
//
//   dialogue chain a-killer-confesses + the-priest-tells-the-son chance 70:
//     when 2.X in blood-for-blood:
//       1.A to 1.P: "You will keep this to yourself, Father."
//       2.P to 2.X: "I did not keep it."
//
// A role may name its part - `2.X` is the second rule's X - and an unprefixed
// role means the first. That holds in conditions as well as in spoken lines.
//
// One more condition asks about CAUSE rather than participation:
//
//   when because the-note-comes-due
//
// which holds when something of that rule, involving one of these people,
// happened earlier AND the programme itself says that rule can bring this
// one about. The causal link is read off the rules (see ConsequenceGraph),
// never declared here, so a line written this way cannot claim a connection
// the world does not actually have.
//
// A clause may carry several speaker lines, which makes an exchange; they are
// returned in order and the composer decides how many it has room for.
//
// The attribution verb comes from the grammar, and there are three of them
// because they are not interchangeable: `<said>` opens a line spoken to
// nobody in particular, `<said-to>` one aimed at someone (so it must be a
// verb that takes "to <name>"), and `<answered>` every line after the first.
// A missing sub falls back to the plain word.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/frontend/Diagnostics.hpp"
#include "graphrewrite/runtime/Chronicle.hpp"
#include "graphrewrite/runtime/ConsequenceGraph.hpp"
#include "graphrewrite/runtime/Grammar.hpp"
#include "graphrewrite/runtime/Narrator.hpp"

#include <set>
#include <string>
#include <vector>

namespace graphrewrite::runtime {

class DialogueBook
{
public:
  bool load(const std::string &text, const std::string &path, DiagnosticSink &sink);
  bool loadFile(const std::string &path, DiagnosticSink &sink);

  // The `sub` lines the dialogue file declared. Point this at the narrator's
  // grammar before loading to pool one vocabulary across both files.
  void setGrammar(Grammar *shared)
  {
    grammar = shared;
  }

  void setSeed(const uint64_t value)
  {
    seed = value;
  }

  bool hasLinesFor(const std::string &ruleTitle) const;

  // Every rule title some condition in the book names. A chronicle told to
  // remember only these answers every question this book can ask and keeps
  // nothing else - which is what makes a world of two hundred thousand
  // affordable.
  std::set<std::string> referencedTitles() const;

  // The rule each block is hung off. A name here that is not a rule means
  // the block never fires and nothing says so, which is why the suite checks
  // these against the compiled programme.
  std::set<std::string> blockTitles() const;

  // The longest `within N` of any condition, or 0 when some condition is
  // unbounded and the chronicle must keep its whole history.
  uint32_t longestWindow() const;

  size_t blockCount() const
  {
    return blocks.size();
  }

  // The spoken lines for one firing, already rendered - "Bertwald said to
  // Cenric, \"...\"". Empty when the rule has no block, no clause held, or
  // the chance roll went the other way. Conditions look strictly back from
  // the firing's own tick, so an event is never its own evidence.
  std::vector<std::string> speak(const FiredEvent &event, const Chronicle &chronicle, const compiler::CompiledProgram &program,
                                 NameTable &names, const Narrator::GenderLookup &genderOf,
                                 const ConsequenceGraph *causes = nullptr) const;

  // The lines for one NARRATED SENTENCE, which may be a folded chain. A
  // chain block matching every part in order wins; failing that each part is
  // offered to its own rule's block, and the first that speaks is used.
  std::vector<std::string> speak(const std::vector<const FiredEvent *> &parts, const Chronicle &chronicle,
                                 const compiler::CompiledProgram &program, NameTable &names,
                                 const Narrator::GenderLookup &genderOf, const ConsequenceGraph *causes = nullptr) const;

private:
  struct Condition
  {
    bool negated = false;
    bool causal = false; // `because <rule>`: no role, asks the cause graph
    std::string role;
    std::string other; // `met` form; empty otherwise
    std::string title;
    uint32_t atLeast = 1u;
    size_t part = 0u;      // which firing of a chain the role belongs to
    size_t otherPart = 0u;
    Window window = Window::any();
  };

  struct Line
  {
    std::string speaker;
    std::string listener; // may be empty
    size_t speakerPart = 0u;
    size_t listenerPart = 0u;
    std::string text;
  };

  struct Clause
  {
    bool always = false; // `else:`
    std::vector<Condition> conditions;
    std::vector<Line> lines;
  };

  struct Block
  {
    std::string title;                    // single-rule blocks
    std::vector<std::string> chainRules;  // chain blocks; empty otherwise
    uint32_t chance = 100u;
    std::vector<Clause> clauses;
  };

  bool parseCondition(const std::string &text, const std::string &path, uint32_t lineNumber, DiagnosticSink &sink, Clause &into) const;
  bool holds(const Clause &clause, const std::vector<const FiredEvent *> &parts, const Chronicle &chronicle,
             const compiler::CompiledProgram &program, const ConsequenceGraph *causes) const;

  // The lines of the first clause that holds, rendered. Empty if none does.
  std::vector<std::string> perform(const Block &block, const std::vector<const FiredEvent *> &parts, const Chronicle &chronicle,
                                   const compiler::CompiledProgram &program, NameTable &names,
                                   const Narrator::GenderLookup &genderOf, const ConsequenceGraph *causes) const;

  std::vector<Block> blocks;
  Grammar *grammar = nullptr;
  Grammar owned;
  uint64_t seed = 1u;
};

} // namespace graphrewrite::runtime
