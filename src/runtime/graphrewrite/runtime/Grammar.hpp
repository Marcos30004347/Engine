#pragma once

// `<name>` expands to one of the alternatives of a `sub`.
//
//   sub anger:  fury *3 | a cold rage | something that would not cool
//   sub said:   said | answered | put it
//
// Alternatives may contain further `<names>`; expansion recurses to a fixed
// depth and then stops rather than looping. `*N` weights an alternative: the
// line above reaches for "fury" three times as often as either of the others.
//
// The choice is a HASH, not a stream: `expand(text, key)` with the same key
// always yields the same words, and the key a caller passes is built from the
// run seed and the event, so a story is reproducible and two firings of one
// rule in one month do not pick the same word. Nothing here holds state, so a
// narration worker on its own thread can share one grammar with the composer.
//
// An unknown `<name>` is left in the text as written - a typo in a narration
// file shows up in the prose instead of silently emptying a sentence.

#include "graphrewrite/frontend/Diagnostics.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace graphrewrite::runtime {

class Grammar
{
public:
  // True if the line is a `sub` line - the narration and dialogue loaders use
  // this to hand their `sub` lines here and parse the rest themselves.
  static bool isSubLine(const std::string &line);

  // Parses one `sub name: a | b *2 | c`. Returns false and reports on `sink`
  // if the line is malformed. A second `sub` for a name adds to it.
  bool addLine(const std::string &line, const std::string &path, uint32_t lineNumber, DiagnosticSink &sink);

  void add(const std::string &name, const std::vector<std::string> &alternatives);

  // Substitutes every `<name>`. `key` picks the alternatives.
  std::string expand(const std::string &text, uint64_t key) const;

  bool has(const std::string &name) const
  {
    return subs.count(name) != 0u;
  }

  size_t size() const
  {
    return subs.size();
  }

  // The alternatives of a sub, weights expanded out. Empty if unknown.
  const std::vector<std::string> &alternativesOf(const std::string &name) const;

private:
  std::string expandAt(const std::string &text, uint64_t key, uint32_t depth, uint32_t &counter) const;

  std::map<std::string, std::vector<std::string>> subs;
};

// The key a caller builds from a seed and whatever identifies the moment.
uint64_t grammarKey(uint64_t seed, uint64_t a, uint64_t b = 0u, uint64_t c = 0u);

} // namespace graphrewrite::runtime
