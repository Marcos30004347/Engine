#include "graphrewrite/runtime/Narrator.hpp"

#include "graphrewrite/runtime/Placeholders.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace graphrewrite::runtime {
namespace {

std::string trim(const std::string &text)
{
  const size_t first = text.find_first_not_of(" \t\r\n");

  if (first == std::string::npos)
  {
    return std::string();
  }

  const size_t last = text.find_last_not_of(" \t\r\n");

  return text.substr(first, last - first + 1u);
}

std::string capitalised(std::string word)
{
  return capitaliseWord(std::move(word));
}

// "two" reads better than "2" for the handful of counts a month summary ever
// reaches; past that the digits are clearer anyway.
std::string spelledOut(const uint32_t n)
{
  static const char *const words[] = {"no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve"};

  return n <= 12u ? words[n] : std::to_string(n);
}

} // namespace

std::string ruleTitleOf(const std::string &variantName)
{
  const size_t hash = variantName.rfind('#');

  return hash == std::string::npos ? variantName : variantName.substr(0u, hash);
}

bool Narrator::load(const std::string &text, const std::string &path, DiagnosticSink &sink)
{
  const uint32_t errorsBefore = sink.errorCount();

  std::istringstream lines(text);
  std::string raw;
  uint32_t number = 0u;

  // A template may continue on the next line if that line is indented.
  std::string pending;
  uint32_t pendingLine = 0u;

  const auto commit = [&]() {
    if (pending.empty())
    {
      return;
    }

    SourceSpan span;

    span.line = pendingLine;
    span.column = 1u;

    const std::string line = pending;

    pending.clear();

    if (line.rfind("important ", 0u) == 0)
    {
      // `important a-rule another-rule ...`: any number per line.
      std::istringstream words(line.substr(10u));
      std::string word;

      while (words >> word)
      {
        important.insert(word);
      }

      return;
    }

    if (line.rfind("silent ", 0u) == 0)
    {
      silent.insert(trim(line.substr(7u)));

      return;
    }

    if (line.rfind("incidental ", 0u) == 0)
    {
      incidental.insert(trim(line.substr(11u)));

      return;
    }

    if (Grammar::isSubLine(line))
    {
      subs.addLine(line, path, span.line, sink);

      return;
    }

    if (line.rfind("weight ", 0u) == 0)
    {
      // `weight 30 a-rule another-rule ...`
      std::istringstream words(line.substr(7u));
      std::string first;

      words >> first;

      const uint32_t weight = static_cast<uint32_t>(std::strtoul(first.c_str(), nullptr, 10));

      if (weight == 0u)
      {
        sink.error(span, path + ":" + std::to_string(span.line) + ": expected 'weight <n> <rule> ...' with n above zero");

        return;
      }

      std::string word;

      while (words >> word)
      {
        weights[word] = weight;
      }

      return;
    }

    if (line.rfind("category ", 0u) == 0)
    {
      // `category death: death-comes old-age ...`
      const std::string body = trim(line.substr(9u));
      const size_t mark = body.find(':');

      if (mark == std::string::npos)
      {
        sink.error(span, path + ":" + std::to_string(span.line) + ": expected 'category <name>: <rule> ...'");

        return;
      }

      const std::string name = trim(body.substr(0u, mark));

      std::istringstream words(body.substr(mark + 1u));
      std::string word;

      while (words >> word)
      {
        categories[word] = name;
      }

      return;
    }

    if (line.rfind("summary ", 0u) == 0)
    {
      // `summary death: a death / {n} deaths`
      const std::string body = trim(line.substr(8u));
      const size_t mark = body.find(':');
      const size_t slash = mark == std::string::npos ? std::string::npos : body.find('/', mark);

      if (mark == std::string::npos || slash == std::string::npos)
      {
        sink.error(span, path + ":" + std::to_string(span.line) + ": expected 'summary <category>: <one> / <many>'");

        return;
      }

      summaries[trim(body.substr(0u, mark))] =
          std::make_pair(trim(body.substr(mark + 1u, slash - mark - 1u)), trim(body.substr(slash + 1u)));

      return;
    }

    // The head ends at the first colon FOLLOWED BY A SPACE, or at a colon that
    // ends the line. A bare colon is not enough: `released:they-set-a-date:`
    // has one inside its key.
    size_t colon = line.find(": ");

    if (colon == std::string::npos && !line.empty() && line.back() == ':')
    {
      colon = line.size() - 1u;
    }

    if (colon == std::string::npos)
    {
      sink.error(span, path + ":" + std::to_string(span.line) + ": expected 'rule: sentence', 'chain a + b within N: sentence' or 'silent pattern'");

      return;
    }

    const std::string head = trim(line.substr(0u, colon));
    const std::string sentence = trim(line.substr(colon + 1u));

    if (head.rfind("chain ", 0u) == 0)
    {
      Chain chain;

      std::string spec = trim(head.substr(6u));

      const size_t within = spec.find(" within ");

      if (within != std::string::npos)
      {
        chain.window = static_cast<uint32_t>(std::strtoul(spec.substr(within + 8u).c_str(), nullptr, 10));
        spec = trim(spec.substr(0u, within));
      }

      size_t at = 0u;

      while (at <= spec.size())
      {
        const size_t plus = spec.find('+', at);
        const std::string part = trim(spec.substr(at, plus == std::string::npos ? std::string::npos : plus - at));

        if (!part.empty())
        {
          chain.rules.push_back(part);
        }

        if (plus == std::string::npos)
        {
          break;
        }

        at = plus + 1u;
      }

      if (chain.rules.size() < 2u)
      {
        sink.error(span, path + ":" + std::to_string(span.line) + ": a chain names two or more rules joined by '+'");

        return;
      }

      chain.sentence = sentence;

      chains.push_back(std::move(chain));

      return;
    }

    templates[head] = sentence;
  };

  while (std::getline(lines, raw))
  {
    ++number;

    const size_t hash = raw.find('#');

    if (hash != std::string::npos)
    {
      raw.erase(hash);
    }

    if (trim(raw).empty())
    {
      continue;
    }

    const bool continuation = raw[0] == ' ' || raw[0] == '\t';

    if (continuation && !pending.empty())
    {
      pending += " " + trim(raw);

      continue;
    }

    commit();

    pending = trim(raw);
    pendingLine = number;
  }

  commit();

  return sink.errorCount() == errorsBefore;
}

bool Narrator::loadFile(const std::string &path, DiagnosticSink &sink)
{
  std::ifstream file(path);

  if (!file)
  {
    SourceSpan span;

    sink.error(span, "cannot open narration file " + path);

    return false;
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  return load(buffer.str(), path, sink);
}

std::string Narrator::substituted(const std::string &text, const std::vector<const FiredEvent *> &parts) const
{
  if (subs.size() == 0u)
  {
    return text;
  }

  // Keyed off the firing, not a counter: the same event in the same run picks
  // the same words however many times it is rendered, and two firings of one
  // rule in one week do not agree by accident.
  const FiredEvent *first = parts.empty() ? nullptr : parts.front();
  const uint64_t key = first == nullptr
                           ? grammarSeed
                           : grammarKey(grammarSeed, first->tick, first->rule, first->bound.empty() ? 0u : first->bound.front());

  return subs.expand(text, key);
}

std::string Narrator::render(const std::string &sentence, const std::vector<const FiredEvent *> &parts,
                             const compiler::CompiledProgram &program, NameTable &names, const GenderLookup &genderOf) const
{
  PlaceholderContext context;

  context.program = &program;
  context.names = &names;
  context.genderOf = genderOf;

  // Substitution first, so an alternative may itself hold a `{A}`.
  return renderPlaceholders(substituted(sentence, parts), parts, context);
}

std::string Narrator::fallback(const FiredEvent &event, const compiler::CompiledProgram &program, NameTable &names) const
{
  if (event.rule >= program.ruleRewrites.size())
  {
    return std::string();
  }

  std::string out;

  for (const compiler::PatternEdge &edge : program.ruleRewrites[event.rule].insertions)
  {
    const std::string &pattern = program.edgeTypes[edge.label].pattern;

    if (silent.count(pattern) != 0u)
    {
      continue;
    }

    // Substitute names for the `?`s, in order: origin then target. A self-loop
    // has one `?`, so the second name is never asked for.
    std::string sentence;
    uint32_t slot = 0u;

    for (const char c : pattern)
    {
      if (c != '?')
      {
        sentence += c;

        continue;
      }

      const uint32_t vertex = slot++ == 0u ? edge.origin : edge.target;

      sentence += vertex < event.bound.size() ? names.nameOf(event.bound[vertex]) : std::string("someone");
    }

    if (!out.empty())
    {
      out += ' ';
    }

    out += capitalised(sentence) + ".";
  }

  return out;
}

std::string Narrator::tell(const FiredEvent &event, const compiler::CompiledProgram &program, NameTable &names,
                           const GenderLookup &genderOf) const
{
  const std::string title = event.rule < program.ruleNames.size() ? ruleTitleOf(program.ruleNames[event.rule]) : std::string();

  // A scheduled or cancelled row is only told if the file has a line for it -
  // reading out its edges would describe something that has not happened.
  if (event.kind != RewriteEventKind::Applied)
  {
    const char *prefix = event.kind == RewriteEventKind::Scheduled ? "scheduled:"
                         : event.kind == RewriteEventKind::Released ? "released:"
                                                                     : "cancel:";

    const auto special = templates.find(prefix + title);

    return special == templates.end() ? std::string() : render(special->second, {&event}, program, names, genderOf);
  }

  const auto found = templates.find(title);

  if (found != templates.end())
  {
    return render(found->second, {&event}, program, names, genderOf);
  }

  return fallback(event, program, names);
}

std::vector<compiler::EdgeTypeId> Narrator::incidentalTypes(const compiler::CompiledProgram &program) const
{
  std::vector<compiler::EdgeTypeId> types;

  for (compiler::EdgeTypeId label = 0u; label < program.edgeTypes.size(); ++label)
  {
    if (incidental.count(program.edgeTypes[label].pattern) != 0u)
    {
      types.push_back(label);
    }
  }

  return types;
}

std::set<std::string> Narrator::namedRules() const
{
  std::set<std::string> named;

  for (const auto &entry : templates)
  {
    named.insert(entry.first);
  }

  for (const Chain &chain : chains)
  {
    named.insert(chain.rules.begin(), chain.rules.end());
  }

  named.insert(important.begin(), important.end());

  for (const auto &entry : weights)
  {
    named.insert(entry.first);
  }

  for (const auto &entry : categories)
  {
    named.insert(entry.first);
  }

  return named;
}

uint32_t Narrator::weightOf(const std::string &ruleTitle) const
{
  const auto found = weights.find(ruleTitle);

  if (found != weights.end())
  {
    return found->second;
  }

  return important.count(ruleTitle) != 0u ? importantWeight : 1u;
}

std::string Narrator::categoryOf(const std::string &ruleTitle) const
{
  const auto found = categories.find(ruleTitle);

  return found == categories.end() ? std::string() : found->second;
}

std::string Narrator::summaryPhrase(const std::string &category, const uint32_t n) const
{
  const auto found = summaries.find(category);

  if (found == summaries.end() || n == 0u)
  {
    return std::string();
  }

  std::string phrase = n == 1u ? found->second.first : found->second.second;

  // `{n}` is the count, spelled out while it is small.
  const size_t mark = phrase.find("{n}");

  if (mark != std::string::npos)
  {
    phrase = phrase.substr(0u, mark) + spelledOut(n) + phrase.substr(mark + 3u);
  }

  return phrase;
}

std::vector<NarratedTick> Narrator::narrate(const std::vector<FiredEvent> &events, const compiler::CompiledProgram &program,
                                            NameTable &names, const GenderLookup &genderOf) const
{
  std::vector<bool> consumed;

  return narrateUpTo(events, ~0u, program, names, genderOf, consumed);
}

uint32_t Narrator::longestChainWindow() const
{
  uint32_t longest = 0u;

  for (const Chain &chain : chains)
  {
    longest = std::max(longest, chain.window);
  }

  return longest;
}

std::vector<NarratedTick> Narrator::narrateUpTo(const std::vector<FiredEvent> &events, const uint32_t uptoTick,
                                                const compiler::CompiledProgram &program, NameTable &names,
                                                const GenderLookup &genderOf, std::vector<bool> &consumedOut) const
{
  struct Told
  {
    size_t order = 0u;
    std::string sentence;
    std::vector<size_t> events;

    bool operator<(const Told &other) const
    {
      return order < other.order;
    }
  };

  std::map<uint32_t, std::vector<Told>> byTick;
  std::vector<bool> consumed(events.size(), false);

  const auto titleOfEvent = [&program](const FiredEvent &event) {
    return event.rule < program.ruleNames.size() ? ruleTitleOf(program.ruleNames[event.rule]) : std::string();
  };

  // Unimportant events are dropped up front when asked: consumed, never told.
  if (onlyImportant)
  {
    for (size_t i = 0u; i < events.size(); ++i)
    {
      if (important.count(titleOfEvent(events[i])) == 0u)
      {
        consumed[i] = true;
      }
    }
  }

  // Chain candidates by (rule title, bound node): a part must share a
  // same-named node with the chain so far, so only events bound to one of
  // the chain's nodes can qualify. Without this every chain scanned every
  // event for every start, which is quadratic in a week of a large world.
  std::map<std::string, std::map<uint32_t, std::vector<size_t>>> byTitleAndNode;

  for (size_t i = 0u; i < events.size(); ++i)
  {
    if (consumed[i] || events[i].kind != RewriteEventKind::Applied)
    {
      continue;
    }

    std::map<uint32_t, std::vector<size_t>> &byNode = byTitleAndNode[titleOfEvent(events[i])];

    for (const uint32_t node : events[i].bound)
    {
      std::vector<size_t> &list = byNode[node];

      if (list.empty() || list.back() != i)
      {
        list.push_back(i);
      }
    }
  }

  const auto titleOf = [&program](const FiredEvent &event) {
    return event.rule < program.ruleNames.size() ? ruleTitleOf(program.ruleNames[event.rule]) : std::string();
  };

  // Two firings belong to one chain when a variable NAME the two rules share
  // is bound to the same node in both, and no shared name disagrees. "Any
  // node in common" was too loose: a man's notice of one woman chained with
  // another woman's answer to him. Rules written for a chain use the same
  // letter for the same person.
  const auto shareANode = [&program](const FiredEvent &a, const FiredEvent &b) {
    const std::vector<std::string> empty;
    const std::vector<std::string> &namesA = a.rule < program.ruleVertexNames.size() ? program.ruleVertexNames[a.rule] : empty;
    const std::vector<std::string> &namesB = b.rule < program.ruleVertexNames.size() ? program.ruleVertexNames[b.rule] : empty;

    bool shared = false;

    for (size_t i = 0u; i < namesA.size() && i < a.bound.size(); ++i)
    {
      const auto found = std::find(namesB.begin(), namesB.end(), namesA[i]);

      if (found == namesB.end())
      {
        continue;
      }

      const size_t j = static_cast<size_t>(found - namesB.begin());

      if (j >= b.bound.size())
      {
        continue;
      }

      if (a.bound[i] != b.bound[j])
      {
        return false;
      }

      shared = true;
    }

    return shared;
  };

  // Chains first, earliest first, so a firing is told once and as part of the
  // larger event when it is part of one. Only rows that actually fired take
  // part; a scheduled notice or a cancellation is its own sentence.
  const auto applied = [&events](const size_t i) { return events[i].kind == RewriteEventKind::Applied; };

  for (const Chain &chain : chains)
  {
    for (size_t i = 0u; i < events.size(); ++i)
    {
      if (consumed[i] || !applied(i) || titleOf(events[i]) != chain.rules[0])
      {
        continue;
      }

      std::vector<size_t> parts{i};

      // Extend one rule at a time. Each part must land no earlier than the
      // previous one, inside the window from the first, and touch someone the
      // chain already involves.
      for (size_t k = 1u; k < chain.rules.size(); ++k)
      {
        bool found = false;

        const auto titled = byTitleAndNode.find(chain.rules[k]);

        if (titled == byTitleAndNode.end())
        {
          break;
        }

        // Candidates: events of the wanted rule bound to a node some part of
        // the chain is bound to, earliest first.
        std::vector<size_t> candidates;

        for (const size_t part : parts)
        {
          for (const uint32_t node : events[part].bound)
          {
            const auto list = titled->second.find(node);

            if (list != titled->second.end())
            {
              candidates.insert(candidates.end(), list->second.begin(), list->second.end());
            }
          }
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        for (const size_t j : candidates)
        {
          if (consumed[j] || !applied(j) || j == i)
          {
            continue;
          }

          if (std::find(parts.begin(), parts.end(), j) != parts.end())
          {
            continue;
          }

          if (events[j].tick < events[parts.back()].tick || events[j].tick > events[i].tick + chain.window)
          {
            continue;
          }

          for (const size_t part : parts)
          {
            if (shareANode(events[part], events[j]))
            {
              parts.push_back(j);
              found = true;

              break;
            }
          }

          if (found)
          {
            break;
          }
        }

        if (!found)
        {
          break;
        }
      }

      if (parts.size() != chain.rules.size())
      {
        continue;
      }

      // A chain told after the cut-off is not told now - and not consumed, so
      // the next call finds it again with the same parts.
      if (events[parts.back()].tick > uptoTick)
      {
        continue;
      }

      std::vector<const FiredEvent *> pointers;

      for (const size_t part : parts)
      {
        consumed[part] = true;
        pointers.push_back(&events[part]);
      }

      byTick[events[parts.back()].tick].push_back(Told{parts.back(), render(chain.sentence, pointers, program, names, genderOf), parts});
    }
  }

  for (size_t i = 0u; i < events.size(); ++i)
  {
    if (consumed[i] || events[i].tick > uptoTick)
    {
      continue;
    }

    consumed[i] = true;

    const std::string sentence = tell(events[i], program, names, genderOf);

    if (!sentence.empty())
    {
      byTick[events[i].tick].push_back(Told{i, sentence, {i}});
    }
  }

  std::vector<NarratedTick> out;

  for (auto &entry : byTick)
  {
    std::sort(entry.second.begin(), entry.second.end());

    NarratedTick tick;

    tick.tick = entry.first;

    for (auto &item : entry.second)
    {
      tick.sentences.push_back(item.sentence);
      tick.detail.push_back(NarratedSentence{std::move(item.sentence), std::move(item.events)});
    }

    out.push_back(std::move(tick));
  }

  consumedOut = std::move(consumed);

  return out;
}

} // namespace graphrewrite::runtime
