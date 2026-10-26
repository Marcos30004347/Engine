#pragma once

// One instruction of a match, as five recorded dispatches.
//
// A pass records ONCE, at registration, so every dispatch here is fixed and the
// per-frame behaviour comes from the params buffer. That is also why the row
// count is a bound rather than an exact size: the dispatch covers `maxRows` and
// each kernel returns early past `params.read_rows`.

#include "graphrewrite/runtime/GpuPagePool.hpp"
#include "os/File.hpp"
#include "rendering/gpgpu/ConditionalCommand.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <string>

namespace graphrewrite::rendering {

// Mirrors MatchParams in graph-match.wgsl.
struct MatchParams
{
  uint32_t partition = 0u;
  uint32_t direction = 1u;
  uint32_t fromColumn = 0u;
  uint32_t writeColumn = 1u;
  // Registers, not row counts: the counts themselves live in a GPU buffer so
  // the dispatch sizes can be computed there and read by cmdDispatchIndirect.
  uint32_t readRegister = 0u;
  uint32_t writeRegister = 0u;
  uint32_t readStride = 0u;  // the stride the input table was WRITTEN at
  uint32_t readCols = 0u;
  uint32_t writeCols = 0u;
  uint32_t writeCapacity = 0u;
  uint32_t firstFrame = 1u;
  uint32_t pageEdges = runtime::pageEdges;

  // One bit per column that already holds a vertex. Unbound columns are still
  // zero, and letting them take part in the injectivity test rejects every
  // match whose new vertex happens to be vertex 0.
  uint32_t boundMask = 0u;

  // Word offsets into the single table buffer. Bindings are fixed when a pass
  // records, so addressing every table out of one buffer is what lets one
  // recorded pass serve a whole programme.
  // 0 = Select (seed from a partition), 1 = Where (extend the read table).
  uint32_t mode = 1u;

  uint32_t blockSumsBase = 0u;  // word offset of the scan's block sums in row_state
  uint32_t readBase = 0u;
  uint32_t writeWordBase = 0u;  // word offset of this table in the shared buffer
  uint32_t pad0 = 0u;
  uint32_t pad1 = 0u;
};

struct MatchStatus
{
  uint32_t incompleteRows = 0u;
  uint32_t stalledRows = 0u;
  uint32_t writtenRows = 0u;
  uint32_t overflowed = 0u;

  // Candidate edges this batch - the size the edge-parallel stage was
  // dispatched at.
  uint32_t candidateTotal = 0u;
};

class GraphMatchPass : public ::rendering::Pass
{
public:
  struct Settings
  {
    runtime::GpuPagePool *pool = nullptr;
    uint32_t maxRows = 4096u;
    uint32_t tableWords = 1u << 16u;

    // Recorded once at registration, so it must cover the largest partition any
    // Select will scan.
    uint32_t maxPagesPerPartition = 4096u;

    // One row counter per register.
    uint32_t maxRegisters = 256u;

    // Candidate edges per thread. A module is built per value; 1 is one thread
    // per edge, higher values amortise the per-thread setup over a block.
    uint32_t edgesPerThread = 8u;
  };

  explicit GraphMatchPass(Settings settings) : settings_(settings)
  {
  }

  ~GraphMatchPass() override
  {
    renderGraph->deleteComputePipeline(resetPipeline_);
    renderGraph->deleteComputePipeline(finalizePipeline_);
    renderGraph->deleteComputePipeline(seedPipeline_);
    renderGraph->deleteComputePipeline(planPipeline_);
    renderGraph->deleteComputePipeline(scanBlocksPipeline_);
    renderGraph->deleteComputePipeline(scanBlockSumsPipeline_);
    renderGraph->deleteComputePipeline(scanAddPipeline_);
    renderGraph->deleteComputePipeline(edgeArgsPipeline_);
    renderGraph->deleteComputePipeline(matchEdgesPipeline_);
    renderGraph->deleteComputePipeline(advancePipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(paramsBuffer_);
    renderGraph->deleteBuffer(readTable_);
    renderGraph->deleteBuffer(writeTable_);
    renderGraph->deleteBuffer(rowState_);
    renderGraph->deleteBuffer(statusBuffer_);
    renderGraph->deleteBuffer(tableRows_);
    renderGraph->deleteBuffer(dispatchArgs_);
  }

  void createResources();

  // Makes the recorded match stages inert for a tick that is doing something
  // else - a rewrite, say - in the same graph. Both passes are recorded once, so
  // "not running" has to be expressed in the params rather than by omission.
  // Whether this pass's dispatches run this frame at all. Off on a rewrite
  // frame: its commands are prepared no-ops and the graph skips them.
  void setActive(const bool active)
  {
    *active_ = active;
  }

  void setIdle(const uint32_t emptyRegister)
  {
    MatchParams idle;

    idle.mode = 1u;
    idle.readRegister = emptyRegister;
    idle.writeRegister = emptyRegister;
    idle.firstFrame = 0u;

    setParams(idle);
  }

  void setParams(const MatchParams &params)
  {
    renderGraph->bufferWrite(paramsBuffer_, 0u, sizeof(MatchParams), const_cast<MatchParams *>(&params));
  }

  const ::rendering::Buffer &readTable() const
  {
    return readTable_;
  }

  const ::rendering::Buffer &writeTable() const
  {
    return writeTable_;
  }

  const ::rendering::Buffer &rowState() const
  {
    return rowState_;
  }

  const ::rendering::Buffer &statusBuffer() const
  {
    return statusBuffer_;
  }

  const ::rendering::Buffer &tableRowsBuffer() const
  {
    return tableRows_;
  }

  uint64_t tableRowsBytes() const
  {
    return static_cast<uint64_t>(settings_.maxRegisters) * sizeof(uint32_t);
  }

  uint64_t tableBytes() const
  {
    return static_cast<uint64_t>(settings_.tableWords) * sizeof(uint32_t);
  }

  // Per-row state, then the scan's block sums.
  uint32_t blockSumsBase() const
  {
    return settings_.maxRows * 8u;
  }

  uint64_t rowStateBytes() const
  {
    const uint64_t blocks = (static_cast<uint64_t>(settings_.maxRows) + 255u) / 256u + 1u;

    return (static_cast<uint64_t>(blockSumsBase()) + blocks) * sizeof(uint32_t);
  }

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
  ::rendering::Buffer readTable_;
  ::rendering::Buffer writeTable_;
  ::rendering::Buffer rowState_;
  ::rendering::Buffer statusBuffer_;
  ::rendering::Buffer tableRows_;
  ::rendering::Buffer dispatchArgs_;

  ::rendering::BindingsLayout layout_;
  ::rendering::BindingGroups bindingGroups_;
  ::rendering::Shader shader_;

  ::rendering::ComputePipeline resetPipeline_;
  ::rendering::ComputePipeline finalizePipeline_;
  ::rendering::ComputePipeline seedPipeline_;
  ::rendering::ComputePipeline planPipeline_;
  ::rendering::ComputePipeline scanBlocksPipeline_;
  ::rendering::ComputePipeline scanBlockSumsPipeline_;
  ::rendering::ComputePipeline scanAddPipeline_;
  ::rendering::ComputePipeline edgeArgsPipeline_;
  ::rendering::ComputePipeline matchEdgesPipeline_;
  ::rendering::ComputePipeline advancePipeline_;
};

inline ::rendering::ComputePipeline GraphMatchPass::makePipeline(const char *entry)
{
  return renderGraph->createComputePipeline(::rendering::ComputePipelineInfo{
      .entry = entry,
      .layout = layout_,
      .name = passName + "_" + entry,
      .shader = shader_,
  });
}

inline void GraphMatchPass::createResources()
{
  using namespace ::rendering;

  if (readTable_.isValid())
  {
    return;
  }

  paramsBuffer_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Params.buffer",
      .size = std::max<uint64_t>(sizeof(MatchParams), renderGraph->getRHI()->GetProperties().uniformBufferAlignment),
      .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
  });

  readTable_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_ReadTable.buffer",
      .size = tableBytes(),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });

  // One buffer for every table. Registers are word offsets into it.
  writeTable_ = readTable_;

  rowState_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_RowState.buffer",
      .size = rowStateBytes(),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });

  statusBuffer_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Status.buffer",
      .size = sizeof(MatchStatus),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });

  tableRows_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_TableRows.buffer",
      .size = tableRowsBytes(),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
  });

  // Six u32: the seed dispatch at byte 0, the per-row dispatch at byte 12.
  dispatchArgs_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_DispatchArgs.buffer",
      .size = 9u * sizeof(uint32_t),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect,
  });

  const auto storage = [](const char *name, const uint32_t binding) {
    return BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .type = BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
    };
  };

  layout_ = renderGraph->createBindingsLayout(BindingsLayoutInfo{
      .name = passName + "_Layout",
      .groups =
          {
              BindingGroupLayout{
                  .buffers =
                      {
                          BindingGroupLayoutBufferEntry{
                              .name = "params",
                              .binding = 0,
                              .type = BufferBindingType::BufferBindingType_UniformBuffer,
                              .visibility = BindingVisibility::BindingVisibility_Compute,
                              .isDynamic = false},
                          storage("page_meta", 1u),
                          storage("part_first", 2u),
                          storage("page_pool", 3u),
                          storage("page_request", 4u),
                      },
              },
              BindingGroupLayout{
                  .buffers =
                      {
                          storage("read_table", 0u),
                          storage("write_table", 1u),
                          storage("row_state", 2u),
                          storage("status", 3u),
                          storage("table_rows", 4u),
                          storage("dispatch_args", 5u),
                      },
              },
          },
  });

  shader_ = renderGraph->createShader(ShaderInfo{
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile("assets/shaders/spirv/graphrewrite-match-ept" + std::to_string(settings_.edgesPerThread) +
                                      ".spirv"),
      .type = ShaderType::SpirV,
  });

  resetPipeline_ = makePipeline("reset_status");
  finalizePipeline_ = makePipeline("finalize");
  seedPipeline_ = makePipeline("seed");
  planPipeline_ = makePipeline("plan");
  scanBlocksPipeline_ = makePipeline("scan_blocks");
  scanBlockSumsPipeline_ = makePipeline("scan_block_sums");
  scanAddPipeline_ = makePipeline("scan_add_offsets");
  edgeArgsPipeline_ = makePipeline("fill_edge_args");
  matchEdgesPipeline_ = makePipeline("match_edges");
  advancePipeline_ = makePipeline("advance");
}

inline void GraphMatchPass::recordCommandBuffer(::rendering::CommandRecorder &commandBuffer)
{
  const size_t firstCommand = commandBuffer.recorded.back().commands.size();

  using namespace ::rendering;

  createResources();

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
                          {.binding = 0, .bufferView = view(paramsBuffer_, sizeof(MatchParams), AccessPattern::SHADER_READ)},
                          {.binding = 1,
                           .bufferView = view(pool.metadataBuffer(), sizeof(runtime::PageMeta) * std::max<size_t>(1u, pool.totalPages()),
                                              AccessPattern::SHADER_READ)},
                          {.binding = 2,
                           .bufferView = view(pool.partitionFirstBuffer(), sizeof(uint32_t) * std::max<size_t>(1u, pool.partitionFirst().size()),
                                              AccessPattern::SHADER_READ)},
                          {.binding = 3,
                           .bufferView = view(pool.pagesBuffer(), pool.pageBytes() * pool.settings().slotCount, AccessPattern::SHADER_READ)},
                          {.binding = 4, .bufferView = view(pool.requestBuffer(), pool.requestBytes(), AccessPattern::SHADER_WRITE)},
                      },
              },
              GroupInfo{
                  .name = "tables",
                  .buffers =
                      {
                          {.binding = 0, .bufferView = view(readTable_, tableBytes(), AccessPattern::SHADER_READ)},
                          {.binding = 1, .bufferView = view(writeTable_, tableBytes(), AccessPattern::SHADER_WRITE)},
                          {.binding = 2, .bufferView = view(rowState_, rowStateBytes(), AccessPattern::SHADER_WRITE)},
                          {.binding = 3, .bufferView = view(statusBuffer_, sizeof(MatchStatus), AccessPattern::SHADER_WRITE)},
                          {.binding = 4, .bufferView = view(tableRows_, tableRowsBytes(), AccessPattern::SHADER_WRITE)},
                          {.binding = 5, .bufferView = view(dispatchArgs_, 9u * sizeof(uint32_t), AccessPattern::SHADER_WRITE)},
                      },
              },
          },
  });

  // A barrier between stages IS a graph node here, and the stages genuinely
  // depend on each other: count reads what plan wrote, scan reads count, write
  // reads scan.
  const auto direct = [&](const ComputePipeline &pipeline, const uint32_t groups) {
    commandBuffer.cmdBindComputePipeline(pipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(groups, 1u, 1u);
    commandBuffer.cmdMemoryBarrier();
  };

  // Indirect: the group count comes from dispatch_args, which fill_dispatch_args
  // wrote from row counts that live on the GPU. A table with three rows launches
  // one workgroup instead of the worst case, and - more to the point - the CPU
  // never has to read a row count back in order to size the next dispatch.
  const auto indirect = [&](const ComputePipeline &pipeline, const uint64_t offset) {
    commandBuffer.cmdBindComputePipeline(pipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatchIndirect(dispatchArgs_, offset);
    commandBuffer.cmdMemoryBarrier();
  };

  constexpr uint64_t seedArgs = 0u;
  constexpr uint64_t rowArgs = 3u * sizeof(uint32_t);
  constexpr uint64_t edgeArgs = 6u * sizeof(uint32_t);

  // reset_status also sizes the seed/row dispatches - the two were separate
  // single-thread kernels with a barrier between them, which is two encoder
  // round trips for a few microseconds of bookkeeping.
  //
  // The args it writes are consumed by cmdDispatchIndirect below. A default
  // compute->compute barrier covers shader reads but NOT the indirect-command
  // read of those very bytes, so it is named explicitly.
  commandBuffer.cmdBindComputePipeline(resetPipeline_);
  commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
  commandBuffer.cmdDispatch(1u, 1u, 1u);
  commandBuffer.cmdMemoryBarrier(PipelineStage::COMPUTE_SHADER, PipelineStage::COMPUTE_SHADER, AccessPattern::SHADER_WRITE,
                                 AccessPattern::INDIRECT_COMMAND_READ);

  // Both a Select (seed) and a Where (plan/count/scan/write) are recorded. In
  // the wrong mode fill_dispatch_args writes zero groups, so the unused half
  // costs a dispatch of nothing rather than a pass over the worst case.
  indirect(seedPipeline_, seedArgs);
  // plan counts as well: see the shader. One dispatch, one barrier.
  indirect(planPipeline_, rowArgs);

  // The scan is three stages: per-block scans in parallel, a serial pass over
  // the BLOCK totals, then a per-row add. Only the middle one is serial, and it
  // is over blocks rather than rows.
  indirect(scanBlocksPipeline_, rowArgs);
  direct(scanBlockSumsPipeline_, 1u);
  indirect(scanAddPipeline_, rowArgs);

  // The candidate total is only known after the scan, so the edge stage's
  // dispatch size is computed here and consumed immediately below. An
  // indirect-command read needs naming explicitly - the default barrier does not
  // cover it.
  commandBuffer.cmdBindComputePipeline(edgeArgsPipeline_);
  commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
  commandBuffer.cmdDispatch(1u, 1u, 1u);
  commandBuffer.cmdMemoryBarrier(PipelineStage::COMPUTE_SHADER, PipelineStage::COMPUTE_SHADER, AccessPattern::SHADER_WRITE,
                                 AccessPattern::INDIRECT_COMMAND_READ);

  // One thread per candidate EDGE. This is the stage that does the actual
  // matching, and it is O(1) per thread.
  indirect(matchEdgesPipeline_, edgeArgs);

  indirect(advancePipeline_, rowArgs);

  direct(finalizePipeline_, 1u);

  ::rendering::gpgpu::guardRecordedCommands(commandBuffer, firstCommand, active_);
}

} // namespace graphrewrite::rendering
