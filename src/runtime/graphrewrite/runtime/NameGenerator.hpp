#pragma once

// Names for nodes.
//
// The facts file names the starting cast. Everyone born during a run is a bare
// vertex id until something has to say their name, at which point one is made
// up - deterministically, from the run seed and the id, so the same run always
// tells the same story with the same people in it.
//
// Naming is LAZY on purpose: a newborn's gender edge is inserted by the same
// rewrite that creates the node, so by the time the narrator first mentions the
// child, the graph already knows whether to reach for a masculine or feminine
// ending.

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace graphrewrite::runtime {

enum class Gender
{
  Unknown,
  Masculine,
  Feminine,
};

// Syllable-built, capitalised, two to three syllables, with an ending chosen by
// gender. Pure function of its inputs.
std::string generateName(uint32_t seed, uint32_t id, Gender gender);

// A name as the facts file writes it -> a name as the story reads it.
//
// A world of two hundred thousand needs two hundred thousand distinct TOKENS,
// and a token is one word because the loader matches a line word by word. So
// a generated cast writes `Aelfric_Wulfing__Hartley`: an underscore is a
// space, and everything from a DOUBLE underscore on is there only to keep the
// token unique and is not read out. That is what replaced numbering people
// Wulfric2, Wulfric3.
std::string displayName(std::string token);

class NameTable
{
public:
  // Answers the gender of a node, consulted the first time it needs a name.
  using GenderLookup = std::function<Gender(uint32_t id)>;

  // The node of a person's family, or ~0u. A name generated for someone born
  // during the run takes their family's name after their own, so a child of
  // the Wulfings is Cenwine Wulfing and not a bare given name.
  using FamilyLookup = std::function<uint32_t(uint32_t id)>;

  explicit NameTable(uint32_t seed = 0u);

  void setName(uint32_t id, std::string name);
  void setSeed(uint32_t seed);
  void setGenderLookup(GenderLookup lookup);
  void setFamilyLookup(FamilyLookup lookup);

  // Generates and remembers a name the first time an unnamed id is asked for.
  const std::string &nameOf(uint32_t id);

  // Ids at or above `base` are not people: they are named by `namer` instead
  // of generated. The story uses this for day nodes - "day 12".
  using RangeNamer = std::function<std::string(uint32_t id)>;

  void setRangeNamer(uint32_t base, RangeNamer namer);

  bool has(uint32_t id) const;

  const std::map<uint32_t, std::string> &all() const
  {
    return names;
  }

private:
  uint32_t seed;
  std::map<uint32_t, std::string> names;
  GenderLookup genderOf;
  FamilyLookup familyOf;
  uint32_t rangeBase = ~uint32_t(0);
  RangeNamer rangeNamer;
};

} // namespace graphrewrite::runtime
