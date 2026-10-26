#include "graphrewrite/compiler/EdgeTypeTable.hpp"

namespace graphrewrite::compiler {

EdgeTypeId EdgeTypeTable::intern(const std::string &pattern, const uint32_t arity)
{
  const auto existing = byPattern.find(pattern);

  if (existing != byPattern.end())
  {
    return existing->second;
  }

  const EdgeTypeId id = static_cast<EdgeTypeId>(types.size());

  EdgeType type;

  type.pattern = pattern;
  type.arity = arity;

  types.push_back(std::move(type));
  byPattern.emplace(pattern, id);

  return id;
}

bool EdgeTypeTable::find(const std::string &pattern, EdgeTypeId &out) const
{
  const auto existing = byPattern.find(pattern);

  if (existing == byPattern.end())
  {
    return false;
  }

  out = existing->second;

  return true;
}

const EdgeType &EdgeTypeTable::operator[](const EdgeTypeId id) const
{
  return types[id];
}

EdgeType &EdgeTypeTable::mutableType(const EdgeTypeId id)
{
  return types[id];
}

uint32_t EdgeTypeTable::size() const
{
  return static_cast<uint32_t>(types.size());
}

const std::vector<EdgeType> &EdgeTypeTable::all() const
{
  return types;
}

} // namespace graphrewrite::compiler
