#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::learn {

// A Bayesian network over discrete variables, with exact inference.
//
// This exists for one job - Merrell, Schkufza and Koltun's architectural
// programme (SIGGRAPH Asia 2010, §4) - and it is sized for it. Their networks
// run 42 to 120 nodes with 33 to 92 edges, every variable discrete because
// they quantised the continuous ones before fitting, and the queries are
// always the same shape: clamp a few variables as the brief and sample the
// rest. At that size exact inference is instant and a dependency is not worth
// taking, so this is variable elimination written out.
//
// Why a network rather than a bag of independent probabilities is the paper's
// own argument, and it is worth keeping in front of you while authoring one:
// encoding each room's existence and each adjacency on its own "does not take
// into account conditional dependencies between multiple rooms and
// adjacencies, and can generate unrealistic architectural programs. For
// example, the frequent occurrence in the data of a bedroom-bathroom adjacency
// and a kitchen-bathroom adjacency could lead to architectural programs being
// generated with kitchen-bathroom-bedroom paths, which have very low
// likelihood." A house is the conditional structure, not the marginals.
//
// Nothing here knows what a room is. Names are strings so a network can be
// authored by hand, printed, and later fitted to data without this file
// changing.

// A discrete random variable: a name, and its states in a fixed order. The
// order is the encoding - a CPT's columns are these states - so states are
// named rather than numbered everywhere they cross an interface.
struct Variable
{
  std::string name;
  std::vector<std::string> states;

  uint32_t cardinality() const { return uint32_t(states.size()); }
};

// What is known. One entry per variable; `unobserved` for the ones that are
// not. This is both the brief handed in and the sample handed back, which is
// what makes conditional sampling a loop over the same type.
struct Evidence
{
  static constexpr int32_t unobserved = -1;

  std::vector<int32_t> states;

  void resize(const uint32_t n) { states.assign(n, unobserved); }
  bool known(const uint32_t v) const { return v < states.size() && states[v] >= 0; }
  int32_t at(const uint32_t v) const { return v < states.size() ? states[v] : unobserved; }
  void set(const uint32_t v, const int32_t s)
  {
    if (v < states.size())
    {
      states[v] = s;
    }
  }
  void clear(const uint32_t v) { set(v, unobserved); }
};

class BayesianNetwork
{
public:
  static constexpr uint32_t invalid = 0xFFFFFFFFu;

  // --- building ---------------------------------------------------------

  uint32_t addVariable(std::string name, std::vector<std::string> states);

  // A yes/no variable. State 0 is false and state 1 is true, everywhere,
  // because a CPT row read as {p(no), p(yes)} and one read the other way
  // around are the same eight numbers and produce a house with no kitchen.
  uint32_t addBoolean(std::string name);

  // Parents, in the order the distribution's rows are indexed by. Setting
  // parents discards any distribution already on the node.
  void setParents(uint32_t node, std::vector<uint32_t> parents);

  // The conditional distribution, one row per joint assignment of the parents
  // in row-major order with the *last* parent varying fastest, each row a
  // distribution over this node's states.
  //
  // Rows are not checked for being in the order the author meant - nothing
  // can check that - so author them with the parent assignment in a comment
  // on each row. Rows that do not sum to one are normalised, and a wrong row
  // count is rejected here rather than at sample time.
  bool setDistribution(uint32_t node, const std::vector<std::vector<double>> &rows);

  // A node with no parents: one row.
  bool setPrior(uint32_t node, const std::vector<double> &row);

  // --- reading ----------------------------------------------------------

  uint32_t variableCount() const { return uint32_t(mVariables.size()); }
  const Variable &variableAt(const uint32_t v) const { return mVariables[v]; }
  uint32_t find(const std::string &name) const;
  uint32_t stateOf(uint32_t variable, const std::string &state) const;
  const std::vector<uint32_t> &parentsOf(const uint32_t v) const { return mNodes[v].parents; }

  // Every node has a distribution of the right size, the parent graph is
  // acyclic, and every row is a distribution. Acyclicity is the one thing a
  // hand-authored network gets wrong silently, so it is checked, not assumed.
  bool validate(std::string &why) const;

  // --- reading and writing one row of a table ---
  //
  // Fitting works row by row: find the row an observation falls in, add to it,
  // write it back. These exist for that and are the only way in.

  uint32_t rowCount(uint32_t node) const;
  std::vector<double> row(uint32_t node, uint32_t index) const;
  bool setRow(uint32_t node, uint32_t index, const std::vector<double> &);

  // Which row of `node`'s table an assignment selects, or `invalid` when any
  // parent of the node is unobserved - in which case the observation tells you
  // nothing about this node and must not be counted.
  uint32_t rowIndexOf(uint32_t node, const Evidence &) const;

  // --- carrying one about ---
  //
  // A network that took an hour of counting to fit is worth writing down. The
  // format is JSON and deliberately legible: a fitted table is a thing people
  // will want to read, argue with and hand-edit, and a binary blob makes all
  // three impossible. Node names are the identity, so a network saved before a
  // variable was added still loads - the missing node keeps its prior.

  std::string toJson() const;

  // Replaces everything. False, with `why` set, if the document is malformed
  // or its tables do not match the shapes it declares.
  bool fromJson(const std::string &, std::string &why);

  // --- inference --------------------------------------------------------

  // Exact marginal over `node` given `evidence`, by variable elimination with
  // a greedy min-degree ordering. Empty when the evidence has probability
  // zero - which for a hand-authored network means the brief asked for
  // something the network says cannot happen, and the caller must say so
  // rather than sample from a uniform.
  std::vector<double> marginal(uint32_t node, const Evidence &evidence) const;

  // Joint probability of a fully observed assignment. Used by the tests and
  // by the structure score later; the product of the CPT entries.
  double likelihood(const Evidence &complete) const;

  // Sample every unobserved variable, conditioned on the observed ones.
  //
  // Exact, not rejection: the variables are visited in topological order and
  // each is drawn from its marginal given the evidence *and* everything drawn
  // so far, then added to the evidence. One elimination per node, which for
  // networks this size is still immediate. Rejection sampling would be four
  // lines shorter and would hang on a brief that clamps a rare state.
  bool sample(const Evidence &evidence, uint64_t seed, Evidence &out) const;

  // The order `sample` visits nodes in: parents before children. Empty if the
  // graph has a cycle.
  std::vector<uint32_t> topologicalOrder() const;

  // The node's own conditional row, given that every parent is observed.
  //
  // This is the *exact* marginal whenever no descendant of the node is
  // observed - the local Markov property: a node is independent of its
  // non-descendants given its parents - and during ancestral sampling that is
  // the common case, because everything drawn so far is topologically before
  // it. It turns a full elimination into a table read, and a brief that
  // clamps nothing then costs one lookup per node instead of one elimination.
  std::vector<double> conditionalRow(uint32_t node, const Evidence &) const;

private:
  struct Node
  {
    std::vector<uint32_t> parents;
    std::vector<double> table;  // rows x cardinality, row-major.
    uint32_t rows = 0;
  };

  std::vector<Variable> mVariables;
  std::vector<Node> mNodes;
};

}  // namespace procedural::learn
