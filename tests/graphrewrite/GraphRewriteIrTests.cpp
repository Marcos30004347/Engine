// Phase 2.1 [G1]: edge type ids follow declaration order.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Ir.hpp"
#include "graphrewrite/frontend/Parser.hpp"

#include <fstream>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::compiler;
using namespace graphrewrite::testing;

namespace {

struct Compiled
{
  std::unique_ptr<Program> program;
  AnalysedProgram analysed;
  EdgeTypeTable table;
  DiagnosticSink sink;
};

void build(const std::string &source, Compiled &out)
{
  Parser parser(source, "t.graphwrite", out.sink);

  out.program = Program::parse(parser);

  analyse(*out.program, OrExpansionOptions(), out.sink, out.analysed);
  buildEdgeTypeTable(out.analysed, out.table, out.sink);
}

void idsFollowDeclarationOrder()
{
  Compiled compiled;

  build("let A at B.\n"
        "let A likes B.\n"
        "let A is dead.",
        compiled);

  checkEqual(compiled.sink.count(), 0u, "clean");
  checkEqual(compiled.table.size(), 3u, "three edge types");
  checkEqual(compiled.table[0].pattern, std::string("? at ?"), "id 0");
  checkEqual(compiled.table[1].pattern, std::string("? likes ?"), "id 1");
  checkEqual(compiled.table[2].pattern, std::string("? is dead"), "id 2");
}

void arityIsRecorded()
{
  Compiled compiled;

  build("let A at B.\nlet A is dead.", compiled);

  checkEqual(compiled.table[0].arity, 2u, "binary pattern arity");
  checkEqual(compiled.table[1].arity, 1u, "unary pattern arity");
}

void directionUsageStartsUnset()
{
  Compiled compiled;

  build("let A at B.", compiled);

  check(!compiled.table[0].usedForward, "usedForward starts false");
  check(!compiled.table[0].usedBackward, "usedBackward starts false");
}

void lookupByPattern()
{
  Compiled compiled;

  build("let A at B.\nlet A likes B.", compiled);

  EdgeTypeId id = invalidEdgeType;

  check(compiled.table.find("? likes ?", id), "find locates a declared pattern");
  checkEqual(id, EdgeTypeId(1), "with the right id");
  check(!compiled.table.find("? hates ?", id), "find fails for an undeclared pattern");
}

void internIsIdempotent()
{
  EdgeTypeTable table;

  const EdgeTypeId first = table.intern("? at ?", 2u);
  const EdgeTypeId second = table.intern("? at ?", 2u);

  checkEqual(first, second, "interning the same pattern twice gives the same id");
  checkEqual(table.size(), 1u, "and does not grow the table");
}

void corpusHasNineteenEdgeTypes()
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/basic.graphwrite");

  check(file.good(), "corpus asset found");

  if (!file)
  {
    return;
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  Compiled compiled;

  build(buffer.str(), compiled);

  checkEqual(compiled.sink.count(), 0u, "corpus builds an edge type table cleanly");
  checkEqual(compiled.table.size(), 19u, "19 edge types");

  // Spot-check the ends of the declaration order.
  checkEqual(compiled.table[0].pattern, std::string("? at ?"), "first declared pattern");
  checkEqual(compiled.table[18].pattern, std::string("? is city of ?"), "last declared pattern");

  const std::string dump = dumpEdgeTypeTable(compiled.table);

  check(dump.find("0: \"? at ?\" arity=2") != std::string::npos, "dump renders id, pattern and arity");
}

} // namespace

int main()
{
  idsFollowDeclarationOrder();
  arityIsRecorded();
  directionUsageStartsUnset();
  lookupByPattern();
  internIsIdempotent();
  corpusHasNineteenEdgeTypes();

  return report("GraphRewriteIrTests");
}
