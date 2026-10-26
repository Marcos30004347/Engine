#include "graphrewrite/frontend/Diagnostics.hpp"

namespace graphrewrite {

std::string Diagnostic::format(const std::string &path) const
{
  std::string out;

  out.reserve(path.size() + message.size() + 32u);

  out += path;
  out += ':';
  out += std::to_string(span.line);
  out += ':';
  out += std::to_string(span.column);
  out += ": ";
  out += severity == Severity::Error ? "error: " : "warning: ";
  out += message;

  return out;
}

void DiagnosticSink::report(const Severity severity, const SourceSpan span, std::string message)
{
  diagnostics.push_back(Diagnostic{severity, span, std::move(message)});

  if (severity == Severity::Error)
  {
    ++errors;
  }
}

void DiagnosticSink::error(const SourceSpan span, std::string message)
{
  report(Severity::Error, span, std::move(message));
}

void DiagnosticSink::warning(const SourceSpan span, std::string message)
{
  report(Severity::Warning, span, std::move(message));
}

bool DiagnosticSink::hasErrors() const
{
  return errors > 0u;
}

uint32_t DiagnosticSink::errorCount() const
{
  return errors;
}

uint32_t DiagnosticSink::count() const
{
  return static_cast<uint32_t>(diagnostics.size());
}

const std::vector<Diagnostic> &DiagnosticSink::all() const
{
  return diagnostics;
}

const Diagnostic &DiagnosticSink::operator[](const uint32_t index) const
{
  return diagnostics[index];
}

void DiagnosticSink::clear()
{
  diagnostics.clear();
  errors = 0u;
}

} // namespace graphrewrite
