#pragma once

#include "procedural/geometry/TerrainMesh.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <backends/imgui_impl_vulkan.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace lowpoly
{

namespace gpu = rendering;
namespace vkbackend = rendering::backend::vulkan;

class TerrainRHI final : public vkbackend::VulkanRHI
{
public:
  using vkbackend::VulkanRHI::VulkanRHI;

  vkbackend::VulkanTextureView createDepthTextureView(gpu::TextureId texture);
  void destroyDepthTextureView(vkbackend::VulkanTextureView &view);
  VkBuffer bufferHandle(gpu::BufferId buffer);
  void writeChecked(gpu::BufferId buffer, uint64_t bytes, const void *data);
};

// Colour plus depth, and the ImGui overlay in the same render pass.
//
// Depth clears to zero and compares GREATER: reverse-Z. A 128 km world viewed
// from two metres and from fifty kilometres in one session is exactly what eats
// a forward-Z buffer, and terraced ground makes the fighting maximally visible.
class FramePass
{
public:
  explicit FramePass(TerrainRHI &rhi);
  ~FramePass();

  FramePass(const FramePass &) = delete;
  FramePass &operator=(const FramePass &) = delete;

  void initialize(gpu::SwapChain swapChain);
  void destroy();
  void destroyFramebuffers();
  void onSwapChainRecreated(gpu::SwapChain swapChain);
  void render(
      gpu::SwapChain &swapChain,
      const gpu::TextureView &swapChainView,
      ImDrawData *drawData,
      const std::function<void(VkCommandBuffer, VkExtent2D)> &beforeImGui = {});

  static void check(VkResult result);
  VkRenderPass renderPass() const
  {
    return renderPass_;
  }

private:
  struct Resources
  {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    gpu::TextureId depthTexture = gpu::TextureId::Invalid;
    vkbackend::VulkanTextureView depthView{};
  };

  static VkRenderPass createRenderPass(VkDevice device, VkFormat format);

  TerrainRHI &rhi_;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  std::unordered_map<uintptr_t, Resources> framebuffers_;
};

struct TerrainCamera
{
  // World metres. y is up.
  float x = 0.0f;
  float y = 3000.0f;
  float z = 0.0f;
  float yawRadians = 0.0f;
  float pitchRadians = -0.5f;
  // Log-scaled by the wheel; metres per second.
  float speedMetresPerSecond = 400.0f;
  // Scale the speed by height above the ground, so the same input works at two
  // metres and at a hundred kilometres without constant readjustment.
  bool altitudeRelativeSpeed = true;
  float groundHeightMetres = 0.0f;

  std::array<float, 3> forward() const;
  // Column-major, ready for the uniform block.
  std::array<float, 16> viewProjection(float aspect) const;
  float effectiveSpeed() const;
};

// One draw per tile, one pipeline, no culling beyond the frustum test the
// driver already does. Everything the sample needs and nothing it does not.
class TerrainMeshPass
{
public:
  TerrainMeshPass(TerrainRHI &rhi, std::string shaderPath);
  ~TerrainMeshPass();

  TerrainMeshPass(const TerrainMeshPass &) = delete;
  TerrainMeshPass &operator=(const TerrainMeshPass &) = delete;

  void initialize(VkRenderPass renderPass);
  void destroy();
  bool upload(const procedural::geometry::TerrainMesh &mesh, std::string *error);
  void setFlatShading(bool value) { flatShading_ = value; }
  void setUploadBudget(uint64_t bytes) { uploadBudget_ = bytes; }
  // Packed RGBA8 for a CellType. Set before uploading; the pass has no opinion
  // about what the map's material bytes mean.
  void setMaterialColour(std::function<uint32_t(uint8_t, double, double)> colour)
  {
    materialColour_ = std::move(colour);
  }
  void render(
      VkCommandBuffer commandBuffer,
      VkExtent2D extent,
      const TerrainCamera &camera,
      bool wireframe);

  bool ready() const
  {
    return indexCount_ != 0u;
  }
  size_t vertexCount() const
  {
    return vertexCount_;
  }
  size_t triangleCount() const
  {
    return indexCount_ / 3u;
  }

private:
  struct Uniforms
  {
    std::array<float, 16> viewProjection{};
    std::array<float, 4> sunDirection{};
    std::array<float, 4> skyColour{};
    std::array<float, 4> groundColour{};
    std::array<float, 4> options{};
  };

  struct Vertex
  {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32_t colour = 0u;
    // Averaged over every face meeting this world position, across tiles. The
    // colour stays hard per face - a field is one material - while the shading
    // stops reporting every tiny triangle as its own facet.
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
  };

  void createDescriptors();
  void createPipeline(VkRenderPass renderPass);
  void destroyMesh();

  bool flatShading_ = false;
  uint64_t uploadBudget_ = 256ull * 1024 * 1024;
  TerrainRHI &rhi_;
  std::string shaderPath_;
  std::function<uint32_t(uint8_t, double, double)> materialColour_;
  gpu::BufferId vertexBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId indexBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId uniformBuffer_ = gpu::BufferId::Invalid;
  size_t vertexCount_ = 0u;
  size_t indexCount_ = 0u;
  VkShaderModule shaderModule_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkPipeline wirePipeline_ = VK_NULL_HANDLE;
};

} // namespace lowpoly
