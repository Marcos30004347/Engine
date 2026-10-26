#include "graphrewrite/frontend/SemanticAnalysis.hpp"

#include <map>
#include <set>
#include <string>

namespace graphrewrite {
namespace {

void checkDuplicateLets(const Program &program, DiagnosticSink &sink, AnalysedProgram &out,
                        std::map<std::string, const LetDecl *> &patterns)
{
  for (const std::unique_ptr<Declaration> &declaration : program.declarations())
  {
    if (declaration->kind() != NodeKind::LetDecl)
    {
      continue;
    }

    const LetDecl &let = static_cast<const LetDecl &>(*declaration);
    const std::string &pattern = let.sentence().pattern();

    const auto existing = patterns.find(pattern);

    if (existing != patterns.end())
    {
      sink.error(let.span, "pattern '" + pattern + "' is already declared at line " + std::to_string(existing->second->span.line));

      continue;
    }

    patterns.emplace(pattern, &let);
    out.declarations.push_back(&let);
  }
}

void checkDuplicateRuleTitles(const Program &program, DiagnosticSink &sink)
{
  std::map<std::string, const RuleDecl *> titles;

  for (const std::unique_ptr<Declaration> &declaration : program.declarations())
  {
    if (declaration->kind() != NodeKind::RuleDecl)
    {
      continue;
    }

    const RuleDecl &rule = static_cast<const RuleDecl &>(*declaration);
    const std::string &title = rule.title().original();

    const auto existing = titles.find(title);

    if (existing != titles.end())
    {
      sink.error(rule.span, "rule '" + title + "' is already declared at line " + std::to_string(existing->second->span.line));

      continue;
    }

    titles.emplace(title, &rule);
  }
}

// Every sentence must name a pattern some `let` declared, and its arity must
// match that declaration.
void checkSentenceAgainstDeclarations(const Sentence &sentence, const std::string &context,
                                      const std::map<std::string, const LetDecl *> &patterns, DiagnosticSink &sink)
{
  const auto declared = patterns.find(sentence.pattern());

  if (declared == patterns.end())
  {
    sink.error(sentence.span, context + ": no 'let' declares '" + sentence.pattern() + "'");

    return;
  }

  const size_t declaredArity = declared->second->sentence().variables().size();

  if (sentence.variables().size() != declaredArity)
  {
    sink.error(sentence.span, context + ": '" + sentence.pattern() + "' takes " + std::to_string(declaredArity) + " variable(s), used with " +
                                  std::to_string(sentence.variables().size()));
  }
}

// `C'` is new, `C` is not. Only the THEN can introduce one, which is what the
// caller checks by reaching here at all.
bool isNewVariable(const std::string &name)
{
  return !name.empty() && name.back() == '\'';
}

void checkVariant(const RuleVariant &variant, const std::map<std::string, const LetDecl *> &patterns, DiagnosticSink &sink)
{
  std::set<std::string> bound;

  for (const Sentence *sentence : variant.when)
  {
    checkSentenceAgainstDeclarations(*sentence, "variant '" + variant.name + "'", patterns, sink);

    for (const std::string &variable : sentence->variables())
    {
      bound.insert(variable);
    }
  }

  std::set<std::string> unbound;

  for (const Sentence *sentence : variant.then)
  {
    checkSentenceAgainstDeclarations(*sentence, "variant '" + variant.name + "'", patterns, sink);

    for (const std::string &variable : sentence->variables())
    {
      if (bound.count(variable) != 0u)
      {
        continue;
      }

      // A trailing quote declares the variable NEW: it is not a match that
      // failed to bind, it is a node the rule creates. The backend already
      // supports these - anything numbered at or above matchedVertexCount gets a
      // fresh id per row - but without a marker there is no way to tell a
      // deliberate birth from a typo, so every one of them was an error.
      //
      // The corpus also uses C' as an ordinary bound variable name. That stays
      // legal: a quoted variable bound in the WHEN never reaches this branch.
      if (isNewVariable(variable))
      {
        continue;
      }

      unbound.insert(variable);
    }
  }

  // A clause refers to variables; it binds none. Everything it names must be
  // bound by a sentence somewhere in the rule - a WHEN variable, or one the THEN
  // creates (a schedule may well mention the child it is about to create).
  std::set<std::string> known = bound;

  for (const Sentence *sentence : variant.then)
  {
    for (const std::string &variable : sentence->variables())
    {
      known.insert(variable);
    }
  }

  for (const std::vector<const Sentence *> *clause : {&variant.when, &variant.then})
  {
    for (const Sentence *sentence : *clause)
    {
      for (const std::string &variable : sentence->clauseVariables())
      {
        if (known.count(variable) == 0u)
        {
          sink.error(sentence->span, "variant '" + variant.name + "': clause on '" + sentence->original() + "' uses '" + variable +
                                         "', which no sentence binds");
        }
      }
    }
  }

  if (unbound.empty())
  {
    return;
  }

  std::string names;

  for (const std::string &variable : unbound)
  {
    if (!names.empty())
    {
      names += ", ";
    }

    names += variable;
  }

  sink.error(variant.span, "variant '" + variant.name + "': THEN uses unbound variable(s) " + names);
}

} // namespace

bool analyse(const Program &program, const OrExpansionOptions &options, DiagnosticSink &sink, AnalysedProgram &out)
{
  const uint32_t errorsBefore = sink.errorCount();

  std::map<std::string, const LetDecl *> patterns;

  checkDuplicateLets(program, sink, out, patterns);
  checkDuplicateRuleTitles(program, sink);

  for (const std::unique_ptr<Declaration> &declaration : program.declarations())
  {
    if (declaration->kind() == NodeKind::RuleDecl)
    {
      for (RuleVariant &variant : expandOr(static_cast<const RuleDecl &>(*declaration), options, sink))
      {
        checkVariant(variant, patterns, sink);

        out.variants.push_back(std::move(variant));
      }

      continue;
    }
  }

  return sink.errorCount() == errorsBefore;
}

} // namespace graphrewrite
