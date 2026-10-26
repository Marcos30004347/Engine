#pragma once

#include "procedural/PolygonTerrain.hpp"
#include "procedural/SphericalVoronoiMesh.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <backends/imgui_impl_vulkan.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace polygonterrain
{

namespace gpu = rendering;
namespace vkbackend = rendering::backend::vulkan;

class TerrainPreviewRHI final : public vkbackend::VulkanRHI
{
public:
  using vkbackend::VulkanRHI::VulkanRHI;
  using vkbackend::VulkanRHI::allocateCommandBuffers;
  using vkbackend::VulkanRHI::beginCommandBuffer;
  using vkbackend::VulkanRHI::cmdBindBindingGroups;
  using vkbackend::VulkanRHI::cmdBindComputePipeline;
  using vkbackend::VulkanRHI::cmdDispatch;
  using vkbackend::VulkanRHI::cmdImageBarrier;
  using vkbackend::VulkanRHI::endCommandBuffer;
  using vkbackend::VulkanRHI::submit;

  vkbackend::VulkanTextureView createSampledTextureView(gpu::TextureId texture);
  vkbackend::VulkanTextureView createDepthTextureView(gpu::TextureId texture);
  void destroySampledTextureView(vkbackend::VulkanTextureView &view);
  VkSampler samplerHandle(gpu::SamplerId sampler);
  VkBuffer bufferHandle(gpu::BufferId buffer);
  VkDeviceSize bufferSize(gpu::BufferId buffer);
  bool readTextureRGBA32F(gpu::TextureId texture, uint32_t width, uint32_t height, std::vector<float> &rgba);
};

struct GpuPreviewTexture
{
  gpu::TextureId texture = gpu::TextureId::Invalid;
  vkbackend::VulkanTextureView view{};
  VkDescriptorSet descriptor = VK_NULL_HANDLE;

  bool valid() const
  {
    return descriptor != VK_NULL_HANDLE;
  }
};

void destroyGpuPreviewTexture(TerrainPreviewRHI &rhi, GpuPreviewTexture &texture);

struct PreviewTextureSet
{
  GpuPreviewTexture height;
  GpuPreviewTexture rivers;
  GpuPreviewTexture regions;
  GpuPreviewTexture materials;
  GpuPreviewTexture terrainForms;
  GpuPreviewTexture temperature;
  GpuPreviewTexture precipitation;
  GpuPreviewTexture wind;
  GpuPreviewTexture forests;
  GpuPreviewTexture settlements;
  GpuPreviewTexture continents;
  GpuPreviewTexture tectonics;
  GpuPreviewTexture mountains;
  GpuPreviewTexture sphere;

  void reset(TerrainPreviewRHI *rhi = nullptr);
};

class TerrainPreviewTextureUploadPass
{
public:
  TerrainPreviewTextureUploadPass(TerrainPreviewRHI &rhi, gpu::SamplerId sampler);

  GpuPreviewTexture uploadRGBA(const std::string &name, uint32_t width, uint32_t height, const std::vector<uint8_t> &rgba);

private:
  TerrainPreviewRHI &rhi_;
  gpu::SamplerId sampler_ = gpu::SamplerId::Invalid;
};

// Converts the compact generated maps into all coloured ImGui preview atlases
// in two compute dispatches. The CPU/headless path retains the source maps for
// export, while interactive preview shading and relief sampling stay on-GPU.
class TerrainPreviewRasterPass
{
public:
  TerrainPreviewRasterPass(TerrainPreviewRHI &rhi, gpu::SamplerId sampler, std::string shaderPath);
  ~TerrainPreviewRasterPass();

  TerrainPreviewRasterPass(const TerrainPreviewRasterPass &) = delete;
  TerrainPreviewRasterPass &operator=(const TerrainPreviewRasterPass &) = delete;

  bool rasterize(
      const procedural::PolygonTerrainMaps &maps,
      const std::vector<uint8_t> &mountains,
      PreviewTextureSet &textures,
      std::string &error);
  void destroy();

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> dimensions{};
  };

  static gpu::TextureView sampledView(gpu::TextureId texture);
  static gpu::TextureView storageView(gpu::TextureId texture);
  gpu::TextureId createInput(const std::string &name, uint32_t width, uint32_t height, const std::vector<uint8_t> &rgba);
  GpuPreviewTexture createOutput(const std::string &name, uint32_t width, uint32_t height);
  void initializePipeline();

  TerrainPreviewRHI &rhi_;
  gpu::SamplerId sampler_ = gpu::SamplerId::Invalid;
  std::string shaderPath_;
  gpu::BindingsLayoutId bindingsLayoutId_ = gpu::BindingsLayoutId::Invalid;
  gpu::ShaderId shaderId_ = gpu::ShaderId::Invalid;
  gpu::ComputePipelineId pipelineId_ = gpu::ComputePipelineId::Invalid;
};

struct TerrainPreviewHydraulicErosionSettings
{
  uint32_t iterations = 128u;
  uint32_t iterationsPerBatch = 4u;
  float timeStep = 0.10f;
  float rainfall = 0.00028f;
  float evaporation = 0.018f;
  float gravity = 1.20f;
  float sedimentCapacity = 5.0f;
  float depositionRate = 0.16f;
  float erosionRate = 0.065f;
  float minimumSlope = 0.0015f;
};

class TerrainPreviewHydraulicErosionPass
{
public:
  TerrainPreviewHydraulicErosionPass(TerrainPreviewRHI &rhi, std::string shaderPath);
  ~TerrainPreviewHydraulicErosionPass();

  TerrainPreviewHydraulicErosionPass(const TerrainPreviewHydraulicErosionPass &) = delete;
  TerrainPreviewHydraulicErosionPass &operator=(const TerrainPreviewHydraulicErosionPass &) = delete;

  bool start(
      uint32_t width,
      uint32_t height,
      const std::vector<uint16_t> &heightmap,
      const std::vector<uint8_t> &precipitation,
      const std::vector<uint8_t> &tectonics,
      const TerrainPreviewHydraulicErosionSettings &settings,
      std::string &error);
  bool poll(std::vector<uint16_t> &erodedHeightmap, std::vector<uint8_t> &erosionFlowMap, std::string &error);
  void cancel();
  void destroy();

  bool running() const { return running_; }
  float progress() const;
  uint32_t completedIterations() const { return completedIterations_; }
  uint32_t totalIterations() const { return settings_.iterations; }

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> dimensions{};
    std::array<float, 4> water{};
    std::array<float, 4> sediment{};
    std::array<float, 4> terrain{};
  };

  static gpu::TextureView sampledView(gpu::TextureId texture);
  static gpu::TextureView storageView(gpu::TextureId texture);
  void createResources(
      const std::vector<uint16_t> &heightmap,
      const std::vector<uint8_t> &precipitation,
      const std::vector<uint8_t> &tectonics);
  void createBindings();
  void submitBatch();
  bool readResult(std::vector<uint16_t> &heightmap, std::vector<uint8_t> &flowMap, std::string &error);

  TerrainPreviewRHI &rhi_;
  std::string shaderPath_;
  TerrainPreviewHydraulicErosionSettings settings_{};
  uint32_t width_ = 0u;
  uint32_t height_ = 0u;
  uint32_t completedIterations_ = 0u;
  uint32_t submittedIterations_ = 0u;
  uint32_t currentFlux_ = 0u;
  bool running_ = false;
  bool cancelRequested_ = false;
  gpu::GPUFuture future_{};
  gpu::BufferId uniformBuffer_ = gpu::BufferId::Invalid;
  std::array<gpu::TextureId, 2> stateTextures_{gpu::TextureId::Invalid, gpu::TextureId::Invalid};
  std::array<gpu::TextureId, 2> fluxTextures_{gpu::TextureId::Invalid, gpu::TextureId::Invalid};
  gpu::TextureId climateTexture_ = gpu::TextureId::Invalid;
  gpu::BindingsLayoutId bindingsLayoutId_ = gpu::BindingsLayoutId::Invalid;
  gpu::ShaderId shaderId_ = gpu::ShaderId::Invalid;
  std::array<gpu::ComputePipelineId, 3> pipelineIds_{gpu::ComputePipelineId::Invalid, gpu::ComputePipelineId::Invalid, gpu::ComputePipelineId::Invalid};
  std::array<gpu::BindingGroupsId, 6> bindingGroupIds_{
      gpu::BindingGroupsId::Invalid, gpu::BindingGroupsId::Invalid, gpu::BindingGroupsId::Invalid,
      gpu::BindingGroupsId::Invalid, gpu::BindingGroupsId::Invalid, gpu::BindingGroupsId::Invalid};
};

class TerrainPreviewImGuiOverlayPass
{
public:
  explicit TerrainPreviewImGuiOverlayPass(TerrainPreviewRHI &rhi);
  ~TerrainPreviewImGuiOverlayPass();

  TerrainPreviewImGuiOverlayPass(const TerrainPreviewImGuiOverlayPass &) = delete;
  TerrainPreviewImGuiOverlayPass &operator=(const TerrainPreviewImGuiOverlayPass &) = delete;

  void initialize(gpu::SwapChain swapChain);
  void destroy();
  void destroyFramebuffers();
  void onSwapChainRecreated(gpu::SwapChain swapChain);
  // preserveColor loads the swapchain image instead of clearing it, for frames
  // where the adaptive globe's RenderGraph has already drawn into it. The two
  // render passes differ only in load op, so they stay render-pass compatible
  // and the single ImGui pipeline built against renderPass() works with both.
  void render(
      gpu::SwapChain &swapChain,
      const gpu::TextureView &swapChainView,
      ImDrawData *drawData,
      const std::function<void(VkCommandBuffer, VkExtent2D)> &beforeImGui = {},
      bool preserveColor = false);
  static void checkVkResult(VkResult result);
  VkRenderPass renderPass() const
  {
    return renderPass_;
  }

private:
  struct FramebufferResources
  {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    gpu::TextureId depthTexture = gpu::TextureId::Invalid;
    vkbackend::VulkanTextureView depthView{};
  };

  static VkRenderPass createRenderPass(VkDevice device, VkFormat swapChainFormat, bool preserveColor);

  TerrainPreviewRHI &rhi_;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  VkRenderPass preserveRenderPass_ = VK_NULL_HANDLE;
  std::unordered_map<uintptr_t, FramebufferResources> framebuffers_;
};

struct TerrainPreviewSphereSettings
{
  float yawDegrees = -25.0f;
  float pitchDegrees = -12.0f;
  float zoom = 1.0f;
  float lightX = -0.38f;
  float lightY = 0.44f;
  float lightZ = 0.82f;
  float lighting = 1.0f;
  float showPlateBoundaries = 1.0f;
  float showOcean = 1.0f;
};

struct TerrainPreviewSphereLine
{
  std::array<float, 3> a{};
  std::array<float, 3> b{};
  std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
  float width = 0.003f;
};

class TerrainPreviewSpherePass
{
public:
  TerrainPreviewSpherePass(TerrainPreviewRHI &rhi, gpu::SamplerId sampler, std::string shaderPath);
  ~TerrainPreviewSpherePass();

  TerrainPreviewSpherePass(const TerrainPreviewSpherePass &) = delete;
  TerrainPreviewSpherePass &operator=(const TerrainPreviewSpherePass &) = delete;

  void initialize(VkRenderPass renderPass);
  void destroy();
  void setAtlases(const GpuPreviewTexture &atlas, const GpuPreviewTexture &regions);
  void setSurfaceMesh(
      const std::vector<procedural::SpherePoint> &positions,
      const std::vector<uint16_t> &elevations,
      const std::vector<uint32_t> &indices,
      const std::vector<uint32_t> *colors = nullptr);
  void setDirectAtlas(uint32_t width, uint32_t height, const std::vector<uint8_t> &rgba, const std::vector<uint16_t> *heightmap = nullptr);
  void setOverlayLines(const std::vector<TerrainPreviewSphereLine> &lines);
  void render(VkCommandBuffer commandBuffer, VkExtent2D extent, const TerrainPreviewSphereSettings &settings);
  bool ready() const;

private:
  struct Vertex
  {
    float position[3];
    float uv[2];
    float normal[3];
    float color[4];
  };

  struct Uniforms
  {
    std::array<float, 16> mvp{};
    std::array<float, 16> model{};
    std::array<float, 4> lightAndLighting{};
    std::array<float, 4> atlasInfo{};
  };

  static std::string readBinaryFile(const std::string &path);
  static VkShaderModule createShaderModule(VkDevice device, const std::string &spirv);
  static void buildIcosphere(uint32_t subdivisions, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices);
  static Uniforms makeUniforms(const TerrainPreviewSphereSettings &settings, VkExtent2D extent);
  void createMesh();
  void createDescriptors();
  void createPipeline(VkRenderPass renderPass);
  void updateDescriptorImage();

  TerrainPreviewRHI &rhi_;
  gpu::SamplerId sampler_ = gpu::SamplerId::Invalid;
  std::string shaderPath_;
  gpu::BufferId vertexBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId indexBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId uniformBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId overlayVertexBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId overlayIndexBuffer_ = gpu::BufferId::Invalid;
  std::vector<Vertex> vertices_;
  uint32_t indexCount_ = 0u;
  uint32_t overlayIndexCount_ = 0u;
  VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkShaderModule shaderModule_ = VK_NULL_HANDLE;
  VkImageView atlasView_ = VK_NULL_HANDLE;
  VkImageView regionsView_ = VK_NULL_HANDLE;
};

} // namespace polygonterrain
