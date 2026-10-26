#pragma once

// Diagnostics for the graph-rewrite front end.
//
// The system this is ported from reported every error by printf followed by
// abort(). That is fine for a standalone compiler binary and unacceptable for a
// library linked into the engine: a malformed rule file must not take the
// process down, and one run must be able to report every problem it found
// rather than the first.
//
// Nothing in this module calls abort, exit or printf. Errors travel as values.

#include <cstdint>
#include <string>
#include <vector>

namespace graphrewrite {

// A half-open byte range in one source file, carrying the line and column of
// its first character so a diagnostic can be printed without re-scanning.
struct SourceSpan
{
  uint32_t offset = 0u;
  uint32_t length = 0u;
  uint32_t line = 1u;
  uint32_t column = 1u;
};

enum class Severity
{
  Error,
  Warning,
};

struct Diagnostic
{
  Severity severity = Severity::Error;
  SourceSpan span;
  std::string message;

  // "path:line:column: error: message"
  std::string format(const std::string &path) const;
};

// Collects diagnostics instead of terminating. A sink that has seen an error is
// still usable: analysis continues so that one run reports everything.
class DiagnosticSink
{
public:
  void report(Severity severity, SourceSpan span, std::string message);
  void error(SourceSpan span, std::string message);
  void warning(SourceSpan span, std::string message);

  bool hasErrors() const;
  uint32_t errorCount() const;
  uint32_t count() const;

  const std::vector<Diagnostic> &all() const;
  const Diagnostic &operator[](uint32_t index) const;

  void clear();

private:
  std::vector<Diagnostic> diagnostics;
  uint32_t errors = 0u;
};

} // namespace graphrewrite
