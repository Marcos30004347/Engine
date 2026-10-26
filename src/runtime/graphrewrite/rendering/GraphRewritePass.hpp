#pragma once

// Applies one rule's rewrite on the GPU.
//
// Recorded once, like every pass: the stages are fixed and which rule they act
// on is decided per frame by the params buffer. The page compaction is an
// INDIRECT dispatch off the dirty-page list, so pages nothing touched are never
// visited.

#include "graphrewrite/runtime/GpuPagePool.hpp"
#include "os/File.hpp"
#include "rendering/gpgpu/ConditionalCommand.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>

namespace graphrewrite::rendering {

// Mirrors RuleEdgeDesc in graph-rewrite.wgsl.
//
// The columns are TABLE columns - the merged graph's - not the rule's own vertex
// numbers. A vertex the THEN creates has no column; it is named by `fresh`
// instead: bits 0-15 for the origin, 16-31 for the target, each 0 for "read the
// column" or k+1 for "the row's k-th new vertex". Before this the shader took
// any column >= matched_vertices to mean a new vertex, which was also exactly
// what a matched vertex mapped to a later column looked like.
struct RuleEdgeDesc
{
  uint32_t originColumn = 0u;
  uint32_t targetColumn = 0u;
  uint32_t partition = 0u;
  uint32_t fresh = 0u;
};

inline uint32_t freshCode(const uint32_t originFresh, const uint32_t targetFresh)
{
  return (originFresh & 0xffffu) | (targetFresh << 16u);
}

struct DeltaEdge
{
  uint32_t partition = 0u;
  uint32_t origin = 0u;
  uint32_t dest = 0u;
  uint32_t pad = 0u;
};

struct RewriteParams
{
  uint32_t rowCount = 0u;
  uint32_t tableWordBase = 0u;
  uint32_t tableStride = 0u;
  uint32_t removalCount = 0u;
  uint32_t insertionCount = 0u;
  uint32_t removalBase = 0u;
  uint32_t insertionBase = 0u;
  uint32_t matchedVertices = 0u;
  uint32_t totalVertices = 0u;
  uint32_t deltaCapacity = 0u;
  uint32_t dirtyCapacity = 0u;
  uint32_t vertexBase = 0u;
  uint32_t dirtyBase = 0u;
  uint32_t rowBase = 0u;
  uint32_t seed = 0u;
  uint32_t chance = 1000u; // thousandths; 1000 is ALWAYS
  uint32_t phase = 0u;     // 0 apply; 1 gather candidates only; 2 apply the CPU's verdict
  uint32_t claimGen = 0u;
  uint32_t ruleId = 0u;
  uint32_t ctrlStride = 1u;
  uint32_t claimMask = 0u;
  uint32_t candidateCapacity = 0u;
  uint32_t firedCapacity = 0u;
  uint32_t pad0 = 0u;
};

// Fixed-size records the kernels exchange with the CPU: a count word at [0],
// then `recordWords` u32 per record. A candidate is (row, matched, v[0..]); a
// fired record is (rule, row, matched, total, v[0..]) with the created
// vertices after the matched ones.
inline constexpr uint32_t recordWords = 16u;
inline constexpr uint32_t recordVertices = 12u;

// Bit 31 of RuleEdgeDesc::partition: an insertion the CPU queue lands later.
inline constexpr uint32_t deferredEdgeBit = 0x80000000u;

struct RewriteCounters
{
  uint32_t deltaCount = 0u;
  uint32_t dirtyCount = 0u;
  uint32_t vertexCount = 0u;
  uint32_t removed = 0u;
  uint32_t overflow = 0u;
  uint32_t declined = 0u;
  uint32_t stale = 0u;
  uint32_t rejected = 0u;   // rows that lost a claim
  uint32_t fired = 0u;      // rows that fired
  uint32_t candidates = 0u; // records gathered in phase 1
};

class GraphRewritePass : public ::rendering::Pass
{
public:
  struct Settings
  {
    runtime::GpuPagePool *pool = nullptr;
    ::rendering::Buffer tableBuffer;
    uint64_t tableBytes = 0u;

    uint32_t maxRuleEdges = 512u;
    uint32_t deltaCapacity = 65536u;
    uint32_t dirtyCapacity = 4096u;
    uint32_t maxRows = 4096u;

    // Records per rule application the CPU can read back, and the claim
    // table (a power of two; 1<<22 entries is 16 MB).
    uint32_t candidateCapacity = 65536u;
    uint32_t firedCapacity = 65536u;
    uint32_t claimEntries = 1u << 22u;

    // Words per row of the control buffer: one verdict word, then one id per
    // vertex a THEN may create (a bind names it; zero means "allocate").
    uint32_t ctrlStride = 5u;
  };

  explicit GraphRewritePass(Settings settings) : settings_(settings)
  {
  }

  ~GraphRewritePass() override
  {
    for (::rendering::ComputePipeline &pipeline :
         {std::ref(resetPipeline_), std::ref(removePipeline_), std::ref(insertPipeline_), std::ref(argsPipeline_),
          std::ref(compactPipeline_), std::ref(filterPipeline_), std::ref(gatherPipeline_), std::ref(claimPipeline_),
          std::ref(resolvePipeline_), std::ref(firedPipeline_), std::ref(clearClaimsPipeline_)})
    {
      renderGraph->deleteComputePipeline(pipeline);
    }

    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(paramsBuffer_);
    renderGraph->deleteBuffer(ruleEdges_);
    renderGraph->deleteBuffer(tombstone_);
    renderGraph->deleteBuffer(dirtyPages_);
    renderGraph->deleteBuffer(dirtyEdges_);
    renderGraph->deleteBuffer(deltaEdges_);
    renderGraph->deleteBuffer(counters_);
    renderGraph->deleteBuffer(sortArgs_);
    renderGraph->deleteBuffer(candidates_);
    renderGraph->deleteBuffer(rowCtrl_);
    renderGraph->deleteBuffer(fired_);
    renderGraph->deleteBuffer(claims_);
    renderGraph->deleteBuffer(rowOrdinal_);
    renderGraph->deleteBuffer(vertexColumns_);
  }

  void createResources();

  void setParams(const RewriteParams &params)
  {
    RewriteParams filled = params;

    // The tombstone buffer's region offsets are the pass's own business, not the
    // caller's. Leaving them to be passed in is what produced the overlap this
    // layout replaced: a caller that forgets them gets zero, every region lands
    // on top of the edge bits, and live edges quietly vanish.
    filled.dirtyBase = dirtyBase();
    filled.rowBase = rowBase();
    filled.ctrlStride = settings_.ctrlStride;
    filled.claimMask = settings_.claimEntries - 1u;
    filled.candidateCapacity = settings_.candidateCapacity;
    filled.firedCapacity = settings_.firedCapacity;

    renderGraph->bufferWrite(paramsBuffer_, 0u, sizeof(RewriteParams), &filled);
  }

  // Phase 2 reads the CPU's verdict per row: `words` is rowCount * ctrlStride.
  void uploadRowControl(const std::vector<uint32_t> &words)
  {
    if (!words.empty())
    {
      renderGraph->bufferWrite(rowCtrl_, 0u, std::min<uint64_t>(words.size() * sizeof(uint32_t), rowCtrlBytes()),
                               const_cast<uint32_t *>(words.data()));
    }
  }

  const ::rendering::Buffer &claimsBuffer() const
  {
    return claims_;
  }

  // The rule's vertex -> table column map, for the records the kernels write.
  void uploadVertexColumns(const std::vector<uint32_t> &columns)
  {
    std::vector<uint32_t> padded(maxRecordColumns, 0u);

    for (size_t i = 0u; i < columns.size() && i < padded.size(); ++i)
    {
      padded[i] = columns[i];
    }

    renderGraph->bufferWrite(vertexColumns_, 0u, padded.size() * sizeof(uint32_t), padded.data());
  }

  static constexpr uint32_t maxRecordColumns = 32u;

  const ::rendering::Buffer &candidatesBuffer() const
  {
    return candidates_;
  }

  const ::rendering::Buffer &firedBuffer() const
  {
    return fired_;
  }

  uint64_t candidatesBytes() const
  {
    return (1ull + static_cast<uint64_t>(settings_.candidateCapacity) * recordWords) * sizeof(uint32_t);
  }

  uint64_t firedBytes() const
  {
    return (1ull + static_cast<uint64_t>(settings_.firedCapacity) * recordWords) * sizeof(uint32_t);
  }

  uint64_t rowCtrlBytes() const
  {
    return static_cast<uint64_t>(settings_.maxRows) * settings_.ctrlStride * sizeof(uint32_t);
  }

  const Settings &settings() const
  {
    return settings_;
  }

  void uploadRuleEdges(const std::vector<RuleEdgeDesc> &edges)
  {
    if (edges.empty())
    {
      return;
    }

    renderGraph->bufferWrite(ruleEdges_, 0u, edges.size() * sizeof(RuleEdgeDesc), const_cast<RuleEdgeDesc *>(edges.data()));
  }

  // Tombstones and dirty flags are per rewrite round, not per rule.
  void clearMarks();

  // Whether this pass's dispatches run this frame at all. Off on a match
  // frame: its commands are prepared no-ops and the graph skips them.
  void setActive(const bool active)
  {
    *active_ = active;
  }

  const ::rendering::Buffer &countersBuffer() const
  {
    return counters_;
  }

  const ::rendering::Buffer &dirtyPagesBuffer() const
  {
    return dirtyPages_;
  }

  // Compacted survivors, one page slot per dirty index. Reading the first
  // dirtyCount pages of this is equivalent to reading the whole pool, and is
  // proportional to what the rewrite changed.
  const ::rendering::Buffer &dirtyEdgesBuffer() const
  {
    return dirtyEdges_;
  }

  uint64_t dirtyEdgeBytes() const
  {
    return static_cast<uint64_t>(settings_.dirtyCapacity) * runtime::pageEdges * sizeof(runtime::Edge);
  }

  const ::rendering::Buffer &deltaBuffer() const
  {
    return deltaEdges_;
  }

  uint64_t dirtyPagesBytes() const
  {
    return static_cast<uint64_t>(settings_.dirtyCapacity) * 2u * sizeof(uint32_t);
  }

  uint64_t deltaBytes() const
  {
    return static_cast<uint64_t>(settings_.deltaCapacity) * sizeof(DeltaEdge);
  }

  uint64_t tombstoneBytes() const;
  uint64_t edgeTombstoneWords() const;
  uint64_t dirtyFlagWords() const;
  uint64_t rowFlagWords() const;
  uint32_t dirtyBase() const;
  uint32_t rowBase() const;

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override;

private:
  static uint32_t divCeil(const uint32_t value, const uint32_t divisor)
  {
    return (value + divisor - 1u) / divisor;
  }

  ::rendering::ComputePipeline makePipeline(const char *entry);

  Settings settings_;
  std::shared_ptr<bool> active_ = std::make_shared<bool>(true);

  ::rendering::Buffer paramsBuffer_;
  ::rendering::Buffer ruleEdges_;
  ::rendering::Buffer tombstone_;
  ::rendering::Buffer dirtyPages_;
  ::rendering::Buffer dirtyEdges_;
  ::rendering::Buffer deltaEdges_;
  ::rendering::Buffer counters_;
  ::rendering::Buffer sortArgs_;
  ::rendering::Buffer candidates_;
  ::rendering::Buffer rowCtrl_;
  ::rendering::Buffer fired_;
  ::rendering::Buffer claims_;
  ::rendering::Buffer rowOrdinal_;
  ::rendering::Buffer vertexColumns_;

  ::rendering::BindingsLayout layout_;
  ::rendering::BindingGroups bindingGroups_;
  ::rendering::Shader shader_;

  ::rendering::ComputePipeline resetPipeline_;
  ::rendering::ComputePipeline removePipeline_;
  ::rendering::ComputePipeline insertPipeline_;
  ::rendering::ComputePipeline argsPipeline_;
  ::rendering::ComputePipeline compactPipeline_;
  ::rendering::ComputePipeline filterPipeline_;
  ::rendering::ComputePipeline gatherPipeline_;
  ::rendering::ComputePipeline claimPipeline_;
  ::rendering::ComputePipeline resolvePipeline_;
  ::rendering::ComputePipeline firedPipeline_;
  ::rendering::ComputePipeline clearClaimsPipeline_;
};

inline uint64_t GraphRewritePass::tombstoneBytes() const
{
  return (edgeTombstoneWords() + dirtyFlagWords() + rowFlagWords()) * sizeof(uint32_t);
}

// The three regions, laid out back to back. They are computed rather than
// implied because the implied version overlapped: the dirty flags used to start
// at `dirtyCapacity` words, which is smaller than the edge-bit region as soon as
// the pool passes 512 slots.
inline uint64_t GraphRewritePass::edgeTombstoneWords() const
{
  return divCeil(settings_.pool->settings().slotCount * runtime::pageEdges, 32u);
}

inline uint64_t GraphRewritePass::dirtyFlagWords() const
{
  // One bit per page in the pool's metadata, which is every page, resident or
  // not - mark_dirty is indexed by metadata index.
  return divCeil(settings_.pool->metadataCapacity(), 32u) + 64u;
}

inline uint64_t GraphRewritePass::rowFlagWords() const
{
  // One WORD per row, not one bit: filter_rows writes it and the two apply
  // kernels only read it, so there is nothing to make atomic and a plain store
  // is cheaper than a read-modify-write.
  return settings_.maxRows;
}

inline uint32_t GraphRewritePass::dirtyBase() const
{
  return static_cast<uint32_t>(edgeTombstoneWords());
}

inline uint32_t GraphRewritePass::rowBase() const
{
  return static_cast<uint32_t>(edgeTombstoneWords() + dirtyFlagWords());
}

inline ::rendering::ComputePipeline GraphRewritePass::makePipeline(const char *entry)
{
  return renderGraph->createComputePipeline(::rendering::ComputePipelineInfo{
      .entry = entry,
      .layout = layout_,
      .name = passName + "_" + entry,
      .shader = shader_,
  });
}

inline void GraphRewritePass::clearMarks()
{
  const std::vector<uint32_t> zeros(tombstoneBytes() / sizeof(uint32_t), 0u);

  renderGraph->bufferWrite(tombstone_, 0u, tombstoneBytes(), const_cast<uint32_t *>(zeros.data()));
}

inline void GraphRewritePass::createResources()
{
  using namespace ::rendering;

  if (counters_.isValid())
  {
    return;
  }

  const auto storage = [](const char *name, const uint32_t binding) {
    return BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .type = BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
    };
  };

  paramsBuffer_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Params.buffer",
      .size = std::max<uint64_t>(sizeof(RewriteParams), renderGraph->getRHI()->GetProperties().uniformBufferAlignment),
      .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
  });

  ruleEdges_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_RuleEdges.buffer",
      .size = static_cast<uint64_t>(settings_.maxRuleEdges) * sizeof(RuleEdgeDesc),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
  });

  tombstone_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Tombstone.buffer",
      .size = tombstoneBytes(),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
  });

  dirtyPages_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_DirtyPages.buffer",
      .size = dirtyPagesBytes(),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });

  dirtyEdges_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_DirtyEdges",
      .size = dirtyEdgeBytes(),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });

  deltaEdges_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Delta.buffer",
      .size = deltaBytes(),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });

  counters_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Counters.buffer",
      .size = sizeof(RewriteCounters),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });

  // Three dispatch-argument triples: [0] the compaction over dirty pages,
  // [3] the per-row kernels, [6] the per-edge kernels.
  sortArgs_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_SortArgs.buffer",
      .size = 9u * sizeof(uint32_t),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect,
  });

  candidates_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Candidates.buffer",
      .size = candidatesBytes(),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });

  rowCtrl_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_RowCtrl.buffer",
      .size = rowCtrlBytes(),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
  });

  fired_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Fired.buffer",
      .size = firedBytes(),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });

  claims_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Claims.buffer",
      .size = static_cast<uint64_t>(settings_.claimEntries) * sizeof(uint32_t),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
  });

  rowOrdinal_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_RowOrdinal.buffer",
      .size = static_cast<uint64_t>(settings_.maxRows) * sizeof(uint32_t),
      .usage = BufferUsage::BufferUsage_Storage,
  });

  vertexColumns_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_VertexColumns.buffer",
      .size = maxRecordColumns * sizeof(uint32_t),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
  });

  layout_ = renderGraph->createBindingsLayout(BindingsLayoutInfo{
      .name = passName + "_Layout",
      .groups =
          {
              BindingGroupLayout{
                  .buffers =
                      {
                          BindingGroupLayoutBufferEntry{.name = "params",
                                                        .binding = 0,
                                                        .type = BufferBindingType::BufferBindingType_UniformBuffer,
                                                        .visibility = BindingVisibility::BindingVisibility_Compute,
                                                        .isDynamic = false},
                          storage("page_meta", 1u),
                          storage("part_first", 2u),
                          storage("page_pool", 3u),
                          storage("tombstone", 4u),
                          storage("dirty_edges", 5u),
                      },
              },
              BindingGroupLayout{
                  .buffers =
                      {
                          storage("rule_edges", 0u),
                          storage("table", 1u),
                          storage("dirty_pages", 2u),
                          storage("delta_edges", 3u),
                          storage("counters", 4u),
                          storage("sort_args", 5u),
                      },
              },
              BindingGroupLayout{
                  .buffers =
                      {
                          storage("candidates", 0u),
                          storage("row_ctrl", 1u),
                          storage("fired", 2u),
                          storage("claims", 3u),
                          storage("row_ordinal", 4u),
                          storage("vertex_columns", 5u),
                      },
              },
          },
  });

  shader_ = renderGraph->createShader(ShaderInfo{
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile("assets/shaders/spirv/graphrewrite-rewrite.spirv"),
      .type = ShaderType::SpirV,
  });

  resetPipeline_ = makePipeline("reset_counters");
  removePipeline_ = makePipeline("mark_removals");
  insertPipeline_ = makePipeline("emit_insertions");
  argsPipeline_ = makePipeline("fill_sort_args");
  compactPipeline_ = makePipeline("compact_pages");
  filterPipeline_ = makePipeline("filter_rows");
  gatherPipeline_ = makePipeline("gather_candidates");
  claimPipeline_ = makePipeline("claim_removals");
  resolvePipeline_ = makePipeline("resolve_claims");
  firedPipeline_ = makePipeline("emit_fired");
  clearClaimsPipeline_ = makePipeline("clear_claims");

  // The claim table starts clean.
  {
    const std::vector<uint32_t> zeros(settings_.claimEntries, 0u);

    renderGraph->bufferWrite(claims_, 0u, zeros.size() * sizeof(uint32_t), const_cast<uint32_t *>(zeros.data()));
  }
}

inline void GraphRewritePass::recordCommandBuffer(::rendering::CommandRecorder &commandBuffer)
{
  using namespace ::rendering;

  createResources();

  const size_t firstCommand = commandBuffer.recorded.back().commands.size();

  runtime::GpuPagePool &pool = *settings_.pool;

  const auto view = [](const Buffer &buffer, const uint64_t size, const AccessPattern access) {
    return BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
  };

  bindingGroups_ = renderGraph->createBindingGroups(BindingGroupsInfo{
      .name = passName + "_Bindings",
      .layout = layout_,
      .groups =
          {
              GroupInfo{
                  .name = "pages",
                  .buffers =
                      {
                          {.binding = 0, .bufferView = view(paramsBuffer_, sizeof(RewriteParams), AccessPattern::SHADER_READ)},
                          {.binding = 1,
                           .bufferView = view(pool.metadataBuffer(), sizeof(runtime::PageMeta) * std::max<size_t>(1u, pool.totalPages()),
                                              AccessPattern::SHADER_READ)},
                          {.binding = 2,
                           .bufferView = view(pool.partitionFirstBuffer(),
                                              sizeof(uint32_t) * std::max<size_t>(1u, pool.partitionFirst().size()),
                                              AccessPattern::SHADER_READ)},
                          {.binding = 3,
                           .bufferView = view(pool.pagesBuffer(), pool.pageBytes() * pool.settings().slotCount,
                                              AccessPattern::SHADER_READ_WRITE)},
                          {.binding = 4, .bufferView = view(tombstone_, tombstoneBytes(), AccessPattern::SHADER_READ_WRITE)},
                          {.binding = 5, .bufferView = view(dirtyEdges_, dirtyEdgeBytes(), AccessPattern::SHADER_READ_WRITE)},
                      },
              },
              GroupInfo{
                  .name = "rewrite",
                  .buffers =
                      {
                          {.binding = 0,
                           .bufferView = view(ruleEdges_, static_cast<uint64_t>(settings_.maxRuleEdges) * sizeof(RuleEdgeDesc),
                                              AccessPattern::SHADER_READ)},
                          {.binding = 1, .bufferView = view(settings_.tableBuffer, settings_.tableBytes, AccessPattern::SHADER_READ)},
                          {.binding = 2, .bufferView = view(dirtyPages_, dirtyPagesBytes(), AccessPattern::SHADER_READ_WRITE)},
                          {.binding = 3, .bufferView = view(deltaEdges_, deltaBytes(), AccessPattern::SHADER_READ_WRITE)},
                          {.binding = 4, .bufferView = view(counters_, sizeof(RewriteCounters), AccessPattern::SHADER_READ_WRITE)},
                          {.binding = 5, .bufferView = view(sortArgs_, 9u * sizeof(uint32_t), AccessPattern::SHADER_READ_WRITE)},
                      },
              },
              GroupInfo{
                  .name = "phases",
                  .buffers =
                      {
                          {.binding = 0, .bufferView = view(candidates_, candidatesBytes(), AccessPattern::SHADER_READ_WRITE)},
                          {.binding = 1, .bufferView = view(rowCtrl_, rowCtrlBytes(), AccessPattern::SHADER_READ)},
                          {.binding = 2, .bufferView = view(fired_, firedBytes(), AccessPattern::SHADER_READ_WRITE)},
                          {.binding = 3,
                           .bufferView = view(claims_, static_cast<uint64_t>(settings_.claimEntries) * sizeof(uint32_t),
                                              AccessPattern::SHADER_READ_WRITE)},
                          {.binding = 4,
                           .bufferView = view(rowOrdinal_, static_cast<uint64_t>(settings_.maxRows) * sizeof(uint32_t),
                                              AccessPattern::SHADER_READ_WRITE)},
                          {.binding = 5,
                           .bufferView = view(vertexColumns_, maxRecordColumns * sizeof(uint32_t), AccessPattern::SHADER_READ)},
                      },
              },
          },
  });

  const auto direct = [&](const ComputePipeline &pipeline, const uint32_t groups) {
    commandBuffer.cmdBindComputePipeline(pipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(groups, 1u, 1u);
    commandBuffer.cmdMemoryBarrier();
  };

  // Indirect off the argument triples reset_counters wrote from the row
  // count. Recorded once like everything else, so a frame that applies no
  // rule - every match frame - launches NOTHING here, where sizing these by
  // the row capacity launched hundreds of thousands of empty workgroups per
  // frame at a capacity of millions.
  const auto indirect = [&](const ComputePipeline &pipeline, const uint64_t offset) {
    commandBuffer.cmdBindComputePipeline(pipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatchIndirect(sortArgs_, offset);
    commandBuffer.cmdMemoryBarrier();
  };

  constexpr uint64_t rowArgs = 3u * sizeof(uint32_t);
  constexpr uint64_t edgeArgs = 6u * sizeof(uint32_t);

  commandBuffer.cmdBindComputePipeline(resetPipeline_);
  commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
  commandBuffer.cmdDispatch(1u, 1u, 1u);
  commandBuffer.cmdMemoryBarrier(PipelineStage::COMPUTE_SHADER, PipelineStage::COMPUTE_SHADER, AccessPattern::SHADER_WRITE,
                                 AccessPattern::INDIRECT_COMMAND_READ);
  commandBuffer.cmdMemoryBarrier();

  // The filter decides, for every row at once, whether it fires - dice and
  // staleness both - before a single edge is tombstoned. Running it as its own
  // pass is the point: once removals start, a row can no longer distinguish an
  // edge a PREVIOUS rewrite took from one a sibling row in this same batch just
  // took, and only the first of those should stop it.
  indirect(filterPipeline_, rowArgs);

  // Phase 1 stops here in effect: every later kernel returns at once when the
  // params say so. Phase 0 and 2 claim, resolve, then apply.
  indirect(gatherPipeline_, rowArgs);
  indirect(claimPipeline_, edgeArgs);
  indirect(resolvePipeline_, rowArgs);
  indirect(removePipeline_, edgeArgs);
  indirect(insertPipeline_, edgeArgs);
  indirect(firedPipeline_, rowArgs);

  commandBuffer.cmdBindComputePipeline(argsPipeline_);
  commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
  commandBuffer.cmdDispatch(1u, 1u, 1u);

  // The dirty count was just written and is about to be read as dispatch
  // arguments; the default barrier does not cover an indirect-command read.
  commandBuffer.cmdMemoryBarrier(PipelineStage::COMPUTE_SHADER, PipelineStage::COMPUTE_SHADER, AccessPattern::SHADER_WRITE,
                                 AccessPattern::INDIRECT_COMMAND_READ);

  commandBuffer.cmdBindComputePipeline(compactPipeline_);
  commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
  commandBuffer.cmdDispatchIndirect(sortArgs_, 0u);
  commandBuffer.cmdMemoryBarrier();

  ::rendering::gpgpu::guardRecordedCommands(commandBuffer, firstCommand, active_);
}

} // namespace graphrewrite::rendering
