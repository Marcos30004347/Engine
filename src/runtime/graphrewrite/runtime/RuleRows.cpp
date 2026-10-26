#include "graphrewrite/runtime/RuleRows.hpp"

namespace graphrewrite::runtime {

std::vector<uint32_t> ruleVertexColumns(const compiler::CompiledProgram &program, const uint32_t rule)
{
  const size_t vertices = rule < program.ruleVertexNames.size() ? program.ruleVertexNames[rule].size() : 0u;

  std::vector<uint32_t> columns(vertices, compiler::noRuleColumn);

  for (const compiler::Instruction &instruction : program.instructions)
  {
    if (instruction.op != compiler::Op::Select && instruction.op != compiler::Op::Where)
    {
      continue;
    }

    for (const compiler::RuleColumns &mapping : instruction.perRule)
    {
      if (mapping.ruleId != rule)
      {
        continue;
      }

      if (mapping.fromColumn < columns.size())
      {
        columns[mapping.fromColumn] = instruction.fromColumn;
      }

      if (mapping.writeColumn < columns.size())
      {
        columns[mapping.writeColumn] = instruction.writeColumn;
      }
    }
  }

  return columns;
}

bool ruleColumnsAreIdentity(const compiler::CompiledProgram &program)
{
  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    const std::vector<uint32_t> columns = ruleVertexColumns(program, rule);
    const uint32_t matched = rule < program.ruleRewrites.size() ? program.ruleRewrites[rule].matchedVertexCount : 0u;

    for (uint32_t vertex = 0u; vertex < matched && vertex < columns.size(); ++vertex)
    {
      if (columns[vertex] != vertex)
      {
        return false;
      }
    }
  }

  return true;
}

std::vector<MatchedRow> gatherRuleRows(const compiler::CompiledProgram &program, const CpuProgramRunner &runner, const uint32_t rule,
                                       const RowFilter &keep)
{
  std::vector<MatchedRow> rows;

  const MatchTable *table = runner.finalTableForRule(rule);

  if (table == nullptr || table->rows() == 0u || rule >= program.ruleRewrites.size())
  {
    return rows;
  }

  const uint32_t matched = program.ruleRewrites[rule].matchedVertexCount;
  const std::vector<uint32_t> columns = ruleVertexColumns(program, rule);

  if (!keep)
  {
    rows.reserve(table->rows());
  }

  for (uint32_t row = 0u; row < table->rows(); ++row)
  {
    if (keep && !keep(row))
    {
      continue;
    }

    MatchedRow out;

    out.rule = rule;
    out.index = row;
    out.vertices.resize(matched, 0u);

    for (uint32_t vertex = 0u; vertex < matched; ++vertex)
    {
      const uint32_t column = vertex < columns.size() ? columns[vertex] : compiler::noRuleColumn;

      if (column != compiler::noRuleColumn && column < table->columns())
      {
        out.vertices[vertex] = table->at(column, row);
      }
    }

    rows.push_back(std::move(out));
  }

  return rows;
}

} // namespace graphrewrite::runtime
