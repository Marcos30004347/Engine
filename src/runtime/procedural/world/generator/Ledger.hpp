#pragma once

#include "PatchRegion.hpp"

#include <string>
#include <vector>

namespace procedural::world::generator {

// Typed record of what a generator could not, or chose not to, do. Replaces
// free-form rejection strings so reviews and renders can group entries.
enum class LedgerKind : uint8_t
{
  Rejected, // Input judged unusable; nothing produced for it.
  Omitted,  // Produced elsewhere, left out of this product on purpose.
  Capacity, // A declared budget was exceeded.
  Repaired  // A bounded repair was applied; the product still stands.
};

struct LedgerEntry
{
  LedgerKind kind = LedgerKind::Rejected;
  PatchId region = 0;
  std::string message;
};

class Ledger
{
public:
  void add(LedgerKind kind, PatchId region, std::string message)
  {
    entries_.push_back({kind, region, std::move(message)});
  }

  size_t count(LedgerKind kind) const
  {
    size_t n = 0;

    for (const auto &entry : entries_)
    {
      if (entry.kind == kind)
      {
        ++n;
      }
    }

    return n;
  }

  const std::vector<LedgerEntry> &entries() const
  {
    return entries_;
  }

  std::vector<std::string> messages() const
  {
    std::vector<std::string> out;

    for (const auto &entry : entries_)
    {
      out.push_back(entry.message);
    }

    return out;
  }

private:
  std::vector<LedgerEntry> entries_;
};

} // namespace procedural::world::generator
