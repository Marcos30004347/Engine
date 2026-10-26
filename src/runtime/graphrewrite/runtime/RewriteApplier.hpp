#pragma once

// Applies one round of rewrites to the graph.
//
// Whoever matched - GPU or CPU - hands over the matched rows; this decides what
// actually fires and changes the store. The claim step is what makes a parallel
// round equal to SOME sequential order: two rules can match the same edge in one
// round, and without claims the outcome depends on which happened to run first.
//
// Time. A THEN edge with a schedule clause is not inserted at once; the row's
// insertions are grouped by the time each is due, the group due now lands now,
// and every later group waits in a queue with the concrete WHEN edges it was
// matched on. applyDue() releases the groups whose time has come - after
// checking those edges still exist. A wedding set for five days hence, whose
// groom is murdered on day two, is cancelled on day five, not held.
//
// REMOVALS ALWAYS HAPPEN NOW. Consumption is what stops a rule re-matching, and
// a consumption that waited would let the same couple set a date every day
// until the first one arrived. So a rule that wants an effect later inserts the
// later edge on a schedule and lets another rule consume what it replaces when
// it lands - `B is dead (occurAt(now() + 2))`, and a rule that removes `alive`
// wherever it meets `dead`.
//
// The queue is plain arrays of node ids and times so it can move to the GPU
// later; nothing in it refers back to a table row.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/runtime/EdgePageStore.hpp"
#include "graphrewrite/runtime/ExpressionHost.hpp"
#include "graphrewrite/runtime/RewriteEvent.hpp"

#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace graphrewrite::runtime {

struct RewriteStats
{
  uint32_t rulesFired = 0u;
  uint32_t claimsRejected = 0u;
  uint32_t edgesRemoved = 0u;
  uint32_t edgesInserted = 0u;
  uint32_t verticesCreated = 0u;
  uint32_t declined = 0u; // rows whose dice said no
  uint32_t stale = 0u;    // rows whose edges an earlier rewrite already removed

  // The graph is a SET of edges, not a bag. A row that would only re-insert
  // edges already present changes nothing and does not fire; an insertion that
  // duplicates an existing edge is dropped even when the row does fire.
  uint32_t noops = 0u;
  uint32_t duplicatesSkipped = 0u;

  uint32_t guarded = 0u;   // rows a guard clause turned away
  uint32_t scheduled = 0u; // groups put in the queue
  uint32_t cancelled = 0u; // queued groups whose match no longer held
  uint32_t released = 0u;  // queued groups applied by applyDue
  std::set<uint32_t> touchedPartitions;
};

// One matched row of one rule: the vertex bound to each of that rule's
// vertices, in rule-vertex order.
struct MatchedRow
{
  uint32_t rule = 0u;
  std::vector<uint32_t> vertices;

  // Position of this row in its rule's table. It IS the dice input, so it has to
  // be the same number the GPU used, not the index within this batch.
  uint32_t index = 0u;
};

struct ConcreteEdge
{
  EdgeTypeId label = compiler::invalidEdgeType;
  uint32_t origin = 0u;
  uint32_t target = 0u;
};

// A group of a row's edges waiting for its time.
struct ScheduledRewrite
{
  uint32_t time = 0u;
  uint32_t sequence = 0u; // order of scheduling, for a stable release order
  uint32_t rule = 0u;
  uint32_t rowIndex = 0u;
  std::vector<uint32_t> bound;

  std::vector<ConcreteEdge> requires; // the WHEN edges the rule kept; must still hold
  std::vector<ConcreteEdge> insertions;
};

class RewriteApplier
{
public:
  RewriteApplier(const compiler::CompiledProgram &program, EdgePageStore &store, uint32_t diceSeed = 0x9e3779b9u);

  // Applies every row it can claim, in the order given. Rows whose edges were
  // already claimed by an earlier row are rejected rather than applied.
  RewriteStats apply(const std::vector<MatchedRow> &rows);

  // Releases every queued group due at or before `now`, re-validating each.
  // Call it at the start of a tick, before matching, so the graph the matcher
  // sees already reflects what today brought.
  RewriteStats applyDue();

  // Something outside the programme can scale a rule's chance per call - the
  // story driver raises the death rate as a village grows. Given the rule and
  // its compiled chance in thousandths, returns the chance to roll with.
  using ChanceScale = std::function<uint32_t(uint32_t rule, uint32_t permille)>;

  void setChanceScale(ChanceScale scale);

  // Which round this is. Each call to apply() over a re-matched graph must
  // roll fresh dice, or a row keeps the roll it had last time.
  void setRound(uint32_t round);

  // The current time, in whatever unit the programme's clauses use. `now()`
  // evaluates to this, and schedules are compared against it.
  void setNow(uint32_t now);

  uint32_t now() const
  {
    return currentTime;
  }

  // Evaluates guards and schedules. Without one, a guarded row does not fire
  // and every schedule is "now".
  void setHost(const ExpressionHost *host);

  // Called for every row that fired, was queued, or was cancelled. For a firing
  // the binding is complete: matched vertices first, then the ones the THEN
  // created. This is the narrator's feed.
  using FireObserver = std::function<void(const RewriteEvent &event)>;

  void setFireObserver(FireObserver observer);

  const std::vector<ScheduledRewrite> &pending() const
  {
    return queue;
  }

  // The pieces a matcher that applies rewrites ELSEWHERE (the GPU) needs from
  // this class: the clause evaluation, and the queue. `guardsHold` asks the
  // rule's guards about a row (a guard may act - book the church - so ask
  // only for rows otherwise certain to fire); `resolveBinds` gives the THEN's
  // bound vertices their ids; `scheduleDeferred` queues every insertion that
  // carries a schedule clause, with the WHEN edges the rule kept, and tells
  // the observer. Returns the groups queued.
  bool guardsHold(uint32_t rule, const std::vector<uint32_t> &vertices) const;
  void resolveBinds(uint32_t rule, std::vector<uint32_t> &bound) const;
  uint32_t scheduleDeferred(uint32_t rule, const std::vector<uint32_t> &bound, uint32_t rowIndex, RewriteStats &stats);

  // Tells the observer a row fired now with this binding - for a device that
  // applied the rewrite itself and only reports it.
  void notifyApplied(uint32_t rule, const std::vector<uint32_t> &bound, uint32_t rowIndex) const;

private:
  using EdgeKey = std::tuple<uint32_t, EdgeTypeId, uint32_t>;

  bool evaluateGuards(const compiler::RuleRewrite &rewrite, uint32_t rule, const std::vector<uint32_t> &vertices) const;
  uint32_t scheduleOf(const compiler::PatternEdge &edge, uint32_t rule, const std::vector<uint32_t> &bound) const;

  void removeConcrete(const std::vector<ConcreteEdge> &edges, RewriteStats &stats);
  void insertConcrete(const std::vector<ConcreteEdge> &edges, RewriteStats &stats);
  void notify(uint32_t rule, const std::vector<uint32_t> &bound, uint32_t rowIndex, RewriteEventKind kind, uint32_t time) const;

  const compiler::CompiledProgram &program;
  EdgePageStore &store;
  uint32_t seed;
  uint32_t round = 0u;
  uint32_t currentTime = 0u;
  uint32_t nextSequence = 0u;
  const ExpressionHost *host = nullptr;
  ChanceScale chanceScale;
  FireObserver fireObserver;
  std::vector<ScheduledRewrite> queue;
};

} // namespace graphrewrite::runtime
