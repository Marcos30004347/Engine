#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace rendering
{

inline void hash_combine(std::size_t &seed, std::size_t value)
{
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

enum class SwapChain : uint32_t
{
};

enum class CommandBuffer : uint32_t
{
};

enum Queue
{
  None = 0,
  Graphics,
  Compute,
  Transfer,
  Present,
  QueuesCount,
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

enum BufferUsage
{
  BufferUsage_None       = 0,

  // Shader / pipeline usage
  BufferUsage_Uniform    = 1 << 0,
  BufferUsage_Storage    = 1 << 1,
  BufferUsage_Vertex     = 1 << 4,
  BufferUsage_Index      = 1 << 7,
  BufferUsage_Indirect   = 1 << 5,
  BufferUsage_Timestamp  = 1 << 6,

  // CPU mapping intent
  BufferUsage_Push       = 1 << 2, // MAP_WRITE
  BufferUsage_Pull       = 1 << 3, // MAP_READ

  // Explicit copy intent 
  BufferUsage_CopySrc    = 1 << 8,
  BufferUsage_CopyDst    = 1 << 9,
};

enum class SamplerAddressMode
{
  Repeat,
  MirroredRepeat,
  ClampToEdge,
  ClampToBorder
};

enum class Filter
{
  Nearest,
  Linear
};

enum BindingVisibility
{
  BindingVisibility_Vertex = 1 << 0,
  BindingVisibility_Fragment = 1 << 1,
  BindingVisibility_Compute = 1 << 1,
};

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
  NONE = 0,

  // Reading vertex attribute data from vertex buffers
  // Used in VERTEX_INPUT stage when GPU fetches vertex data
  // Applied to vertex buffers bound with vkCmdBindVertexBuffers
  VERTEX_ATTRIBUTE_READ = 1 << 1,

  // Reading index data from index buffers
  // Used in VERTEX_INPUT stage when GPU fetches index data for indexed draws
  // Applied to index buffers bound with vkCmdBindIndexBuffer
  INDEX_READ = 1 << 2,

  // Reading uniform buffer data in shaders
  // Used when shaders access uniform buffer objects (UBOs)
  // Applied to buffers bound as uniform buffers in descriptor sets
  UNIFORM_READ = 1 << 3,

  // Reading data in shaders (textures, storage buffers, etc.)
  // General shader read access for textures and storage resources
  // Used for texture sampling, storage buffer reads
  SHADER_READ = 1 << 4,

  // Writing data in shaders (storage images, storage buffers)
  // Used for compute shaders writing to storage resources
  // Also for fragment shaders writing to storage images
  SHADER_WRITE = 1 << 5,

  // Reading from color attachments (rare - mainly for blending)
  // Used when GPU needs to read existing framebuffer contents
  // Common in blending operations that need source color
  COLOR_ATTACHMENT_READ = 1 << 6,

  // Writing to color attachments (render targets)
  // Used when fragment shaders write color output
  // Applied to images bound as color attachments in render pass
  COLOR_ATTACHMENT_WRITE = 1 << 7,

  // Reading from depth/stencil attachments
  // Used during depth testing and stencil testing operations
  // Applied when GPU reads existing depth values for comparison
  DEPTH_STENCIL_ATTACHMENT_READ = 1 << 8,

  // Writing to depth/stencil attachments
  // Used when fragment processing writes new depth/stencil values
  // Applied during depth buffer updates and stencil modifications
  DEPTH_STENCIL_ATTACHMENT_WRITE = 1 << 9,

  // Reading data during transfer operations
  // Used when resource is source of copy/blit operations
  // Applied to source resources in vkCmdCopyBuffer, vkCmdBlitImage, etc.
  TRANSFER_READ = 1 << 10,

  // Writing data during transfer operations
  // Used when resource is destination of copy/blit operations
  // Applied to destination resources in copy commands
  TRANSFER_WRITE = 1 << 11,

  // Reading indirect draw/dispatch parameters
  // Used when GPU reads draw parameters from buffer (indirect rendering)
  // Applied to buffers used in vkCmdDrawIndirect, vkCmdDispatchIndirect
  INDIRECT_COMMAND_READ = 1 << 12,

  // Generic memory read access
  // Conservative option covering any type of read operation
  // Used when specific read type is unknown or multiple read types possible
  MEMORY_READ = 1 << 13,

  // Generic memory write access
  // Conservative option covering any type of write operation
  // Used when specific write type is unknown or multiple write types possible
  MEMORY_WRITE = 1 << 14
};

inline ImageAspectFlags GetImageAspectFlags(Format format)
{
  switch (format)
  {
  case Format_Stencil8:
    return (ImageAspectFlags::Stencil);

  case Format_Depth16Unorm:
  case Format_Depth24Plus:
  case Format_Depth32Float:
    return (ImageAspectFlags::Depth);

  case Format_Depth24PlusStencil8:
  case Format_Depth32FloatStencil8:
    return (ImageAspectFlags)((uint32_t)(ImageAspectFlags::Depth) | ((uint32_t)ImageAspectFlags::Stencil));

  default:
    return (ImageAspectFlags::Color);
  }
}

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

Type formatToType(Format format);
Format typeToFormat(Type type);
size_t formatPixelSize(Format fmt);

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

struct BufferInfo
{
  std::string name;
  uint64_t size = 0U;
  BufferUsage usage;
  bool scratch = false;
  // bool persistent = true;
};

struct TextureInfo
{
  std::string name;
  Format format = Format::Format_RGBA8Uint;
  BufferUsage memoryProperties;
  ImageUsage usage;
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t depth = 0U;
  uint32_t mipLevels = 0U;
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

struct Buffer
{
  std::string name;

  bool operator==(const Buffer &other) const noexcept
  {
    return name == other.name;
  }
  bool operator!=(const Buffer &other) const noexcept
  {
    return !(*this == other);
  }
};

struct Texture
{
  std::string name;

  bool operator==(const Texture &other) const noexcept
  {
    return name == other.name;
  }
  bool operator!=(const Texture &other) const noexcept
  {
    return !(*this == other);
  }
};

struct Sampler
{
  std::string name;

  bool operator==(const Sampler &other) const noexcept
  {
    return name == other.name;
  }
  bool operator!=(const Sampler &other) const noexcept
  {
    return !(*this == other);
  }
};
struct Shader
{
  std::string name;
  bool operator==(const Shader &other) const noexcept
  {
    return name == other.name;
  }
  bool operator!=(const Shader &other) const noexcept
  {
    return !(*this == other);
  }
};

struct BindingsLayout
{
  std::string name;
  bool operator==(const BindingsLayout &other) const noexcept
  {
    return name == other.name;
  }
  bool operator!=(const BindingsLayout &other) const noexcept
  {
    return !(*this == other);
  }
};

struct GraphicsPipeline
{
  std::string name;
  bool operator==(const GraphicsPipeline &other) const noexcept
  {
    return name == other.name;
  }
  bool operator!=(const GraphicsPipeline &other) const noexcept
  {
    return !(*this == other);
  }
};

struct ComputePipeline
{
  std::string name;
  bool operator==(const ComputePipeline &other) const noexcept
  {
    return name == other.name;
  }
  bool operator!=(const ComputePipeline &other) const noexcept
  {
    return !(*this == other);
  }
};

struct BindingGroups
{
  std::string name;
  bool operator==(const BindingGroups &other) const noexcept
  {
    return name == other.name;
  }
  bool operator!=(const BindingGroups &other) const noexcept
  {
    return !(*this == other);
  }
};

struct BufferView
{
  Buffer buffer;
  uint64_t offset = 0U;
  uint64_t size = 0U;
  AccessPattern access;

  bool operator==(const BufferView &other) const noexcept
  {
    return buffer == other.buffer && offset == other.offset && size == other.size && access == other.access;
  }
  bool operator!=(const BufferView &other) const noexcept
  {
    return !(*this == other);
  }
};

struct TextureView
{
  Texture texture;
  SwapChain swapChain;
  uint32_t index;

  ImageAspectFlags flags = ImageAspectFlags::Color;

  uint32_t baseMipLevel = 0;
  uint32_t levelCount = 1;
  uint32_t baseArrayLayer = 0;
  uint32_t layerCount = 1;

  AccessPattern access;
  ResourceLayout layout;

  bool operator==(const TextureView &other) const noexcept
  {
    return texture == other.texture && flags == other.flags && baseMipLevel == other.baseMipLevel && levelCount == other.levelCount && baseArrayLayer == other.baseArrayLayer &&
           layerCount == other.layerCount && access == other.access && layout == other.layout;
  }
  bool operator!=(const TextureView &other) const noexcept
  {
    return !(*this == other);
  }
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

struct RenderPassInfo
{
  std::string name;
  Viewport viewport;
  Rect2D scissor;

  std::vector<ColorAttachmentInfo> colorAttachments;
  DepthStencilAttachmentInfo *depthStencilAttachment = nullptr;
};

struct BindingBuffer
{
  BufferView bufferView;
  uint32_t binding;
};

struct BindingSampler
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

struct GroupInfo
{
  std::string name;
  std::vector<BindingBuffer> buffers;
  std::vector<BindingSampler> samplers;
  std::vector<BindingTextureInfo> textures;
  std::vector<BindingStorageTextureInfo> storageTextures;
};

struct BindingGroupsInfo
{
  std::string name;
  BindingsLayout layout;
  std::vector<GroupInfo> groups;
};

enum BufferBindingType
{
  BufferBindingType_UniformBuffer,
  BufferBindingType_StorageBuffer,
};

struct BindingGroupLayoutBufferEntry
{
  std::string name;
  uint32_t binding;
  BufferBindingType type;
  BindingVisibility visibility;
  bool isDynamic = false;
};

struct BindingGroupLayoutSamplerEntry
{
  std::string name;
  uint32_t binding;
  BindingVisibility visibility;
};

struct BindingGroupLayoutTextureEntry
{
  std::string name;
  uint32_t binding;
  BindingVisibility visibility;
  bool multisampled = false;
};

struct BindingGroupLayoutStorageTextureEntry
{
  std::string name;
  uint32_t binding;
  BindingVisibility visibility;
};

struct BindingGroupLayout
{
  std::vector<BindingGroupLayoutBufferEntry> buffers;
  std::vector<BindingGroupLayoutSamplerEntry> samplers;
  std::vector<BindingGroupLayoutTextureEntry> textures;
  std::vector<BindingGroupLayoutStorageTextureEntry> storageTextures;
};

struct BindingsLayoutInfo
{
  std::string name;
  std::vector<BindingGroupLayout> groups;
};
struct VertexLayoutElement
{
  std::string name;
  Type type;
  uint32_t binding;
  uint32_t offset;
  uint32_t location;
};
struct GraphicsPipelineVertexStage
{
  Shader vertexShader;
  std::string shaderEntry;

  std::vector<VertexLayoutElement> vertexLayoutElements;

  PrimitiveType primitiveType;
  PrimitiveCullType cullType;
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

struct GraphicsPipelineFragmentStage
{
  Shader fragmentShader;
  std::string shaderEntry;
  std::vector<ColorAttatchment> colorAttatchments;
  DepthAttatchment *depthAttatchment = nullptr;
};

struct GraphicsPipelineInfo
{
  std::string name;
  BindingsLayout layout;
  GraphicsPipelineVertexStage vertexStage;
  GraphicsPipelineFragmentStage fragmentStage;
};

struct ComputePipelineInfo
{
  std::string name;
  Shader shader;
  const char *entry;
  BindingsLayout layout;
};

enum ShaderType
{
  SpirV,
  WGSL,
};

struct ShaderInfo
{
  std::string name;
  std::string src;
  ShaderType type;
  BindingsLayout layout;
};

} // namespace rendering

template <> struct std::hash<rendering::Shader>
{
  std::size_t operator()(const rendering::Shader &b) const noexcept
  {
    return std::hash<std::string>{}(b.name);
  }
};

template <> struct std::hash<rendering::Buffer>
{
  std::size_t operator()(const rendering::Buffer &b) const noexcept
  {
    return std::hash<std::string>{}(b.name);
  }
};

template <> struct std::hash<rendering::BindingGroups>
{
  std::size_t operator()(const rendering::BindingGroups &b) const noexcept
  {
    return std::hash<std::string>{}(b.name);
  }
};

template <> struct std::hash<rendering::GraphicsPipeline>
{
  std::size_t operator()(const rendering::GraphicsPipeline &b) const noexcept
  {
    return std::hash<std::string>{}(b.name);
  }
};

template <> struct std::hash<rendering::BindingsLayout>
{
  std::size_t operator()(const rendering::BindingsLayout &b) const noexcept
  {
    return std::hash<std::string>{}(b.name);
  }
};
template <> struct std::hash<rendering::ComputePipeline>
{
  std::size_t operator()(const rendering::ComputePipeline &b) const noexcept
  {
    return std::hash<std::string>{}(b.name);
  }
};

template <> struct std::hash<rendering::Texture>
{
  std::size_t operator()(const rendering::Texture &t) const noexcept
  {
    return std::hash<std::string>{}(t.name);
  }
};
template <> struct std::hash<rendering::Sampler>
{
  std::size_t operator()(const rendering::Sampler &s) const noexcept
  {
    return std::hash<std::string>{}(s.name);
  }
};

// template <> struct std::hash<rendering::TextureView>
// {
//   std::size_t operator()(const rendering::TextureView &tv) const noexcept
//   {
//     std::size_t h = std::hash<rendering::Texture>{}(tv.texture);
//     rendering::hash_combine(h, std::hash<uint32_t>{}(static_cast<uint32_t>(tv.flags)));
//     rendering::hash_combine(h, std::hash<uint32_t>{}(tv.baseMipLevel));
//     rendering::hash_combine(h, std::hash<uint32_t>{}(tv.levelCount));
//     rendering::hash_combine(h, std::hash<uint32_t>{}(tv.baseArrayLayer));
//     rendering::hash_combine(h, std::hash<uint32_t>{}(tv.layerCount));
//     rendering::hash_combine(h, std::hash<int>{}(static_cast<int>(tv.access)));
//     rendering::hash_combine(h, std::hash<int>{}(static_cast<int>(tv.layout)));
//     return h;
//   }
// };
template <> struct std::hash<rendering::BufferView>
{
  std::size_t operator()(const rendering::BufferView &bv) const noexcept
  {
    std::size_t h = std::hash<rendering::Buffer>{}(bv.buffer);
    rendering::hash_combine(h, std::hash<uint32_t>{}(bv.offset));
    rendering::hash_combine(h, std::hash<uint32_t>{}(bv.size));
    rendering::hash_combine(h, std::hash<int>{}(static_cast<int>(bv.access)));
    return h;
  }
};


inline rendering::AccessPattern operator|(rendering::AccessPattern a, rendering::AccessPattern b)
{
  return static_cast<rendering::AccessPattern>(
      static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline bool operator&(rendering::AccessPattern a, rendering::AccessPattern b)
{
  return (static_cast<uint64_t>(a) & static_cast<uint64_t>(b)) != 0;
}

inline rendering::BufferUsage operator|(rendering::BufferUsage a, rendering::BufferUsage b)
{
  return static_cast<rendering::BufferUsage>(
      static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline bool operator&(rendering::BufferUsage a, rendering::BufferUsage b)
{
  return (static_cast<uint64_t>(a) & static_cast<uint64_t>(b)) != 0;
}


inline rendering::ImageUsage operator|(rendering::ImageUsage a, rendering::ImageUsage b)
{
  return static_cast<rendering::ImageUsage>(
      static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline bool operator&(rendering::ImageUsage a, rendering::ImageUsage b)
{
  return (static_cast<uint64_t>(a) & static_cast<uint64_t>(b)) != 0;
}