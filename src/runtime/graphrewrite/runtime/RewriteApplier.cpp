#include "graphrewrite/runtime/RewriteApplier.hpp"

#include "graphrewrite/runtime/Dice.hpp"

#include <algorithm>

namespace graphrewrite::runtime {

RewriteApplier::RewriteApplier(const compiler::CompiledProgram &compiled, EdgePageStore &pageStore, const uint32_t diceSeed)
    : program(compiled), store(pageStore), seed(diceSeed)
{
}

void RewriteApplier::setRound(const uint32_t value)
{
  round = value;
}

void RewriteApplier::setChanceScale(ChanceScale scale)
{
  chanceScale = std::move(scale);
}

void RewriteApplier::setFireObserver(FireObserver observer)
{
  fireObserver = std::move(observer);
}

void RewriteApplier::setNow(const uint32_t now)
{
  currentTime = now;
}

void RewriteApplier::setHost(const ExpressionHost *expressionHost)
{
  host = expressionHost;
}

void RewriteApplier::notify(const uint32_t rule, const std::vector<uint32_t> &bound, const uint32_t rowIndex,
                            const RewriteEventKind kind, const uint32_t time) const
{
  if (!fireObserver)
  {
    return;
  }

  RewriteEvent event;

  event.rule = rule;
  event.bound = bound;
  event.rowIndex = rowIndex;
  event.kind = kind;
  event.time = time;

  fireObserver(event);
}

bool RewriteApplier::evaluateGuards(const compiler::RuleRewrite &rewrite, const uint32_t rule,
                                    const std::vector<uint32_t> &vertices) const
{
  CallContext context;

  context.now = currentTime;
  context.rule = rule;
  context.bound = &vertices;

  for (const compiler::PatternEdge &edge : rewrite.when)
  {
    if (!edge.hasClause)
    {
      continue;
    }

    uint32_t value = 0u;

    // No host, or a host that cannot answer, fails the guard: a condition the
    // programme cannot evaluate is not one it may assume.
    if (host == nullptr || !host->evaluate(edge.clause, context, value) || value == 0u)
    {
      return false;
    }
  }

  return true;
}

uint32_t RewriteApplier::scheduleOf(const compiler::PatternEdge &edge, const uint32_t rule, const std::vector<uint32_t> &bound) const
{
  // A bind is not a schedule: the edge it sits on lands now.
  if (!edge.hasClause || host == nullptr || edge.clause.kind == Expr::Kind::Bind)
  {
    return currentTime;
  }

  CallContext context;

  context.now = currentTime;
  context.rule = rule;
  context.bound = &bound;

  uint32_t time = currentTime;

  if (!host->evaluate(edge.clause, context, time))
  {
    return currentTime;
  }

  return time;
}

void RewriteApplier::removeConcrete(const std::vector<ConcreteEdge> &edges, RewriteStats &stats)
{
  for (const ConcreteEdge &edge : edges)
  {
    if (store.removeEdge(edge.label, Edge{edge.origin, edge.target}))
    {
      ++stats.edgesRemoved;

      if (store.has(edge.label, KeyOrder::ByOrigin))
      {
        stats.touchedPartitions.insert(store.partition(edge.label, KeyOrder::ByOrigin).id());
      }
    }
  }
}

void RewriteApplier::insertConcrete(const std::vector<ConcreteEdge> &edges, RewriteStats &stats)
{
  for (const ConcreteEdge &edge : edges)
  {
    const Edge inserted{edge.origin, edge.target};

    if (store.containsEdge(edge.label, inserted))
    {
      ++stats.duplicatesSkipped;

      continue;
    }

    store.addEdge(edge.label, inserted);

    ++stats.edgesInserted;

    if (store.has(edge.label, KeyOrder::ByOrigin))
    {
      stats.touchedPartitions.insert(store.partition(edge.label, KeyOrder::ByOrigin).id());
    }
  }
}

RewriteStats RewriteApplier::apply(const std::vector<MatchedRow> &rows)
{
  RewriteStats stats;

  std::set<EdgeKey> claimed;

  // Decide every row against the graph as it is NOW, before a single edge is
  // removed - which is exactly what filter_rows does on the GPU. Folding these
  // tests into the apply loop instead would judge later rows against a store the
  // earlier ones had already changed, so a row sharing an edge with a row that
  // just fired would read as "an earlier rewrite took this" rather than as the
  // same-batch conflict it is, and the two devices would disagree.
  std::vector<bool> live(rows.size(), false);

  for (size_t i = 0u; i < rows.size(); ++i)
  {
    const MatchedRow &row = rows[i];

    if (row.rule >= program.ruleRewrites.size())
    {
      continue;
    }

    const compiler::RuleRewrite &rewrite = program.ruleRewrites[row.rule];

    uint32_t chance = rewrite.policy.permille();

    if (chanceScale)
    {
      chance = chanceScale(row.rule, chance);
    }

    if (!dice::fires(dice::seedFor(dice::roundSeed(seed, round), row.rule), row.index, chance))
    {
      ++stats.declined;

      continue;
    }

    // Does the match still hold? An earlier rewrite may have removed one of
    // these edges, which leaves this row describing a graph that is gone; firing
    // anyway would insert THEN edges for a WHEN that is no longer true.
    const bool present = std::all_of(rewrite.removals.begin(), rewrite.removals.end(),
                                     [this, &row](const compiler::PatternEdge &edge) {
                                       return edge.origin < row.vertices.size() && edge.target < row.vertices.size() &&
                                              store.containsEdge(edge.label,
                                                                 Edge{row.vertices[edge.origin], row.vertices[edge.target]});
                                     });

    if (!present)
    {
      ++stats.stale;

      continue;
    }

    // Would this row change anything? Removals always do. An insertion does
    // unless the edge is already there - and an edge touching a vertex the
    // THEN creates cannot already be there. A scheduled insertion counts as a
    // change: it is a promise, and promises are events.
    bool changes = !rewrite.removals.empty();

    for (const compiler::PatternEdge &edge : rewrite.insertions)
    {
      if (changes)
      {
        break;
      }

      const bool fresh = edge.origin >= rewrite.matchedVertexCount || edge.target >= rewrite.matchedVertexCount;

      changes = fresh || edge.hasClause || edge.origin >= row.vertices.size() || edge.target >= row.vertices.size() ||
                !store.containsEdge(edge.label, Edge{row.vertices[edge.origin], row.vertices[edge.target]});
    }

    if (!changes)
    {
      // Firing it would only tell the narrator something happened that did not.
      ++stats.noops;

      continue;
    }

    live[i] = true;
  }

  for (const MatchedRow &row : rows)
  {
    if (row.rule >= program.ruleRewrites.size())
    {
      continue;
    }

    const compiler::RuleRewrite &rewrite = program.ruleRewrites[row.rule];

    if (row.vertices.size() < rewrite.matchedVertexCount)
    {
      continue;
    }

    if (!live[&row - rows.data()])
    {
      continue;
    }

    // Claim every edge this row would remove. A row that cannot claim all of
    // them does not fire at all - a partial rewrite would leave the graph in a
    // state no sequential order produces.
    std::vector<EdgeKey> wanted;

    bool conflicted = false;

    for (const compiler::PatternEdge &edge : rewrite.removals)
    {
      if (edge.origin >= row.vertices.size() || edge.target >= row.vertices.size())
      {
        conflicted = true;

        break;
      }

      const EdgeKey key{row.vertices[edge.origin], edge.label, row.vertices[edge.target]};

      if (claimed.count(key) != 0u)
      {
        conflicted = true;

        break;
      }

      wanted.push_back(key);
    }

    if (conflicted)
    {
      ++stats.claimsRejected;

      continue;
    }

    // Guards last, once the row is otherwise certain to fire: a guard's handler
    // may act on being asked - book the church - and must not be asked on
    // behalf of a row that then loses its claim.
    if (!evaluateGuards(rewrite, row.rule, row.vertices))
    {
      ++stats.guarded;

      continue;
    }

    for (const EdgeKey &key : wanted)
    {
      claimed.insert(key);
    }

    // Vertices the THEN clause introduces are fresh for every firing.
    std::vector<uint32_t> bound = row.vertices;

    bound.resize(std::max<size_t>(bound.size(), rewrite.totalVertexCount), 0u);

    for (uint32_t vertex = rewrite.matchedVertexCount; vertex < rewrite.totalVertexCount; ++vertex)
    {
      bound[vertex] = store.allocateVertex();

      ++stats.verticesCreated;
    }

    // A bind clause gives a new vertex its identity from an expression -
    // `D' = day(now() + 4)` - so a value the rule computed becomes a node any
    // later rule can match on. Binds resolve before schedules, which may
    // refer to them.
    if (host != nullptr)
    {
      CallContext context;

      context.now = currentTime;
      context.rule = row.rule;
      context.bound = &bound;

      for (const compiler::PatternEdge &edge : rewrite.insertions)
      {
        if (!edge.hasClause || edge.clause.kind != Expr::Kind::Bind || edge.clause.args.size() != 2u)
        {
          continue;
        }

        const uint32_t target = edge.clause.args[0].vertex;
        uint32_t value = 0u;

        if (target >= rewrite.matchedVertexCount && target < bound.size() && host->evaluate(edge.clause.args[1], context, value))
        {
          bound[target] = value;
        }
      }
    }

    std::vector<ConcreteEdge> removals;

    for (const EdgeKey &key : wanted)
    {
      removals.push_back(ConcreteEdge{std::get<1>(key), std::get<0>(key), std::get<2>(key)});
    }

    // Consumption is immediate, whatever the insertions' timing: a WHEN edge
    // the THEN does not restate is gone now, and that is what stops this row
    // matching again tomorrow.
    removeConcrete(removals, stats);

    // Insertions are grouped by when they are due. The group due now lands
    // now; the rest wait, each with the WHEN edges the rule KEPT - the ones it
    // will still need to be true when its time comes.
    std::map<uint32_t, std::vector<ConcreteEdge>> groups;

    for (const compiler::PatternEdge &edge : rewrite.insertions)
    {
      if (edge.origin >= bound.size() || edge.target >= bound.size())
      {
        continue;
      }

      uint32_t time = scheduleOf(edge, row.rule, bound);

      if (time < currentTime)
      {
        time = currentTime;
      }

      groups[time].push_back(ConcreteEdge{edge.label, bound[edge.origin], bound[edge.target]});
    }

    std::vector<ConcreteEdge> requires;

    for (const compiler::PatternEdge &edge : rewrite.when)
    {
      if (edge.origin >= rewrite.matchedVertexCount || edge.target >= rewrite.matchedVertexCount)
      {
        continue;
      }

      const bool consumed = std::any_of(rewrite.removals.begin(), rewrite.removals.end(),
                                        [&edge](const compiler::PatternEdge &removed) { return removed.sameShapeAs(edge); });

      if (!consumed)
      {
        requires.push_back(ConcreteEdge{edge.label, bound[edge.origin], bound[edge.target]});
      }
    }

    bool appliedNow = !removals.empty();

    for (auto &group : groups)
    {
      if (group.first <= currentTime)
      {
        insertConcrete(group.second, stats);
        appliedNow = true;

        continue;
      }

      ScheduledRewrite later;

      later.time = group.first;
      later.sequence = nextSequence++;
      later.rule = row.rule;
      later.rowIndex = row.index;
      later.bound = bound;
      later.requires = requires;
      later.insertions = std::move(group.second);

      queue.push_back(std::move(later));

      ++stats.scheduled;

      notify(row.rule, bound, row.index, RewriteEventKind::Scheduled, group.first);
    }

    if (appliedNow)
    {
      ++stats.rulesFired;

      notify(row.rule, bound, row.index, RewriteEventKind::Applied, currentTime);
    }
  }

  return stats;
}

bool RewriteApplier::guardsHold(const uint32_t rule, const std::vector<uint32_t> &vertices) const
{
  return rule < program.ruleRewrites.size() && evaluateGuards(program.ruleRewrites[rule], rule, vertices);
}

void RewriteApplier::resolveBinds(const uint32_t rule, std::vector<uint32_t> &bound) const
{
  if (host == nullptr || rule >= program.ruleRewrites.size())
  {
    return;
  }

  const compiler::RuleRewrite &rewrite = program.ruleRewrites[rule];

  bound.resize(std::max<size_t>(bound.size(), rewrite.totalVertexCount), 0u);

  CallContext context;

  context.now = currentTime;
  context.rule = rule;
  context.bound = &bound;

  for (const compiler::PatternEdge &edge : rewrite.insertions)
  {
    if (!edge.hasClause || edge.clause.kind != Expr::Kind::Bind || edge.clause.args.size() != 2u)
    {
      continue;
    }

    const uint32_t target = edge.clause.args[0].vertex;
    uint32_t value = 0u;

    if (target >= rewrite.matchedVertexCount && target < bound.size() && host->evaluate(edge.clause.args[1], context, value))
    {
      bound[target] = value;
    }
  }
}

uint32_t RewriteApplier::scheduleDeferred(const uint32_t rule, const std::vector<uint32_t> &bound, const uint32_t rowIndex,
                                          RewriteStats &stats)
{
  if (rule >= program.ruleRewrites.size())
  {
    return 0u;
  }

  const compiler::RuleRewrite &rewrite = program.ruleRewrites[rule];

  std::map<uint32_t, std::vector<ConcreteEdge>> groups;

  for (const compiler::PatternEdge &edge : rewrite.insertions)
  {
    // Only the insertions with a SCHEDULE clause: the rest the device applied.
    if (!edge.hasClause || edge.clause.kind == Expr::Kind::Bind)
    {
      continue;
    }

    if (edge.origin >= bound.size() || edge.target >= bound.size())
    {
      continue;
    }

    uint32_t time = scheduleOf(edge, rule, bound);

    if (time < currentTime)
    {
      time = currentTime;
    }

    groups[time].push_back(ConcreteEdge{edge.label, bound[edge.origin], bound[edge.target]});
  }

  if (groups.empty())
  {
    return 0u;
  }

  std::vector<ConcreteEdge> requires;

  for (const compiler::PatternEdge &edge : rewrite.when)
  {
    if (edge.origin >= rewrite.matchedVertexCount || edge.target >= rewrite.matchedVertexCount || edge.origin >= bound.size() ||
        edge.target >= bound.size())
    {
      continue;
    }

    const bool consumed = std::any_of(rewrite.removals.begin(), rewrite.removals.end(),
                                      [&edge](const compiler::PatternEdge &removed) { return removed.sameShapeAs(edge); });

    if (!consumed)
    {
      requires.push_back(ConcreteEdge{edge.label, bound[edge.origin], bound[edge.target]});
    }
  }

  uint32_t queued = 0u;

  for (auto &group : groups)
  {
    ScheduledRewrite later;

    later.time = group.first;
    later.sequence = nextSequence++;
    later.rule = rule;
    later.rowIndex = rowIndex;
    later.bound = bound;
    later.requires = requires;
    later.insertions = std::move(group.second);

    queue.push_back(std::move(later));

    ++stats.scheduled;
    ++queued;

    notify(rule, bound, rowIndex, RewriteEventKind::Scheduled, group.first);
  }

  return queued;
}

void RewriteApplier::notifyApplied(const uint32_t rule, const std::vector<uint32_t> &bound, const uint32_t rowIndex) const
{
  notify(rule, bound, rowIndex, RewriteEventKind::Applied, currentTime);
}

RewriteStats RewriteApplier::applyDue()
{
  RewriteStats stats;

  std::stable_sort(queue.begin(), queue.end(), [](const ScheduledRewrite &a, const ScheduledRewrite &b) {
    return a.time != b.time ? a.time < b.time : a.sequence < b.sequence;
  });

  size_t kept = 0u;

  for (size_t i = 0u; i < queue.size(); ++i)
  {
    ScheduledRewrite &entry = queue[i];

    if (entry.time > currentTime)
    {
      // Compact in place. Never move an element onto itself: a self-move
      // leaves a vector empty, and an empty insertion list released on its day
      // is a wedding at which nothing happens.
      if (kept != i)
      {
        queue[kept] = std::move(entry);
      }

      ++kept;

      continue;
    }

    // The match this was made on has to hold TODAY. Anything a rewrite since
    // then took - a groom, a church - cancels it.
    const bool holds = std::all_of(entry.requires.begin(), entry.requires.end(), [this](const ConcreteEdge &edge) {
      return store.containsEdge(edge.label, Edge{edge.origin, edge.target});
    });

    if (!holds)
    {
      ++stats.cancelled;

      notify(entry.rule, entry.bound, entry.rowIndex, RewriteEventKind::Cancelled, currentTime);

      continue;
    }

    insertConcrete(entry.insertions, stats);

    ++stats.released;
    ++stats.rulesFired;

    notify(entry.rule, entry.bound, entry.rowIndex, RewriteEventKind::Released, currentTime);
  }

  queue.resize(kept);

  return stats;
}

} // namespace graphrewrite::runtime
