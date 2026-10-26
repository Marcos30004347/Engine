// Phase 2.7 [G4]: usage values.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/ColumnMapping.hpp"
#include "graphrewrite/compiler/Ir.hpp"
#include "graphrewrite/compiler/ProgramBuilder.hpp"
#include "graphrewrite/compiler/UsageValues.hpp"
#include "graphrewrite/frontend/Parser.hpp"

#include <fstream>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::compiler;
using namespace graphrewrite::testing;

namespace {

struct Compiled
{
  std::unique_ptr<graphrewrite::Program> ast;
  AnalysedProgram analysed;
  EdgeTypeTable table;
  std::vector<RuleGraph> graphs;
  std::vector<MergedGraph> threads;
  CompiledProgram program;
  UsageValues usage;
  DiagnosticSink sink;
};

void build(const std::string &source, Compiled &out)
{
  Parser parser(source, "t.graphwrite", out.sink);

  out.ast = graphrewrite::Program::parse(parser);

  analyse(*out.ast, OrExpansionOptions(), out.sink, out.analysed);
  buildEdgeTypeTable(out.analysed, out.table, out.sink);

  for (const RuleVariant &variant : out.analysed.variants)
  {
    RuleGraph graph;

    buildRuleGraph(variant, out.table, out.sink, graph);
    out.graphs.push_back(std::move(graph));
  }

  out.threads = mergeRuleGraphs(out.graphs);

  buildProgram(out.threads, out.graphs, out.table, out.sink, out.program);
  assignRuleColumns(out.threads, out.graphs, out.program, out.sink);

  out.usage = computeUsageValues(out.program);
}

// A hand-built programme, so the expected table can be computed by eye.
CompiledProgram handBuilt()
{
  CompiledProgram program;

  program.edgeTypes.intern("? a ?", 2u);
  program.edgeTypes.intern("? b ?", 2u);
  program.edgeTypes.intern("? c ?", 2u);

  const auto query = [](const Op op, const uint32_t reg, const EdgeTypeId label) {
    Instruction instruction;

    instruction.op = op;
    instruction.reg = reg;
    instruction.label = label;
    instruction.rules = {0u};

    return instruction;
  };

  program.instructions.push_back(query(Op::Select, 0u, 0u)); // i=0 reads a
  program.instructions.push_back(query(Op::Where, 1u, 1u));  // i=1 reads b
  program.instructions.push_back(query(Op::Where, 2u, 0u));  // i=2 reads a
  program.instructions.push_back(query(Op::Where, 3u, 2u));  // i=3 reads c

  return program;
}

void handComputedTable()
{
  const CompiledProgram program = handBuilt();
  const UsageValues usage = computeUsageValues(program);

  checkEqual(usage.instructionCount, 4u, "four instructions");
  checkEqual(usage.edgeTypeCount, 3u, "three edge types");

  // type a is read at 0 and 2, b at 1, c at 3
  checkEqual(usage.at(0u, 0u), 0u, "at i=0, type a is needed now");
  checkEqual(usage.at(0u, 1u), 1u, "at i=0, type b is needed at 1");
  checkEqual(usage.at(0u, 2u), 3u, "at i=0, type c is needed at 3");

  checkEqual(usage.at(1u, 0u), 2u, "at i=1, type a is next needed at 2");
  checkEqual(usage.at(1u, 1u), 1u, "at i=1, type b is needed now");

  checkEqual(usage.at(2u, 0u), 2u, "at i=2, type a is needed now");
  checkEqual(usage.at(2u, 1u), UsageValues::never, "at i=2, type b is never needed again");

  checkEqual(usage.at(3u, 0u), UsageValues::never, "at i=3, type a is done");
  checkEqual(usage.at(3u, 2u), 3u, "at i=3, type c is needed now");
}

void neverIsSticky()
{
  const CompiledProgram program = handBuilt();
  const UsageValues usage = computeUsageValues(program);

  // Once a type is never needed again it must stay that way to the end.
  for (uint32_t type = 0u; type < usage.edgeTypeCount; ++type)
  {
    bool sawNever = false;

    for (uint32_t i = 0u; i < usage.instructionCount; ++i)
    {
      if (usage.at(i, type) == UsageValues::never)
      {
        sawNever = true;
      }
      else
      {
        check(!sawNever, "a type never needed again does not come back");
      }
    }
  }
}

void monotoneBackwards()
{
  Compiled compiled;

  build("let C at L.\n"
        "let C anger towards D.\n"
        "let C philia towards D.\n"
        "rule insult: when C at L, D at L, C anger towards D then C at L.\n"
        "rule compliment: when C at L, D at L, C philia towards D then C at L.\n",
        compiled);

  checkEqual(compiled.sink.count(), 0u, "clean");

  bool monotone = true;

  for (uint32_t type = 0u; type < compiled.usage.edgeTypeCount; ++type)
  {
    for (uint32_t i = 0u; i + 1u < compiled.usage.instructionCount; ++i)
    {
      const uint32_t here = compiled.usage.at(i, type);
      const uint32_t next = compiled.usage.at(i + 1u, type);

      // Moving forward, the next use either stays put or moves later.
      if (here != UsageValues::never && next != UsageValues::never && next < here)
      {
        monotone = false;
      }
    }
  }

  check(monotone, "next-use values never move earlier as the programme advances");
}

void nextUseIsAlwaysAtOrAfterTheInstruction()
{
  Compiled compiled;

  build("let A likes B.\n"
        "let A is dead.\n"
        "rule r: when A likes B, B is dead then A likes B.\n",
        compiled);

  for (uint32_t i = 0u; i < compiled.usage.instructionCount; ++i)
  {
    for (uint32_t type = 0u; type < compiled.usage.edgeTypeCount; ++type)
    {
      const uint32_t value = compiled.usage.at(i, type);

      check(value == UsageValues::never || value >= i, "a next use is never in the past");
    }
  }
}

void readingInstructionSeesItself()
{
  Compiled compiled;

  build("let A likes B.\n"
        "let A is dead.\n"
        "rule r: when A likes B, B is dead then A likes B.\n",
        compiled);

  for (uint32_t i = 0u; i < compiled.usage.instructionCount; ++i)
  {
    const Instruction &instruction = compiled.program.instructions[i];

    if (instruction.op != Op::Select && instruction.op != Op::Where)
    {
      continue;
    }

    checkEqual(compiled.usage.at(i, instruction.label), i, "an instruction's own type is needed at that instruction");
  }
}

void corpusUsage()
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/basic.graphwrite");

  if (!file)
  {
    check(false, "corpus asset found");

    return;
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  Compiled compiled;

  build(buffer.str(), compiled);

  checkEqual(compiled.sink.count(), 0u, "corpus computes usage cleanly");
  checkEqual(compiled.usage.edgeTypeCount, 19u, "19 edge types");
  checkEqual(compiled.usage.instructionCount, static_cast<uint32_t>(compiled.program.instructions.size()), "one row per instruction");

  // Types the programme never reads must be `never` everywhere - the corpus
  // declares patterns that no rule matches on.
  uint32_t neverUsed = 0u;

  for (uint32_t type = 0u; type < compiled.usage.edgeTypeCount; ++type)
  {
    if (compiled.usage.at(0u, type) == UsageValues::never)
    {
      ++neverUsed;
    }
  }

  std::cout << "    corpus: " << (compiled.usage.edgeTypeCount - neverUsed) << " of " << compiled.usage.edgeTypeCount
            << " edge types are actually matched\n";

  check(neverUsed < compiled.usage.edgeTypeCount, "at least some edge types are used");
}

} // namespace

int main()
{
  handComputedTable();
  neverIsSticky();
  monotoneBackwards();
  nextUseIsAlwaysAtOrAfterTheInstruction();
  readingInstructionSeesItself();
  corpusUsage();

  return report("GraphRewriteUsageTests");
}
