#include "graphrewrite/runtime/Chronicler.hpp"

#include "graphrewrite/runtime/StoryRunner.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace graphrewrite::runtime {
namespace {

// The default connectives, used when the narration file declares no `<then>`.
const char *const defaultThen[] = {"Then", "Soon after,", "Before the month was out,", "That same month,"};

// Used instead when the programme's own rules say the later firing could
// have been brought about by the earlier one.
const char *const defaultTherefore[] = {"And so", "Which is why", "It followed that", "The upshot was that"};

// After a connective the sentence continues, so its first word drops its
// capital - but only if it is a common word. Most sentences here open on a
// name, and "Then aldric" is worse than the capital it would have kept. A
// list is the honest test: anything not on it is taken for a name.
std::string lowerFirst(const std::string &text)
{
  static const char *const common[] = {"The",  "A",    "An",       "Whatever", "Nobody",  "With",    "To",
                                       "It",   "Word", "Forewarned", "Their",  "His",     "Her",     "Both",
                                       "That", "For",  "Neither",  "Everyone", "Someone", "Nothing"};

  if (text.size() < 2u || text[0] < 'A' || text[0] > 'Z')
  {
    return text;
  }

  const size_t space = text.find(' ');
  const std::string first = text.substr(0u, space == std::string::npos ? text.size() : space);

  for (const char *const word : common)
  {
    if (first == word)
    {
      std::string lowered = text;

      lowered[0] = static_cast<char>(lowered[0] - 'A' + 'a');

      return lowered;
    }
  }

  return text;
}

int find(std::vector<int> &parent, const int node)
{
  int root = node;

  while (parent[static_cast<size_t>(root)] != root)
  {
    root = parent[static_cast<size_t>(root)];
  }

  int walk = node;

  while (parent[static_cast<size_t>(walk)] != root)
  {
    const int next = parent[static_cast<size_t>(walk)];

    parent[static_cast<size_t>(walk)] = root;
    walk = next;
  }

  return root;
}

} // namespace

MonthStream::MonthStream(const Narrator &teller, const compiler::CompiledProgram &compiled, ChronicleSettings options)
    : narrator(teller), program(compiled), settings(options)
{
  memory.setNodeCeiling(settings.nodeCeiling);

  // Read off the programme once: which rule can bring which other rule
  // about. The narration file's `incidental` lines take edge types out of
  // it - `keeps the peace` comes back to a man the moment his plot resolves,
  // which is true and is not a reason to tell a reader that one thing led
  // to another.
  causes.build(program, narrator.incidentalTypes(program));
}

void MonthStream::setDialogue(const DialogueBook *book)
{
  dialogue = book;

  if (book == nullptr)
  {
    return;
  }

  // The chronicle only has to answer the questions this book asks, which is
  // a dozen rules out of a hundred and fifty and a bounded stretch of weeks.
  memory.setRemembered(book->referencedTitles());
  forgetHorizon = book->longestWindow();
}

void MonthStream::remember(const std::vector<FiredEvent> &events)
{
  for (const FiredEvent &fired : events)
  {
    ChronicleEvent event;

    event.tick = fired.tick;
    event.rule = fired.rule;
    event.kind = fired.kind;
    event.bound = fired.bound;
    event.title = fired.rule < program.ruleNames.size() ? ruleTitleOf(program.ruleNames[fired.rule]) : std::string();
    event.category = narrator.categoryOf(event.title);
    event.weight = narrator.weightOf(event.title);

    memory.add(std::move(event));
  }
}

void MonthStream::offer(const NarratedTick &tick, const std::vector<FiredEvent> &pool)
{
  uint32_t key = 0u;
  const std::string heading = StoryRunner::monthPhrase(tick.tick, settings.startWeek, key);

  PendingMonth &month = months[key];

  if (month.heading.empty())
  {
    month.key = key;
    month.week = tick.tick;
    month.heading = heading;
  }

  for (const NarratedSentence &sentence : tick.detail)
  {
    TellingSentence telling;

    telling.tick = tick.tick;
    telling.text = sentence.text;
    telling.order = order++;

    for (const size_t index : sentence.events)
    {
      if (index >= pool.size())
      {
        continue;
      }

      const FiredEvent &fired = pool[index];
      const std::string title = fired.rule < program.ruleNames.size() ? ruleTitleOf(program.ruleNames[fired.rule]) : std::string();

      // The sentence takes the identity of its heaviest part: a chain that
      // ends in a killing is a killing, whatever it opened with.
      const uint32_t weight = narrator.weightOf(title);

      if (telling.title.empty() || weight > telling.weight)
      {
        telling.title = title;
        telling.category = narrator.categoryOf(title);
        telling.rule = fired.rule;
      }

      telling.weight = std::max(telling.weight, weight);

      for (const uint32_t node : fired.bound)
      {
        if (node < settings.nodeCeiling && std::find(telling.people.begin(), telling.people.end(), node) == telling.people.end())
        {
          telling.people.push_back(node);
        }
      }

      telling.events.push_back(fired);
    }

    admit(month, std::move(telling));
  }
}

// Ranking: heavier first, and among equals the one told first. Both are
// fixed the moment a sentence arrives, which is what lets the streaming
// driver evict as it goes and still end with the set a whole-month pass
// would have chosen.
bool MonthStream::heavier(const TellingSentence &a, const TellingSentence &b)
{
  return a.weight != b.weight ? a.weight > b.weight : a.order < b.order;
}

void MonthStream::drop(PendingMonth &month, const TellingSentence &sentence) const
{
  ++month.summarised;

  if (sentence.category.empty())
  {
    ++month.droppedOther;
  }
  else
  {
    ++month.dropped[sentence.category];
  }

  if (!sentence.title.empty() && month.keptOfRule.count(sentence.title) != 0u && month.keptOfRule[sentence.title] > 0u)
  {
    --month.keptOfRule[sentence.title];
  }
}

void MonthStream::hold(PendingMonth &month, TellingSentence sentence) const
{
  ++month.keptOfRule[sentence.title];

  month.kept.push_back(std::move(sentence));
}

void MonthStream::admit(PendingMonth &month, TellingSentence sentence) const
{
  // A rule that has already said its piece this month competes only with
  // itself: the cap is what stops a thousand sudden illnesses from being the
  // whole month. The lightest of its own is displaced, or it is counted.
  // "Tell everything" means everything: a budget of zero lifts the cap too,
  // or a run asking for the full log would silently lose most of it.
  const bool capped = settings.perRuleCap != 0u && settings.eventsPerMonth != 0u;

  if (capped && month.keptOfRule[sentence.title] >= settings.perRuleCap)
  {
    size_t lightest = month.kept.size();

    for (size_t i = 0u; i < month.kept.size(); ++i)
    {
      if (month.kept[i].title != sentence.title)
      {
        continue;
      }

      if (lightest == month.kept.size() || heavier(month.kept[lightest], month.kept[i]))
      {
        lightest = i;
      }
    }

    if (lightest != month.kept.size() && heavier(sentence, month.kept[lightest]))
    {
      const TellingSentence displaced = month.kept[lightest];

      month.kept[lightest] = std::move(sentence);
      drop(month, displaced);
      ++month.keptOfRule[displaced.title];
    }
    else
    {
      drop(month, sentence);
      ++month.keptOfRule[sentence.title];
    }

    return;
  }

  if (settings.eventsPerMonth == 0u || month.kept.size() < settings.eventsPerMonth)
  {
    hold(month, std::move(sentence));

    return;
  }

  // The budget is full: the lightest thing held gives way, if this is heavier.
  size_t lightest = 0u;

  for (size_t i = 1u; i < month.kept.size(); ++i)
  {
    if (heavier(month.kept[lightest], month.kept[i]))
    {
      lightest = i;
    }
  }

  if (heavier(sentence, month.kept[lightest]))
  {
    const TellingSentence displaced = month.kept[lightest];

    month.kept[lightest] = std::move(sentence);
    ++month.keptOfRule[month.kept[lightest].title];
    drop(month, displaced);
  }
  else
  {
    drop(month, sentence);
    ++month.keptOfRule[sentence.title];
  }
}

std::vector<ChronicledMonth> MonthStream::takeCompleted(const uint32_t safeTick, NameTable &names,
                                                        const Narrator::GenderLookup &genderOf)
{
  // The worker's hold-back runs the safe tick behind the clock, so on the
  // first weeks of a run it is BEFORE the story's first week. Nothing can be
  // complete then.
  if (safeTick <= settings.startWeek)
  {
    return {};
  }

  uint32_t safeKey = 0u;

  StoryRunner::monthPhrase(safeTick, settings.startWeek, safeKey);

  std::vector<ChronicledMonth> out;

  for (auto it = months.begin(); it != months.end();)
  {
    if (it->first >= safeKey)
    {
      break;
    }

    out.push_back(composeMonth(it->second, names, genderOf));
    it = months.erase(it);
  }

  // Nothing can ask further back than the longest window any condition names.
  if (forgetHorizon != 0u && safeTick > forgetHorizon)
  {
    memory.forgetBefore(safeTick - forgetHorizon);
  }

  return out;
}

std::vector<ChronicledMonth> MonthStream::takeAll(NameTable &names, const Narrator::GenderLookup &genderOf)
{
  std::vector<ChronicledMonth> out;

  for (auto &entry : months)
  {
    out.push_back(composeMonth(entry.second, names, genderOf));
  }

  months.clear();

  return out;
}

ChronicledMonth MonthStream::composeMonth(PendingMonth &month, NameTable &names, const Narrator::GenderLookup &genderOf)
{
  ChronicledMonth composed;

  composed.key = month.key;
  composed.week = month.week;
  composed.heading = month.heading;
  composed.told = static_cast<uint32_t>(month.kept.size());
  composed.summarised = month.summarised;

  std::vector<TellingSentence> &kept = month.kept;

  // Time order: a block reads forwards, whatever the ranking said.
  std::sort(kept.begin(), kept.end(), [](const TellingSentence &a, const TellingSentence &b) { return a.order < b.order; });

  // A rule binds villages, houses, churches and realms alongside its people,
  // and they are nodes below the week-node ceiling like anyone else. Left in,
  // "these two sentences have two nodes in common" is satisfied by a man and
  // the village he lives in, and every birth in the parish joins every
  // confession in it. A person is a node the world has a gender for.
  std::map<uint32_t, bool> isPerson;

  const auto person = [&](const uint32_t node) {
    const auto known = isPerson.find(node);

    if (known != isPerson.end())
    {
      return known->second;
    }

    const bool yes = genderOf && genderOf(node) != Gender::Unknown;

    isPerson[node] = yes;

    return yes;
  };

  for (TellingSentence &told : kept)
  {
    std::vector<uint32_t> only;

    for (const uint32_t node : told.people)
    {
      if (person(node))
      {
        only.push_back(node);
      }
    }

    told.people.swap(only);
  }

  // --- who speaks ----------------------------------------------------------
  // The budget goes to the heaviest events that actually have a line. Most
  // rules have no dialogue block at all, so picking the heaviest events and
  // then asking would spend the whole month's budget on silence.
  std::map<size_t, std::vector<std::string>> spokenAt;

  if (dialogue != nullptr && settings.dialoguePerMonth > 0u)
  {
    std::vector<size_t> byWeight(kept.size());

    for (size_t i = 0u; i < kept.size(); ++i)
    {
      byWeight[i] = i;
    }

    std::sort(byWeight.begin(), byWeight.end(), [&kept](const size_t a, const size_t b) {
      if (kept[a].weight != kept[b].weight)
      {
        return kept[a].weight > kept[b].weight;
      }

      return kept[a].order < kept[b].order;
    });

    for (const size_t index : byWeight)
    {
      if (spokenAt.size() >= settings.dialoguePerMonth)
      {
        break;
      }

      // The whole sentence is offered at once, so a folded chain can reach
      // a chain block before either of its halves is tried on its own.
      std::vector<const FiredEvent *> parts;

      for (const FiredEvent &fired : kept[index].events)
      {
        parts.push_back(&fired);
      }

      std::vector<std::string> lines = dialogue->speak(parts, memory, program, names, genderOf, &causes);

      if (!lines.empty())
      {
        spokenAt[index] = std::move(lines);
      }
    }
  }

  // --- block: what is actually one narrative --------------------------------
  //
  // Not a transitive closure over shared people. That merges through a hub -
  // one man's confession, his daughter's birth and a plot against him become
  // one paragraph because he is in all three - and the result reads as a
  // ramble. A sentence joins a block when the programme says something in it
  // brought this about, or when it has at least `sharedToBlock` people in
  // common with it. One person and no cause is a new block.
  struct Group
  {
    std::vector<size_t> members;
    std::vector<uint32_t> cast;
    uint32_t weight = 0u;
    uint32_t week = 0u;
  };

  std::vector<Group> groups;

  const auto shareCount = [](const std::vector<uint32_t> &a, const std::vector<uint32_t> &b) {
    uint32_t shared = 0u;

    for (const uint32_t person : a)
    {
      shared += std::find(b.begin(), b.end(), person) != b.end() ? 1u : 0u;
    }

    return shared;
  };

  const auto causedBy = [&](const TellingSentence &earlier, const TellingSentence &later) {
    ChronicleEvent cause;
    ChronicleEvent effect;

    cause.tick = earlier.tick;
    cause.rule = earlier.rule;
    cause.bound = earlier.people;

    effect.tick = later.tick;
    effect.rule = later.rule;
    effect.bound = later.people;

    return causes.linked(cause, effect, settings.nodeCeiling, settings.causeSteps);
  };

  for (size_t i = 0u; i < kept.size(); ++i)
  {
    size_t best = groups.size();
    uint32_t bestShared = 0u;

    for (size_t g = 0u; g < groups.size(); ++g)
    {
      if (settings.sentencesPerParagraph != 0u && groups[g].members.size() >= settings.sentencesPerParagraph)
      {
        continue;
      }

      const uint32_t shared = shareCount(kept[i].people, groups[g].cast);

      if (shared == 0u)
      {
        continue;
      }

      bool belongs = shared >= settings.sharedToBlock;

      for (size_t m = 0u; m < groups[g].members.size() && !belongs; ++m)
      {
        belongs = causedBy(kept[groups[g].members[m]], kept[i]);
      }

      if (belongs && shared > bestShared)
      {
        best = g;
        bestShared = shared;
      }
    }

    if (best == groups.size())
    {
      Group fresh;

      fresh.week = kept[i].tick;

      groups.push_back(std::move(fresh));
      best = groups.size() - 1u;
    }

    groups[best].members.push_back(i);
    groups[best].weight = std::max(groups[best].weight, kept[i].weight);

    for (const uint32_t person : kept[i].people)
    {
      if (std::find(groups[best].cast.begin(), groups[best].cast.end(), person) == groups[best].cast.end())
      {
        groups[best].cast.push_back(person);
      }
    }
  }

  // --- order the blocks -----------------------------------------------------
  //
  // A block continuing something the reader saw in a recent month leads;
  // after that, weight. Then, having emitted one, any block sharing a person
  // with it is pulled up next, so a man's several strands sit together
  // without being run into one another.
  std::vector<bool> emitted(groups.size(), false);
  std::vector<size_t> order;

  const auto continuing = [&](const Group &group) {
    if (settings.continuityMonths == 0u)
    {
      return false;
    }

    for (const uint32_t person : group.cast)
    {
      const auto seen = lastToldIn.find(person);

      if (seen != lastToldIn.end() && month.key > seen->second && month.key - seen->second <= settings.continuityMonths)
      {
        return true;
      }
    }

    return false;
  };

  std::vector<bool> carries(groups.size(), false);

  for (size_t g = 0u; g < groups.size(); ++g)
  {
    carries[g] = continuing(groups[g]);
  }

  const auto better = [&](const size_t a, const size_t b) -> bool {
    if (carries[a] != carries[b])
    {
      return carries[a];
    }

    if (groups[a].weight != groups[b].weight)
    {
      return groups[a].weight > groups[b].weight;
    }

    return groups[a].members.front() < groups[b].members.front();
  };

  while (order.size() < groups.size())
  {
    size_t pick = groups.size();

    // Prefer a block sharing people with the one just told.
    if (!order.empty())
    {
      for (size_t g = 0u; g < groups.size(); ++g)
      {
        if (emitted[g] || shareCount(groups[g].cast, groups[order.back()].cast) == 0u)
        {
          continue;
        }

        if (pick == groups.size() || better(g, pick))
        {
          pick = g;
        }
      }
    }

    if (pick == groups.size())
    {
      for (size_t g = 0u; g < groups.size(); ++g)
      {
        if (!emitted[g] && (pick == groups.size() || better(g, pick)))
        {
          pick = g;
        }
      }
    }

    emitted[pick] = true;
    order.push_back(pick);
  }

  // --- render ---------------------------------------------------------------
  for (const size_t g : order)
  {
    const Group &group = groups[g];

    ChronicledBlock block;

    block.cast = group.cast;
    block.weight = group.weight;
    block.week = group.week;
    block.continues = carries[g];

    std::vector<std::string> sentences;

    for (size_t i = 0u; i < group.members.size(); ++i)
    {
      const TellingSentence &told = kept[group.members[i]];

      std::string text = told.text;

      if (i > 0u)
      {
        const uint64_t key = grammarKey(settings.seed, told.tick, told.order, i);

        // Is this sentence a CONSEQUENCE of one already in the block? The
        // rules say whether it can be, the people say whether it is.
        bool consequence = false;

        for (size_t j = 0u; j < i && !consequence; ++j)
        {
          consequence = causedBy(kept[group.members[j]], told);
        }

        const char *const which = consequence ? "therefore" : "then";

        const std::string connective =
            narrator.grammar().has(which)
                ? narrator.grammar().expand(std::string("<") + which + ">", key)
                : consequence ? std::string(defaultTherefore[key % (sizeof(defaultTherefore) / sizeof(defaultTherefore[0]))])
                              : std::string(defaultThen[key % (sizeof(defaultThen) / sizeof(defaultThen[0]))]);

        // The file writes `Then` without a trailing space (a sub trims), so
        // the space is added here rather than depending on the spelling.
        text = connective.empty() ? text : connective + " " + lowerFirst(text);
      }

      sentences.push_back(std::move(text));

      // Dialogue hangs off the sentence, inside the block, so the line sits
      // next to the event it answers. It already closes on a quote;
      // joinParagraph knows not to put a stop after one.
      const auto said = spokenAt.find(group.members[i]);

      if (said != spokenAt.end())
      {
        sentences.insert(sentences.end(), said->second.begin(), said->second.end());
      }
    }

    if (!sentences.empty())
    {
      block.paragraphs.push_back(Chronicler::joinParagraph(sentences));
    }

    for (const std::string &paragraph : block.paragraphs)
    {
      composed.paragraphs.push_back(paragraph);
    }

    composed.blocks.push_back(std::move(block));
  }

  // Everyone told about this month is recent, for the next month's ordering.
  for (const TellingSentence &told : kept)
  {
    for (const uint32_t person : told.people)
    {
      lastToldIn[person] = month.key;
    }
  }

  // --- what was dropped, as counts ------------------------------------------
  if (month.summarised != 0u)
  {
    std::vector<std::string> phrases;
    uint32_t uncategorised = month.droppedOther;

    for (const auto &count : month.dropped)
    {
      const std::string phrase = narrator.summaryPhrase(count.first, count.second);

      if (!phrase.empty())
      {
        phrases.push_back(phrase);
      }
      else
      {
        uncategorised += count.second;
      }
    }

    if (uncategorised > 0u)
    {
      const std::string phrase = narrator.summaryPhrase("other", uncategorised);

      phrases.push_back(phrase.empty() ? std::to_string(uncategorised) + (uncategorised == 1u ? " smaller matter" : " smaller matters")
                                       : phrase);
    }

    if (!phrases.empty())
    {
      std::string sentence = "Elsewhere that month: ";

      for (size_t i = 0u; i < phrases.size(); ++i)
      {
        sentence += i == 0u ? "" : i + 1u == phrases.size() ? " and " : ", ";
        sentence += phrases[i];
      }

      composed.summary = sentence + ".";
      composed.paragraphs.push_back(composed.summary);
    }
  }

  return composed;
}

Chronicler::Chronicler(const Narrator &teller, const compiler::CompiledProgram &compiled, ChronicleSettings options)
    : narrator(teller), program(compiled), settings(options)
{
}

std::string Chronicler::joinParagraph(const std::vector<std::string> &sentences)
{
  std::string out;

  for (const std::string &sentence : sentences)
  {
    if (!out.empty())
    {
      out += ' ';
    }

    out += sentence;

    // Every sentence ends in a stop, so a narration template that forgot one
    // does not run into the next.
    if (!out.empty() && out.back() != '.' && out.back() != '!' && out.back() != '?' && out.back() != '"')
    {
      out += '.';
    }
  }

  return out;
}

std::vector<ChronicledMonth> Chronicler::compose(const std::vector<FiredEvent> &events, NameTable &names,
                                                 const Narrator::GenderLookup &genderOf) const
{
  MonthStream stream(narrator, program, settings);

  if (dialogue != nullptr)
  {
    stream.setDialogue(dialogue);
  }

  // The batch driver has the whole log, so the chronicle takes it in one go
  // and no month is ever taken before the end.
  stream.remember(events);

  for (const NarratedTick &tick : narrator.narrate(events, program, names, genderOf))
  {
    stream.offer(tick, events);
  }

  return stream.takeAll(names, genderOf);
}

std::string Chronicler::text(const std::vector<FiredEvent> &events, NameTable &names, const Narrator::GenderLookup &genderOf) const
{
  std::ostringstream out;

  for (const ChronicledMonth &month : compose(events, names, genderOf))
  {
    appendMonth(out, month);
  }

  return out.str();
}

void Chronicler::appendMonth(std::ostream &out, const ChronicledMonth &month)
{
  out << "-- " << month.heading << " --\n\n";

  for (const std::string &paragraph : month.paragraphs)
  {
    out << paragraph << "\n\n";
  }
}

} // namespace graphrewrite::runtime
