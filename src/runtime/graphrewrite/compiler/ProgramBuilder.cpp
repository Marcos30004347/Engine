#include "graphrewrite/compiler/ProgramBuilder.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace graphrewrite::compiler {
namespace {

// Orders candidate edges: most rules served first, then by index so the choice
// never depends on container order.
uint32_t pickNextEdge(const std::vector<std::vector<uint32_t>> &live, const std::vector<bool> &emitted)
{
  uint32_t best = ~uint32_t(0);
  size_t bestCount = 0u;

  for (uint32_t i = 0u; i < static_cast<uint32_t>(live.size()); ++i)
  {
    if (emitted[i] || live[i].empty())
    {
      continue;
    }

    if (best == ~uint32_t(0) || live[i].size() > bestCount)
    {
      best = i;
      bestCount = live[i].size();
    }
  }

  return best;
}

// Emission builds a TRIE over the rules' edge sequences.
//
// Each rule needs its own table, and that table is threaded through exactly the
// instructions that serve the rule. So every rule's edges must lie on a single
// root-to-node path. Sharing is then precisely a shared PREFIX: while two rules
// want the same next edge they use the same instruction, and the moment they
// diverge the trie forks.
//
// Two weaker structures were tried first and both are wrong. A single chain
// makes every rule inherit the others' filters. A tree keyed only on "rules(i)
// is a subset of rules(parent)" lets one rule's edges land under two different
// roots, leaving it with no table lineage at all.

struct TrieNode
{
  uint32_t mergedEdge = ~uint32_t(0);
  uint32_t parent = ~uint32_t(0);
  uint32_t reg = 0u;
  std::vector<uint32_t> rules;
  std::vector<uint32_t> children;
  std::map<uint32_t, uint32_t> columns; // merged vertex -> column
};

// One rule's edges, ordered so each attaches to a vertex an earlier edge bound.
// Shared edges are preferred early, which is what makes prefixes coincide.
// How many edges of an edge type the graph holds, or "unknown" when no
// statistics were supplied.
uint32_t cardinalityOf(const std::vector<uint32_t> &counts, const EdgeTypeId label)
{
  return label < counts.size() ? counts[label] : ~uint32_t(0);
}

std::vector<uint32_t> sequenceFor(const MergedGraph &merged, const uint32_t rule, const std::vector<uint32_t> &counts)
{
  std::vector<uint32_t> mine;

  for (uint32_t i = 0u; i < static_cast<uint32_t>(merged.edges.size()); ++i)
  {
    if (std::find(merged.edgeRules[i].begin(), merged.edgeRules[i].end(), rule) != merged.edgeRules[i].end())
    {
      mine.push_back(i);
    }
  }

  std::vector<uint32_t> out;
  std::set<uint32_t> bound;
  std::vector<bool> taken(mine.size(), false);

  while (out.size() < mine.size())
  {
    uint32_t best = ~uint32_t(0);
    size_t bestShare = 0u;
    uint32_t bestCardinality = ~uint32_t(0);

    for (uint32_t i = 0u; i < static_cast<uint32_t>(mine.size()); ++i)
    {
      if (taken[i])
      {
        continue;
      }

      const PatternEdge &edge = merged.edges[mine[i]];

      const bool attachable = out.empty() || bound.count(edge.origin) != 0u || bound.count(edge.target) != 0u;

      if (!attachable)
      {
        continue;
      }

      const size_t share = merged.edgeRules[mine[i]].size();
      const uint32_t cardinality = cardinalityOf(counts, merged.edges[mine[i]].label);

      // Rarest first when statistics are available - it is the single biggest
      // lever on how large the intermediate tables get. Sharing breaks ties,
      // because a shared edge still costs one traversal instead of two.
      const bool better = best == ~uint32_t(0) || cardinality < bestCardinality ||
                          (cardinality == bestCardinality && (share > bestShare || (share == bestShare && mine[i] < mine[best])));

      if (better)
      {
        best = i;
        bestShare = share;
        bestCardinality = cardinality;
      }
    }

    if (best == ~uint32_t(0))
    {
      // Disconnected remainder: take the most-shared edge and carry on, which
      // opens a new root for this rule rather than dropping its edges.
      for (uint32_t i = 0u; i < static_cast<uint32_t>(mine.size()); ++i)
      {
        if (!taken[i])
        {
          best = i;

          break;
        }
      }
    }

    if (best == ~uint32_t(0))
    {
      break;
    }

    const PatternEdge &edge = merged.edges[mine[best]];

    taken[best] = true;
    bound.insert(edge.origin);
    bound.insert(edge.target);
    out.push_back(mine[best]);
  }

  return out;
}

void emitThread(const MergedGraph &merged, const uint32_t threadIndex, CompiledProgram &out, EdgeTypeTable &table,
                const std::vector<uint32_t> &counts)
{
  if (merged.edges.empty() || merged.rules.empty())
  {
    return;
  }

  // Build the trie. Roots are children of a virtual node.
  std::vector<TrieNode> nodes;
  std::vector<uint32_t> roots;

  for (const uint32_t rule : merged.rules)
  {
    const std::vector<uint32_t> sequence = sequenceFor(merged, rule, counts);

    uint32_t current = ~uint32_t(0);

    for (const uint32_t edge : sequence)
    {
      uint32_t match = ~uint32_t(0);

      {
        // Read the sibling list through a const reference and drop it before
        // growing `nodes`: pushing a node reallocates the vector, and a
        // reference into nodes[current].children taken beforehand would dangle.
        const std::vector<uint32_t> &siblings = current == ~uint32_t(0) ? roots : nodes[current].children;

        for (const uint32_t candidate : siblings)
        {
          if (nodes[candidate].mergedEdge == edge)
          {
            match = candidate;

            break;
          }
        }
      }

      if (match == ~uint32_t(0))
      {
        TrieNode node;

        node.mergedEdge = edge;
        node.parent = current;

        nodes.push_back(std::move(node));

        match = static_cast<uint32_t>(nodes.size() - 1u);

        if (current == ~uint32_t(0))
        {
          roots.push_back(match);
        }
        else
        {
          nodes[current].children.push_back(match);
        }
      }

      nodes[match].rules.push_back(rule);
      current = match;
    }
  }

  out.threadStarts.push_back(static_cast<uint32_t>(out.instructions.size()));

  // Emit depth-first: a parent's table must exist before a child extends it.
  std::vector<uint32_t> stack(roots.rbegin(), roots.rend());

  while (!stack.empty())
  {
    const uint32_t index = stack.back();

    stack.pop_back();

    TrieNode &node = nodes[index];
    const PatternEdge &edge = merged.edges[node.mergedEdge];

    Instruction instruction;

    instruction.reg = static_cast<uint32_t>(out.instructions.size());
    instruction.label = edge.label;
    instruction.origin = edge.origin;
    instruction.target = edge.target;
    instruction.rules = node.rules;
    instruction.thread = threadIndex;

    if (node.parent == ~uint32_t(0))
    {
      instruction.op = Op::Select;

      node.columns[edge.origin] = 0u;
      instruction.fromColumn = 0u;

      if (edge.origin == edge.target)
      {
        instruction.direction = StepDirection::Cyclic;
        instruction.writeColumn = 0u;
      }
      else
      {
        node.columns[edge.target] = 1u;
        instruction.direction = StepDirection::Forward;
        instruction.writeColumn = 1u;
      }

      table.mutableType(edge.label).usedForward = true;
    }
    else
    {
      instruction.op = Op::Where;
      instruction.from = nodes[node.parent].reg;

      node.columns = nodes[node.parent].columns;

      const bool knownOrigin = node.columns.count(edge.origin) != 0u;
      const bool knownTarget = node.columns.count(edge.target) != 0u;

      const uint32_t nextColumn = static_cast<uint32_t>(node.columns.size());

      if (knownOrigin && knownTarget)
      {
        instruction.direction = StepDirection::Cyclic;
        instruction.fromColumn = node.columns[edge.origin];
        instruction.writeColumn = node.columns[edge.target];

        table.mutableType(edge.label).usedForward = true;
      }
      else if (knownOrigin)
      {
        instruction.direction = StepDirection::Forward;
        instruction.fromColumn = node.columns[edge.origin];
        node.columns[edge.target] = nextColumn;
        instruction.writeColumn = nextColumn;

        table.mutableType(edge.label).usedForward = true;
      }
      else if (knownTarget)
      {
        instruction.direction = StepDirection::Backward;
        instruction.fromColumn = node.columns[edge.target];
        node.columns[edge.origin] = nextColumn;
        instruction.writeColumn = nextColumn;

        table.mutableType(edge.label).usedBackward = true;
      }
      else
      {
        // Neither endpoint bound: this edge starts a disconnected component of
        // the rule, so it becomes a root of its own.
        instruction.op = Op::Select;
        instruction.from = noRegister;

        node.columns.clear();
        node.columns[edge.origin] = 0u;
        instruction.fromColumn = 0u;

        if (edge.origin == edge.target)
        {
          instruction.direction = StepDirection::Cyclic;
          instruction.writeColumn = 0u;
        }
        else
        {
          node.columns[edge.target] = 1u;
          instruction.direction = StepDirection::Forward;
          instruction.writeColumn = 1u;
        }

        table.mutableType(edge.label).usedForward = true;
      }
    }

    node.reg = instruction.reg;

    out.instructions.push_back(std::move(instruction));

    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
    {
      stack.push_back(*child);
    }
  }
}

// A rule is reported at the last instruction that serves it; a register is freed
// after the last instruction that reads or defines it.
void addBookkeeping(CompiledProgram &out)
{
  std::map<uint32_t, uint32_t> lastForRule;
  std::map<uint32_t, uint32_t> lastForRegister;

  for (uint32_t i = 0u; i < static_cast<uint32_t>(out.instructions.size()); ++i)
  {
    const Instruction &instruction = out.instructions[i];

    for (const uint32_t rule : instruction.rules)
    {
      lastForRule[rule] = i;
    }

    if (instruction.reg != noRegister)
    {
      lastForRegister[instruction.reg] = i;
    }

    if (instruction.from != noRegister)
    {
      lastForRegister[instruction.from] = i;
    }
  }

  std::vector<Instruction> expanded;

  expanded.reserve(out.instructions.size() * 2u);

  std::vector<uint32_t> remap(out.instructions.size(), 0u);

  for (uint32_t i = 0u; i < static_cast<uint32_t>(out.instructions.size()); ++i)
  {
    remap[i] = static_cast<uint32_t>(expanded.size());

    expanded.push_back(out.instructions[i]);

    for (const auto &entry : lastForRule)
    {
      if (entry.second != i)
      {
        continue;
      }

      Instruction report;

      report.op = Op::ReportRewrite;
      report.from = out.instructions[i].reg;
      report.rule = entry.first;
      report.rules = {entry.first};

      expanded.push_back(std::move(report));
    }

    for (const auto &entry : lastForRegister)
    {
      if (entry.second != i)
      {
        continue;
      }

      Instruction free;

      free.op = Op::FreeTable;
      free.from = entry.first;

      expanded.push_back(std::move(free));
    }
  }

  for (uint32_t &start : out.threadStarts)
  {
    start = remap[start];
  }

  out.instructions.swap(expanded);
}

} // namespace

const char *opName(const Op op)
{
  switch (op)
  {
  case Op::Select:
    return "SELECT";
  case Op::Where:
    return "WHERE";
  case Op::FreeTable:
    return "FREE";
  case Op::ReportRewrite:
    return "REPORT";
  }

  return "<unknown>";
}

bool buildProgram(const std::vector<MergedGraph> &threads, const std::vector<RuleGraph> &graphs, EdgeTypeTable table,
                  DiagnosticSink &sink, CompiledProgram &out, const std::vector<uint32_t> &edgeCounts)
{
  const uint32_t errorsBefore = sink.errorCount();

  out.instructions.clear();
  out.threadStarts.clear();

  for (const RuleGraph &graph : graphs)
  {
    out.ruleNames.push_back(graph.name);
    out.ruleEdgeCounts.push_back(static_cast<uint32_t>(graph.when.size()));
    out.ruleVertexNames.push_back(graph.vertexNames);

    RuleRewrite rewrite;

    // A WHEN edge the THEN clause does not restate is removed.
    for (const PatternEdge &edge : graph.when)
    {
      const bool preserved = std::any_of(graph.then.begin(), graph.then.end(),
                                         [&edge](const PatternEdge &other) { return other.sameShapeAs(edge); });

      if (!preserved)
      {
        rewrite.removals.push_back(edge);
      }
    }

    // A THEN edge the WHEN clause did not already have is inserted.
    for (const PatternEdge &edge : graph.then)
    {
      const bool existed = std::any_of(graph.when.begin(), graph.when.end(),
                                       [&edge](const PatternEdge &other) { return other.sameShapeAs(edge); });

      if (!existed)
      {
        rewrite.insertions.push_back(edge);
      }
    }

    uint32_t matched = 0u;

    for (const PatternEdge &edge : graph.when)
    {
      matched = std::max(matched, std::max(edge.origin, edge.target) + 1u);
    }

    rewrite.policy = graph.policy;
    rewrite.when = graph.when;
    rewrite.matchedVertexCount = matched;
    rewrite.totalVertexCount = graph.vertexCount();

    out.ruleRewrites.push_back(std::move(rewrite));
  }

  uint32_t expectedEdges = 0u;

  for (uint32_t threadIndex = 0u; threadIndex < static_cast<uint32_t>(threads.size()); ++threadIndex)
  {
    expectedEdges += static_cast<uint32_t>(threads[threadIndex].edges.size());

    emitThread(threads[threadIndex], threadIndex, out, table, edgeCounts);
  }

  // Record what the plan assumed, so the runtime can tell whether it still holds.
  for (EdgeTypeId type = 0u; type < table.size() && type < edgeCounts.size(); ++type)
  {
    table.mutableType(type).assumedCount = edgeCounts[type];
  }

  out.mergedEdgeCount = expectedEdges;

  addBookkeeping(out);

  out.edgeTypes = std::move(table);

  return sink.errorCount() == errorsBefore;
}

std::string dumpProgram(const CompiledProgram &program)
{
  std::string out;

  for (const Instruction &instruction : program.instructions)
  {
    out += std::string(opName(instruction.op));

    switch (instruction.op)
    {
    case Op::Select:
      out += " r" + std::to_string(instruction.reg) + " " + program.edgeTypes[instruction.label].pattern + " col " +
             std::to_string(instruction.fromColumn) + "->" + std::to_string(instruction.writeColumn);
      break;
    case Op::Where:
      out += " r" + std::to_string(instruction.reg) + " from r" + std::to_string(instruction.from) + " " +
             program.edgeTypes[instruction.label].pattern + " " +
             (instruction.direction == StepDirection::Forward    ? "FORWARD"
              : instruction.direction == StepDirection::Backward ? "BACKWARD"
                                                                 : "CYCLIC") +
             " col " + std::to_string(instruction.fromColumn) + "->" + std::to_string(instruction.writeColumn);
      break;
    case Op::FreeTable:
      out += " r" + std::to_string(instruction.from);
      break;
    case Op::ReportRewrite:
      out += " r" + std::to_string(instruction.from) + " rule " + std::to_string(instruction.rule);
      break;
    }

    if (!instruction.rules.empty() && instruction.op != Op::ReportRewrite)
    {
      out += "  rules";

      for (const uint32_t rule : instruction.rules)
      {
        out += " " + std::to_string(rule);
      }
    }

    out += '\n';
  }

  // What each rule actually does to the graph. Worth printing: an edge matched
  // AND restated produces no instruction at all here, and that absence is the
  // only evidence the emitter is not removing and re-adding it.
  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleRewrites.size()); ++rule)
  {
    const RuleRewrite &rewrite = program.ruleRewrites[rule];

    out += "RULE " + (rule < program.ruleNames.size() ? program.ruleNames[rule] : std::to_string(rule));

    if (rewrite.policy.kind == FirePolicy::Maybe)
    {
      out += " maybe " + std::to_string(rewrite.policy.chance);
    }

    const auto edges = [&program, &out](const char *label, const std::vector<PatternEdge> &list) {
      out += std::string("  ") + label + "=";

      if (list.empty())
      {
        out += "none";

        return;
      }

      for (size_t i = 0u; i < list.size(); ++i)
      {
        out += (i == 0u ? "" : " ") + std::to_string(list[i].origin) + program.edgeTypes[list[i].label].pattern +
               std::to_string(list[i].target);

        if (list[i].hasClause)
        {
          out += "(" + printExpr(list[i].clause) + ")";
        }
      }
    };

    std::vector<PatternEdge> guards;

    for (const PatternEdge &edge : rewrite.when)
    {
      if (edge.hasClause)
      {
        guards.push_back(edge);
      }
    }

    if (!guards.empty())
    {
      edges("guard", guards);
    }

    edges("remove", rewrite.removals);
    edges("insert", rewrite.insertions);

    out += '\n';
  }

  return out;
}

} // namespace graphrewrite::compiler
