// Clauses: guards on WHEN edges, schedules on THEN edges, and the host that
// evaluates them.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/compiler/ProgramSerializer.hpp"
#include "graphrewrite/frontend/AstPrinter.hpp"
#include "graphrewrite/frontend/Parser.hpp"
#include "graphrewrite/runtime/ExpressionHost.hpp"

using namespace graphrewrite;
using namespace graphrewrite::testing;

namespace {

const std::string lets = "let Person is alive.\n"
                         "let Person is engaged to Person1.\n"
                         "let Place is a church in Place1.\n"
                         "let Person lives in Place.\n"
                         "let Person wed in Place.\n";

const RuleDecl &firstRule(const Program &program)
{
  for (const std::unique_ptr<Declaration> &declaration : program.declarations())
  {
    if (declaration->kind() == NodeKind::RuleDecl)
    {
      return static_cast<const RuleDecl &>(*declaration);
    }
  }

  std::abort();
}

const Sentence &sentenceAt(const Conjunction &conjunction, const size_t index)
{
  return *conjunction.terms()[index]->alternatives()[0];
}

void parsing()
{
  DiagnosticSink sink;
  Parser parser(lets + "rule wed: when A is engaged to B, A lives in V, C is a church in V (availableAt(C, now() + 5))\n"
                       "then A lives in V, C is a church in V, A wed in C (occurAt(now() + 5, Days)), B wed in C (time.now()).\n",
                "t.graphwrite", sink);

  const auto program = Program::parse(parser);

  checkEqual(sink.errorCount(), 0u, "clauses parse");

  const RuleDecl &rule = firstRule(*program);
  const Sentence &church = sentenceAt(rule.when(), 2u);

  check(church.clause() != nullptr, "the guard is attached to the church sentence");
  checkEqual(church.pattern(), std::string("? is a church in ?"), "and stays out of the pattern");
  checkEqual(printExpr(*church.clause()), std::string("availableAt(C, now() + 5)"), "and prints back as written");
  checkEqual(church.clauseVariables().size(), size_t(1), "it mentions one variable");
  check(sentenceAt(rule.when(), 0u).clause() == nullptr, "a sentence without a clause has none");

  const Sentence &wed = sentenceAt(rule.then(), 2u);

  checkEqual(printExpr(*wed.clause()), std::string("occurAt(now() + 5, Days)"), "a schedule with a unit word");
  checkEqual(printExpr(*sentenceAt(rule.then(), 3u).clause()), std::string("time.now()"), "a dotted call name");

  // Annotations still parse: word(key=value) is told apart by the '='.
  DiagnosticSink annotated;
  Parser again("let Person is alive.\nrule r: when A is alive(weight=3) then A is alive.\n", "t.graphwrite", annotated);

  const auto withAnnotation = Program::parse(again);

  checkEqual(annotated.errorCount(), 0u, "an annotation still parses");
  checkEqual(sentenceAt(firstRule(*withAnnotation).when(), 0u).annotations().size(), size_t(1), "and lands as an annotation");
}

void reprintRoundTrip()
{
  const std::string source = lets + "rule wed: when A is engaged to B, A lives in V, C is a church in V (C availableAt(now() + 5))\n"
                                    "then A lives in V, C is a church in V, A wed in C (occurAt(now() + 5)).\n";

  DiagnosticSink sink;
  Parser parser(source, "t.graphwrite", sink);

  const auto first = Program::parse(parser);

  checkEqual(sink.errorCount(), 0u, "the leading-variable form parses");
  checkEqual(printExpr(*sentenceAt(firstRule(*first).when(), 2u).clause()), std::string("availableAt(C, now() + 5)"),
             "and is sugar for the variable as first argument");

  DiagnosticSink second;
  Parser reparser(printAst(*first), "t.graphwrite", second);

  const auto reparsed = Program::parse(reparser);

  checkEqual(second.errorCount(), 0u, "the reprint parses");
  checkEqual(dumpAst(*reparsed), dumpAst(*first), "to the same tree");
}

void unboundClauseVariable()
{
  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  const bool ok = compiler::compile(lets + "rule r: when A is alive (availableAt(Z)) then A is alive.\n", "t.graphwrite",
                                    compiler::CompileOptions(), sink, program, usage);

  check(!ok, "a clause naming a variable no sentence binds is rejected");
}

void setsRenameClauseVariables()
{
  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  const bool ok = compiler::compile(lets + "set at-church(A, C) = A lives in V, C is a church in V (availableAt(C, now() + 1)).\n"
                                           "rule r: when X is engaged to Y, at-church(X, K) then at-church(X, K), X wed in K (occurAt(now() + 1)).\n",
                                    "t.graphwrite", compiler::CompileOptions(), sink, program, usage);

  check(ok, "a set carrying a clause compiles");

  bool renamed = false;

  for (const compiler::PatternEdge &edge : program.ruleRewrites[0].when)
  {
    if (edge.hasClause)
    {
      renamed = printExpr(edge.clause) == "availableAt(K, now() + 1)";
    }
  }

  check(renamed, "and the clause's variable was renamed with the set's parameter");
}

void compiledAndSerialised()
{
  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  const bool ok = compiler::compile(lets + "rule wed: when A is engaged to B, A lives in V, C is a church in V (availableAt(C, now() + 5))\n"
                                           "then A lives in V, C is a church in V, A wed in C (occurAt(now() + 5)), B wed in C (occurAt(now() + 5)).\n",
                                    "t.graphwrite", compiler::CompileOptions(), sink, program, usage);

  check(ok, "a programme with clauses compiles");

  const compiler::RuleRewrite &rewrite = program.ruleRewrites[0];

  checkEqual(rewrite.when.size(), size_t(3), "the rewrite keeps its WHEN edges");

  uint32_t guards = 0u;
  uint32_t churchVertex = ~0u;

  for (const compiler::PatternEdge &edge : rewrite.when)
  {
    if (edge.hasClause)
    {
      ++guards;
      churchVertex = edge.clause.args[0].vertex;
    }
  }

  checkEqual(guards, 1u, "one guard");
  check(churchVertex != ~0u && churchVertex == rewrite.when[2].origin, "whose variable resolved to the church's vertex");
  checkEqual(rewrite.insertions.size(), size_t(2), "two insertions");
  check(rewrite.insertions[0].hasClause && rewrite.insertions[1].hasClause, "both scheduled");

  std::vector<uint8_t> bytes;

  check(compiler::writeProgram(program, usage, bytes), "serialises");

  compiler::CompiledProgram restored;
  compiler::UsageValues restoredUsage;
  DiagnosticSink readSink;

  check(compiler::readProgram(bytes.data(), bytes.size(), restored, restoredUsage, readSink), "and reads back");
  checkEqual(restored.ruleRewrites[0].when.size(), size_t(3), "with its WHEN edges");
  check(restored.ruleRewrites[0].insertions[0].hasClause, "and its schedules");
  checkEqual(printExpr(restored.ruleRewrites[0].insertions[0].clause), std::string("occurAt(now() + 5)"), "intact");
}

void theHost()
{
  runtime::ExpressionHost host;

  struct Bookings
  {
    std::vector<std::pair<uint32_t, uint32_t>> taken; // (church, day)
  } bookings;

  host.registerHandler(
      "availableAt",
      [](const std::vector<uint32_t> &args, const std::vector<std::string> &, const runtime::CallContext &, void *user, uint32_t &out) {
        Bookings &table = *static_cast<Bookings *>(user);

        for (const auto &entry : table.taken)
        {
          if (entry.first == args[0] && entry.second == args[1])
          {
            return false;
          }
        }

        table.taken.emplace_back(args[0], args[1]);
        out = 1u;

        return true;
      },
      &bookings);

  host.setUnit("Days", 1u);

  const std::vector<uint32_t> bound{7u, 8u, 42u};

  runtime::CallContext context;

  context.now = 10u;
  context.bound = &bound;

  Expr church = Expr::variable("C");

  church.vertex = 2u;

  Expr five;

  five.number = 5u;

  Expr sum;

  sum.kind = Expr::Kind::Add;
  sum.args = {Expr::call("now"), five};

  Expr guard = Expr::call("availableAt");

  guard.args = {church, sum};

  uint32_t value = 0u;

  check(host.evaluate(sum, context, value) && value == 15u, "now() + 5 is 15 at time 10");
  check(host.evaluate(guard, context, value) && value == 1u, "the handler answered and booked the church");
  checkEqual(bookings.taken.size(), size_t(1), "through the context it was given");
  check(bookings.taken[0].first == 42u && bookings.taken[0].second == 15u, "with the bound node id and the computed day");
  check(!host.evaluate(guard, context, value), "asking again for the same day is refused");

  Expr unknown = Expr::call("nowhere");

  check(!host.evaluate(unknown, context, value), "an unregistered call fails");
  checkEqual(host.missing().size(), size_t(1), "and is remembered for the diagnostics");

  Expr days;

  days.kind = Expr::Kind::Word;
  days.name = "Days";

  Expr occur = Expr::call("occurAt");

  occur.args = {sum, days};

  check(host.evaluate(occur, context, value) && value == 15u, "occurAt(now() + 5, Days) is 15 with Days as one tick");
}

} // namespace


// ---------------------------------------------------------------------------
// Scheduling at the applier.

#include "graphrewrite/runtime/RewriteApplier.hpp"

namespace {

struct World
{
  compiler::CompiledProgram program;
  compiler::UsageValues usage;
  runtime::EdgePageStore store;
  DiagnosticSink sink;
  compiler::EdgeTypeId engaged = 0u;
  compiler::EdgeTypeId alive = 0u;
  compiler::EdgeTypeId wed = 0u;
  compiler::EdgeTypeId dead = 0u;
  bool ok = false;
};

void buildWorld(World &world, const std::string &rule)
{
  const std::string source = "let Person is engaged to Person1.\nlet Person is alive.\nlet Person is dead.\nlet Person wed Person1.\n" + rule;

  world.ok = compiler::compile(source, "t.graphwrite", compiler::CompileOptions(), world.sink, world.program, world.usage);

  if (!world.ok)
  {
    return;
  }

  world.program.edgeTypes.find("? is engaged to ?", world.engaged);
  world.program.edgeTypes.find("? is alive", world.alive);
  world.program.edgeTypes.find("? wed ?", world.wed);
  world.program.edgeTypes.find("? is dead", world.dead);

  world.store.configure(world.program.edgeTypes);
  world.store.addEdge(world.engaged, runtime::Edge{1u, 2u});
  world.store.addEdge(world.alive, runtime::Edge{1u, 1u});
  world.store.addEdge(world.alive, runtime::Edge{2u, 2u});
}

std::vector<runtime::MatchedRow> theRow()
{
  runtime::MatchedRow row;

  row.rule = 0u;
  row.index = 0u;
  row.vertices = {1u, 2u};

  return {row};
}

const std::string weddingRule = "rule wed: when A is engaged to B, A is alive, B is alive\n"
                                "then A is alive, B is alive, A wed B (occurAt(now() + 3)).\n";

void scheduling()
{
  World world;

  buildWorld(world, weddingRule);

  check(world.ok, "the scheduling programme compiles");

  runtime::ExpressionHost host;
  runtime::RewriteApplier applier(world.program, world.store);

  std::vector<runtime::RewriteEvent> events;

  applier.setHost(&host);
  applier.setFireObserver([&events](const runtime::RewriteEvent &event) { events.push_back(event); });
  applier.setNow(10u);

  const runtime::RewriteStats stats = applier.apply(theRow());

  checkEqual(stats.rulesFired, 1u, "the row fired now - its removal is immediate");
  checkEqual(stats.scheduled, 1u, "and one group was queued");
  check(!world.store.containsEdge(world.engaged, runtime::Edge{1u, 2u}), "the engagement is consumed today");
  check(!world.store.containsEdge(world.wed, runtime::Edge{1u, 2u}), "the wedding has not happened yet");
  checkEqual(applier.pending().size(), size_t(1), "one rewrite is pending");
  checkEqual(applier.pending()[0].time, 13u, "for day 13");
  checkEqual(applier.pending()[0].requires.size(), size_t(2), "requiring the two edges the rule kept - both alive");
  checkEqual(events.size(), size_t(2), "two events were reported");
  check(events[0].kind == runtime::RewriteEventKind::Scheduled && events[0].time == 13u, "a schedule for day 13");
  check(events[1].kind == runtime::RewriteEventKind::Applied, "and the firing itself");

  applier.setNow(12u);

  checkEqual(applier.applyDue().released, 0u, "nothing is due on day 12");

  applier.setNow(13u);

  const runtime::RewriteStats due = applier.applyDue();

  checkEqual(due.released, 1u, "the wedding is released on day 13");
  check(world.store.containsEdge(world.wed, runtime::Edge{1u, 2u}), "and the edge now exists");
  check(events.back().kind == runtime::RewriteEventKind::Released, "reported as released");
  checkEqual(applier.pending().size(), size_t(0), "and the queue is empty");
}

void cancellation()
{
  World world;

  buildWorld(world, weddingRule);

  runtime::ExpressionHost host;
  runtime::RewriteApplier applier(world.program, world.store);

  std::vector<runtime::RewriteEvent> events;

  applier.setHost(&host);
  applier.setFireObserver([&events](const runtime::RewriteEvent &event) { events.push_back(event); });
  applier.setNow(10u);
  applier.apply(theRow());

  // The groom dies on day 11.
  world.store.removeEdge(world.alive, runtime::Edge{2u, 2u});

  applier.setNow(13u);

  const runtime::RewriteStats due = applier.applyDue();

  checkEqual(due.cancelled, 1u, "a wedding whose groom died is cancelled");
  checkEqual(due.released, 0u, "not held");
  check(!world.store.containsEdge(world.wed, runtime::Edge{1u, 2u}), "and no marriage edge appears");
  check(events.back().kind == runtime::RewriteEventKind::Cancelled, "reported as cancelled");
}

void guards()
{
  World world;

  buildWorld(world, "rule wed: when A is engaged to B, A is alive (blessed(A)), B is alive\n"
                    "then A is alive, B is alive, A wed B.\n");

  check(world.ok, "the guarded programme compiles");

  runtime::ExpressionHost host;
  runtime::RewriteApplier applier(world.program, world.store);

  applier.setHost(&host);

  // No handler registered: the guard cannot be answered, so the row does not fire.
  runtime::RewriteStats stats = applier.apply(theRow());

  checkEqual(stats.guarded, 1u, "an unanswerable guard turns the row away");
  checkEqual(stats.rulesFired, 0u, "and nothing fires");
  check(world.store.containsEdge(world.engaged, runtime::Edge{1u, 2u}), "so nothing was consumed");

  uint32_t asked = 0u;

  host.registerHandler("blessed", [&asked](const std::vector<uint32_t> &args, const std::vector<std::string> &,
                                           const runtime::CallContext &, void *, uint32_t &out) {
    ++asked;
    out = args[0] == 1u ? 1u : 0u;

    return true;
  });

  stats = applier.apply(theRow());

  checkEqual(stats.rulesFired, 1u, "with the handler saying yes, the row fires");
  checkEqual(asked, 1u, "and the handler was asked exactly once");
  check(world.store.containsEdge(world.wed, runtime::Edge{1u, 2u}), "so they wed");
}

} // namespace

int runScheduling()
{
  scheduling();
  cancellation();
  guards();

  return 0;
}


// ---------------------------------------------------------------------------
// Bind clauses: a new node takes the value of an expression.

namespace {

void binding()
{
  DiagnosticSink sink;
  Parser parser(lets + "rule wed: when A is engaged to B, A is alive, B is alive\n"
                       "then A is alive, B is alive, A wed in D' (D' = day(now() + 4)), B wed in D' (occurAt(dayOf(D'))).\n",
                "t.graphwrite", sink);

  const auto program = Program::parse(parser);

  checkEqual(sink.errorCount(), 0u, "a bind clause parses");
  checkEqual(printExpr(*sentenceAt(firstRule(*program).then(), 2u).clause()), std::string("D' = day(now() + 4)"),
             "and prints as written");

  DiagnosticSink bad;
  Parser rejecting(lets + "rule r: when A is alive then A is alive, A wed in A (A = day(1)).\n", "t.graphwrite", bad);

  Program::parse(rejecting);

  check(bad.errorCount() > 0u, "binding a matched variable is rejected");

  // Applied: the new vertex is the day node, not a fresh id, and the schedule
  // reads it back.
  World world;

  buildWorld(world, "rule wed: when A is engaged to B, A is alive, B is alive\n"
                    "then A is alive, B is alive, A wed D' (D' = day(now() + 4)), B wed D' (occurAt(dayOf(D'))).\n");

  check(world.ok, "the binding programme compiles");

  runtime::ExpressionHost host;

  constexpr uint32_t dayBase = 1000u;

  host.registerHandler("day", [](const std::vector<uint32_t> &args, const std::vector<std::string> &, const runtime::CallContext &, void *,
                                 uint32_t &out) {
    out = dayBase + args[0];

    return true;
  });
  host.registerHandler("dayOf", [](const std::vector<uint32_t> &args, const std::vector<std::string> &, const runtime::CallContext &, void *,
                                   uint32_t &out) {
    out = args[0] - dayBase;

    return true;
  });

  runtime::RewriteApplier applier(world.program, world.store);

  applier.setHost(&host);
  applier.setNow(10u);

  const runtime::RewriteStats stats = applier.apply(theRow());

  checkEqual(stats.rulesFired, 1u, "the row fires");
  check(world.store.containsEdge(world.wed, runtime::Edge{1u, dayBase + 14u}), "A wed <day 14>: the new node IS the day node");
  checkEqual(applier.pending().size(), size_t(1), "B's edge is scheduled");
  checkEqual(applier.pending()[0].time, 14u, "for day 14, read back from the bound node");
}

} // namespace

int runBinding()
{
  binding();

  return 0;
}

int main()
{
  parsing();
  reprintRoundTrip();
  unboundClauseVariable();
  setsRenameClauseVariables();
  compiledAndSerialised();
  theHost();
  runScheduling();
  runBinding();

  return report("GraphRewriteClauseTests");
}
