#pragma once

#include "procedural/world/hierarchy/Generator.hpp"

#include <map>
#include <vector>

namespace procedural::world::hierarchy {

// Generator selection by identity, replacing selection by boolean.
//
// The settlement generator currently decides which lot generator runs from
// nineteen booleans across two settings structs, several of them documented by
// precedence - "watabouLots wins over frontageStrips" - which is a dispatch
// table written in prose. Here a generator is bound to a role by name and a
// policy picks between the bindings per region, so adding one is a binding and
// never a new flag with a new precedence rule.
//
// Per region, not per settlement: that is what lets a merchant ward be gridded
// while the quarter beside it stays organic.

struct Policy
{
  // The binding used for a role when no rule matches.
  std::map<Role, std::string> defaults;

  // A rule that claims a region for one binding. The first matching rule in
  // order wins; ordering is explicit rather than emergent.
  struct Rule
  {
    Role role = Role::Lots;
    std::string generator;
    std::function<bool(const Region &)> when;
    std::string because; // Recorded in the ledger, so a render can be explained.
  };

  std::vector<Rule> rules;
};

class Registry
{
public:
  // Binds a generator under its own descriptor id. Two generators may share a
  // role; that is the point.
  bool bind(GeneratorPtr, std::string *error = nullptr);

  GeneratorBase *find(const std::string &id) const;

  std::vector<std::string> boundTo(Role) const;

  // The generator this policy wants for this region, and why. Returns nullptr
  // when nothing is bound for the role, which is a caller error rather than a
  // silent no-op.
  GeneratorBase *choose(Role, const Region &, const Policy &, std::string *because = nullptr) const;

private:
  std::map<std::string, GeneratorPtr> byId_;
  std::map<Role, std::vector<std::string>> byRole_;
};

} // namespace procedural::world::hierarchy
