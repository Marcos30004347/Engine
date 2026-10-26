#pragma once

// AST for the graph-rewrite rule language.
//
// Every node derives from AstNode and parses itself through the Parser's verbs.
// The Parser owns the Lexer privately, so a node cannot reach the token stream
// directly: the rule that nodes only see `peek/accept/expect/error` is enforced
// by the type system rather than by convention.

#include "graphrewrite/frontend/Diagnostics.hpp"
#include "graphrewrite/frontend/Expr.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace graphrewrite {

class Parser;

enum class NodeKind
{
  Program,
  LetDecl,
  RuleDecl,
  SetDecl,
  Conjunction,
  Disjunction,
  Sentence,
};

const char *nodeKindName(NodeKind kind);

class Program;
class LetDecl;
class RuleDecl;
class SetDecl;
class Conjunction;
class Disjunction;
class Sentence;

class AstVisitor
{
public:
  virtual ~AstVisitor() = default;

  virtual void visit(Program &) {}
  virtual void visit(LetDecl &) {}
  virtual void visit(RuleDecl &) {}
  virtual void visit(SetDecl &) {}
  virtual void visit(Conjunction &) {}
  virtual void visit(Disjunction &) {}
  virtual void visit(Sentence &) {}
};

class AstNode
{
public:
  virtual ~AstNode() = default;

  virtual NodeKind kind() const = 0;
  virtual void accept(AstVisitor &visitor) = 0;

  SourceSpan span;
};

// A declaration is one of let / rule / set. The system this is ported from used
// a linked list of nodes carrying three nullable pointers, one per kind; a base
// class with a kind() says the same thing without the two always-null members.
class Declaration : public AstNode
{
};

// `word(key=value, ...)` annotations. Parsed and carried, but deliberately kept
// out of the pattern string: two sentences that differ only in annotations are
// the same edge type.
struct KeyValue
{
  std::string key;
  std::string value;
  SourceSpan span;
};

// A run of words. A word whose first character is uppercase is a variable.
//
//   Character at Location   ->  pattern "? at ?",  variables [Character, Location]
//   Character is dead       ->  pattern "? is dead", variables [Character]
//
// The pattern string IS the edge type's identity, which is why the words are
// joined with a separator. The implementation this is ported from concatenated
// them, so `A b c D` and `A bc D` both produced "?bc?" and silently became one
// edge type - and the duplicate-let check then rejected the second as a
// redeclaration of the first.
class Sentence : public AstNode
{
public:
  struct Word
  {
    std::string text;
    bool isVariable = false;
  };

  static std::unique_ptr<Sentence> parse(Parser &parser, uint32_t minVariables, uint32_t maxVariables);

  // A copy of `source` with its variables renamed. This is how a `set` is
  // expanded at its call site: the set's parameters become the caller's
  // arguments, and everything else about the sentence - and so its edge type -
  // is unchanged, because the pattern writes every variable as `?` anyway.
  static std::unique_ptr<Sentence> substituted(const Sentence &source, const std::map<std::string, std::string> &renaming);

  NodeKind kind() const override
  {
    return NodeKind::Sentence;
  }

  void accept(AstVisitor &visitor) override
  {
    visitor.visit(*this);
  }

  const std::string &pattern() const
  {
    return patternText;
  }

  const std::string &original() const
  {
    return originalText;
  }

  const std::vector<std::string> &variables() const
  {
    return variableNames;
  }

  const std::vector<Word> &words() const
  {
    return wordList;
  }

  const std::vector<KeyValue> &annotations() const
  {
    return annotationList;
  }

  // The parenthesised call after the sentence, if any: a guard on a WHEN edge,
  // a schedule on a THEN edge. Kept out of the pattern string, like
  // annotations - the edge type is the words, not the condition.
  const Expr *clause() const
  {
    return hasClause ? &clauseExpr : nullptr;
  }

  // Variables the clause mentions. Not part of variables(): they do not bind
  // anything, they refer to what the sentences bound.
  const std::vector<std::string> &clauseVariables() const
  {
    return clauseVariableNames;
  }

  // Gives a sentence a clause it did not write itself - `married(A, B)
  // (occurAt(now() + 4))` schedules every sentence the set expands to. A
  // sentence that already has one keeps its own.
  void attachClause(const Expr &expr);

private:
  void finish();

  std::vector<Word> wordList;
  std::vector<KeyValue> annotationList;
  std::string patternText;
  std::string originalText;
  std::vector<std::string> variableNames;
  bool hasClause = false;
  Expr clauseExpr;
  std::vector<std::string> clauseVariableNames;
};

// One or more sentences joined by `or`. A sentence written on its own is a
// one-element disjunction, which is what lets the AND-only grammar stay exactly
// as it was when `or` was added.
class Disjunction : public AstNode
{
public:
  static std::unique_ptr<Disjunction> parse(Parser &parser);

  // Used when a `set` is expanded: a set body may itself contain `or`, so a
  // whole disjunction has to survive the renaming, not just a single sentence.
  static std::unique_ptr<Disjunction> substituted(const Disjunction &source,
                                                  const std::map<std::string, std::string> &renaming);

  NodeKind kind() const override
  {
    return NodeKind::Disjunction;
  }

  void accept(AstVisitor &visitor) override
  {
    visitor.visit(*this);
  }

  const std::vector<std::unique_ptr<Sentence>> &alternatives() const
  {
    return alternativeList;
  }

private:
  std::vector<std::unique_ptr<Sentence>> alternativeList;
};

// Disjunctions joined by `,`. `,` is AND.
class Conjunction : public AstNode
{
public:
  static std::unique_ptr<Conjunction> parse(Parser &parser);

  NodeKind kind() const override
  {
    return NodeKind::Conjunction;
  }

  void accept(AstVisitor &visitor) override
  {
    visitor.visit(*this);
  }

  const std::vector<std::unique_ptr<Disjunction>> &terms() const
  {
    return termList;
  }

private:
  // Set calls are expanded HERE, as the conjunction is parsed, so every later
  // stage - or-expansion, semantic analysis, the emitter - only ever sees plain
  // sentences and needs to know nothing about sets.
  static void parseTerm(Parser &parser, std::vector<std::unique_ptr<Disjunction>> &into);

  std::vector<std::unique_ptr<Disjunction>> termList;
};

// `set <name>(<VAR>, ...) = <conjunction>.`
//
// A named, parameterised conjunction: somewhere to put the group of edges that
// several rules all need to match AND restate. Restating is not optional in this
// language - a WHEN edge the THEN omits is deleted - so the same four or five
// edges get written twice in every rule, and a single omission silently unmakes
// part of the world. A set lets that group be named once and used twice.
//
// Expansion is textual in the variables only: `couple(X, Y)` renames the set's
// parameters to X and Y and splices its sentences in. Nothing is created that a
// hand-written conjunction could not express.
//
// A set may use sets declared BEFORE it. That ordering is what makes recursion
// impossible by construction, rather than by a cycle check.
class SetDecl : public Declaration
{
public:
  static std::unique_ptr<SetDecl> parse(Parser &parser);

  NodeKind kind() const override
  {
    return NodeKind::SetDecl;
  }

  void accept(AstVisitor &visitor) override
  {
    visitor.visit(*this);
  }

  const std::string &name() const
  {
    return setName;
  }

  const std::vector<std::string> &parameters() const
  {
    return parameterNames;
  }

  // `set living(A...) = A is alive.` - the last parameter takes every remaining
  // argument, and the body is emitted once per argument. `living(X, Y, Z)` is
  // then three copies of the body, which is the thing you actually want to write
  // when a rule needs "all of these are alive".
  bool isVariadic() const
  {
    return variadic;
  }

  const Conjunction &body() const
  {
    return *bodyNode;
  }

private:
  std::string setName;
  std::vector<std::string> parameterNames;
  bool variadic = false;
  std::unique_ptr<Conjunction> bodyNode;
};

class LetDecl : public Declaration
{
public:
  static std::unique_ptr<LetDecl> parse(Parser &parser);

  NodeKind kind() const override
  {
    return NodeKind::LetDecl;
  }

  void accept(AstVisitor &visitor) override
  {
    visitor.visit(*this);
  }

  const Sentence &sentence() const
  {
    return *sentenceNode;
  }

private:
  std::unique_ptr<Sentence> sentenceNode;
};

// Whether a matched row actually rewrites.
//
// ALWAYS is the default and what every rule did before this existed. MAYBE(n)
// fires with probability n/10, rolled PER ROW - so one match of a rule can fire
// while another match of the same rule does not.
enum class FirePolicy
{
  Always,
  Maybe,
};

// n in MAYBE(n). 0 never fires, 10 always does; `maybeDenominator` is the scale
// rather than a literal 10 everywhere.
constexpr uint32_t maybeDenominator = 10u;

// `maybe 3` is 3 in 10. `maybe 1 of 200` is 1 in 200: a rule that runs once per
// PAIR of people every tick needs a chance well under a tenth, and a scale of
// ten cannot say so.
struct RulePolicy
{
  FirePolicy kind = FirePolicy::Always;
  uint32_t chance = maybeDenominator;
  uint32_t denominator = maybeDenominator;

  bool alwaysFires() const
  {
    return kind == FirePolicy::Always || chance >= denominator;
  }

  bool neverFires() const
  {
    return kind == FirePolicy::Maybe && chance == 0u;
  }

  // The chance on the runtime's scale of a thousand. ALWAYS is 1000 so the dice
  // can skip the roll. Rounded to nearest, and never rounded to zero for a
  // rule that can fire: `1 of 5000` still gets one thousandth.
  uint32_t permille() const
  {
    if (alwaysFires())
    {
      return 1000u;
    }

    if (chance == 0u || denominator == 0u)
    {
      return 0u;
    }

    const uint64_t scaled = (static_cast<uint64_t>(chance) * 1000u + denominator / 2u) / denominator;

    return static_cast<uint32_t>(scaled == 0u ? 1u : scaled);
  }
};

class RuleDecl : public Declaration
{
public:
  static std::unique_ptr<RuleDecl> parse(Parser &parser);

  NodeKind kind() const override
  {
    return NodeKind::RuleDecl;
  }

  void accept(AstVisitor &visitor) override
  {
    visitor.visit(*this);
  }

  // The title is a sentence with no variables, which is how the grammar keeps
  // `rule do-wake-up:` and a real sentence apart without a separate lexer mode.
  const Sentence &title() const
  {
    return *titleNode;
  }

  const Conjunction &when() const
  {
    return *whenNode;
  }

  const Conjunction &then() const
  {
    return *thenNode;
  }

  const RulePolicy &policy() const
  {
    return firePolicy;
  }

private:
  RulePolicy firePolicy;
  std::unique_ptr<Sentence> titleNode;
  std::unique_ptr<Conjunction> whenNode;
  std::unique_ptr<Conjunction> thenNode;
};

class Program : public AstNode
{
public:
  static std::unique_ptr<Program> parse(Parser &parser);

  NodeKind kind() const override
  {
    return NodeKind::Program;
  }

  void accept(AstVisitor &visitor) override
  {
    visitor.visit(*this);
  }

  const std::vector<std::unique_ptr<Declaration>> &declarations() const
  {
    return declarationList;
  }

  void add(std::unique_ptr<Declaration> declaration);

private:
  std::vector<std::unique_ptr<Declaration>> declarationList;
};

} // namespace graphrewrite
