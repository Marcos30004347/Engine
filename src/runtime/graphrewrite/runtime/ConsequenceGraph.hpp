#pragma once

// Which rule can bring which other rule about, read off the programme itself.
//
// A rule PRODUCES an edge type when its THEN inserts one its WHEN never
// matched - a genuinely new edge, not one of the many a rule has to restate
// to keep. A rule CONSUMES a type when its WHEN matches it. If `poison`
// produces `? killed ?` and `the-widow-learns-who-killed-him` matches it,
// then the second can only have happened because something like the first
// did, and the text is allowed to say so.
//
// Nothing here is declared. The `not in the WHEN` test is what makes it
// useful rather than a graph where everything causes everything: almost
// every rule restates `is alive`, so almost no rule produces it, and the
// bookkeeping types drop out on their own.
//
// A link between two RULES is a possibility. A link between two FIRINGS also
// wants the cause to come first and the two to share a person, which is what
// `linked` adds - and that is the test worth putting in front of a reader,
// because it is the one that means "this happened to these people because
// that happened to them".

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/runtime/Chronicle.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace graphrewrite::runtime {

class ConsequenceGraph
{
public:
  // `incidental` edge types are left out of the links entirely: they are
  // real edges that are not a reason to say one thing led to another.
  void build(const compiler::CompiledProgram &program, const std::vector<compiler::EdgeTypeId> &incidental = {});

  // Could a firing of `cause` bring one of `effect` about, in one step?
  bool leadsTo(uint32_t cause, uint32_t effect) const;

  // ... or through a run of up to `maxSteps` rules. This is the chain of
  // consequences: the crown falls vacant, a pretender takes it, a claimant
  // plots, a man dies, his son swears. Depth-limited on purpose - the full
  // closure of a world that feeds back into itself says everything causes
  // everything, which is true and useless.
  bool leadsToWithin(uint32_t cause, uint32_t effect, uint32_t maxSteps) const;

  // Two FIRINGS: the rules are linked, the cause is not later, and they have
  // someone in common. `nodeCeiling` keeps week nodes from counting as a
  // person in common, which would link half the week.
  bool linked(const ChronicleEvent &cause, const ChronicleEvent &effect, uint32_t nodeCeiling, uint32_t maxSteps = 1u) const;

  // The edge types through which `cause` reaches `effect` directly, for a
  // diagnostic or an explanation.
  std::vector<std::string> linkTypes(uint32_t cause, uint32_t effect, const compiler::CompiledProgram &program) const;

  uint32_t ruleCount() const
  {
    return rules;
  }

  // How many ordered pairs are linked; the density is worth knowing, because
  // a graph that links everything explains nothing.
  size_t directLinks() const;

private:
  bool at(const std::vector<uint64_t> &rows, uint32_t cause, uint32_t effect) const;
  void set(std::vector<uint64_t> &rows, uint32_t cause, uint32_t effect);

  uint32_t rules = 0u;
  uint32_t words = 0u; // 64-bit words per row

  std::vector<uint64_t> direct;  // cause -> effect, one step
  std::vector<uint64_t> withinN; // ... within `closureSteps`
  uint32_t closureSteps = 0u;

  std::vector<std::vector<compiler::EdgeTypeId>> produced; // per rule
  std::vector<std::vector<compiler::EdgeTypeId>> consumed; // per rule
};

} // namespace graphrewrite::runtime
