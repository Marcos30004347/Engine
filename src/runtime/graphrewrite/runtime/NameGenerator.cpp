#include "graphrewrite/runtime/NameGenerator.hpp"

#include "graphrewrite/runtime/Dice.hpp"

namespace graphrewrite::runtime {
namespace {

// Old-English-ish, to sit beside the cast in story.graphfacts.
const char *const onsets[] = {"Ald", "Bert", "Cen", "Dun", "Ead", "Frid", "God", "Hild", "Leof", "Os", "Sig", "Wulf",
                              "Aeth", "Cuth", "Ed", "Ealh", "Wig", "Beorn", "Cyne", "Here"};

const char *const middles[] = {"ric", "wald", "gar", "stan", "gyth", "wyn", "mund", "helm", "burh", "red", "noth", "here",
                               "frith", "laf", "wine", "swith"};

const char *const masculineEnds[] = {"", "ric", "wald", "mund", "helm", "wine", "stan", "here"};
const char *const feminineEnds[] = {"a", "wyn", "gyth", "burh", "swith", "flaed", "thryth", "hild"};
const char *const neutralEnds[] = {"", "a", "wyn", "ric"};

template <size_t N>
const char *pick(const char *const (&table)[N], const uint32_t roll)
{
  return table[roll % static_cast<uint32_t>(N)];
}

} // namespace

std::string displayName(std::string token)
{
  const size_t cut = token.find("__");

  if (cut != std::string::npos)
  {
    token.erase(cut);
  }

  for (char &c : token)
  {
    if (c == '_')
    {
      c = ' ';
    }
  }

  return token;
}

std::string generateName(const uint32_t seed, const uint32_t id, const Gender gender)
{
  // Three independent rolls from the same hash the dice uses: it is already
  // shared with the GPU and already proven stable across runs.
  const uint32_t a = dice::roll(seed ^ 0x6e616d65u, id);
  const uint32_t b = dice::roll(a, id + 1u);
  const uint32_t c = dice::roll(b, id + 2u);

  std::string name = pick(onsets, a);

  // Onset plus ONE more piece. A middle and an ending together made names like
  // Osburhburh and Godgythswith, which read as noise rather than as people.
  // Roughly a third take a middle syllable and a gendered vowel; the rest take a
  // gendered ending.
  const bool useMiddle = (b >> 8u) % 3u == 0u;

  if (useMiddle)
  {
    name += pick(middles, b);
    name += gender == Gender::Feminine ? "a" : "";
  }
  else
  {
    switch (gender)
    {
    case Gender::Masculine:
      name += pick(masculineEnds, c);
      break;
    case Gender::Feminine:
      name += pick(feminineEnds, c);
      break;
    case Gender::Unknown:
      name += pick(neutralEnds, c);
      break;
    }
  }

  // "Hildhild", "Herehere": an onset that already is the ending.
  const size_t half = name.size() / 2u;

  if (half >= 3u && name.size() % 2u == 0u && name.compare(0u, half, name, half, half) == 0)
  {
    name.erase(half);
  }

  return name;
}

NameTable::NameTable(const uint32_t runSeed) : seed(runSeed)
{
}

void NameTable::setName(const uint32_t id, std::string name)
{
  names[id] = displayName(std::move(name));
}

void NameTable::setSeed(const uint32_t runSeed)
{
  seed = runSeed;
}

void NameTable::setGenderLookup(GenderLookup lookup)
{
  genderOf = std::move(lookup);
}

void NameTable::setFamilyLookup(FamilyLookup lookup)
{
  familyOf = std::move(lookup);
}

void NameTable::setRangeNamer(const uint32_t base, RangeNamer namer)
{
  rangeBase = base;
  rangeNamer = std::move(namer);
}

const std::string &NameTable::nameOf(const uint32_t id)
{
  const auto existing = names.find(id);

  if (existing != names.end())
  {
    return existing->second;
  }

  if (rangeNamer && id >= rangeBase)
  {
    return names.emplace(id, rangeNamer(id)).first->second;
  }

  const Gender gender = genderOf ? genderOf(id) : Gender::Unknown;

  std::string name = generateName(seed, id, gender);

  // Whoever they are born to, they are of that family.
  if (familyOf)
  {
    const uint32_t family = familyOf(id);

    if (family != ~uint32_t(0) && family != id)
    {
      const auto known = names.find(family);

      // Never recurse: a family's own name is its given name, nothing more.
      const std::string surname = known != names.end() ? known->second : generateName(seed, family, Gender::Unknown);

      if (!surname.empty())
      {
        name += " " + surname;
      }
    }
  }

  // Two people with one name is a story nobody can follow. Bump the seed until
  // the name is unused; the loop ends because the table is finite.
  for (uint32_t bump = 1u; bump < 64u; ++bump)
  {
    bool taken = false;

    for (const auto &entry : names)
    {
      if (entry.second == name)
      {
        taken = true;

        break;
      }
    }

    if (!taken)
    {
      break;
    }

    name = generateName(seed + bump, id, gender);
  }

  return names.emplace(id, std::move(name)).first->second;
}

bool NameTable::has(const uint32_t id) const
{
  return names.count(id) != 0u;
}

} // namespace graphrewrite::runtime
