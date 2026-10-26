#include "graphrewrite/runtime/Grammar.hpp"

namespace graphrewrite::runtime {
namespace {

// How deep a sub may reach into other subs before expansion gives up. A
// grammar that names itself is a mistake in the file, not a hang.
constexpr uint32_t maxDepth = 8u;

// How many copies of an alternative a `*N` may ask for. A weight is a nudge,
// not a way to allocate a megabyte from a text file.
constexpr uint32_t maxWeight = 64u;

std::string trim(const std::string &text)
{
  const size_t first = text.find_first_not_of(" \t\r\n");

  if (first == std::string::npos)
  {
    return std::string();
  }

  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1u);
}

// A name is what may sit between the angle brackets: letters, digits, dash,
// underscore. Anything else means the `<` was punctuation, not a placeholder.
bool isNameChar(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

uint64_t mix(uint64_t value)
{
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33u;

  return value;
}

uint64_t hashName(const std::string &name)
{
  uint64_t value = 0xcbf29ce484222325ull;

  for (const char c : name)
  {
    value = (value ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
  }

  return value;
}

} // namespace

uint64_t grammarKey(const uint64_t seed, const uint64_t a, const uint64_t b, const uint64_t c)
{
  return mix(mix(mix(seed * 0x9e3779b97f4a7c15ull + a) + b) + c);
}

bool Grammar::isSubLine(const std::string &line)
{
  return trim(line).rfind("sub ", 0u) == 0;
}

const std::vector<std::string> &Grammar::alternativesOf(const std::string &name) const
{
  static const std::vector<std::string> none;

  const auto found = subs.find(name);

  return found == subs.end() ? none : found->second;
}

void Grammar::add(const std::string &name, const std::vector<std::string> &alternatives)
{
  std::vector<std::string> &into = subs[name];

  into.insert(into.end(), alternatives.begin(), alternatives.end());
}

bool Grammar::addLine(const std::string &line, const std::string &path, const uint32_t lineNumber, DiagnosticSink &sink)
{
  SourceSpan span;

  span.line = lineNumber;
  span.column = 1u;

  const std::string body = trim(trim(line).substr(4u));
  const size_t colon = body.find(':');

  if (colon == std::string::npos)
  {
    sink.error(span, path + ":" + std::to_string(lineNumber) + ": expected 'sub <name>: a | b | c'");

    return false;
  }

  const std::string name = trim(body.substr(0u, colon));

  if (name.empty())
  {
    sink.error(span, path + ":" + std::to_string(lineNumber) + ": a sub needs a name");

    return false;
  }

  std::vector<std::string> alternatives;

  const std::string list = body.substr(colon + 1u);
  size_t at = 0u;

  while (at <= list.size())
  {
    const size_t bar = list.find('|', at);
    std::string part = trim(list.substr(at, bar == std::string::npos ? std::string::npos : bar - at));

    // `text *3` repeats the alternative, which is how a weight is spelled.
    uint32_t weight = 1u;
    const size_t star = part.rfind(" *");

    if (star != std::string::npos)
    {
      const std::string digits = trim(part.substr(star + 2u));
      bool numeric = !digits.empty();

      for (const char c : digits)
      {
        numeric = numeric && c >= '0' && c <= '9';
      }

      if (numeric)
      {
        weight = static_cast<uint32_t>(std::strtoul(digits.c_str(), nullptr, 10));
        weight = weight == 0u ? 1u : weight > maxWeight ? maxWeight : weight;
        part = trim(part.substr(0u, star));
      }
    }

    if (!part.empty())
    {
      for (uint32_t i = 0u; i < weight; ++i)
      {
        alternatives.push_back(part);
      }
    }

    if (bar == std::string::npos)
    {
      break;
    }

    at = bar + 1u;
  }

  if (alternatives.empty())
  {
    sink.error(span, path + ":" + std::to_string(lineNumber) + ": sub '" + name + "' has no alternatives");

    return false;
  }

  add(name, alternatives);

  return true;
}

std::string Grammar::expand(const std::string &text, const uint64_t key) const
{
  uint32_t counter = 0u;

  return expandAt(text, key, 0u, counter);
}

std::string Grammar::expandAt(const std::string &text, const uint64_t key, const uint32_t depth, uint32_t &counter) const
{
  if (subs.empty() || text.find('<') == std::string::npos)
  {
    return text;
  }

  std::string out;
  size_t at = 0u;

  while (at < text.size())
  {
    const size_t open = text.find('<', at);

    if (open == std::string::npos)
    {
      out += text.substr(at);

      break;
    }

    const size_t close = text.find('>', open);

    if (close == std::string::npos)
    {
      out += text.substr(at);

      break;
    }

    const std::string name = text.substr(open + 1u, close - open - 1u);

    bool plausible = !name.empty();

    for (const char c : name)
    {
      plausible = plausible && isNameChar(c);
    }

    out += text.substr(at, open - at);

    const auto found = plausible ? subs.find(name) : subs.end();

    if (found == subs.end() || found->second.empty())
    {
      // Not a sub: leave it exactly as written.
      out += text.substr(open, close - open + 1u);
      at = close + 1u;

      continue;
    }

    // Every expansion in one text draws a different roll, so a sentence with
    // two `<said>`s does not say the same word twice by construction.
    const uint64_t roll = mix(key ^ hashName(name) ^ (static_cast<uint64_t>(counter++) * 0x9e3779b97f4a7c15ull));
    const std::string &chosen = found->second[roll % found->second.size()];

    out += depth + 1u >= maxDepth ? chosen : expandAt(chosen, key, depth + 1u, counter);

    at = close + 1u;
  }

  return out;
}

} // namespace graphrewrite::runtime
