// Rewrite fire policies: ALWAYS / MAYBE(0..10), rolled per matched row.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/compiler/ProgramSerializer.hpp"
#include "graphrewrite/frontend/AstPrinter.hpp"
#include "graphrewrite/frontend/Parser.hpp"
#include "graphrewrite/runtime/Dice.hpp"
#include "graphrewrite/runtime/RewriteApplier.hpp"

#include <vector>

using namespace graphrewrite;
using namespace graphrewrite::testing;

namespace {

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

std::unique_ptr<Program> parseOf(const std::string &source, DiagnosticSink &sink)
{
  Parser parser(source, "policy.graphwrite", sink);

  return Program::parse(parser);
}

void parsing()
{
  const std::string prelude = "let Person follows Person1.\n";

  {
    DiagnosticSink sink;

    const auto program = parseOf(prelude + "rule plain: when A follows B then B follows A.\n", sink);

    checkEqual(sink.errorCount(), 0u, "a rule with no policy parses");
    check(firstRule(*program).policy().alwaysFires(), "and defaults to ALWAYS");
  }

  {
    DiagnosticSink sink;

    const auto program = parseOf(prelude + "rule loud always: when A follows B then B follows A.\n", sink);

    checkEqual(sink.errorCount(), 0u, "an explicit ALWAYS parses");
    check(firstRule(*program).policy().alwaysFires(), "and fires always");
  }

  {
    DiagnosticSink sink;

    const auto program = parseOf(prelude + "rule coin maybe 3: when A follows B then B follows A.\n", sink);

    checkEqual(sink.errorCount(), 0u, "MAYBE with a chance parses");
    checkEqual(firstRule(*program).policy().chance, 3u, "and keeps the chance");
    check(!firstRule(*program).policy().alwaysFires(), "and is not an always-rule");
  }

  {
    DiagnosticSink sink;

    parseOf(prelude + "rule never maybe 0: when A follows B then B follows A.\n", sink);

    checkEqual(sink.errorCount(), 0u, "MAYBE 0 is legal");
  }

  {
    DiagnosticSink sink;

    parseOf(prelude + "rule bad maybe 11: when A follows B then B follows A.\n", sink);

    checkEqual(sink.errorCount(), 1u, "a chance above 10 is rejected");
  }
}

void reprintRoundTrip()
{
  const std::string source = "let Person follows Person1.\nrule coin maybe 7: when A follows B then B follows A.\n";

  DiagnosticSink sink;

  const auto first = parseOf(source, sink);

  checkEqual(sink.errorCount(), 0u, "source parses");

  DiagnosticSink second;

  const auto reparsed = parseOf(printAst(*first), second);

  checkEqual(second.errorCount(), 0u, "reprinted source parses");
  checkEqual(reparsed == nullptr ? 99u : firstRule(*reparsed).policy().chance, 7u, "and the chance survives the reprint");
}

void throughTheCompiler()
{
  const std::string source = "let Person follows Person1.\nlet Person blocks Person1.\n"
                             "rule coin maybe 4: when A follows B, B blocks A then A follows B.\n";

  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  const bool ok = compiler::compile(source, "policy.graphwrite", compiler::CompileOptions(), sink, program, usage);

  check(ok, "a programme with a MAYBE rule compiles");
  check(!program.ruleRewrites.empty(), "and produced a rewrite");
  checkEqual(program.ruleRewrites[0].policy.chance, 4u, "carrying the chance to the compiled form");

  // The runtime reads the policy out of a serialised programme, so it has to
  // survive the round trip - a dropped field here reads as ALWAYS, which fires
  // every row instead of some of them.
  std::vector<uint8_t> bytes;

  check(compiler::writeProgram(program, usage, bytes), "the programme serialises");

  compiler::CompiledProgram restored;
  compiler::UsageValues restoredUsage;
  DiagnosticSink readSink;

  check(compiler::readProgram(bytes.data(), bytes.size(), restored, restoredUsage, readSink), "the programme deserialises");
  checkEqual(restored.ruleRewrites[0].policy.chance, 4u, "and the chance round-trips");
  check(restored.ruleRewrites[0].policy.kind == FirePolicy::Maybe, "and so does the kind");
}

void theDice()
{
  // ALWAYS and MAYBE 10 both skip the roll; MAYBE 0 never fires.
  for (uint32_t row = 0u; row < 64u; ++row)
  {
    check(runtime::dice::fires(1234u, row, 1000u), "chance 1000 always fires");
    check(!runtime::dice::fires(1234u, row, 0u), "chance 0 never fires");
  }

  // A chance in between fires for some rows and not others, and the same row
  // always gives the same answer - the roll is a hash of (seed, row), not a
  // draw from a sequence.
  uint32_t fired = 0u;

  constexpr uint32_t rows = 4000u;
  constexpr uint32_t seed = 0x51ed2701u;

  std::vector<bool> first(rows, false);

  for (uint32_t row = 0u; row < rows; ++row)
  {
    first[row] = runtime::dice::fires(seed, row, 500u);

    if (first[row])
    {
      ++fired;
    }
  }

  // Rolled again, in a separate pass: the roll is a hash of (seed, row), so it
  // must not depend on when or in what order it was asked. This is the property
  // the CPU and GPU runners rely on to agree.
  uint32_t stable = 0u;

  for (uint32_t row = rows; row-- > 0u;)
  {
    if (runtime::dice::fires(seed, row, 500u) == first[row])
    {
      ++stable;
    }
  }

  checkEqual(stable, rows, "every row rolls the same answer when asked again, in any order");

  // 5/10 over four thousand rows. A wide band: this asserts the dice is not
  // stuck, not that it is a good generator.
  check(fired > rows * 35u / 100u && fired < rows * 65u / 100u,
        "chance 5 fires roughly half the rows (" + std::to_string(fired) + "/" + std::to_string(rows) + ")");

  // Two rules must not roll the same number for the same row, or every rule
  // would fire on exactly the same subset of a shared table.
  uint32_t differing = 0u;

  for (uint32_t row = 0u; row < rows; ++row)
  {
    if (runtime::dice::fires(runtime::dice::seedFor(seed, 0u), row, 500u) !=
        runtime::dice::fires(runtime::dice::seedFor(seed, 1u), row, 500u))
    {
      ++differing;
    }
  }

  check(differing > rows / 5u, "two rules roll independently on the same row");
}

// The policy has to change what the graph ends up looking like, not just what
// the compiled form says. This drives the CPU applier directly, which is the
// same path the GPU's filter_rows mirrors.
void applying()
{
  const std::string source = "let Person follows Person1.\nlet Person blocks Person1.\n"
                             "rule coin maybe 5: when A follows B, B blocks A then A follows B.\n";

  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  check(compiler::compile(source, "policy.graphwrite", compiler::CompileOptions(), sink, program, usage), "programme compiles");

  const compiler::EdgeTypeId follows = 0u;
  const compiler::EdgeTypeId blocks = 1u;

  constexpr uint32_t pairs = 400u;

  const auto seedStore = [&](runtime::EdgePageStore &store, const compiler::CompiledProgram &of) {
    store.configure(of.edgeTypes);

    for (uint32_t i = 0u; i < pairs; ++i)
    {
      store.addEdge(follows, runtime::Edge{i * 2u, i * 2u + 1u});
      store.addEdge(blocks, runtime::Edge{i * 2u + 1u, i * 2u});
    }
  };

  const auto rowsFor = [&]() {
    std::vector<runtime::MatchedRow> rows;

    for (uint32_t i = 0u; i < pairs; ++i)
    {
      runtime::MatchedRow row;

      row.rule = 0u;
      row.index = i;
      row.vertices = {i * 2u, i * 2u + 1u};

      rows.push_back(row);
    }

    return rows;
  };

  runtime::EdgePageStore store;

  seedStore(store, program);

  runtime::RewriteApplier applier(program, store, 0x51ed2701u);

  const runtime::RewriteStats stats = applier.apply(rowsFor());

  check(stats.declined > 0u, "a MAYBE 5 rule declines some rows (" + std::to_string(stats.declined) + ")");
  check(stats.rulesFired > 0u, "and fires others (" + std::to_string(stats.rulesFired) + ")");
  checkEqual(stats.declined + stats.rulesFired, pairs, "every row either fired or was declined");

  // An ALWAYS rule over the same graph fires every row, which is what makes the
  // number above a policy effect rather than a matching one.
  const std::string always = "let Person follows Person1.\nlet Person blocks Person1.\n"
                             "rule sure: when A follows B, B blocks A then A follows B.\n";

  DiagnosticSink alwaysSink;
  compiler::CompiledProgram alwaysProgram;
  compiler::UsageValues alwaysUsage;

  check(compiler::compile(always, "policy.graphwrite", compiler::CompileOptions(), alwaysSink, alwaysProgram, alwaysUsage),
        "the ALWAYS variant compiles");

  runtime::EdgePageStore alwaysStore;

  seedStore(alwaysStore, alwaysProgram);

  runtime::RewriteApplier alwaysApplier(alwaysProgram, alwaysStore, 0x51ed2701u);

  const runtime::RewriteStats alwaysStats = alwaysApplier.apply(rowsFor());

  checkEqual(alwaysStats.declined, 0u, "an ALWAYS rule declines nothing");
  checkEqual(alwaysStats.rulesFired, pairs, "and fires every row");

  // Staleness: a row whose removal edge is already gone must not fire, or it
  // would insert THEN edges for a WHEN that no longer holds.
  runtime::EdgePageStore staleStore;

  seedStore(staleStore, alwaysProgram);

  staleStore.removeEdge(blocks, runtime::Edge{1u, 0u});

  runtime::RewriteApplier staleApplier(alwaysProgram, staleStore, 0x51ed2701u);

  const runtime::RewriteStats staleStats = staleApplier.apply(rowsFor());

  checkEqual(staleStats.stale, 1u, "the row whose edge was already removed is filtered as stale");
  checkEqual(staleStats.rulesFired, pairs - 1u, "and every other row still fires");

  // The graph is a set. A rule that only adds an edge fires once per row and
  // then never again, because a second firing would change nothing.
  const std::string adding = "let Person follows Person1.\nlet Person blocks Person1.\n"
                             "rule grudge: when A follows B then A follows B, A blocks B.\n";

  DiagnosticSink addSink;
  compiler::CompiledProgram addProgram;
  compiler::UsageValues addUsage;

  check(compiler::compile(adding, "policy.graphwrite", compiler::CompileOptions(), addSink, addProgram, addUsage),
        "the adding programme compiles");

  runtime::EdgePageStore addStore;

  addStore.configure(addProgram.edgeTypes);

  for (uint32_t i = 0u; i < pairs; ++i)
  {
    addStore.addEdge(follows, runtime::Edge{i * 2u, i * 2u + 1u});
  }

  runtime::RewriteApplier addApplier(addProgram, addStore, 0x51ed2701u);

  const runtime::RewriteStats firstPass = addApplier.apply(rowsFor());

  checkEqual(firstPass.rulesFired, pairs, "every row fires the first time");
  checkEqual(firstPass.edgesInserted, pairs, "inserting one edge each");

  addApplier.setRound(2u);

  const runtime::RewriteStats secondPass = addApplier.apply(rowsFor());

  checkEqual(secondPass.rulesFired, 0u, "and no row fires the second time");
  checkEqual(secondPass.noops, pairs, "because every one of them would change nothing");
  checkEqual(addStore.edgeCount(blocks), pairs, "so the graph holds each edge exactly once");
}

} // namespace

int main()
{
  parsing();
  reprintRoundTrip();
  throughTheCompiler();
  theDice();
  applying();

  return report("GraphRewritePolicyTests");
}
