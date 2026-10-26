#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/material/VoxelMaterialFieldAtlas.hpp"
#include "voxel/material/VoxelShadingIR.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelGBufferPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer instances;
    ::rendering::Buffer materials;
    ::rendering::Buffer resolvedSurfaces;
    ::rendering::Buffer microMaterials;
    ::rendering::Buffer microPayloads;
    ::rendering::Buffer voxelShadingPrograms;
    ::rendering::Buffer voxelShadingInstructions;
    ::rendering::Buffer voxelShadingParameters;
    ::rendering::Buffer voxelMaterialFieldDefaults;
    ::rendering::Buffer materialSampleAddresses;
    ::rendering::Buffer voxelShadingPalettes;
  };

  struct FrameTarget
  {
    ::rendering::Texture instanceId;
    ::rendering::Texture voxelId;
    ::rendering::Texture materialId;
    ::rendering::Texture materialUv;
    ::rendering::Texture baseColor;
    ::rendering::Texture materialProperties;
    ::rendering::Texture emissive;
    ::rendering::Texture geometryNormal;
    ::rendering::Texture normal;
    ::rendering::Texture motionVector;
    ::rendering::Texture depth;
    // LOD-stable surface identity used by temporal consumers. voxelId remains
    // the selected hierarchy node for diagnostics and material boundaries.
    ::rendering::Texture temporalId;
  };

  struct MaterialFields
  {
    ::rendering::Buffer resolvers;
    ::rendering::Buffer mappings;
    ::rendering::Texture atlas;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU uniforms{};
    uint32_t instanceCapacity = 1u;
    uint32_t materialCapacity = 1u;
    float clearDepth = 0.0f;
    // Authoring overrides for roughness and metallic: value then blend, with a
    // blend of zero keeping whatever the import produced. MTL carries neither
    // reliably, so the imported values are usually a constant guess and being
    // able to sweep them without re-voxelising is what makes the lighting
    // model's response inspectable.
    std::array<float, 4> materialOverride{0.6f, 0.0f, 0.0f, 0.0f};
    // x blend strength, y bilateral depth sigma in pixel footprints, z
    // roughness increase for unresolved normal variance, w reserved.
    std::array<float, 4> normalFilter{0.65f, 2.0f, 0.5f, 0.0f};
    // x enabled, y start distance, z end distance, w normal strength.
    std::array<float, 4> distantFilter{1.0f, 250.0f, 1000.0f, 0.85f};
    // x albedo strength, y maximum screen-space radius, z plane sigma in
    // pixel footprints, w local-detail variance threshold.
    std::array<float, 4> distantFilterDetail{0.55f, 3.0f, 2.0f, 0.035f};
    // x enabled, y terrain/micro-voxel hue variation, z virtual height normal
    // strength, w maximum inward face recess in world metres.
    std::array<float, 4> microDetail{1.0f, 0.12f, 1.0f, 0.01f};
    // x finest world-space voxel size, y hard micro-face shadow strength,
    // z stable per-voxel brightness variation; w is reserved.
    std::array<float, 4> microGrid{0.05f, 0.55f, 0.24f, 0.0f};
    uint32_t microMaterialCapacity = 1u;
    uint32_t voxelShadingProgramCapacity = 1u;
    uint32_t voxelShadingInstructionCapacity = 1u;
    uint32_t voxelShadingParameterCapacity = 1u;
    uint32_t voxelMaterialFieldDefaultCapacity = 1u;
    uint32_t voxelMaterialFieldResolverCapacity = 1u;
    uint32_t voxelMaterialFieldMappingCapacity = 1u;
    uint32_t voxelShadingPaletteCapacity = 1u;
  };

  SparseVoxelGBufferPass(
      const Buffers buffers, const FrameTarget target,
      const MaterialFields materialFields, const Settings settings)
      : buffers_(buffers), target_(target), materialFields_(materialFields),
        settings_(settings)
  {
  }

  void updateUniforms(const SparseVoxelRasterUniformsGPU &uniforms)
  {
    settings_.uniforms = uniforms;
    settings_.uniforms.gbufferClearDepth = settings_.clearDepth;
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
    uploadMaterialSettings();
  }

  void updateMaterialOverride(const std::array<float, 4> &override)
  {
    settings_.materialOverride = override;
    uploadMaterialSettings();
  }

  void updateNormalFilter(const std::array<float, 4> &filter)
  {
    settings_.normalFilter = filter;
    uploadMaterialSettings();
  }

  void updateDistantFilter(
      const std::array<float, 4> &filter,
      const std::array<float, 4> &detail)
  {
    settings_.distantFilter = filter;
    settings_.distantFilterDetail = detail;
    uploadMaterialSettings();
  }

  void updateMicroDetail(
      const std::array<float, 4> &detail,
      const float finestVoxelSize,
      const float shadowStrength,
      const float brightnessVariation)
  {
    settings_.microDetail = detail;
    settings_.microGrid = {
      std::max(finestVoxelSize, 1.0e-6f),
      std::clamp(shadowStrength, 0.0f, 0.85f),
      std::clamp(brightnessVariation, 0.0f, 0.35f), 0.0f};
    uploadMaterialSettings();
  }

  ~SparseVoxelGBufferPass() override
  {
    if (graphicsPipeline_.isValid())
      renderGraph->deleteGraphicsPipeline(graphicsPipeline_);
    if (materialPipeline_.isValid())
      renderGraph->deleteComputePipeline(materialPipeline_);
    if (graphicsShader_.isValid())
      renderGraph->deleteShader(graphicsShader_);
    if (materialShader_.isValid())
      renderGraph->deleteShader(materialShader_);
    if (graphicsBindingGroups_.isValid())
      renderGraph->deleteBindingGroups(graphicsBindingGroups_);
    if (materialBindingGroups_.isValid())
      renderGraph->deleteBindingGroups(materialBindingGroups_);
    if (graphicsLayout_.isValid())
      renderGraph->deleteBindingsLayout(graphicsLayout_);
    if (materialLayout_.isValid())
      renderGraph->deleteBindingsLayout(materialLayout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    settings_.uniforms.gbufferClearDepth = settings_.clearDepth;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
    uploadMaterialSettings();

    commandBuffer.cmdBindComputePipeline(materialPipeline_);
    commandBuffer.cmdBindBindingGroups(materialBindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (settings_.uniforms.viewportTiles[0] + 7u) / 8u),
        std::max(1u, (settings_.uniforms.viewportTiles[1] + 7u) / 8u),
        1u);

    ::rendering::RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass";
    renderPass.scissor = ::rendering::Rect2D(0, 0, settings_.uniforms.viewportTiles[0], settings_.uniforms.viewportTiles[1]);
    renderPass.viewport = ::rendering::Viewport(settings_.uniforms.viewportTiles[0], settings_.uniforms.viewportTiles[1]);
    renderPass.colorAttachments = {
      colorAttachment("InstanceId", target_.instanceId),
      colorAttachment("VoxelId", target_.voxelId),
      colorAttachment("MaterialId", target_.materialId),
      colorAttachment("MaterialUv", target_.materialUv),
      colorAttachment("GeometryNormal", target_.geometryNormal),
      colorAttachment("MotionVector", target_.motionVector),
      colorAttachment("TemporalId", target_.temporalId),
    };
    renderPass.depthStencilAttachment = {
      .name = passName + "_Depth",
      .view = depthView(target_.depth),
      .clearDepth = settings_.clearDepth,
      .clearStencil = 0u,
      .enabled = true,
    };

    commandBuffer.cmdBindGraphicsPipeline(graphicsPipeline_);
    commandBuffer.cmdBeginRenderPass(renderPass);
    commandBuffer.cmdBindBindingGroups(graphicsBindingGroups_, nullptr, 0u);
    commandBuffer.cmdDraw(6u, 1u, 0u, 0u);
    commandBuffer.cmdEndRenderPass();
  }

private:
  struct alignas(16) MaterialSettingsGPU
  {
    std::array<float, 4> roughnessMetallic{};
    std::array<float, 4> normalFilter{};
    std::array<float, 4> distantFilter{};
    std::array<float, 4> distantFilterDetail{};
    std::array<float, 4> microDetail{};
    std::array<float, 4> microGrid{};
  };
  static_assert(sizeof(MaterialSettingsGPU) == 96u);

  void uploadMaterialSettings()
  {
    if (!materialOverrideBuffer_.isValid())
      return;
    const MaterialSettingsGPU values{
      settings_.materialOverride, settings_.normalFilter,
      settings_.distantFilter, settings_.distantFilterDetail,
      settings_.microDetail, settings_.microGrid};
    renderGraph->bufferWrite(
        materialOverrideBuffer_, 0u, sizeof(values),
        const_cast<MaterialSettingsGPU *>(&values));
  }

  static uint64_t capacityBytes(const uint32_t count, const uint64_t stride)
  {
    return std::max<uint64_t>(1u, count) * stride;
  }

  ::rendering::TextureView storageView(const ::rendering::Texture texture) const
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_WRITE,
      .layout = ::rendering::ResourceLayout::GENERAL,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  ::rendering::TextureView colorView(const ::rendering::Texture texture) const
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::COLOR_ATTACHMENT_WRITE,
      .layout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  ::rendering::TextureView sampledView(
      const ::rendering::Texture texture) const
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  ::rendering::TextureView depthView(const ::rendering::Texture texture) const
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ | ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
      .layout = ::rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Depth,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  ::rendering::ColorAttachmentInfo colorAttachment(const char *suffix, const ::rendering::Texture texture) const
  {
    return {
      .name = passName + "_" + suffix,
      .view = colorView(texture),
      .clearValue = ::rendering::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f),
    };
  }

  void ensureResources()
  {
    if (graphicsLayout_.isValid())
      return;

    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms.buffer", .size = sizeof(SparseVoxelRasterUniformsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    materialOverrideBuffer_ = renderGraph->createBuffer(
        // The pass owns one cached binding group while these renderer settings
        // are shared by every in-flight frame. A frame-local allocation made
        // bufferWrite update a different physical instance than the one bound
        // by the cached group, leaving microGrid/variation at zero.
        {.name = passName + "_MaterialOverride.buffer", .size = sizeof(MaterialSettingsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = false});
    graphicsLayout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_GraphicsLayout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "instances", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "materials", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "resolvedSurfaces", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "materialOverride", .binding = 8u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "microMaterials", .binding = 9u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
           }}}});

    materialLayout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_MaterialLayout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "instances", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "materials", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "resolvedSurfaces", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "materialOverride", .binding = 8u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "microMaterials", .binding = 9u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "microPayloads", .binding = 10u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "voxelShadingPrograms", .binding = 11u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "voxelShadingInstructions", .binding = 12u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "voxelShadingParameters", .binding = 13u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "voxelMaterialFieldDefaults", .binding = 14u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "materialSampleAddresses", .binding = 15u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "voxelMaterialFieldResolvers", .binding = 16u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "voxelMaterialFieldMappings", .binding = 17u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "voxelShadingPalettes", .binding = 19u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .textures = {
             {.name = "voxelMaterialFieldAtlas", .binding = 18u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .storageTextures = {
             {.name = "baseColorTexture", .binding = 4u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "materialPropertiesTexture", .binding = 5u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "emissiveTexture", .binding = 6u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "normalTexture", .binding = 7u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           }}}});

    const auto bufferView = [](const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
    {
      return ::rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
    };
    const uint32_t pixelCapacity = std::max(1u, settings_.uniforms.viewportTiles[0] * settings_.uniforms.viewportTiles[1]);
    const std::vector<::rendering::BindingBuffer> commonBuffers = {
      {.binding = 0u, .bufferView = bufferView(uniformBuffer_, sizeof(SparseVoxelRasterUniformsGPU), ::rendering::AccessPattern::UNIFORM_READ)},
      {.binding = 1u, .bufferView = bufferView(buffers_.instances, capacityBytes(settings_.instanceCapacity, sizeof(SparseVoxelInstanceGPU)), ::rendering::AccessPattern::SHADER_READ)},
      {.binding = 2u, .bufferView = bufferView(buffers_.materials, capacityBytes(settings_.materialCapacity, sizeof(SparseVoxelMaterialGPU)), ::rendering::AccessPattern::SHADER_READ)},
      {.binding = 3u, .bufferView = bufferView(buffers_.resolvedSurfaces, capacityBytes(pixelCapacity, sizeof(ResolvedVoxelSurfaceGPU)), ::rendering::AccessPattern::SHADER_READ)},
    };
    std::vector<::rendering::BindingBuffer> graphicsBuffers = commonBuffers;
    graphicsBuffers.push_back(
      {.binding = 8u, .bufferView = bufferView(materialOverrideBuffer_, sizeof(MaterialSettingsGPU), ::rendering::AccessPattern::UNIFORM_READ)});
    graphicsBuffers.push_back(
      {.binding = 9u, .bufferView = bufferView(buffers_.microMaterials, capacityBytes(settings_.microMaterialCapacity, sizeof(SparseVoxelMicroMaterialGPU)), ::rendering::AccessPattern::SHADER_READ)});
    std::vector<::rendering::BindingBuffer> materialBuffers = commonBuffers;
    materialBuffers.push_back(
      {.binding = 8u, .bufferView = bufferView(materialOverrideBuffer_, sizeof(MaterialSettingsGPU), ::rendering::AccessPattern::UNIFORM_READ)});
    materialBuffers.push_back(
      {.binding = 9u, .bufferView = bufferView(buffers_.microMaterials, capacityBytes(settings_.microMaterialCapacity, sizeof(SparseVoxelMicroMaterialGPU)), ::rendering::AccessPattern::SHADER_READ)});
    materialBuffers.push_back(
      {.binding = 10u, .bufferView = bufferView(buffers_.microPayloads, capacityBytes(pixelCapacity, sizeof(uint32_t)), ::rendering::AccessPattern::SHADER_READ)});
    materialBuffers.push_back(
      {.binding = 11u, .bufferView = bufferView(buffers_.voxelShadingPrograms, capacityBytes(settings_.voxelShadingProgramCapacity, sizeof(voxel::material::VoxelShadingProgramGPU)), ::rendering::AccessPattern::SHADER_READ)});
    materialBuffers.push_back(
      {.binding = 12u, .bufferView = bufferView(buffers_.voxelShadingInstructions, capacityBytes(settings_.voxelShadingInstructionCapacity, sizeof(voxel::material::VoxelShadingInstructionGPU)), ::rendering::AccessPattern::SHADER_READ)});
    materialBuffers.push_back(
      {.binding = 13u, .bufferView = bufferView(buffers_.voxelShadingParameters, capacityBytes(settings_.voxelShadingParameterCapacity, sizeof(voxel::material::VoxelShadingParameterBlockGPU)), ::rendering::AccessPattern::SHADER_READ)});
    materialBuffers.push_back(
      {.binding = 14u, .bufferView = bufferView(buffers_.voxelMaterialFieldDefaults, capacityBytes(settings_.voxelMaterialFieldDefaultCapacity, sizeof(voxel::material::VoxelMaterialFieldDefaultsGPU)), ::rendering::AccessPattern::SHADER_READ)});
    materialBuffers.push_back(
      {.binding = 15u, .bufferView = bufferView(
          buffers_.materialSampleAddresses,
          capacityBytes(pixelCapacity, sizeof(VoxelMaterialSampleAddressGPU)),
          ::rendering::AccessPattern::SHADER_READ)});
    materialBuffers.push_back(
      {.binding = 16u, .bufferView = bufferView(
          materialFields_.resolvers,
          capacityBytes(
              settings_.voxelMaterialFieldResolverCapacity,
              sizeof(voxel::material::VoxelMaterialFieldResolverGPU)),
          ::rendering::AccessPattern::SHADER_READ)});
    materialBuffers.push_back(
      {.binding = 17u, .bufferView = bufferView(
          materialFields_.mappings,
          capacityBytes(
              settings_.voxelMaterialFieldMappingCapacity,
              sizeof(voxel::material::VoxelMaterialFieldMappingGPU)),
          ::rendering::AccessPattern::SHADER_READ)});
    materialBuffers.push_back(
      {.binding = 19u, .bufferView = bufferView(
          buffers_.voxelShadingPalettes,
          capacityBytes(
              settings_.voxelShadingPaletteCapacity,
              sizeof(voxel::material::VoxelShadingPaletteGPU)),
          ::rendering::AccessPattern::SHADER_READ)});
    graphicsBindingGroups_ = renderGraph->createBindingGroups(
        {.layout = graphicsLayout_,
         .name = passName + "_GraphicsGroups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = graphicsBuffers,
         }}});
    materialBindingGroups_ = renderGraph->createBindingGroups(
        {.layout = materialLayout_,
         .name = passName + "_MaterialGroups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = materialBuffers,
           .textures = {
             {.textureView = sampledView(materialFields_.atlas), .binding = 18u},
           },
           .storageTextures = {
             {.textureView = storageView(target_.baseColor), .binding = 4u},
             {.textureView = storageView(target_.materialProperties), .binding = 5u},
             {.textureView = storageView(target_.emissive), .binding = 6u},
             {.textureView = storageView(target_.normal), .binding = 7u},
           }}}});

    graphicsShader_ = renderGraph->createShader(
        {.name = passName + "_GraphicsShader", .layout = graphicsLayout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-gbuffer.spirv"), .type = ::rendering::ShaderType::SpirV});
    materialShader_ = renderGraph->createShader(
        {.name = passName + "_MaterialShader", .layout = materialLayout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-material-gbuffer.spirv"), .type = ::rendering::ShaderType::SpirV});
    materialPipeline_ = renderGraph->createComputePipeline(
        {.entry = "writeVoxelMaterialGBuffer", .layout = materialLayout_, .name = passName + "_MaterialPipeline", .shader = materialShader_});

    ::rendering::DepthAttatchment depthAttachment{};
    depthAttachment.enabled = true;
    depthAttachment.comparison = ::rendering::ComparisonOp::ComparisonOp_Always;
    depthAttachment.format = ::rendering::Format::Format_Depth32Float;
    depthAttachment.loadOp = ::rendering::LoadOp::LoadOp_Clear;
    depthAttachment.storeOp = ::rendering::StoreOp::StoreOp_Store;

    ::rendering::GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_Pipeline";
    pipelineInfo.layout = graphicsLayout_;
    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = graphicsShader_;
    pipelineInfo.vertexStage.shaderEntry = "writeVoxelGBufferVertex";
    pipelineInfo.vertexStage.cullType = ::rendering::CullMode::None;
    pipelineInfo.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipelineInfo.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.fragmentShader = graphicsShader_;
    pipelineInfo.fragmentStage.shaderEntry = "writeVoxelGBuffer";
    pipelineInfo.fragmentStage.colorAttatchments = {
      colorPipelineAttachment(::rendering::Format::Format_R32Uint),
      colorPipelineAttachment(::rendering::Format::Format_R32Uint),
      colorPipelineAttachment(::rendering::Format::Format_R32Uint),
      colorPipelineAttachment(::rendering::Format::Format_RG32Float),
      colorPipelineAttachment(::rendering::Format::Format_RGBA16Float),
      colorPipelineAttachment(::rendering::Format::Format_RG16Float),
      colorPipelineAttachment(::rendering::Format::Format_R32Uint),
    };
    pipelineInfo.fragmentStage.depthAttatchment = depthAttachment;
    graphicsPipeline_ = renderGraph->createGraphicsPipeline(pipelineInfo);
  }

  static ::rendering::ColorAttatchment colorPipelineAttachment(const ::rendering::Format format)
  {
    return {
      .format = format,
      .loadOp = ::rendering::LoadOp::LoadOp_Clear,
      .storeOp = ::rendering::StoreOp::StoreOp_Store,
      .blendMode = ::rendering::ColorBlendMode::Replace,
      .initialLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .finalLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
    };
  }

  Buffers buffers_{};
  FrameTarget target_{};
  MaterialFields materialFields_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::Buffer materialOverrideBuffer_{};
  ::rendering::BindingsLayout graphicsLayout_{};
  ::rendering::BindingsLayout materialLayout_{};
  ::rendering::BindingGroups graphicsBindingGroups_{};
  ::rendering::BindingGroups materialBindingGroups_{};
  ::rendering::Shader graphicsShader_{};
  ::rendering::Shader materialShader_{};
  ::rendering::ComputePipeline materialPipeline_{};
  ::rendering::GraphicsPipeline graphicsPipeline_{};
};

} // namespace voxel::svraster::rendering
