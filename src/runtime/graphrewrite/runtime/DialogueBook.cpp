#include "graphrewrite/runtime/DialogueBook.hpp"

#include "graphrewrite/runtime/Placeholders.hpp"

#include <algorithm>
#include <cstdlib>
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

  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1u);
}

uint32_t indentOf(const std::string &line)
{
  uint32_t indent = 0u;

  for (const char c : line)
  {
    if (c == ' ')
    {
      ++indent;
    }
    else if (c == '\t')
    {
      indent += 2u;
    }
    else
    {
      break;
    }
  }

  return indent;
}

// The text between the first pair of double quotes, or everything after the
// colon when the line is unquoted.
std::string spokenText(const std::string &text)
{
  const size_t open = text.find('"');

  if (open == std::string::npos)
  {
    return trim(text);
  }

  const size_t close = text.rfind('"');

  return close > open ? text.substr(open + 1u, close - open - 1u) : trim(text.substr(open + 1u));
}

// `2.X` is the second firing's X; a bare `X` is the first's. Returns the
// part and strips the prefix.
size_t splitPart(std::string &role)
{
  const size_t dot = role.find('.');

  if (dot == std::string::npos || dot == 0u)
  {
    return 0u;
  }

  for (size_t i = 0u; i < dot; ++i)
  {
    if (role[i] < '0' || role[i] > '9')
    {
      return 0u;
    }
  }

  const unsigned long index = std::strtoul(role.substr(0u, dot).c_str(), nullptr, 10);

  role = role.substr(dot + 1u);

  return index == 0u ? 0u : static_cast<size_t>(index - 1u);
}

} // namespace

bool DialogueBook::loadFile(const std::string &path, DiagnosticSink &sink)
{
  std::ifstream file(path);

  if (!file)
  {
    SourceSpan span;

    sink.error(span, "cannot open dialogue file " + path);

    return false;
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  return load(buffer.str(), path, sink);
}

bool DialogueBook::parseCondition(const std::string &text, const std::string &path, const uint32_t lineNumber, DiagnosticSink &sink,
                                  Clause &into) const
{
  SourceSpan span;

  span.line = lineNumber;
  span.column = 1u;

  const auto fail = [&](const std::string &what) {
    sink.error(span, path + ":" + std::to_string(lineNumber) + ": " + what);

    return false;
  };

  // `and` splits, so each piece is one condition.
  std::vector<std::string> pieces;
  std::string rest = trim(text);

  for (;;)
  {
    const size_t joint = rest.find(" and ");

    if (joint == std::string::npos)
    {
      pieces.push_back(trim(rest));

      break;
    }

    pieces.push_back(trim(rest.substr(0u, joint)));
    rest = rest.substr(joint + 5u);
  }

  for (const std::string &piece : pieces)
  {
    Condition condition;

    std::istringstream words(piece);
    std::vector<std::string> word;
    std::string one;

    while (words >> one)
    {
      word.push_back(one);
    }

    if (word.empty())
    {
      return fail("empty condition");
    }

    size_t at = 0u;

    if (word[at] == "unless")
    {
      condition.negated = true;
      ++at;
    }
    else if (word[at] == "when")
    {
      ++at;
    }

    if (at >= word.size())
    {
      return fail("a condition needs a role");
    }

    // `because <rule>` asks the cause graph instead of a participant.
    if (word[at] == "because")
    {
      ++at;

      if (at >= word.size())
      {
        return fail("'because' needs a rule title");
      }

      condition.causal = true;
      condition.title = word[at++];

      while (at < word.size())
      {
        if (word[at] == "within" && at + 1u < word.size())
        {
          condition.window = Window{static_cast<uint32_t>(std::strtoul(word[at + 1u].c_str(), nullptr, 10))};
          at += 2u;
        }
        else
        {
          return fail("unexpected '" + word[at] + "' after 'because " + condition.title + "'");
        }
      }

      into.conditions.push_back(std::move(condition));

      continue;
    }

    condition.role = word[at++];
    condition.part = splitPart(condition.role);

    if (at < word.size() && word[at] == "met")
    {
      ++at;

      if (at >= word.size())
      {
        return fail("'met' needs a second role");
      }

      condition.other = word[at++];
      condition.otherPart = splitPart(condition.other);
    }

    if (at >= word.size() || word[at] != "in")
    {
      return fail("expected 'in <rule>' in condition '" + piece + "'");
    }

    ++at;

    if (at >= word.size())
    {
      return fail("'in' needs a rule title");
    }

    condition.title = word[at++];

    // Trailing modifiers, in any order: `twice`, `N times`, `within N`.
    while (at < word.size())
    {
      if (word[at] == "twice")
      {
        condition.atLeast = 2u;
        ++at;
      }
      else if (word[at] == "within" && at + 1u < word.size())
      {
        condition.window = Window{static_cast<uint32_t>(std::strtoul(word[at + 1u].c_str(), nullptr, 10))};
        at += 2u;
      }
      else if (at + 1u < word.size() && word[at + 1u] == "times")
      {
        condition.atLeast = static_cast<uint32_t>(std::strtoul(word[at].c_str(), nullptr, 10));
        condition.atLeast = condition.atLeast == 0u ? 1u : condition.atLeast;
        at += 2u;
      }
      else
      {
        return fail("unexpected '" + word[at] + "' in condition '" + piece + "'");
      }
    }

    into.conditions.push_back(std::move(condition));
  }

  return true;
}

bool DialogueBook::load(const std::string &text, const std::string &path, DiagnosticSink &sink)
{
  const uint32_t errorsBefore = sink.errorCount();

  if (grammar == nullptr)
  {
    grammar = &owned;
  }

  std::istringstream lines(text);
  std::string raw;
  uint32_t number = 0u;

  Block *block = nullptr;
  Clause *clause = nullptr;

  // A `sub` whose line ends in `|` continues on the next: a list of whole
  // sentences does not fit on one line and should not have to.
  std::string continued;
  uint32_t continuedAt = 0u;

  while (std::getline(lines, raw))
  {
    ++number;

    // A `#` inside a spoken line is part of what was said only if the line is
    // quoted; comments outside quotes are stripped.
    const size_t quote = raw.find('"');
    const size_t hash = raw.find('#');

    if (hash != std::string::npos && (quote == std::string::npos || hash < quote))
    {
      raw.erase(hash);
    }

    const std::string line = trim(raw);

    if (line.empty())
    {
      continue;
    }

    const uint32_t indent = indentOf(raw);

    SourceSpan span;

    span.line = number;
    span.column = 1u;

    if (!continued.empty())
    {
      continued += " " + line;

      if (!continued.empty() && continued.back() == '|')
      {
        continue;
      }

      grammar->addLine(continued, path, continuedAt, sink);
      continued.clear();

      continue;
    }

    if (Grammar::isSubLine(line))
    {
      if (line.back() == '|')
      {
        continued = line;
        continuedAt = number;

        continue;
      }

      grammar->addLine(line, path, number, sink);

      continue;
    }

    if (indent == 0u)
    {
      if (line.rfind("dialogue ", 0u) != 0)
      {
        sink.error(span, path + ":" + std::to_string(number) + ": expected 'dialogue <rule>:' or 'sub <name>: ...'");

        continue;
      }

      std::string head = trim(line.substr(9u));

      if (!head.empty() && head.back() == ':')
      {
        head.pop_back();
      }

      Block fresh;

      std::istringstream words(head);
      std::string word;

      words >> word;

      // `dialogue chain a + b + c [chance N]:` hangs off a folded chain. The
      // rules are joined by `+`, exactly as the narration file writes them,
      // so the two files can be read against each other.
      const bool isChain = word == "chain";

      if (isChain)
      {
        // `word` still holds "chain"; it is reused below to carry whichever
        // option ended the rule list, and means "none" until then.
        word.clear();

        std::string part;
        bool expectRule = true;

        while (words >> part)
        {
          if (part == "+")
          {
            expectRule = true;

            continue;
          }

          if (part == "chance" || part == "within")
          {
            word = part;

            break;
          }

          if (!expectRule)
          {
            sink.error(span, path + ":" + std::to_string(number) + ": expected '+' between the rules of a chain");

            break;
          }

          // `a + b` may also arrive as `a` `+b` or `a+ b`.
          std::string name = part;

          while (!name.empty() && name.front() == '+')
          {
            name.erase(name.begin());
          }

          while (!name.empty() && name.back() == '+')
          {
            name.pop_back();
            expectRule = true;
          }

          if (name.empty())
          {
            continue;
          }

          fresh.chainRules.push_back(name);
          expectRule = part.back() == '+';
        }

        if (fresh.chainRules.size() < 2u)
        {
          sink.error(span, path + ":" + std::to_string(number) + ": a dialogue chain names two or more rules joined by '+'");

          continue;
        }

        fresh.title = fresh.chainRules.front();
      }
      else
      {
        fresh.title = word;
        word.clear();
      }

      const auto option = [&](const std::string &name) {
        if (name == "chance")
        {
          std::string value;

          if (words >> value)
          {
            const uint32_t percent = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));

            fresh.chance = percent > 100u ? 100u : percent;
          }

          return true;
        }

        if (name == "within")
        {
          // The narration file owns the window; accepted here so the two
          // headers can be written the same way, and ignored.
          std::string value;

          words >> value;

          return true;
        }

        return false;
      };

      if (!word.empty() && !option(word))
      {
        sink.error(span, path + ":" + std::to_string(number) + ": unexpected '" + word + "' after the rule title");
      }

      while (words >> word)
      {
        if (!option(word))
        {
          sink.error(span, path + ":" + std::to_string(number) + ": unexpected '" + word + "' after the rule title");
        }
      }

      if (fresh.title.empty())
      {
        sink.error(span, path + ":" + std::to_string(number) + ": a dialogue block needs a rule title");

        continue;
      }

      // A second block for the same rule is never reached: the first that
      // speaks wins, so the later one is dead text and nothing would say so.
      for (const Block &existing : blocks)
      {
        if (existing.chainRules == fresh.chainRules && existing.title == fresh.title)
        {
          sink.error(span, path + ":" + std::to_string(number) + ": a second dialogue block for '" + fresh.title +
                               "' is never reached; merge its clauses into the first");

          break;
        }
      }

      blocks.push_back(std::move(fresh));
      block = &blocks.back();
      clause = nullptr;

      continue;
    }

    if (block == nullptr)
    {
      sink.error(span, path + ":" + std::to_string(number) + ": this line is not inside a dialogue block");

      continue;
    }

    const bool isClauseHead = (line.rfind("when ", 0u) == 0 || line.rfind("unless ", 0u) == 0 || line == "else:") && line.back() == ':';

    if (isClauseHead)
    {
      Clause fresh;

      if (line == "else:")
      {
        fresh.always = true;
      }
      else if (!parseCondition(line.substr(0u, line.size() - 1u), path, number, sink, fresh))
      {
        continue;
      }

      block->clauses.push_back(std::move(fresh));
      clause = &block->clauses.back();

      continue;
    }

    // A speaker line. `B to X: "..."` or `B: "..."`. An unconditional line
    // straight under the header opens an implicit always-clause.
    const size_t colon = line.find(':');

    if (colon == std::string::npos)
    {
      sink.error(span, path + ":" + std::to_string(number) + ": expected '<role>: \"...\"' or '<role> to <role>: \"...\"'");

      continue;
    }

    if (clause == nullptr)
    {
      Clause fresh;

      fresh.always = true;

      block->clauses.push_back(std::move(fresh));
      clause = &block->clauses.back();
    }

    Line spoken;

    const std::string who = trim(line.substr(0u, colon));
    const size_t to = who.find(" to ");

    if (to == std::string::npos)
    {
      spoken.speaker = who;
    }
    else
    {
      spoken.speaker = trim(who.substr(0u, to));
      spoken.listener = trim(who.substr(to + 4u));
    }

    spoken.speakerPart = splitPart(spoken.speaker);

    if (!spoken.listener.empty())
    {
      spoken.listenerPart = splitPart(spoken.listener);
    }

    if (spoken.speaker.empty())
    {
      sink.error(span, path + ":" + std::to_string(number) + ": a spoken line needs a speaker");

      continue;
    }

    spoken.text = spokenText(line.substr(colon + 1u));

    if (spoken.text.empty())
    {
      sink.error(span, path + ":" + std::to_string(number) + ": a spoken line needs something said");

      continue;
    }

    clause->lines.push_back(std::move(spoken));
  }

  if (!continued.empty())
  {
    grammar->addLine(continued, path, continuedAt, sink);
  }

  return sink.errorCount() == errorsBefore;
}

bool DialogueBook::hasLinesFor(const std::string &ruleTitle) const
{
  for (const Block &block : blocks)
  {
    if (block.title == ruleTitle)
    {
      return true;
    }
  }

  return false;
}

std::set<std::string> DialogueBook::referencedTitles() const
{
  std::set<std::string> titles;

  for (const Block &block : blocks)
  {
    for (const Clause &clause : block.clauses)
    {
      for (const Condition &condition : clause.conditions)
      {
        titles.insert(condition.title);
      }
    }
  }

  return titles;
}

std::set<std::string> DialogueBook::blockTitles() const
{
  std::set<std::string> titles;

  for (const Block &block : blocks)
  {
    titles.insert(block.title);
    titles.insert(block.chainRules.begin(), block.chainRules.end());
  }

  return titles;
}

uint32_t DialogueBook::longestWindow() const
{
  uint32_t longest = 0u;

  for (const Block &block : blocks)
  {
    for (const Clause &clause : block.clauses)
    {
      for (const Condition &condition : clause.conditions)
      {
        if (condition.window.unbounded())
        {
          // One question that reaches back forever settles it for all of them.
          return 0u;
        }

        longest = std::max(longest, condition.window.weeks);
      }
    }
  }

  return longest;
}

bool DialogueBook::holds(const Clause &clause, const std::vector<const FiredEvent *> &parts, const Chronicle &chronicle,
                         const compiler::CompiledProgram &program, const ConsequenceGraph *causes) const
{
  if (clause.always)
  {
    return true;
  }

  if (parts.empty() || parts.front() == nullptr)
  {
    return false;
  }

  // A condition looks strictly back from the FIRST firing of the sentence:
  // the later parts of a chain are the same event still happening, and must
  // not be evidence for the line that tells it.
  const uint32_t before = parts.front()->tick;

  for (const Condition &condition : clause.conditions)
  {
    bool met = false;

    if (condition.causal)
    {
      // Something of that rule, touching one of these people, that the
      // programme says can bring this about.
      if (causes != nullptr)
      {
        ChronicleEvent effect;

        effect.tick = before;
        effect.rule = parts.front()->rule;

        for (const FiredEvent *part : parts)
        {
          if (part != nullptr)
          {
            effect.bound.insert(effect.bound.end(), part->bound.begin(), part->bound.end());
          }
        }

        for (const uint32_t person : effect.bound)
        {
          const size_t index = chronicle.last(person, condition.title, before, condition.window);

          if (index != ~size_t(0) && causes->linked(chronicle.at(index), effect, ~uint32_t(0)))
          {
            met = true;

            break;
          }
        }
      }
    }
    else
    {
      const FiredEvent *part = condition.part < parts.size() ? parts[condition.part] : nullptr;
      const uint32_t who = part == nullptr ? ~uint32_t(0) : boundNode(*part, condition.role, program);

      if (who != ~uint32_t(0))
      {
        if (condition.other.empty())
        {
          met = chronicle.count(who, condition.title, before, condition.window) >= condition.atLeast;
        }
        else
        {
          const FiredEvent *second = condition.otherPart < parts.size() ? parts[condition.otherPart] : nullptr;
          const uint32_t with = second == nullptr ? ~uint32_t(0) : boundNode(*second, condition.other, program);

          met = with != ~uint32_t(0) && chronicle.tookTogether(who, with, condition.title, before, condition.window);
        }
      }
    }

    if (met == condition.negated)
    {
      return false;
    }
  }

  return true;
}

std::vector<std::string> DialogueBook::perform(const Block &block, const std::vector<const FiredEvent *> &parts,
                                               const Chronicle &chronicle, const compiler::CompiledProgram &program,
                                               NameTable &names, const Narrator::GenderLookup &genderOf,
                                               const ConsequenceGraph *causes) const
{
  if (parts.empty() || parts.front() == nullptr)
  {
    return {};
  }

  const FiredEvent &head = *parts.front();
  const uint64_t key = grammarKey(seed, head.tick, head.rule, head.bound.empty() ? 0u : head.bound.front());

  if (block.chance < 100u && (key >> 8u) % 100u >= block.chance)
  {
    return {};
  }

  const Clause *chosen = nullptr;

  for (const Clause &clause : block.clauses)
  {
    if (!clause.lines.empty() && holds(clause, parts, chronicle, program, causes))
    {
      chosen = &clause;

      break;
    }
  }

  if (chosen == nullptr)
  {
    return {};
  }

  PlaceholderContext context;

  context.program = &program;
  context.names = &names;
  context.genderOf = genderOf;

  std::vector<std::string> out;

  for (size_t i = 0u; i < chosen->lines.size(); ++i)
  {
    const Line &line = chosen->lines[i];

    const FiredEvent *from = line.speakerPart < parts.size() ? parts[line.speakerPart] : nullptr;
    const uint32_t speaker = from == nullptr ? ~uint32_t(0) : boundNode(*from, line.speaker, program);

    if (speaker == ~uint32_t(0))
    {
      continue;
    }

    const FiredEvent *at = line.listenerPart < parts.size() ? parts[line.listenerPart] : nullptr;
    const uint32_t listener = line.listener.empty() || at == nullptr ? ~uint32_t(0) : boundNode(*at, line.listener, program);
    const bool addressed = listener != ~uint32_t(0);

    // Each line rolls separately, so an exchange does not repeat one verb.
    // Three subs, because the verb is not interchangeable: the FIRST line of
    // a clause opens the exchange and cannot "reply", and a verb followed by
    // "to <name>" has to be one that takes an indirect object - "put it to
    // him" is English and "put it" on its own is not.
    const uint64_t lineKey = grammarKey(key, i + 1u);
    const char *const which = i != 0u ? "answered" : addressed ? "said-to" : "said";
    const char *const fallback = i != 0u ? "answered" : "said";
    const std::string verb = grammar != nullptr && grammar->has(which) ? grammar->expand(std::string("<") + which + ">", lineKey)
                             : grammar != nullptr && grammar->has(fallback)
                                 ? grammar->expand(std::string("<") + fallback + ">", lineKey)
                                 : std::string(fallback);

    std::string attribution = names.nameOf(speaker) + " " + verb;

    if (addressed)
    {
      attribution += " to " + names.nameOf(listener);
    }

    std::string said = line.text;

    if (grammar != nullptr)
    {
      said = grammar->expand(said, lineKey);
    }

    out.push_back(attribution + ", \"" + renderPlaceholders(said, parts, context) + "\"");
  }

  return out;
}

std::vector<std::string> DialogueBook::speak(const FiredEvent &event, const Chronicle &chronicle,
                                             const compiler::CompiledProgram &program, NameTable &names,
                                             const Narrator::GenderLookup &genderOf, const ConsequenceGraph *causes) const
{
  return speak(std::vector<const FiredEvent *>{&event}, chronicle, program, names, genderOf, causes);
}

std::vector<std::string> DialogueBook::speak(const std::vector<const FiredEvent *> &parts, const Chronicle &chronicle,
                                             const compiler::CompiledProgram &program, NameTable &names,
                                             const Narrator::GenderLookup &genderOf, const ConsequenceGraph *causes) const
{
  if (parts.empty() || parts.front() == nullptr)
  {
    return {};
  }

  const auto titleOf = [&program](const FiredEvent &event) {
    return event.rule < program.ruleNames.size() ? ruleTitleOf(program.ruleNames[event.rule]) : std::string();
  };

  // A chain block that matches every part in order wins: the dialogue for a
  // whole arc is not the dialogue for either half of it.
  if (parts.size() > 1u)
  {
    for (const Block &block : blocks)
    {
      if (block.chainRules.size() != parts.size())
      {
        continue;
      }

      bool matches = true;

      for (size_t i = 0u; i < parts.size() && matches; ++i)
      {
        matches = parts[i] != nullptr && titleOf(*parts[i]) == block.chainRules[i];
      }

      if (!matches)
      {
        continue;
      }

      std::vector<std::string> lines = perform(block, parts, chronicle, program, names, genderOf, causes);

      if (!lines.empty())
      {
        return lines;
      }
    }
  }

  // Failing that, each part is offered to its own rule's block, earliest
  // first, and the first that speaks is used.
  for (const FiredEvent *part : parts)
  {
    if (part == nullptr)
    {
      continue;
    }

    const std::string title = titleOf(*part);

    for (const Block &block : blocks)
    {
      if (!block.chainRules.empty() || block.title != title)
      {
        continue;
      }

      std::vector<std::string> lines =
          perform(block, std::vector<const FiredEvent *>{part}, chronicle, program, names, genderOf, causes);

      if (!lines.empty())
      {
        return lines;
      }
    }
  }

  return {};
}

} // namespace graphrewrite::runtime
