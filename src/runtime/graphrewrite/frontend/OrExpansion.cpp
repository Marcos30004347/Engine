#include "graphrewrite/frontend/OrExpansion.hpp"

#include <set>

namespace graphrewrite {
namespace {

constexpr uint64_t saturation = ~uint64_t(0);

uint64_t multiplySaturating(const uint64_t a, const uint64_t b)
{
  if (a == 0u || b == 0u)
  {
    return 0u;
  }

  if (a > saturation / b)
  {
    return saturation;
  }

  return a * b;
}

uint64_t productOf(const Conjunction &conjunction)
{
  uint64_t total = 1u;

  for (const std::unique_ptr<Disjunction> &term : conjunction.terms())
  {
    total = multiplySaturating(total, term->alternatives().size());
  }

  return total;
}

// Advances an odometer over the alternative counts. Returns false when it wraps,
// which is what ends the enumeration.
bool advance(std::vector<uint32_t> &digits, const std::vector<uint32_t> &limits)
{
  for (size_t i = digits.size(); i-- > 0u;)
  {
    if (++digits[i] < limits[i])
    {
      return true;
    }

    digits[i] = 0u;
  }

  return false;
}

void collectLimits(const Conjunction &conjunction, std::vector<uint32_t> &limits)
{
  for (const std::unique_ptr<Disjunction> &term : conjunction.terms())
  {
    limits.push_back(static_cast<uint32_t>(term->alternatives().size()));
  }
}

void select(const Conjunction &conjunction, const std::vector<uint32_t> &digits, const size_t offset,
            std::vector<const Sentence *> &out)
{
  const std::vector<std::unique_ptr<Disjunction>> &terms = conjunction.terms();

  for (size_t i = 0u; i < terms.size(); ++i)
  {
    out.push_back(terms[i]->alternatives()[digits[offset + i]].get());
  }
}

// Identity of a variant for de-duplication: the pattern and variable list of
// every sentence, in order. Two alternatives written identically produce the
// same key and only the first survives.
std::string keyOf(const std::vector<const Sentence *> &when, const std::vector<const Sentence *> &then)
{
  std::string key;

  for (const std::vector<const Sentence *> *clause : {&when, &then})
  {
    key += '|';

    for (const Sentence *sentence : *clause)
    {
      key += sentence->pattern();
      key += '<';

      for (const std::string &variable : sentence->variables())
      {
        key += variable;
        key += ',';
      }

      key += ">;";
    }
  }

  return key;
}

} // namespace

uint64_t countVariants(const RuleDecl &rule)
{
  return multiplySaturating(productOf(rule.when()), productOf(rule.then()));
}

std::vector<RuleVariant> expandOr(const RuleDecl &rule, const OrExpansionOptions &options, DiagnosticSink &sink)
{
  std::vector<RuleVariant> variants;

  const uint64_t total = countVariants(rule);

  if (total > options.maxVariantsPerRule)
  {
    sink.error(rule.span, "rule '" + rule.title().original() + "' expands to " + std::to_string(total) + " variants, over the limit of " +
                              std::to_string(options.maxVariantsPerRule));

    return variants;
  }

  std::vector<uint32_t> limits;

  collectLimits(rule.when(), limits);

  const size_t whenTerms = limits.size();

  collectLimits(rule.then(), limits);

  if (limits.empty())
  {
    return variants;
  }

  std::vector<uint32_t> digits(limits.size(), 0u);
  std::set<std::string> seen;

  do
  {
    std::vector<const Sentence *> when;
    std::vector<const Sentence *> then;

    select(rule.when(), digits, 0u, when);
    select(rule.then(), digits, whenTerms, then);

    const std::string key = keyOf(when, then);

    if (seen.count(key) != 0u)
    {
      continue;
    }

    seen.insert(key);

    RuleVariant variant;

    // Numbered over the survivors so the names stay dense after de-duplication.
    variant.name = rule.title().original() + "#" + std::to_string(variants.size());
    variant.policy = rule.policy();
    variant.when = std::move(when);
    variant.then = std::move(then);
    variant.span = rule.span;

    variants.push_back(std::move(variant));
  } while (advance(digits, limits));

  return variants;
}

} // namespace graphrewrite
