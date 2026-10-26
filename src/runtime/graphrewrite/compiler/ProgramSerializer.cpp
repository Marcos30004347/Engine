#include "graphrewrite/compiler/ProgramSerializer.hpp"

#include <cstring>

namespace graphrewrite::compiler {
namespace {

void putExpr(std::vector<uint8_t> &out, const Expr &expr);
void putU32(std::vector<uint8_t> &out, uint32_t value);
void putString(std::vector<uint8_t> &out, const std::string &text);

} // namespace
namespace {

void putU32(std::vector<uint8_t> &out, const uint32_t value)
{
  out.push_back(static_cast<uint8_t>(value & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

void putString(std::vector<uint8_t> &out, const std::string &value)
{
  putU32(out, static_cast<uint32_t>(value.size()));

  out.insert(out.end(), value.begin(), value.end());
}

void putExpr(std::vector<uint8_t> &out, const Expr &expr)
{
  putU32(out, static_cast<uint32_t>(expr.kind));
  putU32(out, expr.number);
  putString(out, expr.name);
  putU32(out, expr.vertex);
  putU32(out, static_cast<uint32_t>(expr.args.size()));

  for (const Expr &arg : expr.args)
  {
    putExpr(out, arg);
  }
}

class Reader
{
public:
  Reader(const uint8_t *data, const size_t size) : data(data), size(size)
  {
  }

  bool u32(uint32_t &out)
  {
    if (head + 4u > size)
    {
      overran = true;

      return false;
    }

    out = static_cast<uint32_t>(data[head]) | (static_cast<uint32_t>(data[head + 1u]) << 8u) |
          (static_cast<uint32_t>(data[head + 2u]) << 16u) | (static_cast<uint32_t>(data[head + 3u]) << 24u);

    head += 4u;

    return true;
  }

  bool string(std::string &out)
  {
    uint32_t length = 0u;

    if (!u32(length))
    {
      return false;
    }

    if (head + length > size)
    {
      overran = true;

      return false;
    }

    out.assign(reinterpret_cast<const char *>(data + head), length);
    head += length;

    return true;
  }

  bool ok() const
  {
    return !overran;
  }

private:
  const uint8_t *data;
  size_t size;
  size_t head = 0u;
  bool overran = false;
};

void readExpr(Reader &reader, Expr &expr)
{
  uint32_t kind = 0u;

  reader.u32(kind);
  expr.kind = static_cast<Expr::Kind>(kind);
  reader.u32(expr.number);
  reader.string(expr.name);
  reader.u32(expr.vertex);

  uint32_t count = 0u;

  reader.u32(count);

  // Bounded so a corrupt blob cannot ask for a billion nested arguments.
  for (uint32_t i = 0u; i < count && i < 64u && reader.ok(); ++i)
  {
    Expr arg;

    readExpr(reader, arg);
    expr.args.push_back(std::move(arg));
  }
}

} // namespace

bool writeProgram(const CompiledProgram &program, const UsageValues &usage, std::vector<uint8_t> &out)
{
  out.clear();

  putU32(out, programMagic);
  putU32(out, programVersion);

  putU32(out, program.edgeTypes.size());

  for (EdgeTypeId id = 0u; id < program.edgeTypes.size(); ++id)
  {
    const EdgeType &type = program.edgeTypes[id];

    putString(out, type.pattern);
    putU32(out, type.arity);
    putU32(out, type.usedForward ? 1u : 0u);
    putU32(out, type.usedBackward ? 1u : 0u);
    putU32(out, type.assumedCount);
  }

  putU32(out, static_cast<uint32_t>(program.ruleNames.size()));

  for (const std::string &name : program.ruleNames)
  {
    putString(out, name);
  }

  putU32(out, static_cast<uint32_t>(program.ruleColumnCounts.size()));

  for (const uint32_t count : program.ruleColumnCounts)
  {
    putU32(out, count);
  }

  putU32(out, static_cast<uint32_t>(program.threadStarts.size()));

  for (const uint32_t start : program.threadStarts)
  {
    putU32(out, start);
  }

  putU32(out, program.mergedEdgeCount);

  putU32(out, static_cast<uint32_t>(program.ruleEdgeCounts.size()));

  for (const uint32_t count : program.ruleEdgeCounts)
  {
    putU32(out, count);
  }

  putU32(out, static_cast<uint32_t>(program.ruleVertexNames.size()));

  for (const std::vector<std::string> &names : program.ruleVertexNames)
  {
    putU32(out, static_cast<uint32_t>(names.size()));

    for (const std::string &name : names)
    {
      putString(out, name);
    }
  }

  putU32(out, static_cast<uint32_t>(program.ruleRewrites.size()));

  for (const RuleRewrite &rewrite : program.ruleRewrites)
  {
    putU32(out, rewrite.matchedVertexCount);
    putU32(out, rewrite.totalVertexCount);
    putU32(out, rewrite.policy.kind == FirePolicy::Maybe ? 1u : 0u);
    putU32(out, rewrite.policy.chance);
    putU32(out, rewrite.policy.denominator);

    for (const std::vector<PatternEdge> *list : {&rewrite.removals, &rewrite.insertions, &rewrite.when})
    {
      putU32(out, static_cast<uint32_t>(list->size()));

      for (const PatternEdge &edge : *list)
      {
        putU32(out, edge.origin);
        putU32(out, edge.target);
        putU32(out, edge.label);
        putU32(out, edge.index);
        putU32(out, edge.hasClause ? 1u : 0u);

        if (edge.hasClause)
        {
          putExpr(out, edge.clause);
        }
      }
    }
  }

  putU32(out, static_cast<uint32_t>(program.instructions.size()));

  for (const Instruction &instruction : program.instructions)
  {
    putU32(out, static_cast<uint32_t>(instruction.op));
    putU32(out, instruction.reg);
    putU32(out, instruction.from);
    putU32(out, instruction.label);
    putU32(out, instruction.origin);
    putU32(out, instruction.target);
    putU32(out, static_cast<uint32_t>(instruction.direction));
    putU32(out, instruction.fromColumn);
    putU32(out, instruction.writeColumn);
    putU32(out, instruction.rule);
    putU32(out, instruction.thread);

    putU32(out, static_cast<uint32_t>(instruction.rules.size()));

    for (const uint32_t rule : instruction.rules)
    {
      putU32(out, rule);
    }

    putU32(out, static_cast<uint32_t>(instruction.perRule.size()));

    for (const RuleColumns &columns : instruction.perRule)
    {
      putU32(out, columns.ruleId);
      putU32(out, columns.fromColumn);
      putU32(out, columns.writeColumn);
      putU32(out, columns.edgeColumn);
    }
  }

  putU32(out, usage.instructionCount);
  putU32(out, usage.edgeTypeCount);

  for (const uint32_t value : usage.nextUse)
  {
    putU32(out, value);
  }

  return true;
}

bool readProgram(const uint8_t *data, const size_t size, CompiledProgram &program, UsageValues &usage, DiagnosticSink &sink)
{
  Reader reader(data, size);

  SourceSpan span;

  uint32_t magic = 0u;
  uint32_t version = 0u;

  if (!reader.u32(magic) || !reader.u32(version))
  {
    sink.error(span, "programme blob is truncated in its header");

    return false;
  }

  if (magic != programMagic)
  {
    sink.error(span, "not a graph-rewrite programme blob");

    return false;
  }

  if (version != programVersion)
  {
    sink.error(span, "programme blob is version " + std::to_string(version) + ", expected " + std::to_string(programVersion));

    return false;
  }

  program = CompiledProgram();

  uint32_t typeCount = 0u;

  reader.u32(typeCount);

  for (uint32_t i = 0u; i < typeCount && reader.ok(); ++i)
  {
    std::string pattern;
    uint32_t arity = 0u;
    uint32_t forward = 0u;
    uint32_t backward = 0u;

    reader.string(pattern);
    reader.u32(arity);
    reader.u32(forward);
    reader.u32(backward);

    uint32_t assumed = 0u;

    reader.u32(assumed);

    const EdgeTypeId id = program.edgeTypes.intern(pattern, arity);

    program.edgeTypes.mutableType(id).usedForward = forward != 0u;
    program.edgeTypes.mutableType(id).usedBackward = backward != 0u;
    program.edgeTypes.mutableType(id).assumedCount = assumed;
  }

  uint32_t ruleCount = 0u;

  reader.u32(ruleCount);

  for (uint32_t i = 0u; i < ruleCount && reader.ok(); ++i)
  {
    std::string name;

    reader.string(name);
    program.ruleNames.push_back(std::move(name));
  }

  uint32_t columnCounts = 0u;

  reader.u32(columnCounts);

  for (uint32_t i = 0u; i < columnCounts && reader.ok(); ++i)
  {
    uint32_t value = 0u;

    reader.u32(value);
    program.ruleColumnCounts.push_back(value);
  }

  uint32_t threadCount = 0u;

  reader.u32(threadCount);

  for (uint32_t i = 0u; i < threadCount && reader.ok(); ++i)
  {
    uint32_t value = 0u;

    reader.u32(value);
    program.threadStarts.push_back(value);
  }

  reader.u32(program.mergedEdgeCount);

  uint32_t edgeCounts = 0u;

  reader.u32(edgeCounts);

  for (uint32_t i = 0u; i < edgeCounts && reader.ok(); ++i)
  {
    uint32_t value = 0u;

    reader.u32(value);
    program.ruleEdgeCounts.push_back(value);
  }

  uint32_t vertexNameRules = 0u;

  reader.u32(vertexNameRules);

  for (uint32_t i = 0u; i < vertexNameRules && reader.ok(); ++i)
  {
    uint32_t count = 0u;

    reader.u32(count);

    std::vector<std::string> names;

    for (uint32_t n = 0u; n < count && reader.ok(); ++n)
    {
      std::string name;

      reader.string(name);
      names.push_back(std::move(name));
    }

    program.ruleVertexNames.push_back(std::move(names));
  }

  uint32_t rewrites = 0u;

  reader.u32(rewrites);

  for (uint32_t i = 0u; i < rewrites && reader.ok(); ++i)
  {
    RuleRewrite rewrite;

    reader.u32(rewrite.matchedVertexCount);
    reader.u32(rewrite.totalVertexCount);

    uint32_t maybe = 0u;

    reader.u32(maybe);
    reader.u32(rewrite.policy.chance);
    reader.u32(rewrite.policy.denominator);

    rewrite.policy.kind = maybe != 0u ? FirePolicy::Maybe : FirePolicy::Always;

    for (std::vector<PatternEdge> *list : {&rewrite.removals, &rewrite.insertions, &rewrite.when})
    {
      uint32_t count = 0u;

      reader.u32(count);

      for (uint32_t e = 0u; e < count && reader.ok(); ++e)
      {
        PatternEdge edge;

        reader.u32(edge.origin);
        reader.u32(edge.target);
        reader.u32(edge.label);
        reader.u32(edge.index);

        uint32_t hasClause = 0u;

        reader.u32(hasClause);

        if (hasClause != 0u)
        {
          edge.hasClause = true;
          readExpr(reader, edge.clause);
        }

        list->push_back(edge);
      }
    }

    program.ruleRewrites.push_back(std::move(rewrite));
  }

  uint32_t instructionCount = 0u;

  reader.u32(instructionCount);

  for (uint32_t i = 0u; i < instructionCount && reader.ok(); ++i)
  {
    Instruction instruction;

    uint32_t op = 0u;
    uint32_t direction = 0u;

    reader.u32(op);
    reader.u32(instruction.reg);
    reader.u32(instruction.from);
    reader.u32(instruction.label);
    reader.u32(instruction.origin);
    reader.u32(instruction.target);
    reader.u32(direction);
    reader.u32(instruction.fromColumn);
    reader.u32(instruction.writeColumn);
    reader.u32(instruction.rule);
    reader.u32(instruction.thread);

    instruction.op = static_cast<Op>(op);
    instruction.direction = static_cast<StepDirection>(direction);

    uint32_t rules = 0u;

    reader.u32(rules);

    for (uint32_t r = 0u; r < rules && reader.ok(); ++r)
    {
      uint32_t value = 0u;

      reader.u32(value);
      instruction.rules.push_back(value);
    }

    uint32_t mappings = 0u;

    reader.u32(mappings);

    for (uint32_t m = 0u; m < mappings && reader.ok(); ++m)
    {
      RuleColumns columns;

      reader.u32(columns.ruleId);
      reader.u32(columns.fromColumn);
      reader.u32(columns.writeColumn);
      reader.u32(columns.edgeColumn);

      instruction.perRule.push_back(columns);
    }

    program.instructions.push_back(std::move(instruction));
  }

  usage = UsageValues();

  reader.u32(usage.instructionCount);
  reader.u32(usage.edgeTypeCount);

  const size_t expected = static_cast<size_t>(usage.instructionCount) * usage.edgeTypeCount;

  for (size_t i = 0u; i < expected && reader.ok(); ++i)
  {
    uint32_t value = 0u;

    reader.u32(value);
    usage.nextUse.push_back(value);
  }

  if (!reader.ok())
  {
    sink.error(span, "programme blob is truncated");

    return false;
  }

  return true;
}

} // namespace graphrewrite::compiler
