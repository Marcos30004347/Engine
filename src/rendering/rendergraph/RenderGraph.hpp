#pragma once

#include "rhi/Device.hpp"

namespace rendering
{

enum BufferResourceType
{
  ResourceType_StorageBuffer,
  ResourceType_UniformBuffer,
  ResourceType_IndirectBuffer,
  ResourceType_IndexBuffer,
  ResourceType_VertexBuffer,
  ResourceType_PullBuffer,
};

enum SamplerResourceType
{
  SamplerResourceType_Sampler2D,
  SamplerResourceType_Sampler3D,
};

enum TextureResourceType
{
  TextureResourceType_ImageView,
  TextureResourceType_Image,
  TextureResourceType_Storage,
};

enum RenderPassType
{
  RenderPass_Compute = 1 << 0,
  RenderPass_Graphics = 1 << 1,
};

enum ResourceVisibility
{
  ResourceVisibility_Vertex = 1 << 0,
  ResourceVisibility_Fragment = 1 << 1,
  ResourceVisibility_Compute = 1 << 2
};

enum BufferResourceSource
{
  ResourceSource_Internal,
  ResourceSource_Input,
};

enum SamplerResourceSource
{
  ResourceSource_Internal,
  ResourceSource_Input,
};

// enum TextureResourceSource
// {
//   ResourceSource_SwapChain,
//   ResourceSource_Internal,
//   ResourceSource_Input,
// };

enum ResourceUsage
{
  ResourceUsage_Read = 1 << 0,
  ResourceUsage_Write = 1 << 1,
};

class RenderPassBufferResource
{
public:
  BufferResourceType type;
  BufferResourceSource source;
  ResourceUsage usage;
  uint64_t size;
};

class RenderPassSamplerResource
{
public:
  SamplerResourceType type;
  SamplerResourceSource source;
  ResourceUsage usage;
  uint32_t width = 0;
  uint32_t height = 0;
};

class RenderPassTextureResource
{
public:
  TextureResourceType type;
//   TextureResourceSource source;

  ResourceUsage usage;

  rhi::Format format;
  rhi::Color clear;

  rhi::LoadOp loadOp;
  rhi::StoreOp storeOp;

  uint32_t width = 0;
  uint32_t height = 0;
};

class RenderPassResource
{
  const char *name;
  rhi::GPUBuffer buffer;
  rhi::TextureView textureView;
  rhi::Texture texture;
  rhi::Sampler sampler;
};

class RenderPassInputs
{
  RenderPassResource *inputs;
  uint32_t inputsCount;
};

class RenderPassRuntime
{
};

class RenderPass
{
public:
  //   void defineInputBufferResource(const char *name, const RenderPassBufferResource resource);
  //   void defineInputSamplerResource(const char *name, const RenderPassSamplerResource resource);
  //   void defineInputTextureResource(const char *name, const RenderPassTextureResource resource);
  //   void defineOutputBufferResource(const char *name, const RenderPassBufferResource resource);
  //   void defineOutputSamplerResource(const char *name, const RenderPassSamplerResource resource);
  //   void defineOutputTextureResource(const char *name, const RenderPassTextureResource resource);
  virtual const RenderPassBufferResource *getBufferInputResources() = 0;
  virtual const RenderPassBufferResource *getBufferOutputResources() = 0;

  virtual ~RenderPass() = default;
  virtual rhi::GPUFuture submit(RenderPassResource *resources) = 0;
};

}; // namespace rendering