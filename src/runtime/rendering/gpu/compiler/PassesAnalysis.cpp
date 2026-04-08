#include "RenderGraphAnalyses.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/commands/Commands.hpp"
#include "os/Logger.hpp"
#include <map>
#include <stdexcept>

#define RENDER_GRAPH_FATAL(...)                                                                                                                                                                                            \
  do                                                                                                                                                                                                                       \
  {                                                                                                                                                                                                                        \
    os::Logger::errorf(__VA_ARGS__);                                                                                                                                                                                       \
    exit(1);                                                                                                                                                                                                               \
  } while (0)

namespace rendering
{

const char *PassesAnalysis::name() const
{
  return "analysePasses";
}

void PassesAnalysis::run(RenderGraphCompiler &, RenderGraph &renderGraph) const
{
  auto &resources = renderGraph.resources;

  auto registerNodeCommandResources = [&](const CommandPtr &nodeCmd, uint32_t id, Queue currentQueue, std::string &pipelineName)
  {
#define FIND_OR_THROW(map, key, msg, it)                                                                                                                                                                                   \
  auto it = map.find(key);                                                                                                                                                                                                 \
  if (it == map.end())                                                                                                                                                                                                     \
    throw std::runtime_error(msg);

    if (auto *beginRenderPass = dynamic_cast<BeginRenderPassCommand *>(nodeCmd.get()))
    {
      for (auto &att : beginRenderPass->info.colorAttachments)
      {
        FIND_OR_THROW(resources.textureMetadatas, att.view.texture.name, "Texture not found", it);
        it->second.usages.push_back({att.view, id, currentQueue});
      }
      if (beginRenderPass->info.depthStencilAttachment.enabled)
      {
        auto &att = beginRenderPass->info.depthStencilAttachment;
        FIND_OR_THROW(resources.textureMetadatas, att.view.texture.name, "Texture not found", it);
        it->second.usages.push_back({att.view, id, currentQueue});
      }
      return;
    }

    if (auto *copyBuffer = dynamic_cast<CopyBufferCommand *>(nodeCmd.get()))
    {
      auto src = copyBuffer->src;
      auto dst = copyBuffer->dst;

      FIND_OR_THROW(resources.bufferMetadatas, src.buffer.name, "[RHI] Source buffer not found", srcMeta);
      FIND_OR_THROW(resources.bufferMetadatas, dst.buffer.name, "[RHI] Dest buffer not found", dstMeta);

      const BufferInfo &srcInfo = srcMeta->second.bufferInfo;
      const BufferInfo &dstInfo = dstMeta->second.bufferInfo;

      if (src.size == 0)
        RENDER_GRAPH_FATAL("[RHI] Copy size zero");
      if (src.buffer == dst.buffer)
        RENDER_GRAPH_FATAL("[RHI] Src and Dst same");
      if (src.offset + src.size > srcInfo.size)
        RENDER_GRAPH_FATAL("[RHI] Src overflow");
      if (dst.offset + dst.size > dstInfo.size)
        RENDER_GRAPH_FATAL("[RHI] Dst overflow");
      if (src.size != dst.size)
        RENDER_GRAPH_FATAL("[RHI] Size mismatch");
      if (!(srcInfo.usage & BufferUsage_CopySrc))
        RENDER_GRAPH_FATAL("[RHI] Missing CopySrc");
      if (!(dstInfo.usage & BufferUsage_CopyDst))
        RENDER_GRAPH_FATAL("[RHI] Missing CopyDst");

      srcMeta->second.usages.push_back({src, id, currentQueue});
      dstMeta->second.usages.push_back({dst, id, currentQueue});
      return;
    }

    if (auto *copyImage = dynamic_cast<CopyImageCommand *>(nodeCmd.get()))
    {
      auto src = copyImage->src;
      auto dst = copyImage->dst;

      FIND_OR_THROW(resources.textureMetadatas, src.texture.name, "[RHI] Source texture not found", srcMeta);
      FIND_OR_THROW(resources.textureMetadatas, dst.texture.name, "[RHI] Dest texture not found", dstMeta);

      const TextureInfo &srcInfo = srcMeta->second.textureInfo;
      const TextureInfo &dstInfo = dstMeta->second.textureInfo;

      if (src.texture == dst.texture)
        RENDER_GRAPH_FATAL("[RHI] Source and destination textures are the same");
      if (!(srcInfo.usage & ImageUsage_TransferSrc))
        RENDER_GRAPH_FATAL("[RHI] Missing ImageUsage_TransferSrc");
      if (!(dstInfo.usage & ImageUsage_TransferDst))
        RENDER_GRAPH_FATAL("[RHI] Missing ImageUsage_TransferDst");

      srcMeta->second.usages.push_back({src, id, currentQueue});
      dstMeta->second.usages.push_back({dst, id, currentQueue});
      return;
    }

    if (auto *bindGroups = dynamic_cast<BindBindingGroupsCommand *>(nodeCmd.get()))
    {
      auto &bgName = bindGroups->groups.name;
      FIND_OR_THROW(resources.bindingGroupsMetadata, bgName, "Binding Groups not found", bgIt);

      auto layoutName = bgIt->second.groupsInfo.layout.name;
      FIND_OR_THROW(resources.bindingsLayoutMetadata, layoutName, "Layout not found", lIt);
      lIt->second.usages.push_back({id, currentQueue});

      bgIt->second.usages.push_back({id, currentQueue});

      for (auto &group : bgIt->second.groupsInfo.groups)
      {
        for (auto &buffer : group.buffers)
        {
          FIND_OR_THROW(resources.bufferMetadatas, buffer.bufferView.buffer.name, "Buffer not found", bIt);
          bIt->second.usages.push_back({buffer.bufferView, id, currentQueue});
        }
        for (auto &texture : group.textures)
        {
          FIND_OR_THROW(resources.textureMetadatas, texture.textureView.texture.name, "Texture not found", tIt);
          tIt->second.usages.push_back({texture.textureView, id, currentQueue});
        }
        for (auto &texture : group.storageTextures)
        {
          FIND_OR_THROW(resources.textureMetadatas, texture.textureView.texture.name, "Storage Texture not found", tIt);
          tIt->second.usages.push_back({texture.textureView, id, currentQueue});
        }
        for (auto &sampler : group.samplers)
        {
          FIND_OR_THROW(resources.textureMetadatas, sampler.view.texture.name, "Sampler Texture not found", tIt);
          tIt->second.usages.push_back({sampler.view, id, currentQueue});

          FIND_OR_THROW(resources.samplerMetadatas, sampler.sampler.name, "Sampler not found", sIt);
          sIt->second.usages.push_back({sampler.sampler, id, currentQueue});
        }
      }
      return;
    }

    if (auto *bindVertex = dynamic_cast<BindVertexBufferCommand *>(nodeCmd.get()))
    {
      auto &buf = bindVertex->buffer;
      FIND_OR_THROW(resources.bufferMetadatas, buf.buffer.name, "Vertex Buffer not found", it);
      it->second.usages.push_back({buf, id, currentQueue});
      return;
    }

    if (auto *bindIndex = dynamic_cast<BindIndexBufferCommand *>(nodeCmd.get()))
    {
      auto &buf = bindIndex->buffer;
      FIND_OR_THROW(resources.bufferMetadatas, buf.buffer.name, "Index Buffer not found", it);
      it->second.usages.push_back({buf, id, currentQueue});
      return;
    }

    if (auto *bindCompute = dynamic_cast<BindComputePipelineCommand *>(nodeCmd.get()))
    {
      FIND_OR_THROW(resources.computePipelineMetadata, bindCompute->pipeline.name, "Compute Pipeline not found", it);
      pipelineName = it->second.pipelineInfo.name;
      it->second.usages.push_back({id, currentQueue});
      return;
    }

    if (auto *bindGraphics = dynamic_cast<BindGraphicsPipelineCommand *>(nodeCmd.get()))
    {
      FIND_OR_THROW(resources.graphicsPipelineMetadata, bindGraphics->pipeline.name, "Graphics Pipeline not found", it);
      pipelineName = it->second.pipelineInfo.name;
      it->second.usages.push_back({id, currentQueue});
      return;
    }

    if (auto *drawIndexedIndirect = dynamic_cast<DrawIndexedIndirectCommand *>(nodeCmd.get()))
    {
      auto &buf = drawIndexedIndirect->buffer;
      FIND_OR_THROW(resources.bufferMetadatas, buf.buffer.name, "Indirect Buffer not found", it);
      it->second.usages.push_back({buf, id, currentQueue});
      return;
    }

    if (auto *drawIndirect = dynamic_cast<DrawIndirectCommand *>(nodeCmd.get()))
    {
      auto &buf = drawIndirect->buffer;
      FIND_OR_THROW(resources.bufferMetadatas, buf.buffer.name, "Indirect Buffer not found", it);
      it->second.usages.push_back({buf, id, currentQueue});
      return;
    }

    if (auto *drawIndirectCount = dynamic_cast<DrawIndirectCountCommand *>(nodeCmd.get()))
    {
      auto &buf = drawIndirectCount->indirectBuffer;
      FIND_OR_THROW(resources.bufferMetadatas, buf.buffer.name, "Indirect Buffer not found", indirectIt);
      indirectIt->second.usages.push_back({buf, id, currentQueue});

      auto &countBuf = drawIndirectCount->countBuffer;
      FIND_OR_THROW(resources.bufferMetadatas, countBuf.buffer.name, "Count Buffer not found", counterIt);
      counterIt->second.usages.push_back({countBuf, id, currentQueue});
      return;
    }

    if (auto *dispatchIndirect = dynamic_cast<DispatchIndirectCommand *>(nodeCmd.get()))
    {
      FIND_OR_THROW(resources.bufferMetadatas, dispatchIndirect->indirectBuffer.name, "Dispatch indirect buffer not found", it);
      const uint64_t remainingBytes = it->second.bufferInfo.size > dispatchIndirect->offset ? (it->second.bufferInfo.size - dispatchIndirect->offset) : 0u;
      const uint64_t usageSize = std::min<uint64_t>(remainingBytes, 3u * sizeof(uint32_t));
      if (usageSize == 0u)
      {
        RENDER_GRAPH_FATAL("[RenderGraph] DispatchIndirect offset out of bounds for buffer '%s'", dispatchIndirect->indirectBuffer.name.c_str());
      }

      it->second.usages.push_back(
          {BufferView{
               .buffer = dispatchIndirect->indirectBuffer,
               .offset = dispatchIndirect->offset,
               .size = usageSize,
               .access = AccessPattern::INDIRECT_COMMAND_READ,
           },
           id,
           currentQueue});
      return;
    }

#undef FIND_OR_THROW
  };

  RenderGraph::RenderGraphPass pass;

  RenderGraph::RenderGraphNode currentNode;
  currentNode.commands.reserve(128);

  while (renderGraph.passes.dequeue(pass))
  {
    uint32_t dispatchId = renderGraph.nodes.size();
    uint32_t subIndex = 0;

    for (auto &recordedCommands : pass.cmd.recorded)
    {
      enum class ActivePipeline
      {
        None,
        Graphics,
        Compute,
      };

      struct CarriedState
      {
        ActivePipeline activePipeline = ActivePipeline::None;
        CommandPtr graphicsPipeline;
        CommandPtr computePipeline;
        CommandPtr graphicsBindings;
        CommandPtr computeBindings;
        std::map<uint32_t, CommandPtr> vertexBuffers;
        CommandPtr indexBuffer;
      };

      CarriedState carriedState;
      Queue currentQueue = Queue::None;
      ActivePipeline currentNodeActivePipeline = ActivePipeline::None;

      bool hasGraphicsPipeline = false;
      bool hasComputePipeline = false;
      bool hasBindings = false;
      bool hasDraw = false;
      bool hasDispatch = false;
      std::map<uint32_t, CommandPtr> boundVertexBuffers;
      bool hasIndexBuffer = false;
      bool manualTimerActive = false;
      Timer activeManualTimer{};

      auto instrumentNodeTimers = [&](RenderGraph::RenderGraphNode &node)
      {
        if ((renderGraph.getRHI()->getFeatures() & DeviceFeatures_Timestamp) == 0)
        {
          return;
        }

        bool hasTimedGraphicsWork = false;
        bool hasTimedComputeWork = false;

        for (const auto &nodeCmd : node.commands)
        {
          if (nodeCmd->isDrawCommand())
          {
            hasTimedGraphicsWork = true;
          }
          if (nodeCmd->isDispatchCommand())
          {
            hasTimedComputeWork = true;
          }
        }

        if (!hasTimedGraphicsWork && !hasTimedComputeWork)
        {
          hasTimedGraphicsWork = node.queue == Queue::Graphics;
          hasTimedComputeWork = node.queue == Queue::Compute;
        }

        const std::string timerName = node.name + ".node." + std::to_string(node.id);

        node.timers.push_back(
            RenderGraph::RenderGraphNode::TimerBinding{
              .name = timerName,
              .info =
                  TimerInfo{
                    .name = timerName,
                    .unit = TimerUnit::Miliseconds,
                    .sampleCount = 1u,
                  },
              .startStage = PipelineStage::TOP_OF_PIPE,
              .stopStage = PipelineStage::BOTTOM_OF_PIPE,
              .hasComputeWork = hasTimedComputeWork,
              .hasGraphicsWork = hasTimedGraphicsWork,
            });
      };

      auto appendCommand = [&](const CommandPtr &cmd, bool updateCarriedState)
      {
        Queue queueHint = cmd->queueHint();
        if (queueHint != Queue::None)
        {
          currentQueue = queueHint;
        }

        currentNode.commands.push_back(cmd);

        if (auto *bindGraphics = dynamic_cast<BindGraphicsPipelineCommand *>(cmd.get()))
        {
          hasGraphicsPipeline = true;
          currentNodeActivePipeline = ActivePipeline::Graphics;
          if (updateCarriedState)
          {
            carriedState.graphicsPipeline = bindGraphics->clone();
            carriedState.activePipeline = ActivePipeline::Graphics;
          }
          return;
        }

        if (auto *bindCompute = dynamic_cast<BindComputePipelineCommand *>(cmd.get()))
        {
          hasComputePipeline = true;
          currentNodeActivePipeline = ActivePipeline::Compute;
          if (updateCarriedState)
          {
            carriedState.computePipeline = bindCompute->clone();
            carriedState.activePipeline = ActivePipeline::Compute;
          }
          return;
        }

        if (auto *bindGroups = dynamic_cast<BindBindingGroupsCommand *>(cmd.get()))
        {
          hasBindings = true;
          if (updateCarriedState)
          {
            if (currentNodeActivePipeline == ActivePipeline::Graphics)
            {
              carriedState.graphicsBindings = bindGroups->clone();
            }
            else if (currentNodeActivePipeline == ActivePipeline::Compute)
            {
              carriedState.computeBindings = bindGroups->clone();
            }
          }
          return;
        }

        if (auto *bindVertex = dynamic_cast<BindVertexBufferCommand *>(cmd.get()))
        {
          boundVertexBuffers[bindVertex->slot] = bindVertex->clone();
          if (updateCarriedState)
          {
            carriedState.vertexBuffers[bindVertex->slot] = bindVertex->clone();
          }
          return;
        }

        if (dynamic_cast<BindIndexBufferCommand *>(cmd.get()) != nullptr)
        {
          hasIndexBuffer = true;
          if (updateCarriedState)
          {
            carriedState.indexBuffer = cmd->clone();
          }
          return;
        }

        if (cmd->isDrawCommand())
        {
          hasDraw = true;
          return;
        }

        if (cmd->isDispatchCommand())
        {
          hasDispatch = true;
          return;
        }
      };

      auto ensureGraphicsState = [&](bool needBindings, bool needVertexBuffers, bool needIndexBuffer)
      {
        if (!hasGraphicsPipeline && carriedState.graphicsPipeline != nullptr)
        {
          appendCommand(carriedState.graphicsPipeline->clone(), false);
        }
        if (needBindings && !hasBindings && carriedState.graphicsBindings != nullptr)
        {
          appendCommand(carriedState.graphicsBindings->clone(), false);
        }
        if (needVertexBuffers)
        {
          for (const auto &[slot, command] : carriedState.vertexBuffers)
          {
            if (boundVertexBuffers.find(slot) == boundVertexBuffers.end())
            {
              appendCommand(command->clone(), false);
            }
          }
        }
        if (needIndexBuffer && !hasIndexBuffer && carriedState.indexBuffer != nullptr)
        {
          appendCommand(carriedState.indexBuffer->clone(), false);
        }
      };

      auto ensureComputeState = [&](bool needBindings)
      {
        if (!hasComputePipeline && carriedState.computePipeline != nullptr)
        {
          appendCommand(carriedState.computePipeline->clone(), false);
        }
        if (needBindings && !hasBindings && carriedState.computeBindings != nullptr)
        {
          appendCommand(carriedState.computeBindings->clone(), false);
        }
      };

      auto finalizeCurrentNode = [&]()
      {
        if (currentNode.commands.empty())
        {
          return;
        }

        if (hasDraw && !hasGraphicsPipeline)
        {
          RENDER_GRAPH_FATAL("[RenderGraph] Invalid graphics submission: Draw without Pipeline");
        }
        if (hasDispatch && !hasComputePipeline)
        {
          RENDER_GRAPH_FATAL("[RenderGraph] Invalid compute submission: Dispatch without Pipeline %s", pass.name.c_str());
        }
        if (currentQueue == Queue::None)
        {
          RENDER_GRAPH_FATAL("[RenderGraph] %s is not submitted to any queue", pass.name.c_str());
        }

        uint32_t id = renderGraph.nodes.size();
        currentNode.dispatchId = dispatchId;
        currentNode.id = id;
        currentNode.level = 0;
        currentNode.priority = id;
        currentNode.queue = currentQueue;

        std::string pipelineName;

        for (const auto &nodeCmd : currentNode.commands)
        {
          registerNodeCommandResources(nodeCmd, id, currentQueue, pipelineName);
        }
        currentNode.name = pass.name + "_" + pipelineName + "_" + std::to_string(subIndex++);
        instrumentNodeTimers(currentNode);

        renderGraph.nodes.emplace_back(std::move(currentNode));

        currentNode = RenderGraph::RenderGraphNode();
        currentNode.commands.reserve(128);

        hasGraphicsPipeline = false;
        hasComputePipeline = false;
        hasBindings = false;
        hasDraw = false;
        hasDispatch = false;
        hasIndexBuffer = false;
        boundVertexBuffers.clear();
        currentNodeActivePipeline = ActivePipeline::None;
        currentQueue = Queue::None;
      };

      for (auto &cmd : recordedCommands.commands)
      {
        const bool isStartTimerCommand = dynamic_cast<StartTimerCommand *>(cmd.get()) != nullptr;
        const bool isStopTimerCommand = dynamic_cast<StopTimerCommand *>(cmd.get()) != nullptr;

        if (isStartTimerCommand && !currentNode.commands.empty())
        {
          finalizeCurrentNode();
        }

        Queue queueHint = cmd->queueHint();
        if (!currentNode.commands.empty() && queueHint != Queue::None && currentQueue != Queue::None && queueHint != currentQueue)
        {
          if (manualTimerActive)
          {
            RENDER_GRAPH_FATAL("[RenderGraph] Timer '%s' in pass '%s' spans multiple queue nodes", activeManualTimer.name.c_str(), pass.name.c_str());
          }
          finalizeCurrentNode();
        }

        bool triggerSplit = cmd->triggersNodeSplit();
        if (manualTimerActive && triggerSplit)
        {
          triggerSplit = false;
        }
        if (isStopTimerCommand)
        {
          triggerSplit = true;
        }
        if (dynamic_cast<BeginRenderPassCommand *>(cmd.get()) != nullptr)
        {
          ensureGraphicsState(false, false, false);
        }
        if (dynamic_cast<BindBindingGroupsCommand *>(cmd.get()) != nullptr)
        {
          if (currentNodeActivePipeline == ActivePipeline::Graphics || (currentNodeActivePipeline == ActivePipeline::None && carriedState.activePipeline == ActivePipeline::Graphics))
          {
            ensureGraphicsState(false, false, false);
          }
          else if (currentNodeActivePipeline == ActivePipeline::Compute || (currentNodeActivePipeline == ActivePipeline::None && carriedState.activePipeline == ActivePipeline::Compute))
          {
            ensureComputeState(false);
          }
        }
        if (cmd->isDispatchCommand())
        {
          ensureComputeState(true);
        }
        if (cmd->isDrawCommand())
        {
          ensureGraphicsState(true, true, true);
        }

        appendCommand(cmd, true);

        if (isStartTimerCommand)
        {
          manualTimerActive = true;
          activeManualTimer = static_cast<StartTimerCommand *>(cmd.get())->timer;
        }
        else if (isStopTimerCommand)
        {
          manualTimerActive = false;
          activeManualTimer = {};
        }

        if (triggerSplit)
        {
          finalizeCurrentNode();
        }
      }

      if (manualTimerActive)
      {
        RENDER_GRAPH_FATAL("[RenderGraph] Timer '%s' in pass '%s' is missing a stop command", activeManualTimer.name.c_str(), pass.name.c_str());
      }

      finalizeCurrentNode();
    }
  }
}

} // namespace rendering
