#pragma once

#include "os/print.hpp"
#include <cstdint>
#include <vector>
namespace rhi
{
class Device;
}

#define DECLARE_RESOURCE(resource)                                                                                                                                                 \
  class resource##Imp                                                                                                                                                              \
  {                                                                                                                                                                                \
  public:                                                                                                                                                                          \
    rhi::Device *device;                                                                                                                                                           \
    resource##Info info;                                                                                                                                                           \
    virtual ~resource##Imp() = default;                                                                                                                                            \
  };                                                                                                                                                                               \
                                                                                                                                                                                   \
  class resource                                                                                                                                                                   \
  {                                                                                                                                                                                \
  public:                                                                                                                                                                          \
    resource##Imp *imp = NULL;                                                                                                                                                     \
    inline bool isValid()                                                                                                                                                          \
    {                                                                                                                                                                              \
      return imp != NULL;                                                                                                                                                          \
    }                                                                                                                                                                              \
    inline resource##Imp *get() const                                                                                                                                              \
    {                                                                                                                                                                              \
      return imp;                                                                                                                                                                  \
    }                                                                                                                                                                              \
    inline const resource##Info &getInfo() const                                                                                                                                   \
    {                                                                                                                                                                              \
      return imp->info;                                                                                                                                                            \
    }                                                                                                                                                                              \
  };                                                                                                                                                                               \
                                                                                                                                                                                   \
  inline resource build##resource(resource##Imp *imp, resource##Info info, rhi::Device *device)                                                                                    \
  {                                                                                                                                                                                \
    resource i;                                                                                                                                                                    \
    i.imp = imp;                                                                                                                                                                   \
    imp->device = device;                                                                                                                                                          \
    imp->info = info;                                                                                                                                                              \
    return i;                                                                                                                                                                      \
  }

#define DECLARE_MANAGED_RESOURCE(resource)                                                                                                                                         \
  class resource##Imp                                                                                                                                                              \
  {                                                                                                                                                                                \
  public:                                                                                                                                                                          \
    rhi::Device *device;                                                                                                                                                           \
    resource##Info info;                                                                                                                                                           \
    virtual ~resource##Imp() = default;                                                                                                                                            \
  };                                                                                                                                                                               \
                                                                                                                                                                                   \
  class resource                                                                                                                                                                   \
  {                                                                                                                                                                                \
  private:                                                                                                                                                                         \
    std::shared_ptr<resource##Imp> impl_;                                                                                                                                          \
                                                                                                                                                                                   \
  public:                                                                                                                                                                          \
    resource() = default;                                                                                                                                                          \
    resource(std::shared_ptr<resource##Imp> impl, resource##Info info) : impl_(impl)                                                                                               \
    {                                                                                                                                                                              \
      impl_->info = info;                                                                                                                                                          \
    }                                                                                                                                                                              \
    inline const resource##Info &getInfo() const                                                                                                                                   \
    {                                                                                                                                                                              \
      return impl_->info;                                                                                                                                                          \
    }                                                                                                                                                                              \
    inline bool isValid()                                                                                                                                                          \
    {                                                                                                                                                                              \
      return impl_ != NULL;                                                                                                                                                        \
    }                                                                                                                                                                              \
    resource##Imp *get() const                                                                                                                                                     \
    {                                                                                                                                                                              \
      return impl_.get();                                                                                                                                                          \
    }                                                                                                                                                                              \
  };

#define DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(resource)                                                                                                                      \
                                                                                                                                                                                   \
  namespace rhi                                                                                                                                                                    \
  {                                                                                                                                                                                \
  inline void destroy##resource##Resource(rhi::Device *device, resource##Imp *p)                                                                                                   \
  {                                                                                                                                                                                \
    if (p)                                                                                                                                                                         \
    {                                                                                                                                                                              \
      device->destroy##resource(p);                                                                                                                                                \
    }                                                                                                                                                                              \
  }                                                                                                                                                                                \
                                                                                                                                                                                   \
  inline resource build##resource(resource##Imp *imp, resource##Info info, rhi::Device *device)                                                                                    \
  {                                                                                                                                                                                \
    imp->device = device;                                                                                                                                                          \
    std::shared_ptr<resource##Imp> shared_impl(                                                                                                                                    \
        imp,                                                                                                                                                                       \
        [](resource##Imp *p)                                                                                                                                                       \
        {                                                                                                                                                                          \
          p->device->destroy##resource(p);                                                                                                                                         \
        });                                                                                                                                                                        \
    return resource(shared_impl, info);                                                                                                                                            \
  }                                                                                                                                                                                \
  }

namespace rhi
{

enum class QueueHandle : std::uint64_t
{
};

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

std::string bufferUsageToString(int usage);

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

  Type_Structured,
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

enum ImageUsage : uint32_t
{
  ImageUsage_Sampled = 1 << 0,
  ImageUsage_Storage = 1 << 1,
  ImageUsage_ColorAttachment = 1 << 2,
  ImageUsage_DepthStencilAttachment = 1 << 3,
  ImageUsage_TransferSrc = 1 << 4,
  ImageUsage_TransferDst = 1 << 5,
};

enum class ImageAspectFlags : uint32_t
{
  Color = 1 << 0,
  Depth = 1 << 1,
  Stencil = 1 << 2,
};

enum class PipelineStage
{
  // Pseudo-stage representing the beginning of the pipeline
  // Used for barriers where no actual work has started yet
  // Common for initial resource transitions from UNDEFINED layout
  TOP_OF_PIPE,

  // Stage where vertex and index data is consumed from buffers
  // Includes vertex attribute fetching and index buffer reads
  // Used when transitioning buffers for vertex/index usage
  VERTEX_INPUT,

  // Vertex shader execution stage
  // Where per-vertex computations happen (transformations, lighting setup)
  // Resources accessed: vertex buffers, uniform buffers, textures
  VERTEX_SHADER,

  // // Tessellation control shader stage (optional)
  // // Determines tessellation levels and per-patch data
  // // Part of tessellation pipeline for subdividing primitives
  // TESSELLATION_CONTROL_SHADER,

  // // Tessellation evaluation shader stage (optional)
  // // Generates actual vertex positions from tessellated coordinates
  // // Outputs final tessellated vertices to next stage
  // TESSELLATION_EVALUATION_SHADER,

  // // Geometry shader stage (optional)
  // // Can generate, modify, or discard entire primitives
  // // Can output multiple primitives from single input primitive
  // GEOMETRY_SHADER,

  // Fragment shader execution stage
  // Per-pixel/sample shading computations
  // Most common stage for texture sampling and lighting calculations
  FRAGMENT_SHADER,

  // // Early depth and stencil testing stage
  // // Happens before fragment shader to potentially discard fragments early
  // // Includes depth bounds testing and stencil testing
  // EARLY_FRAGMENT_TESTS,

  // // Late depth and stencil testing stage
  // // Final depth/stencil operations after fragment shader
  // // Used when fragment shader modifies depth or uses discard
  // LATE_FRAGMENT_TESTS,

  // // Color attachment output stage
  // // Where final color values are written to render targets
  // // Includes blending operations with existing framebuffer contents
  // COLOR_ATTACHMENT_OUTPUT,

  // Compute shader execution stage
  // General purpose GPU computing outside of graphics pipeline
  // Can read/write arbitrary buffers and images
  COMPUTE_SHADER,

  // Transfer operations stage
  // Copy operations between resources (buffer-to-buffer, buffer-to-image, etc.)
  // Includes clear operations and blit operations
  TRANSFER,

  // Pseudo-stage representing the end of the pipeline
  // Used for barriers where all previous work must complete
  // Common for final resource transitions or synchronization
  BOTTOM_OF_PIPE,

  // Covers all graphics pipeline stages (but not compute)
  // Equivalent to: VERTEX_INPUT | VERTEX_SHADER | ... | COLOR_ATTACHMENT_OUTPUT
  // Useful for broad synchronization in graphics-only contexts
  ALL_GRAPHICS,

  HOST,
  // Covers all possible pipeline stages
  // Most conservative option - ensures all work completes
  // Can hurt performance if overused, but guarantees correctness
  ALL_COMMANDS
};

enum class ResourceLayout
{
  // Image contents are undefined/uninitialized
  // Used for newly created images or when you don't care about previous contents
  // Transitioning FROM this layout is very cheap (no data preservation needed)
  // Never transition TO this layout (undefined behavior)
  UNDEFINED,

  // Generic layout supporting most operations but not optimally
  // Can be used for any access pattern but with potential performance cost
  // Useful when image will be accessed in multiple ways within same pass
  // Good fallback when you don't know the optimal layout
  GENERAL,

  // Optimized for use as a color render target
  // Used when rendering/drawing to the image as a framebuffer attachment
  // Provides best performance for color attachment writes
  // Must transition to this before using as render target
  COLOR_ATTACHMENT,

  // Optimized for use as depth/stencil render target
  // Used when the image serves as depth buffer or stencil buffer
  // Allows both reading and writing depth/stencil values
  // Required layout for depth testing and depth writes
  DEPTH_STENCIL_ATTACHMENT,

  // Optimized for reading depth/stencil data in shaders
  // Used when depth buffer needs to be sampled as texture (shadow mapping)
  // Read-only access - cannot write depth values in this layout
  // Common for techniques that need to sample depth buffer
  DEPTH_STENCIL_READ_ONLY,

  // Optimized for reading in shaders (textures, samplers)
  // Best performance for texture sampling in fragment/vertex shaders
  // Read-only layout - cannot write to image in this state
  // Most common layout for textures used in rendering
  SHADER_READ_ONLY,

  // Optimized for being source of transfer operations
  // Used when copying FROM this image to another resource
  // Required for operations like texture-to-buffer copies or blits
  // Source image in copy/blit operations
  TRANSFER_SRC,

  // Optimized for being destination of transfer operations
  // Used when copying TO this image from another resource
  // Required for operations like buffer-to-texture uploads
  // Destination image in copy/blit operations
  TRANSFER_DST,

  // Image has been initialized with data before GPU operations
  // Rarely used - mainly for images with pre-existing host data
  // Allows transitioning to other layouts without losing initial content
  // Alternative to UNDEFINED when image already contains valid data
  PREINITIALIZED,

  // Optimized for presentation to display/swapchain
  // Used when image will be presented to screen via swapchain
  // Final layout for images that will be displayed
  // Required before vkQueuePresentKHR calls
  PRESENT_SRC
};

enum class AccessPattern
{
  // No access - used for initialization or when no access is needed
  // Common in source stage of barriers where resource isn't actively used yet
  // Also used in TOP_OF_PIPE barriers
  NONE,

  // Reading vertex attribute data from vertex buffers
  // Used in VERTEX_INPUT stage when GPU fetches vertex data
  // Applied to vertex buffers bound with vkCmdBindVertexBuffers
  VERTEX_ATTRIBUTE_READ,

  // Reading index data from index buffers
  // Used in VERTEX_INPUT stage when GPU fetches index data for indexed draws
  // Applied to index buffers bound with vkCmdBindIndexBuffer
  INDEX_READ,

  // Reading uniform buffer data in shaders
  // Used when shaders access uniform buffer objects (UBOs)
  // Applied to buffers bound as uniform buffers in descriptor sets
  UNIFORM_READ,

  // Reading data in shaders (textures, storage buffers, etc.)
  // General shader read access for textures and storage resources
  // Used for texture sampling, storage buffer reads
  SHADER_READ,

  // Writing data in shaders (storage images, storage buffers)
  // Used for compute shaders writing to storage resources
  // Also for fragment shaders writing to storage images
  SHADER_WRITE,

  // Reading from color attachments (rare - mainly for blending)
  // Used when GPU needs to read existing framebuffer contents
  // Common in blending operations that need source color
  COLOR_ATTACHMENT_READ,

  // Writing to color attachments (render targets)
  // Used when fragment shaders write color output
  // Applied to images bound as color attachments in render pass
  COLOR_ATTACHMENT_WRITE,

  // Reading from depth/stencil attachments
  // Used during depth testing and stencil testing operations
  // Applied when GPU reads existing depth values for comparison
  DEPTH_STENCIL_ATTACHMENT_READ,

  // Writing to depth/stencil attachments
  // Used when fragment processing writes new depth/stencil values
  // Applied during depth buffer updates and stencil modifications
  DEPTH_STENCIL_ATTACHMENT_WRITE,

  // Reading data during transfer operations
  // Used when resource is source of copy/blit operations
  // Applied to source resources in vkCmdCopyBuffer, vkCmdBlitImage, etc.
  TRANSFER_READ,

  // Writing data during transfer operations
  // Used when resource is destination of copy/blit operations
  // Applied to destination resources in copy commands
  TRANSFER_WRITE,

  // Reading indirect draw/dispatch parameters
  // Used when GPU reads draw parameters from buffer (indirect rendering)
  // Applied to buffers used in vkCmdDrawIndirect, vkCmdDispatchIndirect
  INDIRECT_COMMAND_READ,

  // Generic memory read access
  // Conservative option covering any type of read operation
  // Used when specific read type is unknown or multiple read types possible
  MEMORY_READ,

  // Generic memory write access
  // Conservative option covering any type of write operation
  // Used when specific write type is unknown or multiple write types possible
  MEMORY_WRITE
};

std::string toString(ResourceLayout layout);
std::string toString(AccessPattern access);

struct BufferInfo
{
  std::string name;
  BufferUsage usage;
  uint64_t size;
};

DECLARE_MANAGED_RESOURCE(Buffer);

size_t formatPixelSize(Format fmt);
Type formatToType(Format format);
Format typeToFormat(Type type);

struct TextureInfo
{
  std::string name;
  Format format;
  BufferUsage memoryProperties;
  ImageUsage usage;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t mipLevels;
  ResourceLayout layout;
};

DECLARE_MANAGED_RESOURCE(Texture);

struct TextureViewInfo
{
  std::string name;
  Texture texture;

  uint32_t baseMipLevel = 0;
  uint32_t levelCount = 1;
  uint32_t baseArrayLayer = 0;
  uint32_t layerCount = 1;

  ImageAspectFlags flags;
};
DECLARE_MANAGED_RESOURCE(TextureView);

struct VertexLayoutElement
{
  std::string name;
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

// enum class TextureViewDimension : uint32_t
// {
//   D1,
//   D2,
//   D2Array,
//   Cube,
//   CubeArray,
//   D3
// };

// enum class StorageTextureAccess : uint32_t
// {
//   WriteOnly,
//   ReadOnly,
//   ReadWrite
// };

struct BindingGroupLayoutBufferEntry
{
  std::string name;
  uint32_t binding;
  BindingVisibility visibility;
  BufferUsage usage;
  bool isDynamic = false;
};

struct BindingGroupLayoutSamplerEntry
{
  uint32_t binding;
  BindingVisibility visibility;
  // SamplerType type;
};

struct BindingGroupLayoutTextureEntry
{
  uint32_t binding;
  BindingVisibility visibility;
  // Type sampleType;
  // TextureViewDimension viewDimension;
  // bool multisampled = false;
};

struct BindingGroupLayoutStorageTextureEntry
{
  uint32_t binding;
  BindingVisibility visibility;
  // StorageTextureAccess access;
  // Format format;
  // TextureViewDimension viewDimension;
};

struct BindingGroupLayout
{
  std::vector<BindingGroupLayoutBufferEntry> buffers;
  std::vector<BindingGroupLayoutSamplerEntry> samplers;
  std::vector<BindingGroupLayoutTextureEntry> textures;
  std::vector<BindingGroupLayoutStorageTextureEntry> storageTextures;
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
  std::string name;
  std::vector<BindingGroupLayout> groups;
};

DECLARE_MANAGED_RESOURCE(BindingsLayout);

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
  std::string name;
  TextureView view;
  Color clearValue = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct DepthStencilAttachmentInfo
{
  std::string name;
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
  std::string name;
  Viewport viewport;
  Rect2D scissor;

  ColorAttachmentInfo *colorAttachments = NULL;
  uint32_t colorAttachmentsCount = 0;

  DepthStencilAttachmentInfo *depthStencilAttachment = NULL;
};

struct SurfaceInfo
{
  std::string name;
};
DECLARE_RESOURCE(Surface);

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

struct SamplerInfo
{
  std::string name;
  Filter minFilter = Filter::Linear;
  Filter magFilter = Filter::Linear;
  SamplerAddressMode addressModeU = SamplerAddressMode::Repeat;
  SamplerAddressMode addressModeV = SamplerAddressMode::Repeat;
  SamplerAddressMode addressModeW = SamplerAddressMode::Repeat;
  bool anisotropyEnable = false;
  float maxAnisotropy = 1.0f;
  float maxLod = 1.0f;
};
DECLARE_MANAGED_RESOURCE(Sampler);

struct BufferView
{
  std::string name;
  Buffer buffer;
  uint64_t offset;
  uint64_t size;
};
// DECLARE_MANAGED_RESOURCE(BufferView);

struct BindingBufferInfo
{
  std::string name;
  BufferView buffer;
  uint32_t bufferOffset;
  uint32_t binding;
};

struct BindingSamplerInfo
{
  Sampler sampler;
  TextureView view;
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
  std::vector<BindingBufferInfo> buffers;
  std::vector<BindingSamplerInfo> samplers;
  std::vector<BindingTextureInfo> textures;
  std::vector<BindingStorageTextureInfo> storageTextures;
};

struct ShaderInfo
{
  std::string name;
  BindingsLayoutInfo bindingGroupInfo;
};

DECLARE_RESOURCE(Shader);

struct BindingGroupsInfo
{
  std::string name;
  BindingsLayout layout;
  std::vector<BindingGroupInfo> groups;
};
DECLARE_MANAGED_RESOURCE(BindingGroups);

struct CommandBufferInfo
{
  std::string name;
};
DECLARE_MANAGED_RESOURCE(CommandBuffer);

struct SwapChainInfo
{
  std::string name;
  Surface surface;
  uint32_t width;
  uint32_t height;
};
DECLARE_MANAGED_RESOURCE(SwapChain);

struct GraphicsPipelineVertexStage
{
  Shader vertexShader;
  std::string shaderEntry;

  std::vector<VertexLayoutElement> vertexLayoutElements;

  PrimitiveType primitiveType;
  PrimitiveCullType cullType;
};

struct GraphicsPipelineFragmentStage
{
  Shader fragmentShader;
  std::string shaderEntry;
  std::vector<ColorAttatchment> colorAttatchments;
  DepthAttatchment depthAttatchment;
};

struct GraphicsPipelineInfo
{
  std::string name;
  BindingsLayout layout;
  GraphicsPipelineVertexStage vertexStage;
  GraphicsPipelineFragmentStage fragmentStage;
};
DECLARE_MANAGED_RESOURCE(GraphicsPipeline);

struct ComputePipelineInfo
{
  std::string name;
  Shader shader;
  const char *entry;
  BindingsLayout layout;
};
DECLARE_MANAGED_RESOURCE(ComputePipeline);

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

  virtual Buffer createBuffer(const BufferInfo &info, void *data) = 0;

  virtual BufferMapStatus mapBuffer(BufferView, BufferMap, void **) = 0;
  virtual void unmapBuffer(BufferView) = 0;

  virtual BindingsLayout createBindingsLayout(const BindingsLayoutInfo &) = 0;
  virtual SwapChain createSwapChain(Surface handle, uint32_t width, uint32_t height) = 0;
  virtual TextureView getCurrentSwapChainTextureView(SwapChain) = 0;
  virtual BindingGroups createBindingGroups(const BindingGroupsInfo &info) = 0;
  virtual GraphicsPipeline createGraphicsPipeline(GraphicsPipelineInfo) = 0;
  virtual ComputePipeline createComputePipeline(const ComputePipelineInfo &info) = 0;
  virtual CommandBuffer createCommandBuffer(const CommandBufferInfo &) = 0;
  virtual Sampler createSampler(const SamplerInfo &info) = 0;

  // Command recording
  virtual void beginCommandBuffer(CommandBuffer) = 0;
  virtual void endCommandBuffer(CommandBuffer) = 0;

  virtual void cmdBeginRenderPass(CommandBuffer, const RenderPassInfo &) = 0;
  virtual void cmdEndRenderPass(CommandBuffer) = 0;

  virtual void cmdCopyBuffer(CommandBuffer cmdBuffer, BufferView src, BufferView dst, uint32_t srcOffset, uint32_t dstOffset, uint32_t size) = 0;

  virtual void cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount) = 0;
  virtual void cmdBindGraphicsPipeline(CommandBuffer, GraphicsPipeline) = 0;
  virtual void cmdBindComputePipeline(CommandBuffer, ComputePipeline) = 0;

  virtual void cmdBindVertexBuffer(CommandBuffer, uint32_t slot, BufferView) = 0;
  virtual void cmdBindIndexBuffer(CommandBuffer, BufferView, Type type) = 0;

  virtual void cmdDraw(CommandBuffer, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;

  virtual void cmdDrawIndexed(CommandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) = 0;
  virtual void cmdDrawIndexedIndirect(CommandBuffer, BufferView indirectBuffer, size_t offset, uint32_t drawCount, uint32_t stride) = 0;
  virtual void cmdDispatch(CommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;

  virtual void cmdImageBarrier(
      CommandBuffer cmd,
      Texture image,
      PipelineStage src_stage,
      PipelineStage dst_stage,
      AccessPattern src_access,
      AccessPattern dst_access,
      ResourceLayout old_layout,
      ResourceLayout new_layout,
      ImageAspectFlags aspect_mask,
      uint32_t base_mip_level,
      uint32_t level_count,
      uint32_t base_array_layer,
      uint32_t layer_count,
      uint32_t src_queue_family,
      uint32_t dst_queue_family) = 0;

  virtual void cmdBufferBarrier(
      CommandBuffer cmd,
      Buffer b,
      PipelineStage src_stage,
      PipelineStage dst_stage,
      AccessPattern src_access,
      AccessPattern dst_access,
      uint32_t offset,
      uint32_t size,
      uint32_t src_queue_family,
      uint32_t dst_queue_family) = 0;

  virtual void cmdMemoryBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access) = 0;
  virtual void cmdPipelineBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access) = 0;

  virtual GPUFuture submit(QueueHandle queue, CommandBuffer *commandBuffers, uint32_t count, GPUFuture *wait = NULL) = 0;

  // Sync
  virtual void waitIdle() = 0;

  virtual QueueHandle getQueue(QueueType) = 0;
  virtual Format getSwapChainFormat(SwapChain) = 0;

  virtual Texture createTexture(const TextureInfo &info) = 0;
  virtual TextureView createTextureView(const TextureViewInfo &info) = 0;

  virtual void tick() = 0;
  virtual void wait(GPUFuture &future) = 0;

  virtual void destroyBuffer(BufferImp *) = 0;
  virtual void destroySampler(SamplerImp *) = 0;
  virtual void destroyCommandBuffer(CommandBufferImp *) = 0;
  virtual void destroyComputePipeline(ComputePipelineImp *) = 0;
  virtual void destroyGraphicsPipeline(GraphicsPipelineImp *) = 0;
  virtual void destroyBindingGroups(BindingGroupsImp *) = 0;
  virtual void destroySwapChain(SwapChainImp *) = 0;
  virtual void destroyBindingsLayout(BindingsLayoutImp *) = 0;
  virtual void destroyShader(ShaderImp *handle) = 0;
  virtual void destroyTexture(TextureImp *handle) = 0;
  virtual void destroyTextureView(TextureViewImp *handle) = 0;
  // virtual void destroyBufferView(BufferViewImp *handle) = 0;
};

// VertexLayoutElement comparison operators
inline bool operator==(const VertexLayoutElement &lhs, const VertexLayoutElement &rhs)
{
  return lhs.name == rhs.name && lhs.type == rhs.type && lhs.binding == rhs.binding && lhs.location == rhs.location && lhs.offset == rhs.offset;
}

inline bool operator!=(const VertexLayoutElement &lhs, const VertexLayoutElement &rhs)
{
  return !(lhs == rhs);
}

// ColorAttatchment comparison operators
inline bool operator==(const ColorAttatchment &lhs, const ColorAttatchment &rhs)
{
  return lhs.format == rhs.format && lhs.loadOp == rhs.loadOp && lhs.storeOp == rhs.storeOp;
}

inline bool operator!=(const ColorAttatchment &lhs, const ColorAttatchment &rhs)
{
  return !(lhs == rhs);
}

// DepthAttatchment comparison operators
inline bool operator==(const DepthAttatchment &lhs, const DepthAttatchment &rhs)
{
  return lhs.format == rhs.format && lhs.loadOp == rhs.loadOp && lhs.storeOp == rhs.storeOp;
}

inline bool operator!=(const DepthAttatchment &lhs, const DepthAttatchment &rhs)
{
  return !(lhs == rhs);
}

// BindingGroupLayoutBufferEntry comparison
inline bool operator==(const BindingGroupLayoutBufferEntry &lhs, const BindingGroupLayoutBufferEntry &rhs)
{
  return lhs.name == rhs.name && lhs.binding == rhs.binding && lhs.visibility == rhs.visibility && lhs.usage == rhs.usage && lhs.isDynamic == rhs.isDynamic;
}

inline bool operator!=(const BindingGroupLayoutBufferEntry &lhs, const BindingGroupLayoutBufferEntry &rhs)
{
  return !(lhs == rhs);
}

// BindingGroupLayoutSamplerEntry comparison
inline bool operator==(const BindingGroupLayoutSamplerEntry &lhs, const BindingGroupLayoutSamplerEntry &rhs)
{
  return lhs.binding == rhs.binding && lhs.visibility == rhs.visibility; // && lhs.type == rhs.type;
}

inline bool operator!=(const BindingGroupLayoutSamplerEntry &lhs, const BindingGroupLayoutSamplerEntry &rhs)
{
  return !(lhs == rhs);
}

// BindingGroupLayoutTextureEntry comparison
inline bool operator==(const BindingGroupLayoutTextureEntry &lhs, const BindingGroupLayoutTextureEntry &rhs)
{
  return lhs.binding == rhs.binding && lhs.visibility == rhs.visibility /* && lhs.sampleType == rhs.sampleType && lhs.viewDimension == rhs.viewDimension &&*/
                                                                        //  lhs.multisampled == rhs.multisampled;
      ;
}

inline bool operator!=(const BindingGroupLayoutTextureEntry &lhs, const BindingGroupLayoutTextureEntry &rhs)
{
  return !(lhs == rhs);
}

// BindingGroupLayoutStorageTextureEntry comparison
inline bool operator==(const BindingGroupLayoutStorageTextureEntry &lhs, const BindingGroupLayoutStorageTextureEntry &rhs)
{
  return lhs.binding == rhs.binding && lhs.visibility == rhs.visibility; // && lhs.access == rhs.access && lhs.format == rhs.format && lhs.viewDimension == rhs.viewDimension;
}

inline bool operator!=(const BindingGroupLayoutStorageTextureEntry &lhs, const BindingGroupLayoutStorageTextureEntry &rhs)
{
  return !(lhs == rhs);
}

// BindingGroupLayout comparison
inline bool operator==(const BindingGroupLayout &lhs, const BindingGroupLayout &rhs)
{
  return lhs.buffers == rhs.buffers && lhs.samplers == rhs.samplers && lhs.textures == rhs.textures && lhs.storageTextures == rhs.storageTextures;
}

inline bool operator!=(const BindingGroupLayout &lhs, const BindingGroupLayout &rhs)
{
  return !(lhs == rhs);
}

inline bool operator==(const BufferView &lhs, const BufferView &rhs)
{
  return lhs.buffer.get() == rhs.buffer.get() && lhs.offset == rhs.offset && lhs.size == rhs.offset;
}

// BindingBufferInfo comparison
inline bool operator==(const BindingBufferInfo &lhs, const BindingBufferInfo &rhs)
{
  return lhs.name == rhs.name && lhs.buffer == rhs.buffer;
}

inline bool operator!=(const BindingBufferInfo &lhs, const BindingBufferInfo &rhs)
{
  return !(lhs == rhs);
}

// BindingSamplerInfo comparison
inline bool operator==(const BindingSamplerInfo &lhs, const BindingSamplerInfo &rhs)
{
  return lhs.sampler.get() == rhs.sampler.get() && lhs.binding == rhs.binding;
}

inline bool operator!=(const BindingSamplerInfo &lhs, const BindingSamplerInfo &rhs)
{
  return !(lhs == rhs);
}

// BindingTextureInfo comparison
inline bool operator==(const BindingTextureInfo &lhs, const BindingTextureInfo &rhs)
{
  return lhs.textureView.get() == rhs.textureView.get() && lhs.binding == rhs.binding;
}

inline bool operator!=(const BindingTextureInfo &lhs, const BindingTextureInfo &rhs)
{
  return !(lhs == rhs);
}

// BindingStorageTextureInfo comparison
inline bool operator==(const BindingStorageTextureInfo &lhs, const BindingStorageTextureInfo &rhs)
{
  return lhs.textureView.get() == rhs.textureView.get() && lhs.binding == rhs.binding;
}

inline bool operator!=(const BindingStorageTextureInfo &lhs, const BindingStorageTextureInfo &rhs)
{
  return !(lhs == rhs);
}

// BindingGroupInfo comparison
inline bool operator==(const BindingGroupInfo &lhs, const BindingGroupInfo &rhs)
{
  return lhs.buffers == rhs.buffers && lhs.samplers == rhs.samplers && lhs.textures == rhs.textures && lhs.storageTextures == rhs.storageTextures;
}

inline bool operator!=(const BindingGroupInfo &lhs, const BindingGroupInfo &rhs)
{
  return !(lhs == rhs);
}

// BufferInfo comparison operators
inline bool operator==(const BufferInfo &lhs, const BufferInfo &rhs)
{
  return lhs.name == rhs.name && lhs.usage == rhs.usage && lhs.size == rhs.size;
}

inline bool operator!=(const BufferInfo &lhs, const BufferInfo &rhs)
{
  return !(lhs == rhs);
}

// TextureInfo comparison operators
inline bool operator==(const TextureInfo &lhs, const TextureInfo &rhs)
{
  return lhs.name == rhs.name && lhs.format == rhs.format && lhs.memoryProperties == rhs.memoryProperties && lhs.usage == rhs.usage && lhs.width == rhs.width &&
         lhs.height == rhs.height;
}

inline bool operator!=(const TextureInfo &lhs, const TextureInfo &rhs)
{
  return !(lhs == rhs);
}

// TextureViewInfo comparison operators
inline bool operator==(const TextureViewInfo &lhs, const TextureViewInfo &rhs)
{
  return lhs.name == rhs.name && lhs.texture.get() == rhs.texture.get() && lhs.flags == rhs.flags;
}

inline bool operator!=(const TextureViewInfo &lhs, const TextureViewInfo &rhs)
{
  return !(lhs == rhs);
}

// SamplerInfo comparison operators
inline bool operator==(const SamplerInfo &lhs, const SamplerInfo &rhs)
{
  return lhs.name == rhs.name && lhs.minFilter == rhs.minFilter && lhs.magFilter == rhs.magFilter && lhs.addressModeU == rhs.addressModeU && lhs.addressModeV == rhs.addressModeV &&
         lhs.addressModeW == rhs.addressModeW && lhs.anisotropyEnable == rhs.anisotropyEnable && lhs.maxAnisotropy == rhs.maxAnisotropy && lhs.maxLod == rhs.maxLod;
}

inline bool operator!=(const SamplerInfo &lhs, const SamplerInfo &rhs)
{
  return !(lhs == rhs);
}

// SurfaceInfo comparison operators
inline bool operator==(const SurfaceInfo &lhs, const SurfaceInfo &rhs)
{
  return lhs.name == rhs.name;
}

inline bool operator!=(const SurfaceInfo &lhs, const SurfaceInfo &rhs)
{
  return !(lhs == rhs);
}

// SwapChainInfo comparison operators
inline bool operator==(const SwapChainInfo &lhs, const SwapChainInfo &rhs)
{
  return lhs.name == rhs.name && lhs.surface.get() == rhs.surface.get() && lhs.width == rhs.width && lhs.height == rhs.height;
}

inline bool operator!=(const SwapChainInfo &lhs, const SwapChainInfo &rhs)
{
  return !(lhs == rhs);
}

// CommandBufferInfo comparison operators
inline bool operator==(const CommandBufferInfo &lhs, const CommandBufferInfo &rhs)
{
  return lhs.name == rhs.name;
}

inline bool operator!=(const CommandBufferInfo &lhs, const CommandBufferInfo &rhs)
{
  return !(lhs == rhs);
}

// BindingsLayoutInfo comparison operators (must come before ShaderInfo)
inline bool operator==(const BindingsLayoutInfo &lhs, const BindingsLayoutInfo &rhs)
{
  return lhs.name == rhs.name && lhs.groups == rhs.groups;
}

inline bool operator!=(const BindingsLayoutInfo &lhs, const BindingsLayoutInfo &rhs)
{
  return !(lhs == rhs);
}

// ShaderInfo comparison operators
inline bool operator==(const ShaderInfo &lhs, const ShaderInfo &rhs)
{
  return lhs.name == rhs.name && lhs.bindingGroupInfo == rhs.bindingGroupInfo;
}

inline bool operator!=(const ShaderInfo &lhs, const ShaderInfo &rhs)
{
  return !(lhs == rhs);
}

// GraphicsPipelineVertexStage comparison operators
inline bool operator==(const GraphicsPipelineVertexStage &lhs, const GraphicsPipelineVertexStage &rhs)
{
  return lhs.vertexShader.get() == rhs.vertexShader.get() && lhs.shaderEntry == rhs.shaderEntry && lhs.vertexLayoutElements == rhs.vertexLayoutElements &&
         lhs.primitiveType == rhs.primitiveType && lhs.cullType == rhs.cullType;
}

inline bool operator!=(const GraphicsPipelineVertexStage &lhs, const GraphicsPipelineVertexStage &rhs)
{
  return !(lhs == rhs);
}

// GraphicsPipelineFragmentStage comparison operators
inline bool operator==(const GraphicsPipelineFragmentStage &lhs, const GraphicsPipelineFragmentStage &rhs)
{
  return lhs.fragmentShader.get() == rhs.fragmentShader.get() && lhs.shaderEntry == rhs.shaderEntry && lhs.colorAttatchments == rhs.colorAttatchments &&
         lhs.depthAttatchment == rhs.depthAttatchment;
}

inline bool operator!=(const GraphicsPipelineFragmentStage &lhs, const GraphicsPipelineFragmentStage &rhs)
{
  return !(lhs == rhs);
}

// GraphicsPipelineInfo comparison operators
inline bool operator==(const GraphicsPipelineInfo &lhs, const GraphicsPipelineInfo &rhs)
{
  return lhs.name == rhs.name && lhs.layout.get() == rhs.layout.get() && lhs.vertexStage == rhs.vertexStage && lhs.fragmentStage == rhs.fragmentStage;
}

inline bool operator!=(const GraphicsPipelineInfo &lhs, const GraphicsPipelineInfo &rhs)
{
  return !(lhs == rhs);
}

// ComputePipelineInfo comparison operators
inline bool operator==(const ComputePipelineInfo &lhs, const ComputePipelineInfo &rhs)
{
  return lhs.name == rhs.name && lhs.shader.get() == rhs.shader.get() && lhs.entry == rhs.entry && lhs.layout.get() == rhs.layout.get();
}

inline bool operator!=(const ComputePipelineInfo &lhs, const ComputePipelineInfo &rhs)
{
  return !(lhs == rhs);
}

// BindingGroupsInfo comparison operators
inline bool operator==(const BindingGroupsInfo &lhs, const BindingGroupsInfo &rhs)
{
  return lhs.name == rhs.name && lhs.groups == rhs.groups;
}

inline bool operator!=(const BindingGroupsInfo &lhs, const BindingGroupsInfo &rhs)
{
  return !(lhs == rhs);
}

} // namespace rhi

DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(Buffer);
DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(Texture);
DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(Sampler);
DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(SwapChain);
DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(TextureView);
DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(BindingGroups);
DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(CommandBuffer);
DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(BindingsLayout);
DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(ComputePipeline);
DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(GraphicsPipeline);
// DEFINE_MANAGED_RESOURCE_DELETER_AND_BUILDER(BufferView);

#pragma once
#include <functional>
#include <string>

// Add this at the end of your rhi.hpp file, after the namespace rhi closing brace

namespace std
{

// Helper function to combine hash values
template <class T> inline void hash_combine(std::size_t &seed, const T &v)
{
  std::hash<T> hasher;
  seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

// Hash for BufferInfo
template <> struct hash<rhi::BufferInfo>
{
  std::size_t operator()(const rhi::BufferInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    hash_combine(seed, static_cast<uint32_t>(info.usage));
    hash_combine(seed, info.size);
    return seed;
  }
};

// Hash for TextureInfo
template <> struct hash<rhi::TextureInfo>
{
  std::size_t operator()(const rhi::TextureInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    hash_combine(seed, static_cast<uint32_t>(info.format));
    hash_combine(seed, static_cast<uint32_t>(info.memoryProperties));
    hash_combine(seed, static_cast<uint32_t>(info.usage));
    hash_combine(seed, info.width);
    hash_combine(seed, info.height);
    hash_combine(seed, info.depth);
    hash_combine(seed, info.mipLevels);
    return seed;
  }
};

// Hash for TextureViewInfo
template <> struct hash<rhi::TextureViewInfo>
{
  std::size_t operator()(const rhi::TextureViewInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(info.texture.get()));
    hash_combine(seed, info.baseMipLevel);
    hash_combine(seed, info.levelCount);
    hash_combine(seed, info.baseArrayLayer);
    hash_combine(seed, info.layerCount);
    hash_combine(seed, static_cast<uint32_t>(info.flags));
    return seed;
  }
};

// Hash for VertexLayoutElement
template <> struct hash<rhi::VertexLayoutElement>
{
  std::size_t operator()(const rhi::VertexLayoutElement &elem) const
  {
    std::size_t seed = 0;
    hash_combine(seed, elem.name);
    hash_combine(seed, static_cast<uint32_t>(elem.type));
    hash_combine(seed, elem.binding);
    hash_combine(seed, elem.location);
    hash_combine(seed, elem.offset);
    return seed;
  }
};

// Hash for BindingGroupLayoutBufferEntry
template <> struct hash<rhi::BindingGroupLayoutBufferEntry>
{
  std::size_t operator()(const rhi::BindingGroupLayoutBufferEntry &entry) const
  {
    std::size_t seed = 0;
    hash_combine(seed, entry.name);
    hash_combine(seed, entry.binding);
    hash_combine(seed, static_cast<uint32_t>(entry.visibility));
    hash_combine(seed, static_cast<uint32_t>(entry.usage));
    hash_combine(seed, entry.isDynamic);
    return seed;
  }
};

// Hash for BindingGroupLayoutSamplerEntry
template <> struct hash<rhi::BindingGroupLayoutSamplerEntry>
{
  std::size_t operator()(const rhi::BindingGroupLayoutSamplerEntry &entry) const
  {
    std::size_t seed = 0;
    hash_combine(seed, entry.binding);
    hash_combine(seed, static_cast<uint32_t>(entry.visibility));
    // hash_combine(seed, static_cast<uint32_t>(entry.type));
    return seed;
  }
};

// Hash for BindingGroupLayoutTextureEntry
template <> struct hash<rhi::BindingGroupLayoutTextureEntry>
{
  std::size_t operator()(const rhi::BindingGroupLayoutTextureEntry &entry) const
  {
    std::size_t seed = 0;
    hash_combine(seed, entry.binding);
    hash_combine(seed, static_cast<uint32_t>(entry.visibility));
    // hash_combine(seed, static_cast<uint32_t>(entry.sampleType));
    // hash_combine(seed, static_cast<uint32_t>(entry.viewDimension));
    // hash_combine(seed, entry.multisampled);
    return seed;
  }
};

// Hash for BindingGroupLayoutStorageTextureEntry
template <> struct hash<rhi::BindingGroupLayoutStorageTextureEntry>
{
  std::size_t operator()(const rhi::BindingGroupLayoutStorageTextureEntry &entry) const
  {
    std::size_t seed = 0;
    hash_combine(seed, entry.binding);
    hash_combine(seed, static_cast<uint32_t>(entry.visibility));
    // hash_combine(seed, static_cast<uint32_t>(entry.access));
    // hash_combine(seed, static_cast<uint32_t>(entry.format));
    // hash_combine(seed, static_cast<uint32_t>(entry.viewDimension));
    return seed;
  }
};

// Hash for BindingGroupLayout
template <> struct hash<rhi::BindingGroupLayout>
{
  std::size_t operator()(const rhi::BindingGroupLayout &layout) const
  {
    std::size_t seed = 0;
    for (const auto &buffer : layout.buffers)
    {
      hash_combine(seed, buffer);
    }
    for (const auto &sampler : layout.samplers)
    {
      hash_combine(seed, sampler);
    }
    for (const auto &texture : layout.textures)
    {
      hash_combine(seed, texture);
    }
    for (const auto &storageTexture : layout.storageTextures)
    {
      hash_combine(seed, storageTexture);
    }
    return seed;
  }
};

// Hash for ColorAttatchment
template <> struct hash<rhi::ColorAttatchment>
{
  std::size_t operator()(const rhi::ColorAttatchment &att) const
  {
    std::size_t seed = 0;
    hash_combine(seed, static_cast<uint32_t>(att.format));
    hash_combine(seed, static_cast<uint32_t>(att.loadOp));
    hash_combine(seed, static_cast<uint32_t>(att.storeOp));
    return seed;
  }
};

// Hash for DepthAttatchment
template <> struct hash<rhi::DepthAttatchment>
{
  std::size_t operator()(const rhi::DepthAttatchment &att) const
  {
    std::size_t seed = 0;
    hash_combine(seed, static_cast<uint32_t>(att.format));
    hash_combine(seed, static_cast<uint32_t>(att.loadOp));
    hash_combine(seed, static_cast<uint32_t>(att.storeOp));
    return seed;
  }
};

// Hash for BindingsLayoutInfo
template <> struct hash<rhi::BindingsLayoutInfo>
{
  std::size_t operator()(const rhi::BindingsLayoutInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    for (const auto &group : info.groups)
    {
      hash_combine(seed, group);
    }
    return seed;
  }
};

// Hash for SamplerInfo
template <> struct hash<rhi::SamplerInfo>
{
  std::size_t operator()(const rhi::SamplerInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    hash_combine(seed, static_cast<uint32_t>(info.minFilter));
    hash_combine(seed, static_cast<uint32_t>(info.magFilter));
    hash_combine(seed, static_cast<uint32_t>(info.addressModeU));
    hash_combine(seed, static_cast<uint32_t>(info.addressModeV));
    hash_combine(seed, static_cast<uint32_t>(info.addressModeW));
    hash_combine(seed, info.anisotropyEnable);
    hash_combine(seed, info.maxAnisotropy);
    hash_combine(seed, info.maxLod);
    return seed;
  }
};

// Hash for BufferView
template <> struct hash<rhi::BufferView>
{
  std::size_t operator()(const rhi::BufferView &view) const
  {
    std::size_t seed = 0;
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(view.buffer.get()));
    hash_combine(seed, view.offset);
    hash_combine(seed, view.size);
    hash_combine(seed, view.name);
    return seed;
  }
};

// Hash for BindingBufferInfo
template <> struct hash<rhi::BindingBufferInfo>
{
  std::size_t operator()(const rhi::BindingBufferInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    hash_combine(seed, info.buffer);
    hash_combine(seed, info.bufferOffset);
    hash_combine(seed, info.binding);
    return seed;
  }
};

// Hash for BindingSamplerInfo
template <> struct hash<rhi::BindingSamplerInfo>
{
  std::size_t operator()(const rhi::BindingSamplerInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(info.sampler.get()));
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(info.view.get()));
    hash_combine(seed, info.binding);
    return seed;
  }
};

// Hash for BindingTextureInfo
template <> struct hash<rhi::BindingTextureInfo>
{
  std::size_t operator()(const rhi::BindingTextureInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(info.textureView.get()));
    hash_combine(seed, info.binding);
    return seed;
  }
};

// Hash for BindingStorageTextureInfo
template <> struct hash<rhi::BindingStorageTextureInfo>
{
  std::size_t operator()(const rhi::BindingStorageTextureInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(info.textureView.get()));
    hash_combine(seed, info.binding);
    return seed;
  }
};

// Hash for BindingGroupInfo
template <> struct hash<rhi::BindingGroupInfo>
{
  std::size_t operator()(const rhi::BindingGroupInfo &info) const
  {
    std::size_t seed = 0;
    for (const auto &buffer : info.buffers)
    {
      hash_combine(seed, buffer);
    }
    for (const auto &sampler : info.samplers)
    {
      hash_combine(seed, sampler);
    }
    for (const auto &texture : info.textures)
    {
      hash_combine(seed, texture);
    }
    for (const auto &storageTexture : info.storageTextures)
    {
      hash_combine(seed, storageTexture);
    }
    return seed;
  }
};

// Hash for ShaderInfo
template <> struct hash<rhi::ShaderInfo>
{
  std::size_t operator()(const rhi::ShaderInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    hash_combine(seed, info.bindingGroupInfo);
    return seed;
  }
};

// Hash for BindingGroupsInfo
template <> struct hash<rhi::BindingGroupsInfo>
{
  std::size_t operator()(const rhi::BindingGroupsInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    for (const auto &group : info.groups)
    {
      hash_combine(seed, group);
    }
    return seed;
  }
};

// Hash for CommandBufferInfo
template <> struct hash<rhi::CommandBufferInfo>
{
  std::size_t operator()(const rhi::CommandBufferInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    return seed;
  }
};

// Hash for SwapChainInfo
template <> struct hash<rhi::SwapChainInfo>
{
  std::size_t operator()(const rhi::SwapChainInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(info.surface.get()));
    hash_combine(seed, info.width);
    hash_combine(seed, info.height);
    return seed;
  }
};

// Hash for SurfaceInfo
template <> struct hash<rhi::SurfaceInfo>
{
  std::size_t operator()(const rhi::SurfaceInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    return seed;
  }
};

// Hash for GraphicsPipelineVertexStage
template <> struct hash<rhi::GraphicsPipelineVertexStage>
{
  std::size_t operator()(const rhi::GraphicsPipelineVertexStage &stage) const
  {
    std::size_t seed = 0;
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(stage.vertexShader.get()));
    hash_combine(seed, stage.shaderEntry);
    for (const auto &elem : stage.vertexLayoutElements)
    {
      hash_combine(seed, elem);
    }
    hash_combine(seed, static_cast<uint32_t>(stage.primitiveType));
    hash_combine(seed, static_cast<uint32_t>(stage.cullType));
    return seed;
  }
};

// Hash for GraphicsPipelineFragmentStage
template <> struct hash<rhi::GraphicsPipelineFragmentStage>
{
  std::size_t operator()(const rhi::GraphicsPipelineFragmentStage &stage) const
  {
    std::size_t seed = 0;
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(stage.fragmentShader.get()));
    hash_combine(seed, stage.shaderEntry);
    for (const auto &att : stage.colorAttatchments)
    {
      hash_combine(seed, att);
    }
    hash_combine(seed, stage.depthAttatchment);
    return seed;
  }
};

// Hash for GraphicsPipelineInfo
template <> struct hash<rhi::GraphicsPipelineInfo>
{
  std::size_t operator()(const rhi::GraphicsPipelineInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(info.layout.get()));
    hash_combine(seed, info.vertexStage);
    hash_combine(seed, info.fragmentStage);
    return seed;
  }
};

// Hash for ComputePipelineInfo
template <> struct hash<rhi::ComputePipelineInfo>
{
  std::size_t operator()(const rhi::ComputePipelineInfo &info) const
  {
    std::size_t seed = 0;
    hash_combine(seed, info.name);
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(info.shader.get()));
    if (info.entry)
    {
      hash_combine(seed, std::string(info.entry));
    }
    hash_combine(seed, reinterpret_cast<std::uintptr_t>(info.layout.get()));
    return seed;
  }
};

} // namespace std
