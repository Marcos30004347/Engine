#pragma once

#include <cstdint>
#include <vector>
#include "window/window.hpp"

namespace rhi {

enum class BufferHandle : std::uint32_t {};
enum class TextureHandle : std::uint32_t {};
enum class CommandBufferHandle : std::uint32_t {};
enum class ShaderHandle : std::uint32_t {};
enum class RenderPipelineHandle : std::uint32_t {};
enum class ComputePipelineHandle : std::uint32_t {};
enum class SurfaceHandle : std::uint32_t {};
enum class FenceHandle : std::uint32_t {};

template<typename Tag>
struct Handle
{
  std::int64_t value;
};

// using BufferHandle = Handle<struct BufferTag>;
// using Texturehandle = Handle<struct TextureTag>;
enum BufferUsage {
    BufferUsage_Uniform,
    BufferUsage_Storage,
    BufferUsage_Push,
    BufferUsage_Pull,
    BufferUsage_Vertex,
    BufferUsage_Indirect,
    BufferUsage_Timestamp,
};

enum PrimitiveCullType {
    PrimitiveCullType_None,
    PrimitiveCullType_CCW,
    PrimitiveCullType_CW,
};

enum PrimitiveType {
    PrimitiveType_Triangles,
    PrimitiveType_Points,
    PrimitiveType_Lines,
};

enum TextureFormat {
    R32_G32_B32_A32,
    R8_G8_B8_A8,
};

enum ShaderType {
    ShaderType_VertexShader,
    ShaderType_FragmentShader,
    ShaderType_ComputeShader,
};

struct SpirVShaderData {
    const char* src;
    const char* entry;
};

struct ShaderCreateData {
    SpirVShaderData spirVShaderData;
};

struct LayoutElement {
    size_t stride;
    size_t count;
    size_t size;
};

struct BindingDefinition {
    BufferUsage usage;
};

struct BindingDefinitions {
    BindingDefinition* bindings;
    size_t bindingsCount;
};

struct RenderPipelineVertexStage {
    ShaderHandle vertexShader;

    LayoutElement* vertexLayoutElements;
    size_t vertexLayoutElementsCount;

    PrimitiveType primitiveType;
    PrimitiveCullType cullType;

    BindingDefinitions* bindings;
    size_t bindingsCount;
};

struct RenderPipelineFragmentStage {
    ShaderHandle fragmentShader;
    BindingDefinitions* bindings;
    size_t bindingsCount;
};

struct RenderPipelineData {
    RenderPipelineVertexStage vertexStage;
    RenderPipelineFragmentStage fragmentStage;

    BindingDefinitions* bindings;
    size_t bindingsCount;
};

struct ComputePipelineData {
    ShaderHandle shader;

    BindingDefinitions* bindings;
    size_t bindingsCount;
};

enum DeviceBackend {
    DeviceBackend_Vulkan1_2,
};

enum DeviceFeatures {
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
};

struct DeviceRequiredLimits {
    size_t minimumMemory;
    size_t minimumComputeSharedMemory;
    size_t minimumComputeWorkGroupInvocations;
};

struct DeviceProperties {
    size_t sugroupSize;
    size_t maxMemory;
    size_t maxComputeSharedMemorySize;
    size_t maxComputeWorkGroupInvocations;
};

class Device {
    public:

    std::uint64_t featureFlags;
    DeviceProperties properties;

    static Device* create(DeviceBackend, DeviceRequiredLimits, DeviceFeatures);

    // virtual BufferHandle createBuffer(size_t size, BufferUsage, void* data) = 0;
    // virtual void destroyBuffer(BufferHandle) = 0;
    // virtual const void* mapBufferRead(BufferHandle) = 0;
    // virtual void* mapBufferWrite(BufferHandle) = 0;
    // virtual const void unmap(BufferHandle) = 0;

    // virtual SurfaceHandle createSurface(Window* window) = 0;
    // virtual void destroySurface(SurfaceHandle) = 0;

    // virtual TextureHandle createTexture(size_t size, TextureFormat, void* data) = 0;
    // virtual void destroyTexture(TextureHandle) = 0;

    // virtual CommandBufferHandle createCommandBuffer() = 0;
    // virtual void destroyCommandBuffer(CommandBufferHandle) = 0;

    // virtual RenderPipelineHandle createRenderPipeline(RenderPipelineData*) = 0;
    // virtual void destroyRenderPipeline(RenderPipelineHandle) = 0;

    // virtual ComputePipelineHandle createComputePipelineHandlePipeline(ComputePipelineData*) = 0;
    // virtual void destroyComputePipelineHandlePipeline(ComputePipelineHandle) = 0;

    // virtual void drawCommand(RenderPipelineHandle, size_t triangles, TextureHandle depthTexture) = 0; 
    // virtual void drawIndexedCommand(RenderPipelineHandle, size_t instancesCount, size_t firstInstance, TextureHandle depthTexture) = 0; 
    // virtual void multiDrawIndexedIndirectCommand(RenderPipelineHandle, BufferHandle indirect, BufferHandle count, size_t maxDrawCount, TextureHandle depthTexture) = 0;
    
    // virtual void dispatchCommand(ComputePipelineHandle, size_t workGroupX, size_t workGroupY, size_t workGroupZ) = 0; 
    // virtual void dispatchIndirectCommand(ComputePipelineHandle, BufferHandle workGroupSizeBuffer) = 0; 

    // virtual ShaderHandle createShader(ShaderCreateData) = 0;
    // virtual void destroyShader(ShaderHandle) = 0;

    // virtual FenceHandle submitCommandBuffer(CommandBufferHandle);
    // virtual void wait(FenceHandle);
};

}