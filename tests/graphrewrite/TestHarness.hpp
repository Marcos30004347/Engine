#pragma once

// Shared check/report helpers for the graph-rewrite suites.
//
// Deliberately not assert(): the engine's test binaries are built in Release,
// where NDEBUG erases assert() together with the expression inside it, so an
// assert-based suite silently tests nothing. A counter plus a non-zero exit is
// what actually fails a ctest run here.

#include <cstdint>
#include <iostream>
#include <string>

namespace graphrewrite::testing {

inline int failures = 0;
inline int checks = 0;

inline void check(const bool condition, const std::string &what)
{
  ++checks;

  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";

    return;
  }

  std::cerr << "  FAILED " << what << "\n";
  ++failures;
}

template <typename A, typename B> void checkEqual(const A &got, const B &want, const std::string &what)
{
  ++checks;

  if (got == want)
  {
    std::cout << "  PASSED " << what << "\n";

    return;
  }

  std::cerr << "  FAILED " << what << "\n    got  " << got << "\n    want " << want << "\n";
  ++failures;
}

inline int report(const std::string &suite)
{
  std::cout << suite << ": " << (checks - failures) << "/" << checks << " checks passed\n";

  return failures == 0 ? 0 : 1;
}

} // namespace graphrewrite::testing
