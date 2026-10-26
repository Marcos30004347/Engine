#include "graphrewrite/runtime/Placeholders.hpp"

#include "graphrewrite/runtime/Narrator.hpp"

#include <algorithm>

namespace graphrewrite::runtime {

std::string capitaliseWord(std::string word)
{
  if (!word.empty() && word[0] >= 'a' && word[0] <= 'z')
  {
    word[0] = static_cast<char>(word[0] - 'a' + 'A');
  }

  return word;
}

std::string genderWord(const std::string &form, const Gender gender)
{
  struct Entry
  {
    const char *form;
    const char *masculine;
    const char *feminine;
    const char *neutral;
  };

  static const Entry entries[] = {
      {"he", "he", "she", "they"},
      {"him", "him", "her", "them"},
      {"his", "his", "her", "their"},
      {"hers", "his", "hers", "theirs"},
      {"man", "man", "woman", "person"},
      {"son", "son", "daughter", "child"},
      {"husband", "husband", "wife", "spouse"},
      {"father", "father", "mother", "parent"},
      {"brother", "brother", "sister", "sibling"},
      {"boy", "boy", "girl", "child"},
      {"lord", "lord", "lady", "lord"},
      {"sir", "sir", "madam", "friend"},
      {"widow", "widower", "widow", "widow"},
  };

  for (const Entry &entry : entries)
  {
    if (form == entry.form)
    {
      return gender == Gender::Masculine ? entry.masculine : gender == Gender::Feminine ? entry.feminine : entry.neutral;
    }
  }

  return form;
}

uint32_t boundNode(const FiredEvent &event, const std::string &variable, const compiler::CompiledProgram &program)
{
  static const std::vector<std::string> none;

  const std::vector<std::string> &vertexNames = event.rule < program.ruleVertexNames.size() ? program.ruleVertexNames[event.rule] : none;

  const auto found = std::find(vertexNames.begin(), vertexNames.end(), variable);

  if (found == vertexNames.end())
  {
    return ~uint32_t(0);
  }

  const size_t vertex = static_cast<size_t>(found - vertexNames.begin());

  return vertex < event.bound.size() ? event.bound[vertex] : ~uint32_t(0);
}

std::string renderPlaceholders(const std::string &text, const std::vector<const FiredEvent *> &parts, const PlaceholderContext &context)
{
  if (context.program == nullptr || context.names == nullptr)
  {
    return text;
  }

  std::string out;

  size_t at = 0u;

  while (at < text.size())
  {
    const size_t open = text.find('{', at);

    if (open == std::string::npos)
    {
      out += text.substr(at);

      break;
    }

    const size_t close = text.find('}', open);

    if (close == std::string::npos)
    {
      out += text.substr(at);

      break;
    }

    out += text.substr(at, open - at);

    // {A}, {A:his}, {2.A}, {2.A:His}
    std::string spec = text.substr(open + 1u, close - open - 1u);
    size_t part = 0u;

    const size_t dot = spec.find('.');

    if (dot != std::string::npos && dot > 0u && spec[0] >= '1' && spec[0] <= '9')
    {
      part = static_cast<size_t>(spec[0] - '1');
      spec = spec.substr(dot + 1u);
    }

    std::string variable = spec;
    std::string form;

    const size_t colon = spec.find(':');

    if (colon != std::string::npos)
    {
      variable = spec.substr(0u, colon);
      form = spec.substr(colon + 1u);
    }

    std::string replacement = "{" + spec + "}";

    if (part < parts.size() && parts[part] != nullptr)
    {
      const uint32_t id = boundNode(*parts[part], variable, *context.program);

      if (id != ~uint32_t(0))
      {
        if (form.empty())
        {
          replacement = context.names->nameOf(id);
        }
        else
        {
          const bool capital = form[0] >= 'A' && form[0] <= 'Z';
          std::string lower = form;

          if (capital)
          {
            lower[0] = static_cast<char>(lower[0] - 'A' + 'a');
          }

          const std::string word = genderWord(lower, context.genderOf ? context.genderOf(id) : Gender::Unknown);

          replacement = capital ? capitaliseWord(word) : word;
        }
      }
    }

    out += replacement;
    at = close + 1u;
  }

  return out;
}

} // namespace graphrewrite::runtime
