// Phase 1.1: diagnostics collect rather than terminate.
//
// The point of this suite is the negative one: the ported front end must not
// contain the abort() that the original called on every error path. A run that
// hits three problems reports three diagnostics and returns normally.

#include "TestHarness.hpp"
#include "graphrewrite/frontend/Diagnostics.hpp"

using namespace graphrewrite;
using namespace graphrewrite::testing;

namespace {

SourceSpan spanAt(const uint32_t line, const uint32_t column)
{
  SourceSpan span;

  span.offset = 0u;
  span.length = 1u;
  span.line = line;
  span.column = column;

  return span;
}

void collectsInOrder()
{
  DiagnosticSink sink;

  sink.error(spanAt(1u, 1u), "first");
  sink.warning(spanAt(2u, 5u), "second");
  sink.error(spanAt(3u, 9u), "third");

  checkEqual(sink.count(), 3u, "three diagnostics collected");
  checkEqual(sink.errorCount(), 2u, "warnings are not counted as errors");
  check(sink.hasErrors(), "hasErrors reflects severity");

  checkEqual(sink[0].message, std::string("first"), "order preserved 0");
  checkEqual(sink[1].message, std::string("second"), "order preserved 1");
  checkEqual(sink[2].message, std::string("third"), "order preserved 2");
}

void warningsAloneAreNotErrors()
{
  DiagnosticSink sink;

  sink.warning(spanAt(4u, 2u), "just a warning");

  checkEqual(sink.count(), 1u, "warning collected");
  check(!sink.hasErrors(), "a warning alone leaves hasErrors false");
}

void formatting()
{
  DiagnosticSink sink;

  sink.error(spanAt(12u, 34u), "expected DOT");
  sink.warning(spanAt(7u, 1u), "unused declaration");

  checkEqual(sink[0].format("rules.graphwrite"), std::string("rules.graphwrite:12:34: error: expected DOT"), "error formatting");
  checkEqual(sink[1].format("rules.graphwrite"), std::string("rules.graphwrite:7:1: warning: unused declaration"), "warning formatting");
}

void clearResets()
{
  DiagnosticSink sink;

  sink.error(spanAt(1u, 1u), "boom");
  sink.clear();

  checkEqual(sink.count(), 0u, "clear empties the sink");
  check(!sink.hasErrors(), "clear resets the error flag");
}

void survivesManyErrors()
{
  DiagnosticSink sink;

  for (uint32_t i = 0u; i < 1000u; ++i)
  {
    sink.error(spanAt(i + 1u, 1u), "error " + std::to_string(i));
  }

  checkEqual(sink.errorCount(), 1000u, "a thousand errors, no termination");
  checkEqual(sink[999].span.line, 1000u, "span of the last diagnostic survived");
}

} // namespace

int main()
{
  collectsInOrder();
  warningsAloneAreNotErrors();
  formatting();
  clearResets();
  survivesManyErrors();

  return report("GraphRewriteDiagnosticsTests");
}
