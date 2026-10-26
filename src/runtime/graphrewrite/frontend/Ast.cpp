#include <cstdlib>
#include <set>
#include "graphrewrite/frontend/Ast.hpp"

#include "graphrewrite/frontend/Parser.hpp"

namespace graphrewrite {

Expr parseClause(Parser &parser);

namespace {

bool isUppercase(const char c)
{
  return c >= 'A' && c <= 'Z';
}

} // namespace

const char *nodeKindName(const NodeKind kind)
{
  switch (kind)
  {
  case NodeKind::Program:
    return "program";
  case NodeKind::LetDecl:
    return "let declaration";
  case NodeKind::RuleDecl:
    return "rule declaration";
  case NodeKind::Conjunction:
    return "conjunction";
  case NodeKind::Disjunction:
    return "disjunction";
  case NodeKind::Sentence:
    return "sentence";
  }

  return "<unknown>";
}

void Program::add(std::unique_ptr<Declaration> declaration)
{
  declarationList.push_back(std::move(declaration));
}

std::unique_ptr<Program> Program::parse(Parser &parser)
{
  auto program = std::make_unique<Program>();

  const Token first = parser.peek();

  program->span = first.span;

  while (!parser.peekIs(TokenKind::Eof))
  {
    try
    {
      switch (parser.peek().kind)
      {
      case TokenKind::KwLet:
        program->add(LetDecl::parse(parser));
        break;
      case TokenKind::KwRule:
        program->add(RuleDecl::parse(parser));
        break;
      case TokenKind::KwSet:
      {
        // Registered before it is stored, so a later set or rule can call it -
        // and so a set cannot call itself.
        auto declaration = SetDecl::parse(parser);

        parser.addSet(declaration.get());
        program->add(std::move(declaration));

        break;
      }
      default:
        parser.error("expected 'let', 'rule' or 'set'");
      }
    }
    catch (const ParseError &)
    {
      parser.recover();
    }
  }

  return program;
}

void Sentence::finish()
{
  patternText.clear();
  originalText.clear();
  variableNames.clear();
  clauseVariableNames.clear();

  if (hasClause)
  {
    clauseExpr.collectVariables(clauseVariableNames);
  }

  for (const Word &word : wordList)
  {
    if (!patternText.empty())
    {
      patternText += ' ';
      originalText += ' ';
    }

    patternText += word.isVariable ? "?" : word.text;
    originalText += word.text;

    if (word.isVariable)
    {
      variableNames.push_back(word.text);
    }
  }
}

// --- the clause grammar -------------------------------------------------------
//
//   clause := [Variable] call            `(C availableAt(...))` is sugar for
//                                        `(availableAt(C, ...))`
//   call   := ident {'.' ident} '(' [sum {',' sum}] ')'
//   sum    := term {'+' term}
//   term   := Number | call | Variable | Word

Expr parseSum(Parser &parser);

Expr parseCall(Parser &parser)
{
  const Token nameToken = parser.expect(TokenKind::Word);

  std::string name(parser.textOf(nameToken));

  // `time.now(...)`: a dot inside a clause continues the name. A dot that ends
  // the declaration is followed by a keyword or the end of input, never a word.
  while (parser.peekIs(TokenKind::Dot) && parser.peekIs(TokenKind::Word, 1u))
  {
    parser.expect(TokenKind::Dot);
    name += "." + std::string(parser.textOf(parser.expect(TokenKind::Word)));
  }

  Expr call = Expr::call(std::move(name));

  parser.expect(TokenKind::LParen);

  if (!parser.peekIs(TokenKind::RParen))
  {
    do
    {
      call.args.push_back(parseSum(parser));
    } while (parser.accept(TokenKind::Comma));
  }

  parser.expect(TokenKind::RParen);

  return call;
}

Expr parseTerm(Parser &parser)
{
  if (parser.peekIs(TokenKind::Number))
  {
    Expr number;

    number.kind = Expr::Kind::Number;
    number.number = static_cast<uint32_t>(std::strtoul(std::string(parser.textOf(parser.expect(TokenKind::Number))).c_str(), nullptr, 10));

    return number;
  }

  if (!parser.peekIs(TokenKind::Word))
  {
    parser.error("expected a number, a variable or a call in a clause");
  }

  if (parser.peekIs(TokenKind::LParen, 1u) || (parser.peekIs(TokenKind::Dot, 1u) && parser.peekIs(TokenKind::Word, 2u)))
  {
    return parseCall(parser);
  }

  const std::string text(parser.textOf(parser.expect(TokenKind::Word)));

  if (!text.empty() && isUppercase(text[0]))
  {
    return Expr::variable(text);
  }

  Expr word;

  word.kind = Expr::Kind::Word;
  word.name = text;

  return word;
}

Expr parseSum(Parser &parser)
{
  Expr left = parseTerm(parser);

  while (parser.accept(TokenKind::Plus))
  {
    Expr sum;

    sum.kind = Expr::Kind::Add;
    sum.args.push_back(std::move(left));
    sum.args.push_back(parseTerm(parser));

    left = std::move(sum);
  }

  return left;
}

Expr parseClause(Parser &parser)
{
  // `(D' = day(now() + 4))`: the new node D' takes the expression's value.
  if (parser.peekIs(TokenKind::Word) && parser.peekIs(TokenKind::Equal, 1u))
  {
    const Token target = parser.expect(TokenKind::Word);
    const std::string text(parser.textOf(target));

    if (text.empty() || !isUppercase(text[0]) || text.back() != '\'')
    {
      // Only a node the THEN creates can be given a value; a matched node
      // already has one.
      parser.errorAt(target.span, "only a new variable (ending in ') can be bound, got '" + text + "'");
    }

    parser.expect(TokenKind::Equal);

    Expr bind;

    bind.kind = Expr::Kind::Bind;
    bind.args.push_back(Expr::variable(text));
    bind.args.push_back(parseSum(parser));

    return bind;
  }

  // `(C availableAt(...))`: a variable, then the call it is the first argument of.
  if (parser.peekIs(TokenKind::Word) && parser.peekIs(TokenKind::Word, 1u))
  {
    const Token leading = parser.expect(TokenKind::Word);
    const std::string text(parser.textOf(leading));

    if (text.empty() || !isUppercase(text[0]))
    {
      parser.errorAt(leading.span, "a clause starts with a variable or a call, got '" + text + "'");
    }

    Expr call = parseCall(parser);

    call.args.insert(call.args.begin(), Expr::variable(text));

    return call;
  }

  return parseCall(parser);
}

std::unique_ptr<Sentence> Sentence::substituted(const Sentence &source, const std::map<std::string, std::string> &renaming)
{
  auto out = std::make_unique<Sentence>();

  for (const Word &word : source.wordList)
  {
    Word copy = word;

    if (word.isVariable)
    {
      const auto entry = renaming.find(word.text);

      if (entry != renaming.end())
      {
        copy.text = entry->second;
      }
    }

    out->wordList.push_back(std::move(copy));
  }

  out->annotationList = source.annotationList;
  out->hasClause = source.hasClause;
  out->clauseExpr = source.clauseExpr;

  std::vector<std::pair<std::string, std::string>> pairs(renaming.begin(), renaming.end());

  out->clauseExpr.rename(pairs);
  out->finish();
  out->span = source.span;

  return out;
}

void Sentence::attachClause(const Expr &expr)
{
  if (hasClause)
  {
    return;
  }

  hasClause = true;
  clauseExpr = expr;
  finish();
}

std::unique_ptr<Disjunction> Disjunction::substituted(const Disjunction &source,
                                                      const std::map<std::string, std::string> &renaming)
{
  auto out = std::make_unique<Disjunction>();

  for (const std::unique_ptr<Sentence> &alternative : source.alternativeList)
  {
    out->alternativeList.push_back(Sentence::substituted(*alternative, renaming));
  }

  out->span = source.span;

  return out;
}

std::unique_ptr<Sentence> Sentence::parse(Parser &parser, const uint32_t minVariables, const uint32_t maxVariables)
{
  const Token start = parser.peek();

  if (!parser.peekIs(TokenKind::Word))
  {
    parser.error("expected a word to start a sentence");
  }

  auto sentence = std::make_unique<Sentence>();

  while (parser.peekIs(TokenKind::Word))
  {
    const Token wordToken = parser.expect(TokenKind::Word);
    const std::string text(parser.textOf(wordToken));

    // `word(key=value)` is an annotation on the word. Anything else in
    // parentheses is a CLAUSE - a guard, a schedule or a bind - and it ends the
    // sentence. An annotation is `( lowercase-key =`; a bind is `( D' =`, and
    // the capital is what tells them apart.
    const bool annotation = parser.peekIs(TokenKind::LParen) && parser.peekIs(TokenKind::Word, 1u) &&
                            parser.peekIs(TokenKind::Equal, 2u) && !isUppercase(parser.textOf(parser.peek(1u))[0]);

    if (parser.peekIs(TokenKind::LParen) && !annotation)
    {
      Word word;

      word.isVariable = !text.empty() && isUppercase(text[0]);
      word.text = text;

      sentence->wordList.push_back(std::move(word));

      parser.expect(TokenKind::LParen);

      sentence->clauseExpr = parseClause(parser);
      sentence->hasClause = true;

      parser.expect(TokenKind::RParen);

      break;
    }

    if (parser.accept(TokenKind::LParen))
    {
      while (!parser.accept(TokenKind::RParen))
      {
        const Token keyToken = parser.expect(TokenKind::Word);

        parser.expect(TokenKind::Equal);

        Token valueToken = parser.peek();

        if (!parser.accept(TokenKind::Word) && !parser.accept(TokenKind::Number))
        {
          parser.error("expected a word or a number as an annotation value");
        }

        KeyValue pair;

        pair.key = std::string(parser.textOf(keyToken));
        pair.value = std::string(parser.textOf(valueToken));
        pair.span = keyToken.span;

        sentence->annotationList.push_back(std::move(pair));

        parser.accept(TokenKind::Comma);
      }
    }

    Word word;

    word.isVariable = !text.empty() && isUppercase(text[0]);
    word.text = text;

    sentence->wordList.push_back(std::move(word));
  }

  sentence->finish();
  sentence->span = parser.spanFrom(start);

  const uint32_t variableCount = static_cast<uint32_t>(sentence->variableNames.size());

  if (variableCount < minVariables || variableCount > maxVariables)
  {
    parser.errorAt(sentence->span,
                   "sentence '" + sentence->originalText + "' has " + std::to_string(variableCount) + " variable(s), expected " +
                       std::to_string(minVariables) + " to " + std::to_string(maxVariables));
  }

  return sentence;
}

std::unique_ptr<Disjunction> Disjunction::parse(Parser &parser)
{
  const Token start = parser.peek();

  auto disjunction = std::make_unique<Disjunction>();

  disjunction->alternativeList.push_back(Sentence::parse(parser, 1u, 2u));

  // `or` binds tighter than `,`, so the alternatives are gathered here, inside
  // one conjunct, and Conjunction::parse never sees them.
  while (parser.accept(TokenKind::KwOr))
  {
    disjunction->alternativeList.push_back(Sentence::parse(parser, 1u, 2u));
  }

  disjunction->span = parser.spanFrom(start);

  return disjunction;
}

// A call looks like `name(A, B)`. A sentence annotation looks like
// `word(key=value)`. They both start `Word LParen Word`, and the fourth token is
// what separates them: a call has `,` or `)` there, an annotation has `=`. That
// is enough to decide without the parser having to know the set names yet, and
// it means a misspelled set name is reported as an unknown set rather than as a
// baffling complaint about an annotation.
bool looksLikeSetCall(Parser &parser)
{
  return parser.peekIs(TokenKind::Word, 0u) && parser.peekIs(TokenKind::LParen, 1u) && parser.peekIs(TokenKind::Word, 2u) &&
         (parser.peekIs(TokenKind::Comma, 3u) || parser.peekIs(TokenKind::RParen, 3u));
}

void Conjunction::parseTerm(Parser &parser, std::vector<std::unique_ptr<Disjunction>> &into)
{
  if (!looksLikeSetCall(parser))
  {
    into.push_back(Disjunction::parse(parser));

    return;
  }

  const Token nameToken = parser.expect(TokenKind::Word);
  const std::string name(parser.textOf(nameToken));

  parser.expect(TokenKind::LParen);

  std::vector<std::string> arguments;
  std::vector<SourceSpan> argumentSpans;

  do
  {
    const Token argument = parser.expect(TokenKind::Word);
    const std::string text(parser.textOf(argument));

    if (text.empty() || !isUppercase(text[0]))
    {
      parser.errorAt(argument.span, "a set argument must be a variable, got '" + text + "'");
    }

    arguments.push_back(text);
    argumentSpans.push_back(argument.span);
  } while (parser.accept(TokenKind::Comma));

  parser.expect(TokenKind::RParen);

  // `married(A, B) (occurAt(now() + 4))`: a clause after the call applies to
  // every sentence the set expands to.
  bool hasCallClause = false;
  Expr callClause;

  if (parser.peekIs(TokenKind::LParen))
  {
    parser.expect(TokenKind::LParen);
    callClause = parseClause(parser);
    parser.expect(TokenKind::RParen);
    hasCallClause = true;
  }

  const SetDecl *declaration = parser.findSet(name);

  if (declaration == nullptr)
  {
    parser.errorAt(nameToken.span, "unknown set '" + name + "'");
  }

  const size_t declared = declaration->parameters().size();

  if (declaration->isVariadic())
  {
    if (arguments.size() < declared)
    {
      parser.errorAt(nameToken.span, "set '" + name + "' takes at least " + std::to_string(declared) + " argument(s), got " +
                                         std::to_string(arguments.size()));
    }
  }
  else if (declared != arguments.size())
  {
    parser.errorAt(nameToken.span, "set '" + name + "' takes " + std::to_string(declared) + " argument(s), got " +
                                       std::to_string(arguments.size()));
  }

  // The fixed parameters bind once; a variadic parameter is bound again for each
  // remaining argument, and the body is emitted once per binding.
  const size_t fixedCount = declaration->isVariadic() ? declared - 1u : declared;

  std::map<std::string, std::string> renaming;

  for (size_t i = 0u; i < fixedCount; ++i)
  {
    renaming[declaration->parameters()[i]] = arguments[i];
  }

  // Freshen every local, so two calls to the same set in one rule do not collide
  // through it. Without this `together(A, B), together(C, D)` would put all four
  // in one village and quietly change what the rule means.
  std::string signature = name + "(";

  for (const std::string &argument : arguments)
  {
    signature += argument + ",";
  }

  const uint32_t expansion = parser.localScopeFor(signature + ")");

  // Locals are resolved ONCE for the whole call, not once per repetition: in
  // `together(A, B...)` the village V should be the same place for everyone the
  // call names, not a different one per argument.
  for (const std::unique_ptr<Disjunction> &term : declaration->body().terms())
  {
    for (const std::unique_ptr<Sentence> &alternative : term->alternatives())
    {
      std::vector<std::string> mentioned = alternative->variables();

      mentioned.insert(mentioned.end(), alternative->clauseVariables().begin(), alternative->clauseVariables().end());

      for (const std::string &variable : mentioned)
      {
        if (renaming.count(variable) != 0u || variable == declaration->parameters().back())
        {
          continue;
        }

        // A trailing quote marks a new node and has to stay last, so the suffix
        // goes before it.
        const bool isNew = !variable.empty() && variable.back() == '\'';
        const std::string stem = isNew ? variable.substr(0u, variable.size() - 1u) : variable;

        renaming[variable] = stem + "__" + std::to_string(expansion) + (isNew ? "'" : "");
      }
    }
  }

  // One pass for a fixed set; one pass per remaining argument for a variadic one.
  // A repeat that produces an edge the clause already has is collapsed later, in
  // buildRuleGraph, so `living(A, A)` costs nothing.
  const size_t passes = declaration->isVariadic() ? arguments.size() - fixedCount : 1u;

  for (size_t pass = 0u; pass < passes; ++pass)
  {
    if (declaration->isVariadic())
    {
      renaming[declaration->parameters().back()] = arguments[fixedCount + pass];
    }

    for (const std::unique_ptr<Disjunction> &term : declaration->body().terms())
    {
      std::unique_ptr<Disjunction> expanded = Disjunction::substituted(*term, renaming);

      if (hasCallClause)
      {
        Expr renamedClause = callClause;

        renamedClause.rename(std::vector<std::pair<std::string, std::string>>(renaming.begin(), renaming.end()));

        for (const std::unique_ptr<Sentence> &alternative : expanded->alternatives())
        {
          alternative->attachClause(renamedClause);
        }
      }

      into.push_back(std::move(expanded));
    }
  }
}

std::unique_ptr<Conjunction> Conjunction::parse(Parser &parser)
{
  const Token start = parser.peek();

  auto conjunction = std::make_unique<Conjunction>();

  parseTerm(parser, conjunction->termList);

  while (parser.accept(TokenKind::Comma))
  {
    parseTerm(parser, conjunction->termList);
  }

  conjunction->span = parser.spanFrom(start);

  return conjunction;
}

std::unique_ptr<LetDecl> LetDecl::parse(Parser &parser)
{
  const Token start = parser.expect(TokenKind::KwLet);

  auto declaration = std::make_unique<LetDecl>();

  declaration->sentenceNode = Sentence::parse(parser, 1u, 2u);

  parser.expect(TokenKind::Dot);

  declaration->span = parser.spanFrom(start);

  return declaration;
}

std::unique_ptr<RuleDecl> RuleDecl::parse(Parser &parser)
{
  const Token start = parser.expect(TokenKind::KwRule);

  // Set-call locals are scoped to one declaration: the same call in a WHEN and a
  // THEN must agree, two different rules need not.
  parser.resetLocalScopes();

  auto declaration = std::make_unique<RuleDecl>();

  declaration->titleNode = Sentence::parse(parser, 0u, 0u);

  // `rule <title> maybe 3:` / `rule <title> always:` / `rule <title>:`.
  //
  // These are keywords rather than plain words so the title sentence stops
  // before them - the same reason `or` is one. The cost is that a rule can no
  // longer be titled "always"; the benefit is that no lexer mode is needed.
  if (parser.accept(TokenKind::KwAlways))
  {
    declaration->firePolicy = RulePolicy{FirePolicy::Always, maybeDenominator};
  }
  else if (parser.accept(TokenKind::KwMaybe))
  {
    const Token chance = parser.expect(TokenKind::Number);
    const std::string text(parser.textOf(chance));

    const unsigned long value = std::strtoul(text.c_str(), nullptr, 10);

    // `maybe N` is N in 10. `maybe N of M` is N in M.
    unsigned long denominator = maybeDenominator;

    if (parser.peekIs(TokenKind::Word) && parser.textOf(parser.peek()) == "of")
    {
      parser.expect(TokenKind::Word);

      const Token of = parser.expect(TokenKind::Number);
      const std::string ofText(parser.textOf(of));

      denominator = std::strtoul(ofText.c_str(), nullptr, 10);

      if (denominator == 0u)
      {
        parser.errorAt(of.span, "a maybe chance cannot be out of zero");
      }
    }

    if (value > denominator)
    {
      parser.errorAt(chance.span, "a maybe chance is 0 to " + std::to_string(denominator) + ", got " + text);
    }

    declaration->firePolicy = RulePolicy{FirePolicy::Maybe, static_cast<uint32_t>(value), static_cast<uint32_t>(denominator)};
  }

  parser.expect(TokenKind::Colon);
  parser.expect(TokenKind::KwWhen);

  declaration->whenNode = Conjunction::parse(parser);

  parser.expect(TokenKind::KwThen);

  declaration->thenNode = Conjunction::parse(parser);

  parser.expect(TokenKind::Dot);

  declaration->span = parser.spanFrom(start);

  return declaration;
}

std::unique_ptr<SetDecl> SetDecl::parse(Parser &parser)
{
  const Token start = parser.expect(TokenKind::KwSet);

  parser.resetLocalScopes();

  auto declaration = std::make_unique<SetDecl>();

  const Token nameToken = parser.expect(TokenKind::Word);

  declaration->setName = std::string(parser.textOf(nameToken));

  if (parser.findSet(declaration->setName) != nullptr)
  {
    parser.errorAt(nameToken.span, "set '" + declaration->setName + "' is already declared");
  }

  parser.expect(TokenKind::LParen);

  std::set<std::string> seen;

  do
  {
    const Token parameter = parser.expect(TokenKind::Word);
    const std::string text(parser.textOf(parameter));

    if (text.empty() || !isUppercase(text[0]))
    {
      parser.errorAt(parameter.span, "a set parameter must be a variable, got '" + text + "'");
    }

    if (!seen.insert(text).second)
    {
      // Two parameters with one name would make the renaming ambiguous, and the
      // second would silently win.
      parser.errorAt(parameter.span, "set '" + declaration->setName + "' declares '" + text + "' twice");
    }

    if (declaration->variadic)
    {
      // Anything after the variadic parameter could never be bound: the variadic
      // one has already taken every remaining argument.
      parser.errorAt(parameter.span, "set '" + declaration->setName + "': '" + declaration->parameterNames.back() +
                                         "...' must be the last parameter");
    }

    // `A...` lexes as a word then three dots, because '.' ends a word.
    if (parser.peekIs(TokenKind::Dot) && parser.peekIs(TokenKind::Dot, 1u) && parser.peekIs(TokenKind::Dot, 2u))
    {
      parser.expect(TokenKind::Dot);
      parser.expect(TokenKind::Dot);
      parser.expect(TokenKind::Dot);

      declaration->variadic = true;
    }

    declaration->parameterNames.push_back(text);
  } while (parser.accept(TokenKind::Comma));

  parser.expect(TokenKind::RParen);
  parser.expect(TokenKind::Equal);

  // Parsed like any other conjunction, which means a set body may call sets
  // declared before it and gets those expanded here, once, rather than at every
  // call site of THIS set.
  declaration->bodyNode = Conjunction::parse(parser);

  parser.expect(TokenKind::Dot);

  // A body variable that is NOT a parameter is a LOCAL: something the set needs
  // to join through but the caller has no reason to name. `together(A, B)` is
  // the motivating case - both people must live in the SAME place, but which
  // place is nobody's business but the set's.
  //
  // Locals are freshened per call site (see Conjunction::parseTerm). They have
  // to be: two calls in one rule that shared a local would silently force all
  // four people into one village, which is a wrong answer rather than an error.
  //
  // The cost is that a mistyped variable in a body is a local rather than a
  // diagnostic. That is the same trade every rule language with existential
  // body variables makes.

  declaration->span = parser.spanFrom(start);

  return declaration;
}

} // namespace graphrewrite
