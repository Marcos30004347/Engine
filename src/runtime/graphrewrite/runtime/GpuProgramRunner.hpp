#pragma once

// The GPU runner owns its own render graph, its own passes and its own buffers,
// and is driven by tick(). One tick is one run of that graph.
//
// It owns the graph rather than borrowing one because a pass records its command
// buffer ONCE, at registration: the dispatch set has to be fixed by the runner,
// and sharing a graph with unrelated passes would make that fragile. Everything
// per-frame - which instruction, which table, how many rows - travels in the
// params buffer instead.

#include "graphrewrite/compiler/UsageValues.hpp"
#include "graphrewrite/rendering/GraphMatchPass.hpp"
#include "graphrewrite/rendering/GraphRewritePass.hpp"
#include "graphrewrite/runtime/IProgramRunner.hpp"
#include "graphrewrite/runtime/StreamingManager.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>

namespace graphrewrite::runtime {

// Default GPU page-pool budget. A page is 256 edges of two ids, so this is
// about a million slots: enough that a working set of a few million edges is
// resident outright and the streaming manager never has to choose a victim.
//
// The pool is the one dial that decides whether this system scales. Undersized,
// every tick re-requests pages the last tick evicted and the run costs two
// orders of magnitude more; sized for the working set, the bookkeeping below
// never runs at all.
inline constexpr uint64_t defaultPoolBudgetBytes = 2ull << 30u;

struct GpuRunnerSettings
{
  uint32_t slotCount = static_cast<uint32_t>(defaultPoolBudgetBytes / (pageEdges * sizeof(Edge)));
  uint32_t maxRows = 4096u;

  // Words of the shared table buffer. ZERO sizes it from the programme:
  // widest columns x rowCapacityPerTable x (peak live registers + 1), which
  // is exactly what the region free list can need. A non-zero value is used
  // as given.
  uint32_t tableWords = 1u << 20u;
  uint32_t maxPagesPerPartition = 4096u;
  uint32_t rowCapacityPerTable = 2048u;
  uint32_t maxRegisters = 256u;
  uint32_t deltaCapacity = 65536u;
  uint32_t dirtyCapacity = 4096u;

  // Records per rule application the CPU reads back (candidates for a rule
  // with clauses; fired rows for the narrator), the claim table size (a power
  // of two), and how many vertices a THEN may create (the row-control stride
  // is one more than this).
  uint32_t candidateCapacity = 65536u;
  uint32_t firedCapacity = 65536u;
  uint32_t claimEntries = 1u << 22u;
  uint32_t maxFreshVertices = 4u;

  // Per-pass GPU timestamps. Instrumentation, so off by default - turning it on
  // changes the recorded graph, which is why it cannot be toggled after
  // createResources().
  bool enableTimers = false;

  // Copy the whole table buffer back every tick so readRegister() works. Off by
  // default: the buffer is as large as every table put together, and copying it
  // costs more than the match it is meant to observe.
  bool enableTableReadback = false;
  uint32_t edgesPerThread = 8u;

  // Seed for the per-row rewrite dice. Fixed by default so a run is
  // reproducible: a probabilistic rewrite still has to be debuggable, and the
  // CPU/GPU agreement checks depend on both runners rolling the same numbers.
  uint32_t diceSeed = 0x9e3779b9u;

  // Keep a FrameTiming per frame (under a hundred bytes per frame).
  bool recordFrames = true;
};

// One GPU frame: a match instruction, or a rewrite phase of one rule. The
// story's "tick" is one of these.
struct FrameTiming
{
  uint32_t frame = 0u;
  uint32_t kind = 0u; // 0 match, 1 rewrite candidates (phase 1), 2 rewrite apply
  uint32_t instruction = 0u;
  uint32_t rule = ~0u;
  uint32_t rows = 0u;
  double wallMs = 0.0;    // run + wait, on the host
  double gpuMs = 0.0;     // every pass, from the graph's timestamps (timers on)
  double matchGpuMs = 0.0;
  double rewriteGpuMs = 0.0;
};

struct GpuRewriteStats
{
  uint32_t rulesApplied = 0u;
  uint32_t edgesRemoved = 0u;
  uint32_t edgesInserted = 0u;
  uint32_t pagesCompacted = 0u;
  uint32_t verticesCreated = 0u;
  uint32_t rowsFired = 0u;
  uint32_t claimsRejected = 0u;
  uint32_t declined = 0u;
  uint32_t stale = 0u;
  bool overflowed = false;

  // Which buffer ran out, as a bitfield: 1 dirty pages, 2 delta, 4
  // candidates, 8 fired records, 16 the match table.
  uint32_t overflowKinds = 0u;
};

// One row the GPU judged live by dice and staleness, handed to the CPU so it
// can evaluate the rule's clauses (phase 1 of a two-phase apply).
struct CandidateRow
{
  uint32_t row = 0u;
  std::vector<uint32_t> vertices; // the rule's matched vertices, in rule order
};

// One row that fired, as the kernel recorded it: matched vertices first, then
// the ones the THEN created. This is the narrator's feed from the GPU.
struct FiredRecord
{
  uint32_t rule = 0u;
  uint32_t row = 0u;
  std::vector<uint32_t> bound;
};

struct RuleApplyOptions
{
  // Chance in thousandths, or ~0 for the rule's own.
  uint32_t chance = ~0u;

  // Insertions (by index into RuleRewrite::insertions) the GPU must NOT emit,
  // because a schedule clause hands them to the CPU queue.
  std::vector<bool> deferredInsertions;

  // When set, the apply runs in two phases: the GPU gathers the live rows,
  // `decide` fills one control word per row (1 fire / 0 not) followed by the
  // id of each vertex a bind clause named (0 = let the GPU allocate), and the
  // GPU applies that verdict. The control vector is rowCount * (1 + maxFresh).
  std::function<void(const std::vector<CandidateRow> &candidates, std::vector<uint32_t> &control)> decide;
};

struct RegisterTable
{
  uint32_t wordBase = 0u;
  uint32_t rows = 0u;
  uint32_t columns = 0u;
  uint32_t boundMask = 0u;
  bool overflowed = false;
};

class GpuProgramRunner : public IProgramRunner
{
public:
  GpuProgramRunner(::rendering::RHI *rhi, const compiler::CompiledProgram &program, const compiler::UsageValues &usage,
                   EdgePageStore &store, GpuRunnerSettings settings);

  ~GpuProgramRunner() override;

  // Builds the graph, passes and buffers. Must be called before the first tick,
  // and after the store has been seeded - the page metadata is snapshotted here.
  void createResources();

  void seed(const std::vector<Fact> &facts) override;
  void setMatchCallback(MatchCallback callback) override;
  void reset() override;
  TickResult tick() override;
  bool complete() const override;
  RuleResult resultForRule(uint32_t rule) const override;
  const RunnerStats &stats() const override;

  // Per-pass GPU times from the last tick, when timers are enabled.
  const std::vector<::rendering::RenderGraph::PassTimingSummary> &passTimings() const;
  PlanCheck checkPlan() const override;
  std::vector<Fact> snapshot() const override;

  const RegisterTable *tableForRegister(uint32_t reg) const;

  // Applies every matched rule's rewrite on the GPU, then folds the result back
  // into the CPU store: the compacted pages are read back, and the delta the
  // kernel emitted is merged into the sorted runs here, because a split is
  // directory surgery a kernel should not attempt.
  GpuRewriteStats applyRewrites();

  // One rule, with options; the fired records land in `fired` when given.
  // applyRewrites() is this over every rule with default options.
  GpuRewriteStats applyRule(uint32_t rule, const RuleApplyOptions &options, std::vector<FiredRecord> *fired = nullptr);

  // The CPU store changed outside a GPU rewrite (a scheduled insertion landed,
  // say): rebuild the page metadata and drop residency so the next match
  // streams the pages afresh.
  void resyncStore();

  // Call before changing the CPU store outside applyRule (the scheduled
  // insertions landing at the start of a week); resyncStore() then keeps
  // every page that did not change resident.
  void snapshotResidency();

  // By default a register holding some rule's FINAL table keeps its region
  // until the match is over, because applyRewrites() reads it afterwards. A
  // caller that applies each rule from the match callback - the moment its
  // table completes - can let FREE release those too, which bounds the table
  // buffer by the live registers rather than by every rule's final table.
  void setPinFinalTables(bool pin)
  {
    pinFinalTables = pin;
  }

  // Rows (over every rule) the last apply saw in each state.
  uint32_t claimGeneration() const
  {
    return claimGen;
  }

  const std::vector<FrameTiming> &frames() const
  {
    return frameTimings;
  }

  void clearFrames()
  {
    frameTimings.clear();
  }

  GpuPagePool &pool();
  StreamingManager &streaming();
  ::rendering::RenderGraph &renderGraph();

  // Reads a finished register's rows back. For checking against the CPU runner.
  std::vector<uint32_t> readRegister(uint32_t reg) const;

private:
  void beginInstruction(uint32_t index);
  void notifyCompletedRules(uint32_t index);
  void serviceRequests();
  uint32_t partitionIndexOf(const Partition &partition) const;

  ::rendering::RHI *rhi = nullptr;
  const compiler::CompiledProgram &program;
  const compiler::UsageValues &usage;
  EdgePageStore &store;
  GpuRunnerSettings settings;

  std::unique_ptr<::rendering::RenderGraph> graph;
  std::unique_ptr<GpuPagePool> pagePool;
  std::unique_ptr<StreamingManager> streamingManager;

  graphrewrite::rendering::GraphMatchPass *matchPass = nullptr;
  graphrewrite::rendering::GraphRewritePass *rewritePass = nullptr;

  ::rendering::Buffer statusStaging;
  ::rendering::Buffer requestStaging;
  ::rendering::Buffer tableStaging;
  ::rendering::Buffer countersStaging;
  uint32_t rewriteRound = 0u; // applyRewrites calls so far; each rolls fresh dice
  uint32_t claimGen = 0u;     // folded into every claim; wraps at 256 with a table clear

  void rebuildResidency();
  std::vector<GpuPagePool::ResidentPage> residencyBefore;
  void runRewriteFrame(uint32_t kind, uint32_t rule, uint32_t rows);
  void recordFrame(uint32_t kind, uint32_t instruction, uint32_t rule, uint32_t rows, double wallMs);
  std::vector<FrameTiming> frameTimings;
  void readCounters(graphrewrite::rendering::RewriteCounters &counters);

  // Table regions: a FREE'd register's region is reused by the next
  // instruction, except a register that holds some rule's FINAL table, which
  // applyRule reads after the whole match has run.
  std::vector<uint32_t> freeRegions;
  std::set<uint32_t> pinnedRegisters;
  bool pinFinalTables = true;

  // Columns a register table of this thread needs: one past the highest column
  // any of the thread's instructions reads or writes. Tables are in MERGED
  // columns shared by every rule of the thread; sized by one rule's own column
  // count, a table overflowed into the next register's words.
  uint32_t columnsForThread(uint32_t thread) const;

  ::rendering::Buffer dirtyStaging;
  ::rendering::Buffer deltaStaging;
  ::rendering::Buffer poolStaging;
  ::rendering::Buffer candidatesStaging;
  ::rendering::Buffer firedStaging;

  // Readback copies are disabled during matching and enabled only around a
  // rewrite. Matching produces nothing the CPU needs except a status word.
  ::rendering::gpgpu::CopyBufferPass *copyTablesPass = nullptr;
  ::rendering::gpgpu::CopyBufferPass *copyDirtyPass = nullptr;
  ::rendering::gpgpu::CopyBufferPass *copyDeltaPass = nullptr;
  ::rendering::gpgpu::CopyBufferPass *copyPoolPass = nullptr;
  ::rendering::gpgpu::CopyBufferPass *copyCandidatesPass = nullptr;
  ::rendering::gpgpu::CopyBufferPass *copyFiredPass = nullptr;

  // Which readback copies a rewrite frame carries. Every copy is the whole
  // buffer, recorded once, so a frame copies only what it produces: phase 1
  // the candidates, an apply frame the fired records and the fold.
  void setRewriteReadbackEnabled(bool candidates, bool fold);

  std::map<uint32_t, RegisterTable> registers;

  uint32_t cursor = 0u;
  uint32_t nextRegion = 0u;
  uint32_t regionWords = 0u;

  bool instructionStarted = false;
  uint32_t producedRows = 0u;
  uint32_t plannedRowsThisInstruction = 0u; // rows the instruction will iterate; zero completes without a frame
  graphrewrite::rendering::MatchParams params;

  std::vector<uint32_t> zeroRequests;
  std::vector<uint32_t> zeroRowState;

  MatchCallback matchCallback;
  RunnerStats runnerStats;
};

} // namespace graphrewrite::runtime
