// Phase 2.9 [G8]: the whole pipeline, serialisation, and the corpus baseline.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/compiler/ProgramSerializer.hpp"
#include "graphrewrite/compiler/ProgramVerifier.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::compiler;
using namespace graphrewrite::testing;

namespace {

std::string corpus()
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/basic.graphwrite");

  if (!file)
  {
    return std::string();
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  return buffer.str();
}

uint32_t queryCount(const CompiledProgram &program)
{
  uint32_t count = 0u;

  for (const Instruction &instruction : program.instructions)
  {
    if (instruction.op == Op::Select || instruction.op == Op::Where)
    {
      ++count;
    }
  }

  return count;
}

void pipelineCompilesTheCorpus()
{
  const std::string source = corpus();

  check(!source.empty(), "corpus asset found");

  if (source.empty())
  {
    return;
  }

  DiagnosticSink sink;
  CompiledProgram program;
  UsageValues usage;

  const bool ok = compile(source, "basic.graphwrite", CompileOptions(), sink, program, usage);

  if (!ok)
  {
    for (const Diagnostic &diagnostic : sink.all())
    {
      std::cerr << "    " << diagnostic.format("basic.graphwrite") << "\n";
    }
  }

  check(ok, "the corpus compiles end to end and verifies");
  checkEqual(sink.count(), 0u, "with no diagnostics");

  // The recorded baseline. These numbers were produced independently by the
  // Python model in docs/graphrewrite-model and are the regression signal for
  // the join: if the join stops working they go up, not down.
  checkEqual(program.edgeTypes.size(), 19u, "19 edge types");
  checkEqual(program.ruleNames.size(), size_t(15), "15 rule variants");
  checkEqual(program.threadStarts.size(), size_t(2), "2 threads");
  checkEqual(queryCount(program), 24u, "24 queries");
  checkEqual(program.mergedEdgeCount, 15u, "15 merged edges");

  uint32_t shared = 0u;
  uint32_t mappings = 0u;

  for (const Instruction &instruction : program.instructions)
  {
    if (instruction.op != Op::Select && instruction.op != Op::Where)
    {
      continue;
    }

    mappings += static_cast<uint32_t>(instruction.perRule.size());

    if (instruction.rules.size() > 1u)
    {
      ++shared;
    }
  }

  checkEqual(shared, 7u, "7 queries serve more than one rule");
  checkEqual(mappings, 47u, "47 per-rule mappings - the unjoined match-step count, now carried by 24 queries");

  // Every rule's edges must lie on one table lineage, or it can never match.
  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    uint32_t served = 0u;

    for (const Instruction &instruction : program.instructions)
    {
      if (instruction.op != Op::Select && instruction.op != Op::Where)
      {
        continue;
      }

      if (std::find(instruction.rules.begin(), instruction.rules.end(), rule) != instruction.rules.end())
      {
        ++served;
      }
    }

    checkEqual(served, program.ruleEdgeCounts[rule], "rule " + program.ruleNames[rule] + " fully covered");
  }

  std::cout << "    corpus: 47 unjoined match steps -> " << queryCount(program) << " queries ("
            << (47.0 / static_cast<double>(queryCount(program))) << "x fewer)\n";
}

void roundTripIsLossless()
{
  const std::string source = corpus();

  if (source.empty())
  {
    return;
  }

  DiagnosticSink sink;
  CompiledProgram program;
  UsageValues usage;

  compile(source, "basic.graphwrite", CompileOptions(), sink, program, usage);

  std::vector<uint8_t> blob;

  check(writeProgram(program, usage, blob), "programme serialises");
  check(blob.size() > 64u, "and produces a non-trivial blob");

  CompiledProgram restored;
  UsageValues restoredUsage;
  DiagnosticSink readSink;

  check(readProgram(blob.data(), blob.size(), restored, restoredUsage, readSink), "and reads back");
  checkEqual(readSink.count(), 0u, "without diagnostics");

  checkEqual(restored.edgeTypes.size(), program.edgeTypes.size(), "edge type count survives");
  checkEqual(restored.instructions.size(), program.instructions.size(), "instruction count survives");
  checkEqual(restored.ruleNames.size(), program.ruleNames.size(), "rule names survive");
  checkEqual(restored.mergedEdgeCount, program.mergedEdgeCount, "merged edge count survives");
  checkEqual(dumpProgram(restored), dumpProgram(program), "the programme round-trips identically");
  check(restoredUsage.nextUse == usage.nextUse, "usage values round-trip identically");

  // Direction usage is what the runtime builds its page partitions from, so it
  // has to survive the trip.
  bool directionsMatch = true;

  for (EdgeTypeId id = 0u; id < program.edgeTypes.size(); ++id)
  {
    directionsMatch = directionsMatch && restored.edgeTypes[id].usedForward == program.edgeTypes[id].usedForward &&
                      restored.edgeTypes[id].usedBackward == program.edgeTypes[id].usedBackward;
  }

  check(directionsMatch, "per-type direction usage survives");

  // And the restored programme must still verify.
  DiagnosticSink verifySink;

  check(verifyProgram(restored, restoredUsage, verifySink), "the restored programme verifies clean");
}

void truncatedBlobIsRejected()
{
  const std::string source = corpus();

  if (source.empty())
  {
    return;
  }

  DiagnosticSink sink;
  CompiledProgram program;
  UsageValues usage;

  compile(source, "basic.graphwrite", CompileOptions(), sink, program, usage);

  std::vector<uint8_t> blob;

  writeProgram(program, usage, blob);

  blob.resize(blob.size() / 2u);

  CompiledProgram restored;
  UsageValues restoredUsage;
  DiagnosticSink readSink;

  check(!readProgram(blob.data(), blob.size(), restored, restoredUsage, readSink), "a truncated blob is rejected");
  check(readSink.hasErrors(), "with a diagnostic");
}

void wrongMagicIsRejected()
{
  std::vector<uint8_t> blob(64u, 0u);

  CompiledProgram restored;
  UsageValues restoredUsage;
  DiagnosticSink readSink;

  check(!readProgram(blob.data(), blob.size(), restored, restoredUsage, readSink), "a blob with the wrong magic is rejected");
  check(readSink[0].message.find("not a graph-rewrite programme") != std::string::npos, "with the right message");
}

void versionMismatchIsRejected()
{
  std::vector<uint8_t> blob;

  const uint32_t magic = programMagic;
  const uint32_t version = programVersion + 1u;

  for (const uint32_t value : {magic, version})
  {
    blob.push_back(static_cast<uint8_t>(value & 0xffu));
    blob.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    blob.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    blob.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
  }

  CompiledProgram restored;
  UsageValues restoredUsage;
  DiagnosticSink readSink;

  check(!readProgram(blob.data(), blob.size(), restored, restoredUsage, readSink), "a future version is rejected");
  check(readSink[0].message.find("version") != std::string::npos, "naming the version");
}

void badSourceFailsCleanly()
{
  DiagnosticSink sink;
  CompiledProgram program;
  UsageValues usage;

  const bool ok = compile("let A likes B.\nrule r: when A hates B then A likes B.", "t.graphwrite", CompileOptions(), sink, program, usage);

  check(!ok, "a source with an undeclared pattern does not compile");
  check(sink.hasErrors(), "and says why");
}

void determinismEndToEnd()
{
  const std::string source = corpus();

  if (source.empty())
  {
    return;
  }

  std::string first;
  std::string second;

  for (std::string *out : {&first, &second})
  {
    DiagnosticSink sink;
    CompiledProgram program;
    UsageValues usage;

    compile(source, "basic.graphwrite", CompileOptions(), sink, program, usage);

    *out = dumpProgram(program);
  }

  checkEqual(first, second, "compiling the corpus twice produces an identical programme");
}

} // namespace

int main()
{
  pipelineCompilesTheCorpus();
  roundTripIsLossless();
  truncatedBlobIsRejected();
  wrongMagicIsRejected();
  versionMismatchIsRejected();
  badSourceFailsCleanly();
  determinismEndToEnd();

  return report("GraphRewriteCompilerTests");
}
