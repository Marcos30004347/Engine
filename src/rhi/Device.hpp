#pragma once

#include <cstdint>
#include <vector>

#include "GPUHeap.hpp"

#define DECLARE_RESOURCE(name)                                                                                                                                                     \
  class name##Imp                                                                                                                                                                  \
  {                                                                                                                                                                                \
  public:                                                                                                                                                                          \
    virtual ~name##Imp() = default;                                                                                                                                                \
  };                                                                                                                                                                               \
                                                                                                                                                                                   \
  class name                                                                                                                                                                       \
  {                                                                                                                                                                                \
  public:                                                                                                                                                                          \
    name##Imp *imp = NULL;                                                                                                                                                         \
  };                                                                                                                                                                               \
                                                                                                                                                                                   \
  inline name build##name(name##Imp *imp)                                                                                                                                          \
  {                                                                                                                                                                                \
    name i;                                                                                                                                                                        \
    i.imp = imp;                                                                                                                                                                   \
    return i;                                                                                                                                                                      \
  }

namespace rhi
{

enum class QueueHandle : std::uint64_t
{
};

DECLARE_RESOURCE(Shader);
DECLARE_RESOURCE(Surface);
DECLARE_RESOURCE(Texture);
DECLARE_RESOURCE(Sampler);
DECLARE_RESOURCE(SwapChain);
DECLARE_RESOURCE(TextureView);
DECLARE_RESOURCE(BindingGroups);
DECLARE_RESOURCE(CommandBuffer);
DECLARE_RESOURCE(BindingsLayout);
DECLARE_RESOURCE(ComputePipeline);
DECLARE_RESOURCE(GraphicsPipeline);

namespace detail
{
class GPUFutureImp
{
public:
  virtual ~GPUFutureImp() = default;
};
} // namespace detail

class GPUFuture
{
private:
  std::shared_ptr<detail::GPUFutureImp> impl_;

public:
  GPUFuture(detail::GPUFutureImp *impl) : impl_(impl)
  {
  }

  GPUFuture() = default;
  GPUFuture(const GPUFuture &) = default;
  GPUFuture(GPUFuture &&) noexcept = default;

  GPUFuture &operator=(const GPUFuture &) = default;
  GPUFuture &operator=(GPUFuture &&) noexcept = default;

  detail::GPUFutureImp *get() const
  {
    return impl_.get();
  }

  bool valid() const
  {
    return impl_ != nullptr;
  }
};

// enum class GPUPromiseHandle : std::uint64_t
// {
// };

// template <typename Tag> struct Handle
// {
//   std::int64_t value;
// };

// using BufferHandle = Handle<struct BufferTag>;
// using Texturehandle = Handle<struct TextureTag>;
enum BufferUsage
{
  BufferUsage_None = 0,
  BufferUsage_Uniform = 1 << 0,
  BufferUsage_Storage = 1 << 1,
  BufferUsage_Push = 1 << 2, // Flag indicating buffer can be used as dest from cpu write
  BufferUsage_Pull = 1 << 3, // Flag indicating buffer can be mapped and read from cpu
  BufferUsage_Vertex = 1 << 4,
  BufferUsage_Indirect = 1 << 5,
  BufferUsage_Timestamp = 1 << 6, // Flag indicating buffer is used as timestamp storage
  BufferUsage_Index = 1 << 7,
};

enum PrimitiveCullType
{
  PrimitiveCullType_None,
  PrimitiveCullType_CCW,
  PrimitiveCullType_CW,
};

enum PrimitiveType
{
  PrimitiveType_Triangles,
  PrimitiveType_TrianglesFan,
  PrimitiveType_TrianglesStrip,
  PrimitiveType_Points,
  PrimitiveType_Lines,
};
typedef enum Type
{
  Type_None = 0,

  // Unsigned integers
  Type_Uint8,
  Type_Uint8x2,
  Type_Uint8x3,
  Type_Uint8x4,
  Type_Uint16,
  Type_Uint16x2,
  Type_Uint16x3,
  Type_Uint16x4,
  Type_Uint32,
  Type_Uint32x2,
  Type_Uint32x3,
  Type_Uint32x4,

  // Signed integers
  Type_Int8,
  Type_Int8x2,
  Type_Int8x3,
  Type_Int8x4,
  Type_Int16,
  Type_Int16x2,
  Type_Int16x3,
  Type_Int16x4,
  Type_Int32,
  Type_Int32x2,
  Type_Int32x3,
  Type_Int32x4,

  // Floats
  Type_Float16,
  Type_Float16x2,
  Type_Float16x3,
  Type_Float16x4,
  Type_Float32,
  Type_Float32x2,
  Type_Float32x3,
  Type_Float32x4,

  // Packed
  Type_Packed_Uint_2_10_10_10,
  Type_Packed_UFloat_11_11_10,
  Type_Packed_UFloat_9995,

  // Depth / stencil
  Type_Depth,
  Type_Stencil,

  Type_Count
} Type;

typedef enum Format
{
  // 8-bit formats
  Format_R8Unorm,
  Format_R8Snorm,
  Format_R8Uint,
  Format_R8Sint,

  // 16-bit formats
  Format_R16Uint,
  Format_R16Sint,
  Format_R16Float,
  Format_RG8Unorm,
  Format_RG8Snorm,
  Format_RG8Uint,
  Format_RG8Sint,

  // 32-bit single channel
  Format_R32Uint,
  Format_R32Sint,
  Format_R32Float,

  // 32-bit two channel
  Format_RG16Uint,
  Format_RG16Sint,
  Format_RG16Float,

  // 32-bit four channel (8-bit each)
  Format_RGBA8Unorm,
  Format_RGBA8UnormSrgb,
  Format_RGBA8Snorm,
  Format_RGBA8Uint,
  Format_RGBA8Sint,
  Format_BGRA8Unorm,
  Format_BGRA8UnormSrgb,

  // 32-bit packed
  Format_RGB10A2Uint,
  Format_RGB10A2Unorm,
  Format_RG11B10UFloat,
  Format_RGB9E5UFloat,

  // 64-bit formats
  Format_RG32Uint,
  Format_RG32Sint,
  Format_RG32Float,
  Format_RGBA16Uint,
  Format_RGBA16Sint,
  Format_RGBA16Float,

  Format_RGB8Unorm,
  Format_RGB8Snorm,
  Format_RGB8Uint,
  Format_RGB8Sint,

  Format_RGB16Uint,
  Format_RGB16Sint,
  Format_RGB16Float,

  Format_RGB32Uint,
  Format_RGB32Sint,
  Format_RGB32Float,

  // 128-bit formats
  Format_RGBA32Uint,
  Format_RGBA32Sint,
  Format_RGBA32Float,

  // Depth / stencil
  Format_Stencil8,
  Format_Depth16Unorm,
  Format_Depth24Plus,
  Format_Depth24PlusStencil8,
  Format_Depth32Float,
  Format_Depth32FloatStencil8,

  Format_Count,
  Format_None
} Format;

size_t formatPixelSize(Format fmt);
Type formatToType(Format format);
Format typeToFormat(Type type);

struct VertexLayoutElement
{
  Type type;
  uint32_t binding;
  uint32_t location;
  uint32_t offset;
};

enum BindingVisibility
{
  BindingVisibility_Vertex = 1 << 0,
  BindingVisibility_Fragment = 1 << 1,
  BindingVisibility_Compute = 1 << 1,
};

enum class SamplerType : uint32_t
{
  Filtering,
  NonFiltering,
  Comparison
};

enum class TextureViewDimension : uint32_t
{
  D1,
  D2,
  D2Array,
  Cube,
  CubeArray,
  D3
};

enum class StorageTextureAccess : uint32_t
{
  WriteOnly,
  ReadOnly,
  ReadWrite
};

struct BindingGroupLayoutBufferEntry
{
  uint32_t binding;
  BindingVisibility visibility;
  BufferUsage usage;
  bool isDynamic = false;
};

struct BindingGroupLayoutSamplerEntry
{
  uint32_t binding;
  BindingVisibility visibility;
  SamplerType type;
};

struct BindingGroupLayoutTextureEntry
{
  uint32_t binding;
  BindingVisibility visibility;
  Type sampleType;
  TextureViewDimension viewDimension;
  bool multisampled = false;
};

struct BindingGroupLayoutStorageTextureEntry
{
  uint32_t binding;
  BindingVisibility visibility;
  StorageTextureAccess access;
  Format format;
  TextureViewDimension viewDimension;
};

struct BindingGroupLayout
{
  BindingGroupLayoutBufferEntry *buffers = NULL;
  uint32_t buffersCount = 0;
  BindingGroupLayoutSamplerEntry *samplers = NULL;
  uint32_t samplersCount = 0;
  BindingGroupLayoutTextureEntry *textures = NULL;
  uint32_t texturesCount = 0;
  BindingGroupLayoutStorageTextureEntry *storageTextures = NULL;
  uint32_t storageTexturesCount = 0;
};

struct Color
{
  float_t R;
  float_t G;
  float_t B;
  float_t A;

  static Color rgb(float_t R, float_t G, float_t B, float_t A)
  {
    Color color;
    color.R = R;
    color.G = G;
    color.B = B;
    color.A = A;
    return color;
  }
};

enum class LoadOp
{
  LoadOp_Load,    // Preserve existing content
  LoadOp_Clear,   // Clear to specified value
  LoadOp_DontCare // Don't care about previous content
};

enum class StoreOp
{
  StoreOp_Store,
  StoreOp_DontCare
};

struct ColorAttatchment
{
  Format format;
  LoadOp loadOp;
  StoreOp storeOp;
};

struct DepthAttatchment
{
  Format format;
  LoadOp loadOp;
  StoreOp storeOp;
};

struct BindingsLayoutInfo
{
  BindingGroupLayout *groups;
  size_t groupsCount;
};

struct GraphicsPipelineVertexStage
{
  Shader vertexShader;
  const char *shaderEntry;

  VertexLayoutElement *vertexLayoutElements;
  size_t vertexLayoutElementsCount;

  PrimitiveType primitiveType;
  PrimitiveCullType cullType;
};

struct GraphicsPipelineFragmentStage
{
  Shader fragmentShader;
  const char *shaderEntry;

  ColorAttatchment *colorAttatchments;
  uint32_t colorAttatchmentsCount;
  DepthAttatchment depthAttatchment;
};

struct GraphicsPipelineInfo
{
  BindingsLayout layout;
  GraphicsPipelineVertexStage vertexStage;
  GraphicsPipelineFragmentStage fragmentStage;
};

struct ComputePipelineInfo
{
  Shader shader;
  const char *entry;
  BindingsLayout layout;
};

// struct PipelineBinding {
//   BufferHandle buffer;
// };

// struct GraphicsPipelineInfo {

// };

enum DeviceBackend
{
  DeviceBackend_Vulkan_1_2,
};

enum DeviceFeatures
{
  DeviceFeatures_None = 0,
  DeviceFeatures_Atomic32_AllOps = 1 << 0,
  DeviceFeatures_Atomic64_MinMax = 1 << 1,
  DeviceFeatures_Atomic64_AllOps = 1 << 2,
  DeviceFeatures_Bindless = 1 << 3,
  DeviceFeatures_Timestamp = 1 << 4,
  DeviceFeatures_Subgroup_Basic = 1 << 5,
  DeviceFeatures_Subgroup_Vote = 1 << 6,
  DeviceFeatures_Subgroup_Arithmetic = 1 << 7,
  DeviceFeatures_Subgroup_Ballot = 1 << 8,
  DeviceFeatures_Subgroup_Shuffle = 1 << 9,
  DeviceFeatures_Subgroup_ShuffleRelative = 1 << 10,
  // DeviceFeatures_Surface = 1 << 12,
  DeviceFeatures_SwapChain = 1 << 11,
  DeviceFeatures_Compute = 1 << 12,
  DeviceFeatures_Graphics = 1 << 13,
  DeviceFeatures_Dedicated = 1 << 14,
  DeviceFeatures_Integrated = 1 << 15,
  DeviceFeatures_MultiDrawIndirect = 1 << 16,
  DeviceFeatures_DrawIndirectFirstInstance = 1 << 17,
  DeviceFeatures_GeometryShader = 1 << 18,
};

struct DeviceRequiredLimits
{
  size_t minimumMemory;
  size_t minimumComputeSharedMemory;
  size_t minimumComputeWorkGroupInvocations;
};

struct DeviceProperties
{
  size_t sugroupSize;
  size_t maxMemory;
  size_t maxComputeSharedMemorySize;
  size_t maxComputeWorkGroupInvocations;
  size_t uniformBufferAlignment;
};

// struct RenderPassCreateInfo
// {

// };

struct Viewport
{
  uint32_t width;
  uint32_t height;
  Viewport() : width(0), height(0)
  {
  }
  Viewport(uint32_t width, uint32_t height) : width(width), height(height)
  {
  }
};

enum QueueType
{
  Queue_Graphics,
  Queue_Transfer,
  Queue_Compute,
};

struct ColorAttachmentInfo
{
  // LoadOp loadOp = LoadOp::LoadOp_Clear;
  // StoreOp storeOp = StoreOp::StoreOp_Store;
  TextureView view;
  Color clearValue = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct DepthStencilAttachmentInfo
{
  TextureView view;
  float clearDepth = 1.0f;
  uint32_t clearStencil = 0;
};

struct Rect2D
{
  uint32_t width;
  uint32_t height;
  uint32_t x;
  uint32_t y;
  Rect2D() : x(0), y(0), width(0), height(0)
  {
  }
  Rect2D(uint32_t x, uint32_t y, uint32_t width, uint32_t height) : x(x), y(y), width(width), height(height)
  {
  }
};

struct Rect3D
{
  uint32_t width;
  uint32_t height;
  uint32_t depth;

  uint32_t x;
  uint32_t y;
  uint32_t z;
  Rect3D() : x(0), y(0), z(0), width(0), height(0), depth(0)
  {
  }
  Rect3D(uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth) : x(x), y(y), z(z), width(width), height(height), depth(depth)
  {
  }
};

struct RenderPassInfo
{
  Viewport viewport;
  Rect2D scissor;

  ColorAttachmentInfo *colorAttachments = NULL;
  uint32_t colorAttachmentsCount = 0;

  DepthStencilAttachmentInfo *depthStencilAttachment = NULL;
};

enum ImageUsage : uint32_t
{
  ImageUsage_Sampled = 1 << 0,
  ImageUsage_Storage = 1 << 1,
  ImageUsage_ColorAttachment = 1 << 2,
  ImageUsage_DepthStencilAttachment = 1 << 3,
  ImageUsage_TransferSrc = 1 << 4,
  ImageUsage_TransferDst = 1 << 5,
};

struct ImageCreateInfo
{
  Format format;
  BufferUsage memoryProperties;
  ImageUsage usage;
  uint32_t width;
  uint32_t height;
};

enum class ImageAspectFlags : uint32_t
{
  Color = 1 << 0,
  Depth = 1 << 1,
  Stencil = 1 << 2,
};

enum class BufferMap
{
  BufferMap_None = 0,
  BufferMap_Read = 1 << 0,
  BufferMap_Write = 1 << 1,
};

enum BufferMapStatus
{
  BufferMapStatus_Failed,
  BufferMapStatus_Success,
};

struct BindingBufferInfo
{
  GPUBuffer buffer;
  uint32_t binding;
};

struct BindingSamplerInfo
{
  Sampler sampler;
  uint32_t binding;
};

struct BindingTextureInfo
{
  TextureView textureView;
  uint32_t binding;
};

struct BindingStorageTextureInfo
{
  TextureView textureView;
  uint32_t binding;
};

struct BindingGroupInfo
{
  BindingBufferInfo *buffers;
  uint32_t buffersCount = 0;

  BindingSamplerInfo *samplers;
  uint32_t samplersCount = 0;

  BindingTextureInfo *textures;
  uint32_t texturesCount = 0;

  BindingStorageTextureInfo *storageTextures;
  uint32_t storageTexturesCount = 0;
};

struct BindingGroupsInfo
{
  BindingGroupInfo *groups;
  uint32_t groupsCount;
};

enum class Filter
{
  Nearest,
  Linear
};

enum class SamplerAddressMode
{
  Repeat,
  MirroredRepeat,
  ClampToEdge,
  ClampToBorder
};

struct SamplerCreateInfo
{
  Filter minFilter = Filter::Linear;
  Filter magFilter = Filter::Linear;
  SamplerAddressMode addressModeU = SamplerAddressMode::Repeat;
  SamplerAddressMode addressModeV = SamplerAddressMode::Repeat;
  SamplerAddressMode addressModeW = SamplerAddressMode::Repeat;
  bool anisotropyEnable = false;
  float maxAnisotropy = 1.0f;
  float maxLod = 1.0f;
};

class Device
{
public:
  std::uint64_t featureFlags;
  DeviceProperties properties;

  inline uint32_t alignedDynamicUniformObjectSize(size_t size)
  {
    return (size + properties.uniformBufferAlignment - 1) & ~(properties.uniformBufferAlignment - 1);
  }

  virtual ~Device() = default;
  virtual void init() = 0;

  virtual GPUHeap *allocateHeap(size_t size, BufferUsage, void *data) = 0;
  virtual void freeHeap(GPUHeap *) = 0;

  virtual BufferMapStatus mapBuffer(GPUBuffer, BufferMap, void **) = 0;
  virtual void unmapBuffer(GPUBuffer) = 0;

  virtual BindingsLayout createBindingsLayout(const BindingsLayoutInfo &) = 0;
  virtual void destroyBindingsLayout(BindingsLayout) = 0;

  virtual SwapChain createSwapChain(Surface handle, uint32_t width, uint32_t height) = 0;
  virtual TextureView getCurrentSwapChainTextureView(SwapChain) = 0;
  virtual void destroySwapChain(SwapChain handle) = 0;

  virtual BindingGroups createBindingGroups(const BindingsLayout layout, const BindingGroupsInfo &info) = 0;
  virtual void destroyBindingGroups(BindingGroups bindingGroup) = 0;

  virtual GraphicsPipeline createGraphicsPipeline(GraphicsPipelineInfo) = 0;
  virtual void destroyGraphicsPipeline(GraphicsPipeline) = 0;

  virtual ComputePipeline createComputePipeline(const ComputePipelineInfo &info) = 0;
  virtual void destroyComputePipeline(ComputePipeline) = 0;

  virtual CommandBuffer createCommandBuffer() = 0;
  virtual void destroyCommandBuffer(CommandBuffer) = 0;

  virtual Sampler createSampler(const SamplerCreateInfo &info) = 0;
  virtual void destroySampler(Sampler handle) = 0;

  // Command recording
  virtual void beginCommandBuffer(CommandBuffer) = 0;
  virtual void endCommandBuffer(CommandBuffer) = 0;

  virtual void cmdBeginRenderPass(CommandBuffer, const RenderPassInfo &) = 0;
  virtual void cmdEndRenderPass(CommandBuffer) = 0;

  virtual void cmdCopyBuffer(CommandBuffer cmdBuffer, GPUBuffer src, GPUBuffer dst, uint32_t srcOffset, uint32_t dstOffset, uint32_t size) = 0;

  virtual void cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount) = 0;
  virtual void cmdBindGraphicsPipeline(CommandBuffer, GraphicsPipeline) = 0;
  virtual void cmdBindComputePipeline(CommandBuffer, ComputePipeline) = 0;

  virtual void cmdBindVertexBuffer(CommandBuffer, uint32_t slot, GPUBuffer) = 0;
  virtual void cmdBindIndexBuffer(CommandBuffer, GPUBuffer, Type type) = 0;

  virtual void cmdDraw(CommandBuffer, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;

  virtual void cmdDrawIndexed(CommandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) = 0;
  virtual void cmdDrawIndexedIndirect(CommandBuffer, GPUBuffer indirectBuffer, size_t offset, uint32_t drawCount, uint32_t stride) = 0;
  virtual void cmdDispatch(CommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
  // Submission
  virtual GPUFuture submit(QueueHandle queue, CommandBuffer *commandBuffers, uint32_t count) = 0;

  // Sync
  virtual void waitIdle() = 0;

  virtual QueueHandle getQueue(QueueType) = 0;
  virtual Format getSwapChainFormat(SwapChain) = 0;

  virtual void destroyShader(Shader handle) = 0;

  virtual Texture createImage(const ImageCreateInfo &info) = 0;
  virtual void destroyImage(Texture handle) = 0;

  virtual TextureView createImageView(Texture imageHandle, ImageAspectFlags aspectFlags) = 0;
  virtual void destroyImageView(TextureView handle) = 0;

  virtual void tick() = 0;
  virtual void wait(GPUFuture &future) = 0;
};

/*
class BufferHeapAllocator
{
public:
  BufferHeapAllocator(Device *device, size_t totalSize, BufferUsage usage);
  ~BufferHeapAllocator();

  BufferSlice allocate(size_t size, size_t alignment = 16);
  void free(const BufferSlice &slice);

private:
  Device *device;
  BufferHandle buffer;
  size_t totalSize;
  BufferUsage usage;
  std::vector<std::pair<size_t, size_t>> freeRegions;

  static size_t alignUp(size_t value, size_t alignment);
  void mergeFreeRegions();
};

class BufferStackAllocator
{
public:
  BufferStackAllocator(Device *device, size_t totalSize, BufferUsage usage);
  ~BufferStackAllocator();

  BufferSlice allocate(size_t size, size_t alignment = 16);

  void reset();

  BufferHandle getBuffer() const;

private:
  Device *device;
  BufferHandle buffer;
  size_t totalSize;
  BufferUsage usage;

  size_t top;
  std::vector<size_t> allocationStack;

  static size_t alignUp(size_t value, size_t alignment);
};
*/
} // namespace rhi