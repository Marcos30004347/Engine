#include "procedural/learn/BayesianNetwork.hpp"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace procedural::learn {

namespace {

// A factor over a set of variables: the working form of variable elimination.
// `vars` ascends, and `values` is row-major with the *last* variable varying
// fastest, which is the same convention the CPTs use so a CPT becomes a factor
// by copying.
struct Factor
{
  std::vector<uint32_t> vars;
  std::vector<uint32_t> cards;
  std::vector<double> values;

  size_t size() const { return values.size(); }
};

// Position of `var` in a factor, or the size if it is not there.
size_t indexOf(const Factor &f, const uint32_t var)
{
  return size_t(std::lower_bound(f.vars.begin(), f.vars.end(), var) - f.vars.begin());
}

bool has(const Factor &f, const uint32_t var)
{
  const size_t i = indexOf(f, var);

  return i < f.vars.size() && f.vars[i] == var;
}

// Strides so that a joint assignment maps to a flat offset.
std::vector<size_t> stridesOf(const Factor &f)
{
  std::vector<size_t> strides(f.vars.size(), 1);

  for (size_t i = f.vars.size(); i-- > 1;)
  {
    strides[i - 1] = strides[i] * f.cards[i];
  }

  return strides;
}

Factor product(const Factor &a, const Factor &b)
{
  Factor out;

  // Union of the two variable sets, still ascending.
  out.vars.reserve(a.vars.size() + b.vars.size());
  std::set_union(a.vars.begin(), a.vars.end(), b.vars.begin(), b.vars.end(),
                 std::back_inserter(out.vars));

  out.cards.resize(out.vars.size());

  for (size_t i = 0; i < out.vars.size(); ++i)
  {
    const size_t ia = indexOf(a, out.vars[i]);
    out.cards[i] = ia < a.vars.size() && a.vars[ia] == out.vars[i]
                     ? a.cards[ia]
                     : b.cards[indexOf(b, out.vars[i])];
  }

  size_t total = 1;

  for (const uint32_t c : out.cards)
  {
    total *= c;
  }

  out.values.assign(total, 0.);

  const std::vector<size_t> outStrides = stridesOf(out);
  const std::vector<size_t> aStrides = stridesOf(a);
  const std::vector<size_t> bStrides = stridesOf(b);

  // Offset contributed to each operand by each of the output's variables;
  // zero for a variable the operand does not carry.
  std::vector<size_t> toA(out.vars.size(), 0), toB(out.vars.size(), 0);

  for (size_t i = 0; i < out.vars.size(); ++i)
  {
    if (has(a, out.vars[i]))
    {
      toA[i] = aStrides[indexOf(a, out.vars[i])];
    }

    if (has(b, out.vars[i]))
    {
      toB[i] = bStrides[indexOf(b, out.vars[i])];
    }
  }

  std::vector<uint32_t> assignment(out.vars.size(), 0);

  for (size_t flat = 0; flat < total; ++flat)
  {
    size_t offA = 0, offB = 0;

    for (size_t i = 0; i < assignment.size(); ++i)
    {
      offA += assignment[i] * toA[i];
      offB += assignment[i] * toB[i];
    }

    out.values[flat] = a.values[offA] * b.values[offB];

    for (size_t i = assignment.size(); i-- > 0;)
    {
      if (++assignment[i] < out.cards[i])
      {
        break;
      }

      assignment[i] = 0;
    }
  }

  (void)outStrides;

  return out;
}

Factor sumOut(const Factor &f, const uint32_t var)
{
  if (!has(f, var))
  {
    return f;
  }

  const size_t axis = indexOf(f, var);
  const std::vector<size_t> strides = stridesOf(f);
  const size_t stride = strides[axis];
  const uint32_t card = f.cards[axis];

  Factor out;
  out.vars = f.vars;
  out.cards = f.cards;
  out.vars.erase(out.vars.begin() + long(axis));
  out.cards.erase(out.cards.begin() + long(axis));

  size_t total = 1;

  for (const uint32_t c : out.cards)
  {
    total *= c;
  }

  out.values.assign(total, 0.);

  // Walk the source: every flat offset whose digit at `axis` is zero starts a
  // run of `card` entries `stride` apart that collapse to one output entry.
  size_t written = 0;

  for (size_t flat = 0; flat < f.values.size(); ++flat)
  {
    if ((flat / stride) % card != 0)
    {
      continue;
    }

    double sum = 0.;

    for (uint32_t s = 0; s < card; ++s)
    {
      sum += f.values[flat + size_t(s) * stride];
    }

    out.values[written++] = sum;
  }

  return out;
}

}  // namespace

uint32_t BayesianNetwork::addVariable(std::string name, std::vector<std::string> states)
{
  const uint32_t index = uint32_t(mVariables.size());

  mVariables.push_back({std::move(name), std::move(states)});
  mNodes.push_back({});
  mNodes.back().rows = 1;
  mNodes.back().table.assign(mVariables.back().cardinality(),
                             mVariables.back().cardinality() > 0
                               ? 1. / double(mVariables.back().cardinality())
                               : 0.);

  return index;
}

uint32_t BayesianNetwork::addBoolean(std::string name)
{
  return addVariable(std::move(name), {"no", "yes"});
}

void BayesianNetwork::setParents(const uint32_t node, std::vector<uint32_t> parents)
{
  if (node >= mNodes.size())
  {
    return;
  }

  uint32_t rows = 1;

  for (const uint32_t p : parents)
  {
    rows *= p < mVariables.size() ? mVariables[p].cardinality() : 1u;
  }

  mNodes[node].parents = std::move(parents);
  mNodes[node].rows = rows;
  mNodes[node].table.assign(size_t(rows) * mVariables[node].cardinality(), 0.);
}

bool BayesianNetwork::setDistribution(const uint32_t node,
                                      const std::vector<std::vector<double>> &rows)
{
  if (node >= mNodes.size())
  {
    return false;
  }

  Node &n = mNodes[node];
  const uint32_t card = mVariables[node].cardinality();

  if (rows.size() != n.rows)
  {
    return false;
  }

  n.table.assign(size_t(n.rows) * card, 0.);

  for (size_t r = 0; r < rows.size(); ++r)
  {
    if (rows[r].size() != card)
    {
      return false;
    }

    double sum = 0.;

    for (const double v : rows[r])
    {
      if (v < 0.)
      {
        return false;
      }

      sum += v;
    }

    if (sum <= 0.)
    {
      return false;
    }

    for (uint32_t s = 0; s < card; ++s)
    {
      n.table[r * card + s] = rows[r][s] / sum;
    }
  }

  return true;
}

bool BayesianNetwork::setPrior(const uint32_t node, const std::vector<double> &row)
{
  return setDistribution(node, {row});
}

uint32_t BayesianNetwork::find(const std::string &name) const
{
  for (uint32_t i = 0; i < mVariables.size(); ++i)
  {
    if (mVariables[i].name == name)
    {
      return i;
    }
  }

  return invalid;
}

uint32_t BayesianNetwork::stateOf(const uint32_t variable, const std::string &state) const
{
  if (variable >= mVariables.size())
  {
    return invalid;
  }

  const std::vector<std::string> &states = mVariables[variable].states;

  for (uint32_t i = 0; i < states.size(); ++i)
  {
    if (states[i] == state)
    {
      return i;
    }
  }

  return invalid;
}

std::vector<uint32_t> BayesianNetwork::topologicalOrder() const
{
  const size_t n = mNodes.size();

  std::vector<uint32_t> remaining(n, 0);
  std::vector<std::vector<uint32_t>> children(n);

  for (uint32_t v = 0; v < n; ++v)
  {
    remaining[v] = uint32_t(mNodes[v].parents.size());

    for (const uint32_t p : mNodes[v].parents)
    {
      children[p].push_back(v);
    }
  }

  std::vector<uint32_t> order, ready;

  for (uint32_t v = 0; v < n; ++v)
  {
    if (remaining[v] == 0)
    {
      ready.push_back(v);
    }
  }

  while (!ready.empty())
  {
    // Lowest index first, so the order is a function of the network and not
    // of the container - a sample has to be reproducible from its seed.
    const auto pick = std::min_element(ready.begin(), ready.end());
    const uint32_t v = *pick;
    ready.erase(pick);
    order.push_back(v);

    for (const uint32_t c : children[v])
    {
      if (--remaining[c] == 0)
      {
        ready.push_back(c);
      }
    }
  }

  return order.size() == n ? order : std::vector<uint32_t>{};
}

bool BayesianNetwork::validate(std::string &why) const
{
  for (uint32_t v = 0; v < mVariables.size(); ++v)
  {
    if (mVariables[v].cardinality() < 1)
    {
      why = mVariables[v].name + " has no states";

      return false;
    }

    const Node &n = mNodes[v];
    uint32_t rows = 1;

    for (const uint32_t p : n.parents)
    {
      if (p >= mVariables.size())
      {
        why = mVariables[v].name + " has a parent that does not exist";

        return false;
      }

      rows *= mVariables[p].cardinality();
    }

    if (n.rows != rows || n.table.size() != size_t(rows) * mVariables[v].cardinality())
    {
      why = mVariables[v].name + " has no distribution, or one of the wrong size";

      return false;
    }

    for (uint32_t r = 0; r < rows; ++r)
    {
      double sum = 0.;

      for (uint32_t s = 0; s < mVariables[v].cardinality(); ++s)
      {
        sum += n.table[size_t(r) * mVariables[v].cardinality() + s];
      }

      if (std::abs(sum - 1.) > 1e-6)
      {
        why = mVariables[v].name + " row " + std::to_string(r) + " is not a distribution";

        return false;
      }
    }
  }

  if (topologicalOrder().empty() && !mVariables.empty())
  {
    why = "the parent graph has a cycle";

    return false;
  }

  why.clear();

  return true;
}

std::string BayesianNetwork::toJson() const
{
  nlohmann::json document;
  document["note"] = "A discrete Bayesian network. Rows run over the parents' "
                     "joint states with the last parent varying fastest.";
  document["variables"] = nlohmann::json::array();

  for (uint32_t v = 0; v < mVariables.size(); ++v)
  {
    nlohmann::json node;
    node["name"] = mVariables[v].name;
    node["states"] = mVariables[v].states;
    node["parents"] = nlohmann::json::array();

    for (const uint32_t p : mNodes[v].parents)
    {
      node["parents"].push_back(mVariables[p].name);
    }

    node["rows"] = nlohmann::json::array();

    for (uint32_t r = 0; r < mNodes[v].rows; ++r)
    {
      nlohmann::json values = nlohmann::json::array();

      for (const double x : row(v, r))
      {
        // Six figures is far more than the counts justify and still keeps the
        // file readable; a full double per cell triples its size for nothing.
        values.push_back(std::round(x * 1e6) / 1e6);
      }

      node["rows"].push_back(values);
    }

    document["variables"].push_back(node);
  }

  return document.dump(1);
}

bool BayesianNetwork::fromJson(const std::string &text, std::string &why)
{
  why.clear();

  nlohmann::json document;

  try
  {
    document = nlohmann::json::parse(text);
  }
  catch (const std::exception &e)
  {
    why = e.what();

    return false;
  }

  if (!document.contains("variables") || !document["variables"].is_array())
  {
    why = "no variables";

    return false;
  }

  BayesianNetwork built;

  // Two passes: every variable has to exist before any parent can name one.
  for (const auto &node : document["variables"])
  {
    std::vector<std::string> states;

    for (const auto &state : node.value("states", nlohmann::json::array()))
    {
      states.push_back(state.get<std::string>());
    }

    if (states.empty())
    {
      why = "a variable with no states";

      return false;
    }

    built.addVariable(node.value("name", std::string()), std::move(states));
  }

  uint32_t index = 0;

  for (const auto &node : document["variables"])
  {
    std::vector<uint32_t> parents;

    for (const auto &parent : node.value("parents", nlohmann::json::array()))
    {
      const uint32_t p = built.find(parent.get<std::string>());

      if (p == invalid)
      {
        why = "unknown parent '" + parent.get<std::string>() + "'";

        return false;
      }

      parents.push_back(p);
    }

    built.setParents(index, parents);

    std::vector<std::vector<double>> rows;

    for (const auto &values : node.value("rows", nlohmann::json::array()))
    {
      rows.push_back(values.get<std::vector<double>>());
    }

    if (!built.setDistribution(index, rows))
    {
      why = "'" + built.variableAt(index).name + "' has "
            + std::to_string(rows.size()) + " rows where its parents need "
            + std::to_string(built.rowCount(index));

      return false;
    }

    ++index;
  }

  std::string invalidWhy;

  if (!built.validate(invalidWhy))
  {
    why = invalidWhy;

    return false;
  }

  *this = std::move(built);

  return true;
}

uint32_t BayesianNetwork::rowCount(const uint32_t node) const
{
  return node < mNodes.size() ? mNodes[node].rows : 0;
}

std::vector<double> BayesianNetwork::row(const uint32_t node, const uint32_t index) const
{
  if (node >= mNodes.size() || index >= mNodes[node].rows)
  {
    return {};
  }

  const uint32_t card = mVariables[node].cardinality();

  return std::vector<double>(mNodes[node].table.begin() + long(size_t(index) * card),
                             mNodes[node].table.begin() + long(size_t(index + 1) * card));
}

bool BayesianNetwork::setRow(const uint32_t node, const uint32_t index,
                             const std::vector<double> &values)
{
  if (node >= mNodes.size() || index >= mNodes[node].rows)
  {
    return false;
  }

  const uint32_t card = mVariables[node].cardinality();

  if (values.size() != card)
  {
    return false;
  }

  double sum = 0.;

  for (const double v : values)
  {
    if (v < 0.)
    {
      return false;
    }

    sum += v;
  }

  if (!(sum > 0.))
  {
    return false;
  }

  for (uint32_t s = 0; s < card; ++s)
  {
    mNodes[node].table[size_t(index) * card + s] = values[s] / sum;
  }

  return true;
}

uint32_t BayesianNetwork::rowIndexOf(const uint32_t node, const Evidence &evidence) const
{
  if (node >= mNodes.size())
  {
    return invalid;
  }

  size_t row = 0;

  for (const uint32_t parent : mNodes[node].parents)
  {
    if (!evidence.known(parent))
    {
      return invalid;
    }

    row = row * mVariables[parent].cardinality() + size_t(evidence.at(parent));
  }

  return uint32_t(row);
}

std::vector<double> BayesianNetwork::conditionalRow(const uint32_t node,
                                                   const Evidence &evidence) const
{
  if (node >= mVariables.size())
  {
    return {};
  }

  const Node &n = mNodes[node];
  size_t row = 0;

  for (const uint32_t p : n.parents)
  {
    if (!evidence.known(p))
    {
      return {};
    }

    row = row * mVariables[p].cardinality() + size_t(evidence.at(p));
  }

  const uint32_t card = mVariables[node].cardinality();

  return std::vector<double>(n.table.begin() + long(row * card),
                             n.table.begin() + long((row + 1) * card));
}

std::vector<double> BayesianNetwork::marginal(const uint32_t node, const Evidence &evidence) const
{
  if (node >= mVariables.size())
  {
    return {};
  }

  const uint32_t card = mVariables[node].cardinality();

  // Every CPT becomes a factor over {node} u parents, reduced by whatever the
  // evidence already fixes. Reducing at construction is what keeps the
  // factors small enough that the elimination order barely matters.
  std::vector<Factor> factors;
  factors.reserve(mNodes.size());

  for (uint32_t v = 0; v < mNodes.size(); ++v)
  {
    const Node &n = mNodes[v];

    std::vector<uint32_t> scope = n.parents;
    scope.push_back(v);

    // Free variables of this factor, ascending, after evidence.
    std::vector<uint32_t> free;

    for (const uint32_t s : scope)
    {
      if (!evidence.known(s))
      {
        free.push_back(s);
      }
    }

    std::sort(free.begin(), free.end());
    free.erase(std::unique(free.begin(), free.end()), free.end());

    Factor f;
    f.vars = free;
    f.cards.resize(free.size());

    size_t total = 1;

    for (size_t i = 0; i < free.size(); ++i)
    {
      f.cards[i] = mVariables[free[i]].cardinality();
      total *= f.cards[i];
    }

    f.values.assign(total, 0.);

    // Walk every free assignment, read the CPT entry it selects together with
    // the evidence.
    std::vector<uint32_t> assignment(free.size(), 0);

    for (size_t flat = 0; flat < total; ++flat)
    {
      size_t row = 0;

      for (const uint32_t p : n.parents)
      {
        const size_t i = size_t(std::lower_bound(free.begin(), free.end(), p) - free.begin());
        const uint32_t state = i < free.size() && free[i] == p
                                 ? assignment[i]
                                 : uint32_t(evidence.at(p));
        row = row * mVariables[p].cardinality() + state;
      }

      const size_t iSelf = size_t(std::lower_bound(free.begin(), free.end(), v) - free.begin());
      const uint32_t self = iSelf < free.size() && free[iSelf] == v
                              ? assignment[iSelf]
                              : uint32_t(evidence.at(v));

      f.values[flat] = n.table[row * mVariables[v].cardinality() + self];

      for (size_t i = assignment.size(); i-- > 0;)
      {
        if (++assignment[i] < f.cards[i])
        {
          break;
        }

        assignment[i] = 0;
      }
    }

    factors.push_back(std::move(f));
  }

  // Everything free but the query gets eliminated, greedily by how many other
  // free variables it shares a factor with - min-degree. Cheap, and on a
  // network shaped like a house (a few roots, a fan of rooms, a pair of nodes
  // per adjacency) it keeps the intermediate factors to a handful of entries.
  std::vector<uint32_t> toEliminate;

  for (uint32_t v = 0; v < mVariables.size(); ++v)
  {
    if (v != node && !evidence.known(v))
    {
      toEliminate.push_back(v);
    }
  }

  while (!toEliminate.empty())
  {
    uint32_t best = 0;
    size_t bestDegree = size_t(-1);

    for (size_t i = 0; i < toEliminate.size(); ++i)
    {
      std::vector<uint32_t> neighbours;

      for (const Factor &f : factors)
      {
        if (has(f, toEliminate[i]))
        {
          neighbours.insert(neighbours.end(), f.vars.begin(), f.vars.end());
        }
      }

      std::sort(neighbours.begin(), neighbours.end());
      const size_t degree =
        size_t(std::unique(neighbours.begin(), neighbours.end()) - neighbours.begin());

      if (degree < bestDegree)
      {
        bestDegree = degree;
        best = uint32_t(i);
      }
    }

    const uint32_t var = toEliminate[best];
    toEliminate.erase(toEliminate.begin() + long(best));

    Factor merged;
    std::vector<Factor> kept;
    bool first = true;

    for (Factor &f : factors)
    {
      if (!has(f, var))
      {
        kept.push_back(std::move(f));

        continue;
      }

      merged = first ? std::move(f) : product(merged, f);
      first = false;
    }

    if (!first)
    {
      kept.push_back(sumOut(merged, var));
    }

    factors = std::move(kept);
  }

  Factor result;
  result.vars = {node};
  result.cards = {card};
  result.values.assign(card, 1.);

  for (const Factor &f : factors)
  {
    result = product(result, f);
  }

  // A constant factor - every free variable eliminated - means the query
  // variable itself was observed; not our caller's usage, but do not lie.
  if (result.vars.size() != 1 || result.vars[0] != node)
  {
    return {};
  }

  const double sum = std::accumulate(result.values.begin(), result.values.end(), 0.);

  if (!(sum > 0.) || !std::isfinite(sum))
  {
    return {};
  }

  for (double &v : result.values)
  {
    v /= sum;
  }

  return result.values;
}

double BayesianNetwork::likelihood(const Evidence &complete) const
{
  double p = 1.;

  for (uint32_t v = 0; v < mNodes.size(); ++v)
  {
    if (!complete.known(v))
    {
      return 0.;
    }

    const Node &n = mNodes[v];
    size_t row = 0;

    for (const uint32_t parent : n.parents)
    {
      if (!complete.known(parent))
      {
        return 0.;
      }

      row = row * mVariables[parent].cardinality() + size_t(complete.at(parent));
    }

    p *= n.table[row * mVariables[v].cardinality() + size_t(complete.at(v))];
  }

  return p;
}

bool BayesianNetwork::sample(const Evidence &evidence, const uint64_t seed, Evidence &out) const
{
  const std::vector<uint32_t> order = topologicalOrder();

  if (order.size() != mVariables.size())
  {
    return false;
  }

  out = evidence;
  out.states.resize(mVariables.size(), Evidence::unobserved);

  // Which nodes have an observed *descendant*. Only those need an
  // elimination; the rest read their CPT row. The set is fixed by the brief,
  // because everything the loop below adds to the evidence is topologically
  // earlier and so a descendant of nothing it has yet to visit.
  std::vector<bool> observedBelow(mVariables.size(), false);

  for (size_t i = order.size(); i-- > 0;)
  {
    const uint32_t v = order[i];

    for (const uint32_t p : mNodes[v].parents)
    {
      observedBelow[p] = observedBelow[p] || observedBelow[v] || evidence.known(v);
    }
  }

  uint64_t state = seed ^ 0x9E3779B97F4A7C15ull;

  for (const uint32_t v : order)
  {
    if (out.known(v))
    {
      continue;
    }

    const std::vector<double> p =
      observedBelow[v] ? marginal(v, out) : conditionalRow(v, out);

    if (p.empty())
    {
      // The evidence has probability zero under this network. Say so.
      return false;
    }

    // splitmix64, so one seed gives one house on every platform.
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;

    const double u = double(z >> 11) * 0x1.0p-53;

    double running = 0.;
    uint32_t chosen = uint32_t(p.size()) - 1;

    for (uint32_t s = 0; s < p.size(); ++s)
    {
      running += p[s];

      if (u < running)
      {
        chosen = s;

        break;
      }
    }

    out.set(v, int32_t(chosen));
  }

  return true;
}

}  // namespace procedural::learn
