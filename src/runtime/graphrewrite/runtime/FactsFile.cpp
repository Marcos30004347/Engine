#include "graphrewrite/runtime/FactsFile.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace graphrewrite::runtime {
namespace {

std::vector<std::string> wordsOf(const std::string &line)
{
  std::vector<std::string> words;
  std::istringstream stream(line);
  std::string word;

  while (stream >> word)
  {
    words.push_back(word);
  }

  return words;
}

bool isName(const std::string &word)
{
  return !word.empty() && word[0] >= 'A' && word[0] <= 'Z';
}

bool isNumber(const std::string &word)
{
  return !word.empty() && std::all_of(word.begin(), word.end(), [](const char c) { return c >= '0' && c <= '9'; });
}

// Does this line instantiate the pattern? Same word count, literal words equal,
// and a NAME wherever the pattern has `?`. Names are collected in order.
bool matches(const std::vector<std::string> &words, const std::vector<std::string> &pattern, std::vector<std::string> &names)
{
  if (words.size() != pattern.size())
  {
    return false;
  }

  names.clear();

  for (size_t i = 0u; i < words.size(); ++i)
  {
    if (pattern[i] == "?")
    {
      if (!isName(words[i]) && !isNumber(words[i]))
      {
        return false;
      }

      names.push_back(words[i]);
    }
    else if (pattern[i] != words[i])
    {
      return false;
    }
  }

  return true;
}

} // namespace

bool loadFactsText(const std::string &text, const compiler::EdgeTypeTable &types, const std::string &path, DiagnosticSink &sink,
                   LoadedFacts &out)
{
  const uint32_t errorsBefore = sink.errorCount();

  // Tokenised once, not per line: the corpus has a dozen types and a world can
  // have thousands of lines.
  std::vector<std::vector<std::string>> patterns;

  for (compiler::EdgeTypeId type = 0u; type < types.size(); ++type)
  {
    patterns.push_back(wordsOf(types[type].pattern));
  }

  bool unresolved = false;

  const auto idFor = [&out, &unresolved](const std::string &name) {
    if (isNumber(name))
    {
      uint32_t id = 0u;

      if (out.valueResolver && out.valueResolver(static_cast<uint32_t>(std::stoul(name)), id))
      {
        return id;
      }

      unresolved = true;

      return 0u;
    }

    const auto existing = out.ids.find(name);

    if (existing != out.ids.end())
    {
      return existing->second;
    }

    const uint32_t id = static_cast<uint32_t>(out.names.size());

    out.ids.emplace(name, id);
    out.names.push_back(name);

    return id;
  };

  std::istringstream lines(text);
  std::string line;
  uint32_t number = 0u;

  while (std::getline(lines, line))
  {
    ++number;

    const size_t hash = line.find('#');

    if (hash != std::string::npos)
    {
      line.erase(hash);
    }

    const std::vector<std::string> words = wordsOf(line);

    if (words.empty())
    {
      continue;
    }

    SourceSpan span;

    span.line = number;
    span.column = 1u;

    bool found = false;

    for (compiler::EdgeTypeId type = 0u; type < types.size() && !found; ++type)
    {
      std::vector<std::string> names;

      if (!matches(words, patterns[type], names))
      {
        continue;
      }

      found = true;
      unresolved = false;

      if (names.size() == 2u)
      {
        out.facts.emplace_back(idFor(names[0]), type, idFor(names[1]));
      }
      else if (names.size() == 1u)
      {
        // A unary edge is a self-loop, as it is everywhere else in the system.
        const uint32_t id = idFor(names[0]);

        out.facts.emplace_back(id, type, id);
      }
      else
      {
        sink.error(span, path + ": '" + line + "' names " + std::to_string(names.size()) + " nodes; an edge has one or two");
      }
    }

    if (!found)
    {
      sink.error(span, path + ":" + std::to_string(number) + ": '" + line + "' matches no declared edge type");
    }
    else if (unresolved)
    {
      sink.error(span, path + ":" + std::to_string(number) + ": '" + line + "' has a number where a name goes, and nothing resolves values");
    }
  }

  return sink.errorCount() == errorsBefore;
}

bool loadFactsFile(const std::string &path, const compiler::EdgeTypeTable &types, DiagnosticSink &sink, LoadedFacts &out)
{
  std::ifstream file(path);

  if (!file)
  {
    SourceSpan span;

    sink.error(span, "cannot open facts file " + path);

    return false;
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  return loadFactsText(buffer.str(), types, path, sink, out);
}

} // namespace graphrewrite::runtime
