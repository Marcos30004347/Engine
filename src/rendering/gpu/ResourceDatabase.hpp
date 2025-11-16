// #pragma once

// #include "algorithm/crc32.hpp"
// #include "datastructure/ConcurrentBoundedDictionary.hpp"
// #include "rhi/Device.hpp"

// namespace rendering
// {

// class ResourceDatabase
// {
// private:
//   std::vector<rhi::Buffer> buffers;
//   std::vector<rhi::BufferView> bufferViews;
//   std::vector<rhi::Sampler> samplers;
//   std::vector<rhi::Texture> textures;
//   std::vector<rhi::GraphicsPipeline> graphicsPipeline;
//   std::vector<rhi::ComputePipeline> computePipeline;
//   std::vector<rhi::TextureView> textureViews;
//   std::vector<rhi::Shader> shaders;
//   std::vector<rhi::BindingGroups> bindingGroups;

//   std::atomic<uint32_t> buffersCount;
//   std::atomic<uint32_t> buffersViewsCount;
//   std::atomic<uint32_t> samplersCount;
//   std::atomic<uint32_t> texturesCount;
//   std::atomic<uint32_t> graphicsPipelineCount;
//   std::atomic<uint32_t> computePipelineCount;
//   std::atomic<uint32_t> textureViewsCount;
//   std::atomic<uint32_t> shadersCount;
//   std::atomic<uint32_t> bindingGroupsCount;

//   lib::ConcurrentBoundedDictionary<std::string, uint32_t> bufferNameToIndex;
//   lib::ConcurrentBoundedDictionary<std::string, uint32_t> bufferViewNameToIndex;
//   lib::ConcurrentBoundedDictionary<std::string, uint32_t> samplerNameToIndex;
//   lib::ConcurrentBoundedDictionary<std::string, uint32_t> textureNameToIndex;
//   lib::ConcurrentBoundedDictionary<std::string, uint32_t> textureViewNameToIndex;
//   lib::ConcurrentBoundedDictionary<std::string, uint32_t> graphicsPipelineNameToIndex;
//   lib::ConcurrentBoundedDictionary<std::string, uint32_t> computePipelineNameToIndex;
//   lib::ConcurrentBoundedDictionary<std::string, uint32_t> shaderNameToIndex;
//   lib::ConcurrentBoundedDictionary<std::string, uint32_t> bindingGroupsNameToIndex;

//   lib::ConcurrentBoundedDictionary<rhi::BufferInfo, uint32_t> bufferInfoToIndex;
//   lib::ConcurrentBoundedDictionary<rhi::BufferView, uint32_t> bufferViewToIndex;
//   lib::ConcurrentBoundedDictionary<rhi::TextureInfo, uint32_t> textureInfoToIndex;
//   lib::ConcurrentBoundedDictionary<rhi::TextureViewInfo, uint32_t> textureViewInfoToIndex;
//   lib::ConcurrentBoundedDictionary<rhi::SamplerInfo, uint32_t> samplerInfoToIndex;
//   lib::ConcurrentBoundedDictionary<rhi::ComputePipelineInfo, uint32_t> computePipelineInfoToIndex;
//   lib::ConcurrentBoundedDictionary<rhi::GraphicsPipelineInfo, uint32_t> graphicsPipelineInfoToIndex;
//   lib::ConcurrentBoundedDictionary<rhi::ShaderInfo, uint32_t> shaderInfoToIndex;
//   lib::ConcurrentBoundedDictionary<rhi::BindingGroupsInfo, uint32_t> bindingGroupsInfoToIndex;

//   rhi::Device *device;

// public:
//   inline ResourceDatabase(rhi::Device *device, size_t capacity)
//       : buffers(capacity), samplers(capacity), textures(capacity), textureViews(capacity), computePipeline(capacity), graphicsPipeline(capacity), bufferViews(capacity),
//         bindingGroups(capacity), shaders(capacity), bufferInfoToIndex(capacity), textureInfoToIndex(capacity), textureViewInfoToIndex(capacity), samplerInfoToIndex(capacity),
//         computePipelineInfoToIndex(capacity), graphicsPipelineInfoToIndex(capacity), bufferViewToIndex(capacity), shaderInfoToIndex(capacity), bindingGroupsInfoToIndex(capacity),
//         buffersCount(0), samplersCount(0), texturesCount(0), graphicsPipelineCount(0), computePipelineCount(0), textureViewsCount(0), bindingGroupsCount(0), shadersCount(0),
//         buffersViewsCount(0), bufferNameToIndex(capacity), bufferViewNameToIndex(capacity), samplerNameToIndex(capacity), textureNameToIndex(capacity),
//         textureViewNameToIndex(capacity), graphicsPipelineNameToIndex(capacity), computePipelineNameToIndex(capacity), shaderNameToIndex(capacity),
//         bindingGroupsNameToIndex(capacity)
//   {

//     this->device = device;
//   }

//   inline bool insert(const rhi::Buffer &resource)
//   {
//     uint32_t index = buffersCount.fetch_add(1);

//     if (!bufferNameToIndex.insert(resource.getInfo().name, index))
//     {
//       return false;
//     }

//     if (!bufferInfoToIndex.insert(resource.getInfo(), index))
//     {
//       throw std::runtime_error("Invalid insert on database");
//     }

//     buffers[index] = std::move(resource);
//     return true;
//   }

//   inline bool insert(const rhi::BufferView &resource)
//   {
//     uint32_t index = buffersViewsCount.fetch_add(1);
//     if (!bufferViewNameToIndex.insert(resource.name, index))
//     {
//       return false;
//     }

//     if (!bufferViewToIndex.insert(resource, index))
//     {
//       throw std::runtime_error("Invalid insert on database");
//     }

//     bufferViews[index] = std::move(resource);
//     return true;
//   }

//   inline bool insert(const rhi::Sampler &resource)
//   {
//     uint32_t index = samplersCount.fetch_add(1);

//     if (!samplerNameToIndex.insert(resource.getInfo().name, index))
//     {
//       return false;
//     }

//     if (!samplerInfoToIndex.insert(resource.getInfo(), index))
//     {
//       throw std::runtime_error("Invalid insert on database");
//     }

//     samplers[index] = std::move(resource);
//     return true;
//   }

//   inline bool insert(const rhi::Texture &resource)
//   {
//     uint32_t index = texturesCount.fetch_add(1);

//     if (!textureNameToIndex.insert(resource.getInfo().name, index))
//     {
//       return false;
//     }

//     if (!textureInfoToIndex.insert(resource.getInfo(), index))
//     {
//       throw std::runtime_error("Invalid insert on database");
//     }

//     textures[index] = std::move(resource);
//     return true;
//   }

//   inline bool insert(const rhi::ComputePipeline &resource)
//   {
//     uint32_t index = computePipelineCount.fetch_add(1);

//     if (!computePipelineNameToIndex.insert(resource.getInfo().name, index))
//     {
//       return false;
//     }

//     if (!computePipelineInfoToIndex.insert(resource.getInfo(), index))
//     {
//       throw std::runtime_error("Invalid insert on database");
//     }

//     computePipeline[index] = std::move(resource);
//     return true;
//   }

//   inline bool insert(const rhi::GraphicsPipeline &resource)
//   {
//     uint32_t index = graphicsPipelineCount.fetch_add(1);

//     if (!graphicsPipelineNameToIndex.insert(resource.getInfo().name, index))
//     {
//       return false;
//     }

//     if (!graphicsPipelineInfoToIndex.insert(resource.getInfo(), index))
//     {
//       throw std::runtime_error("Invalid insert on database");
//     }

//     graphicsPipeline[index] = std::move(resource);
//     return true;
//   }

//   inline bool insert(const rhi::TextureView &resource)
//   {
//     uint32_t index = textureViewsCount.fetch_add(1);

//     if (!textureViewNameToIndex.insert(resource.getInfo().name, index))
//     {
//       return false;
//     }

//     if (!textureViewInfoToIndex.insert(resource.getInfo(), index))
//     {
//       throw std::runtime_error("Invalid insert on database");
//     }

//     textureViews[index] = std::move(resource);
//     return true;
//   }

//   inline bool insert(const rhi::BindingGroups &resource)
//   {
//     uint32_t index = bindingGroupsCount.fetch_add(1);

//     if (!bindingGroupsNameToIndex.insert(resource.getInfo().name, index))
//     {
//       return false;
//     }

//     if (!bindingGroupsInfoToIndex.insert(resource.getInfo(), index))
//     {
//       throw std::runtime_error("Invalid insert on database");
//     }

//     bindingGroups[index] = std::move(resource);
//     return true;
//   }

//   inline bool insert(const rhi::Shader &resource)
//   {
//     uint32_t index = shadersCount.fetch_add(1);

//     if (!shaderNameToIndex.insert(resource.getInfo().name, index))
//     {
//       return false;
//     }

//     if (!shaderInfoToIndex.insert(resource.getInfo(), index))
//     {
//       throw std::runtime_error("Invalid insert on database");
//     }

//     shaders[index] = std::move(resource);
//     return true;
//   }

//   inline rhi::Buffer &getBuffer(const std::string &id)
//   {
//     return buffers[bufferNameToIndex.get(id)];
//   }
//   inline rhi::Buffer &getBuffer(const uint32_t id)
//   {
//     return buffers[id];
//   }

//   inline rhi::BufferView &getBufferView(const std::string &id)
//   {
//     return bufferViews[bufferViewNameToIndex.get(id)];
//   }
//   inline rhi::BufferView &getBufferView(const uint32_t id)
//   {
//     return bufferViews[id];
//   }

//   inline rhi::Sampler &getSampler(const std::string &id)
//   {
//     return samplers[samplerNameToIndex.get(id)];
//   }
//   inline rhi::Sampler &getSampler(const uint32_t id)
//   {
//     return samplers[id];
//   }

//   inline rhi::Texture &getTexture(const std::string &id)
//   {
//     return textures[textureNameToIndex.get(id)];
//   }
//   inline rhi::Texture &getTexture(const uint32_t id)
//   {
//     return textures[id];
//   }

//   inline rhi::TextureView &getTextureView(const std::string &id)
//   {
//     return textureViews[textureViewNameToIndex.get(id)];
//   }
//   inline rhi::TextureView &getTextureView(const uint32_t id)
//   {
//     return textureViews[id];
//   }

//   inline rhi::GraphicsPipeline &getGraphicsPipeline(const std::string &id)
//   {
//     return graphicsPipeline[graphicsPipelineNameToIndex.get(id)];
//   }
//   inline rhi::GraphicsPipeline &getGraphicsPipeline(const uint32_t id)
//   {
//     return graphicsPipeline[id];
//   }

//   inline rhi::ComputePipeline &getComputePipeline(const std::string &id)
//   {
//     return computePipeline[computePipelineNameToIndex.get(id)];
//   }
//   inline rhi::ComputePipeline &getComputePipeline(const uint32_t id)
//   {
//     return computePipeline[id];
//   }

//   inline rhi::Shader &getShader(const std::string &name)
//   {
//     return shaders[shaderNameToIndex.get(name)];
//   }
//   inline rhi::Shader &getShader(const uint32_t id)
//   {
//     return shaders[id];
//   }

//   inline rhi::BindingGroups &getBindingGroups(const std::string &id)
//   {
//     return bindingGroups[bindingGroupsNameToIndex.get(id)];
//   }
//   inline rhi::BindingGroups &getBindingGroups(const uint32_t id)
//   {
//     return bindingGroups[id];
//   }

//   inline bool containsBuffer(const std::string &id) const
//   {
//     return bufferNameToIndex.contains(id);
//   }

//   inline bool containsBufferView(const std::string &id) const
//   {
//     return bufferViewNameToIndex.contains(id);
//   }

//   inline bool containsSampler(const std::string &id)
//   {
//     return samplerNameToIndex.contains(id);
//   }

//   inline bool containsTexture(const std::string &id)
//   {
//     return textureNameToIndex.contains(id);
//   }

//   inline bool containsBindingGroups(const std::string &id)
//   {
//     return bindingGroupsNameToIndex.contains(id);
//   }

//   inline bool containsShader(const std::string &id)
//   {
//     return shaderNameToIndex.contains(id);
//   }

//   inline bool containsTextureView(const std::string &id)
//   {
//     return textureViewNameToIndex.contains(id);
//   }

//   inline bool containsGraphicsPipeline(const std::string &id)
//   {
//     return graphicsPipelineNameToIndex.contains(id);
//   }

//   inline bool containsComputePipeline(const std::string &id)
//   {
//     return computePipelineNameToIndex.contains(id);
//   }
// };
// } // namespace rendering
