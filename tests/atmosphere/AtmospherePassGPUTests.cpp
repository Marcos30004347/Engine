#include "atmosphere/AtmosphereTypes.hpp"
#include "atmosphere/rendering/AtmosphereLutDebugPass.hpp"
#include "atmosphere/rendering/AtmosphereLutPasses.hpp"
#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpgpu/TextureCopyPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/rendering/SparseVoxelAtmosphereCompositePass.hpp"
#include "voxel/svraster/rendering/SparseVoxelAtmosphereTerrainShadowPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetOverviewAtmospherePass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using namespace ::rendering;
namespace atmosphereRendering = atmosphere::rendering;
namespace svrRendering = voxel::svraster::rendering;
using globalillumination::worldgi::WorldGILightFlag_AtmosphereSH;
using globalillumination::worldgi::WorldGILightFlag_Enabled;
using globalillumination::worldgi::WorldGILightGPU;
using globalillumination::worldgi::WorldGILightType_Sky;

constexpr uint32_t kTransmittanceWidth = 32u;
constexpr uint32_t kTransmittanceHeight = 8u;
constexpr uint32_t kMultipleWidth = 8u;
constexpr uint32_t kMultipleHeight = 8u;
constexpr uint32_t kSkyWidth = 32u;
constexpr uint32_t kSkyHeight = 16u;
constexpr uint32_t kAerialWidth = 8u;
constexpr uint32_t kAerialHeight = 4u;
constexpr uint32_t kAerialDepth = 8u;
constexpr uint32_t kTerrainResolution = 32u;
constexpr uint32_t kCompositeWidth = 16u;
constexpr uint32_t kCompositeHeight = 8u;
constexpr uint32_t kOverviewWidth = 32u;
constexpr uint32_t kOverviewHeight = 32u;
constexpr uint32_t kSummaryCount = 10u;

struct alignas(16) ImageSummaryGPU
{
  std::array<float, 4> mean{};
  std::array<float, 4> minimum{};
  std::array<float, 4> maximum{};
};
static_assert(sizeof(ImageSummaryGPU) == 48u);

void require(const bool condition, const std::string &message)
{
  if (!condition) throw std::runtime_error(message);
}

bool near(const float left, const float right, const float tolerance = 0.002f)
{
  return std::abs(left - right) <= tolerance;
}

bool finite(const ImageSummaryGPU &summary)
{
  for (const auto *values : {&summary.mean, &summary.minimum,
           &summary.maximum})
    for (const float value : *values)
      if (!std::isfinite(value)) return false;
  return true;
}

float rgbSum(const std::array<float, 4> &value)
{
  return value[0] + value[1] + value[2];
}

std::array<float, 16> identityMatrix()
{
  return {1.0f, 0.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 0.0f, 1.0f};
}

std::array<float, 16> rotationY(const float radians)
{
  const float cosine = std::cos(radians);
  const float sine = std::sin(radians);
  return {cosine, 0.0f, -sine, 0.0f,
          0.0f, 1.0f, 0.0f, 0.0f,
          sine, 0.0f, cosine, 0.0f,
          0.0f, 0.0f, 0.0f, 1.0f};
}

Texture createTexture2D(RenderGraph &graph, const std::string &name,
    const Format format, const uint32_t width, const uint32_t height,
    const ImageUsage usage)
{
  return graph.createTexture({
    .name = name,
    .format = format,
    .memoryProperties = BufferUsage::BufferUsage_None,
    .usage = usage,
    .width = width,
    .height = height,
    .depth = 1u,
    .mipLevels = 1u,
  });
}

Texture createTexture3D(RenderGraph &graph, const std::string &name,
    const Format format, const uint32_t width, const uint32_t height,
    const uint32_t depth, const ImageUsage usage)
{
  return graph.createTexture({
    .name = name,
    .format = format,
    .memoryProperties = BufferUsage::BufferUsage_None,
    .usage = usage,
    .width = width,
    .height = height,
    .depth = depth,
    .mipLevels = 1u,
  });
}

template <typename T>
Buffer uploadBuffer(RenderGraph &graph, const std::string &name,
    const std::vector<T> &values, const BufferUsage usage)
{
  const Buffer result = graph.createBuffer({
    .name = name,
    .size = std::max<uint64_t>(sizeof(T), values.size() * sizeof(T)),
    .usage = static_cast<BufferUsage>(usage | BufferUsage::BufferUsage_Push),
  });
  if (!values.empty())
    graph.bufferWrite(result, 0u, values.size() * sizeof(T),
        const_cast<T *>(values.data()));
  return result;
}

Texture uploadRgba32Texture(RenderGraph &graph, const std::string &name,
    const uint32_t width, const uint32_t height,
    const std::array<float, 4> color)
{
  const Texture result = createTexture2D(graph, name,
      Format::Format_RGBA32Float, width, height,
      static_cast<ImageUsage>(ImageUsage::ImageUsage_Sampled |
          ImageUsage::ImageUsage_TransferDst));
  std::vector<float> values(static_cast<size_t>(width) * height * 4u);
  for (size_t pixel = 0u; pixel < static_cast<size_t>(width) * height; ++pixel)
    std::copy(color.begin(), color.end(), values.begin() + pixel * 4u);
  graph.textureWrite(result, values.size() * sizeof(float), values.data());
  return result;
}

class AtmospherePassInspectPass final : public Pass
{
public:
  struct Inputs
  {
    Texture transmittance;
    Texture multipleScattering;
    Texture skyView;
    Texture aerialPerspective;
    Texture terrainShadow;
    Texture debugOutput;
    Texture compositeOutput;
    Texture disabledCompositeOutput;
    Texture overviewOutput;
    Texture disabledOverviewOutput;
  };

  AtmospherePassInspectPass(const Inputs inputs, const Buffer summaries)
      : inputs_(inputs), summaries_(summaries)
  {
  }

  ~AtmospherePassInspectPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
  }

  void recordCommandBuffer(CommandRecorder &commands) override
  {
    ensureResources();
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDispatch(1u, 1u, 1u);
  }

private:
  static TextureView sampled(const Texture texture,
      const ImageAspectFlags flags = ImageAspectFlags::Color)
  {
    return {.texture = texture,
            .index = 0u,
            .flags = flags,
            .baseMipLevel = 0u,
            .levelCount = 1u,
            .baseArrayLayer = 0u,
            .layerCount = 1u,
            .access = AccessPattern::SHADER_READ,
            .layout = ResourceLayout::SHADER_READ_ONLY};
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    BindingGroupLayout group{};
    for (uint32_t binding = 0u; binding < 10u; ++binding)
      group.textures.push_back({
        .name = "input" + std::to_string(binding),
        .binding = binding,
        .visibility = BindingVisibility::BindingVisibility_Compute,
      });
    group.buffers = {{
      .name = "summaries",
      .binding = 10u,
      .type = BufferBindingType::BufferBindingType_StorageBuffer,
      .visibility = BindingVisibility::BindingVisibility_Compute,
      .isDynamic = false,
    }};
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout", .groups = {group},
    });
    const std::array textures{
      inputs_.transmittance,
      inputs_.multipleScattering,
      inputs_.skyView,
      inputs_.aerialPerspective,
      inputs_.terrainShadow,
      inputs_.debugOutput,
      inputs_.compositeOutput,
      inputs_.disabledCompositeOutput,
      inputs_.overviewOutput,
      inputs_.disabledOverviewOutput,
    };
    GroupInfo groupInfo{};
    groupInfo.name = "group0";
    for (uint32_t binding = 0u; binding < textures.size(); ++binding)
      groupInfo.textures.push_back({
        .textureView = sampled(textures[binding],
            binding == 4u ? ImageAspectFlags::Depth : ImageAspectFlags::Color),
        .binding = binding,
      });
    groupInfo.buffers = {{
      .bufferView = {
        .buffer = summaries_, .offset = 0u,
        .size = kSummaryCount * sizeof(ImageSummaryGPU),
        .access = AccessPattern::SHADER_READ_WRITE,
      },
      .binding = 10u,
    }};
    groups_ = renderGraph->createBindingGroups({
      .name = passName + "_Groups", .layout = layout_, .groups = {groupInfo},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/atmosphere/atmosphere-pass-inspect.spirv"),
      .type = ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + "_Pipeline",
      .shader = shader_,
      .entry = "inspectAtmospherePasses",
      .layout = layout_,
    });
  }

  Inputs inputs_{};
  Buffer summaries_{};
  BindingsLayout layout_{};
  BindingGroups groups_{};
  Shader shader_{};
  ComputePipeline pipeline_{};
};

std::vector<svrRendering::SparseVoxelAtmosphereTerrainShadowPass::Vertex>
terrainVertices()
{
  using Vertex = svrRendering::SparseVoxelAtmosphereTerrainShadowPass::Vertex;
  constexpr float tangent = 0.0045f;
  const float up = std::sqrt(1.0f - 2.0f * tangent * tangent);
  return {
    Vertex{{-tangent, up, -tangent, 0.0f}, {-1.0f, -1.0f, 0.0f, 0.0f}},
    Vertex{{ tangent, up, -tangent, 750.0f}, {1.0f, -1.0f, 0.0f, 0.0f}},
    Vertex{{-tangent, up,  tangent, 1500.0f}, {-1.0f, 1.0f, 0.0f, 0.0f}},
    Vertex{{ tangent, up,  tangent, 250.0f}, {1.0f, 1.0f, 0.0f, 0.0f}},
  };
}

void runGraph(RenderGraph &graph)
{
  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);
}

void runFrame(RenderGraph &graph)
{
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);
}

std::array<ImageSummaryGPU, kSummaryCount> readSummaries(
    RenderGraph &graph, const Buffer readback)
{
  std::array<ImageSummaryGPU, kSummaryCount> result{};
  graph.bufferRead(readback, 0u, sizeof(result),
      [&](const void *data) { std::memcpy(result.data(), data, sizeof(result)); });
  return result;
}

WorldGILightGPU readSkyLight(RenderGraph &graph, const Buffer readback)
{
  std::array<WorldGILightGPU, 4u> lights{};
  graph.bufferRead(readback, 0u, sizeof(lights),
      [&](const void *data) { std::memcpy(lights.data(), data, sizeof(lights)); });
  return lights[1u];
}

void validateFirstFrame(
    const std::array<ImageSummaryGPU, kSummaryCount> &summaries,
    const WorldGILightGPU &skyLight)
{
  for (uint32_t index = 0u; index < summaries.size(); ++index)
    require(finite(summaries[index]),
        "atmosphere GPU pass " + std::to_string(index) +
        " emitted a non-finite summary");

  const auto &transmittance = summaries[0u];
  for (uint32_t channel = 0u; channel < 3u; ++channel)
  {
    require(transmittance.minimum[channel] >= -0.001f &&
            transmittance.maximum[channel] <= 1.001f,
        "transmittance must remain in [0,1]");
    require(transmittance.minimum[channel] < transmittance.maximum[channel],
        "transmittance must respond to optical path length");
  }
  require(near(transmittance.minimum[3], 1.0f) &&
          near(transmittance.maximum[3], 1.0f),
      "transmittance alpha must remain one");

  const auto &multiple = summaries[1u];
  require(multiple.minimum[0] >= -0.001f &&
          multiple.minimum[1] >= -0.001f &&
          multiple.minimum[2] >= -0.001f && rgbSum(multiple.mean) > 0.0f,
      "multiple scattering must be finite, nonnegative, and nonzero");
  require(multiple.minimum[3] >= -0.001f &&
          multiple.maximum[3] <= 1.001f,
      "multiple-scattering clamp diagnostic must be binary-range data");

  const auto &sky = summaries[2u];
  require(sky.minimum[0] >= -0.001f && sky.minimum[1] >= -0.001f &&
          sky.minimum[2] >= -0.001f && rgbSum(sky.mean) > 0.0f &&
          near(sky.minimum[3], 1.0f) && near(sky.maximum[3], 1.0f),
      "Sky-View must contain nonnegative radiance and opaque alpha");

  const auto &aerial = summaries[3u];
  require(aerial.minimum[0] >= -0.001f && aerial.minimum[1] >= -0.001f &&
          aerial.minimum[2] >= -0.001f && rgbSum(aerial.mean) > 0.0f,
      "aerial perspective must contain nonnegative in-scattering");
  require(aerial.minimum[3] >= -0.001f && aerial.maximum[3] <= 1.001f &&
          aerial.minimum[3] < aerial.maximum[3],
      "aerial transmittance must remain in range and vary with depth");

  const auto &terrain = summaries[4u];
  require(terrain.minimum[0] >= -0.001f && terrain.maximum[0] <= 1.001f &&
          terrain.mean[0] > 0.0f && terrain.maximum[0] > 0.45f,
      "terrain pass must clear and rasterize a reverse-depth shadow map");

  const auto &debug = summaries[5u];
  require(debug.minimum[0] >= -0.001f && debug.maximum[0] <= 1.001f &&
          near(debug.mean[0], debug.mean[1]) &&
          near(debug.mean[1], debug.mean[2]) &&
          near(debug.minimum[3], 1.0f) && near(debug.maximum[3], 1.0f),
      "LUT diagnostics must present aerial transmittance as finite grayscale");

  const auto &composite = summaries[6u];
  require(composite.minimum[0] >= -0.001f &&
          composite.minimum[1] >= -0.001f &&
          composite.minimum[2] >= -0.001f &&
          rgbSum(composite.mean) > 0.0f &&
          near(composite.minimum[3], 1.0f) &&
          near(composite.maximum[3], 1.0f),
      "local atmosphere composite must emit finite nonnegative color");

  const auto &disabledComposite = summaries[7u];
  require(near(disabledComposite.mean[0], 0.25f) &&
          near(disabledComposite.mean[1], 0.40f) &&
          near(disabledComposite.mean[2], 0.10f) &&
          near(disabledComposite.mean[3], 1.0f),
      "disabled atmosphere composite must preserve the source exactly");

  const auto &overview = summaries[8u];
  require(overview.minimum[0] >= -0.001f &&
          overview.minimum[1] >= -0.001f &&
          overview.minimum[2] >= -0.001f &&
          rgbSum(overview.mean) > 0.0f && overview.maximum[3] > 0.99f,
      "orbit atmosphere pass must shade the projected shell");

  const auto &disabledOverview = summaries[9u];
  require(rgbSum(disabledOverview.maximum) <= 0.001f &&
          disabledOverview.maximum[3] <= 0.001f,
      "disabled orbit atmosphere pass must leave a cleared target");

  const uint32_t requiredFlags = WorldGILightFlag_Enabled |
      WorldGILightFlag_AtmosphereSH;
  require((skyLight.flags[0] & requiredFlags) == requiredFlags &&
          skyLight.flags[1] == WorldGILightType_Sky,
      "ambient pass must publish the enabled atmosphere-SH sky record");
  float coefficientEnergy = 0.0f;
  for (const float *values : {skyLight.positionRange,
           skyLight.directionType, skyLight.shapeParams,
           skyLight.colorIntensity})
    for (uint32_t channel = 0u; channel < 4u; ++channel)
    {
      require(std::isfinite(values[channel]),
          "ambient SH coefficients must remain finite");
      if (channel < 3u) coefficientEnergy += std::abs(values[channel]);
    }
  require(coefficientEnergy > 0.0f && near(skyLight.colorIntensity[3], 0.45f),
      "ambient SH projection must preserve its independent intensity scale");
}

void testAtmospherePassChain(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  RenderGraph graph(&rhi);
  const ImageUsage lutUsage = static_cast<ImageUsage>(
      ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled);
  const ImageUsage colorUsage = static_cast<ImageUsage>(
      ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled);
  const Texture transmittance = createTexture2D(graph,
      "AtmosphereGPU.Transmittance", Format::Format_RGBA16Float,
      kTransmittanceWidth, kTransmittanceHeight, lutUsage);
  const Texture multiple = createTexture2D(graph,
      "AtmosphereGPU.Multiple", Format::Format_RGBA16Float,
      kMultipleWidth, kMultipleHeight, lutUsage);
  const Texture sky = createTexture2D(graph,
      "AtmosphereGPU.Sky", Format::Format_RGBA16Float,
      kSkyWidth, kSkyHeight, lutUsage);
  const Texture aerialCurrent = createTexture3D(graph,
      "AtmosphereGPU.AerialCurrent", Format::Format_RGBA16Float,
      kAerialWidth, kAerialHeight, kAerialDepth, lutUsage);
  const Texture aerial = createTexture3D(graph,
      "AtmosphereGPU.Aerial", Format::Format_RGBA16Float,
      kAerialWidth, kAerialHeight, kAerialDepth,
      static_cast<ImageUsage>(lutUsage | ImageUsage::ImageUsage_TransferSrc));
  const Texture aerialHistory = createTexture3D(graph,
      "AtmosphereGPU.AerialHistory", Format::Format_RGBA16Float,
      kAerialWidth, kAerialHeight, kAerialDepth,
      static_cast<ImageUsage>(ImageUsage::ImageUsage_Sampled |
          ImageUsage::ImageUsage_TransferDst));
  const Texture terrain = createTexture2D(graph,
      "AtmosphereGPU.Terrain", Format::Format_Depth32Float,
      kTerrainResolution, kTerrainResolution,
      static_cast<ImageUsage>(ImageUsage::ImageUsage_DepthStencilAttachment |
          ImageUsage::ImageUsage_Sampled));
  const Texture debug = createTexture2D(graph,
      "AtmosphereGPU.Debug", Format::Format_RGBA16Float,
      kCompositeWidth, kCompositeHeight, lutUsage);
  const Texture composite = createTexture2D(graph,
      "AtmosphereGPU.Composite", Format::Format_RGBA16Float,
      kCompositeWidth, kCompositeHeight, lutUsage);
  const Texture disabledComposite = createTexture2D(graph,
      "AtmosphereGPU.CompositeDisabled", Format::Format_RGBA16Float,
      kCompositeWidth, kCompositeHeight, lutUsage);
  const Texture overview = createTexture2D(graph,
      "AtmosphereGPU.Overview", Format::Format_RGBA16Float,
      kOverviewWidth, kOverviewHeight, colorUsage);
  const Texture disabledOverview = createTexture2D(graph,
      "AtmosphereGPU.OverviewDisabled", Format::Format_RGBA16Float,
      kOverviewWidth, kOverviewHeight, colorUsage);
  const Texture surface = uploadRgba32Texture(graph,
      "AtmosphereGPU.Surface", kCompositeWidth, kCompositeHeight,
      {0.25f, 0.40f, 0.10f, 1.0f});
  const Texture overviewSource = uploadRgba32Texture(graph,
      "AtmosphereGPU.OverviewSource", kOverviewWidth, kOverviewHeight,
      {0.15f, 0.25f, 0.08f, 1.0f});
  const Texture reactive = createTexture2D(graph,
      "AtmosphereGPU.Reactive", Format::Format_R16Float,
      kCompositeWidth, kCompositeHeight,
      static_cast<ImageUsage>(ImageUsage::ImageUsage_Storage |
          ImageUsage::ImageUsage_Sampled |
          ImageUsage::ImageUsage_TransferDst));
  const std::vector<uint16_t> zeroReactive(
      static_cast<size_t>(kCompositeWidth) * kCompositeHeight, 0u);
  graph.textureWrite(reactive, zeroReactive.size() * sizeof(uint16_t),
      const_cast<uint16_t *>(zeroReactive.data()));

  const size_t compositePixels =
      static_cast<size_t>(kCompositeWidth) * kCompositeHeight;
  std::vector<uint32_t> coverage(compositePixels, 0u);
  for (size_t index = 0u; index < coverage.size(); ++index)
    coverage[index] = (index & 1u) == 0u ? 1u : 0u;
  const Buffer coverageBuffer = uploadBuffer(graph,
      "AtmosphereGPU.Coverage", coverage, BufferUsage::BufferUsage_Storage);
  const Buffer depthBuffer = uploadBuffer(graph,
      "AtmosphereGPU.LinearDepth",
      std::vector<float>(compositePixels, 8'000.0f),
      BufferUsage::BufferUsage_Storage);

  std::vector<WorldGILightGPU> initialLights(4u);
  initialLights[1u].flags[1] = WorldGILightType_Sky;
  const Buffer sceneLights = uploadBuffer(graph,
      "AtmosphereGPU.SceneLights", initialLights,
      static_cast<BufferUsage>(BufferUsage::BufferUsage_Storage |
          BufferUsage::BufferUsage_CopySrc));
  const Buffer summaries = graph.createBuffer({
    .name = "AtmosphereGPU.Summaries",
    .size = kSummaryCount * sizeof(ImageSummaryGPU),
    .usage = BufferUsage::BufferUsage_Storage |
        BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer summaryReadback = graph.createBuffer({
    .name = "AtmosphereGPU.SummaryReadback",
    .size = kSummaryCount * sizeof(ImageSummaryGPU),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer lightReadback = graph.createBuffer({
    .name = "AtmosphereGPU.LightReadback",
    .size = initialLights.size() * sizeof(WorldGILightGPU),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });

  const atmosphere::AtmosphereProfile profile =
      atmosphere::earthAtmosphereProfile();
  atmosphere::SunSettings sun{};
  sun.directionPlanetSpace = {0.0, -1.0, 0.0};
  sun.intensity = 1.0f;

  svrRendering::SparseVoxelAtmosphereTerrainShadowPass::Settings
      terrainSettings{};
  terrainSettings.resolution = kTerrainResolution;
  terrainSettings.coverageAvailable = true;
  terrainSettings.rebuild = true;
  terrainSettings.tangentX = {1.0f, 0.0f, 0.0f, 0.0f};
  terrainSettings.tangentUp = {0.0f, 1.0f, 0.0f, 0.0f};
  terrainSettings.tangentZ = {0.0f, 0.0f, 1.0f, 0.0f};
  terrainSettings.tangentCamera = {0.0f, -2.0f, 0.0f, 0.0f};
  terrainSettings.sourceRadiusExtent = {
    static_cast<float>(profile.groundRadiusMetres),
    32'000.0f, 2'000.0f, 0.0f};
  terrainSettings.directionTowardSun = {0.0f, 1.0f, 0.0f, 0.0f};
  terrainSettings.localUp = {0.0f, 1.0f, 0.0f, 0.0f};

  atmosphereRendering::AtmosphereTransmittancePass::Settings transSettings{};
  transSettings.profile = profile;
  transSettings.sun = sun;
  transSettings.width = kTransmittanceWidth;
  transSettings.height = kTransmittanceHeight;
  transSettings.integrationSteps = 32u;
  transSettings.rebuild = true;

  atmosphereRendering::AtmosphereMultipleScatteringPass::Settings
      multipleSettings{};
  multipleSettings.profile = profile;
  multipleSettings.sun = sun;
  multipleSettings.width = kMultipleWidth;
  multipleSettings.height = kMultipleHeight;
  multipleSettings.directionCount = 16u;
  multipleSettings.integrationSteps = 8u;
  multipleSettings.rebuild = true;

  atmosphereRendering::AtmosphereTerrainShadowSampling shadowSampling{};
  shadowSampling.enabled = true;
  shadowSampling.halfExtentMetres = 32'000.0f;
  shadowSampling.halfDepthRangeMetres = 2'000.0f;
  shadowSampling.depthBiasMetres = 12.0f;

  atmosphereRendering::AtmosphereSkyViewPass::Settings skySettings{};
  skySettings.profile = profile;
  skySettings.sun = sun;
  skySettings.cameraAltitudeMetres = 2.0f;
  skySettings.sunZenithCosine = 1.0f;
  skySettings.localUpWorld = {0.0f, 1.0f, 0.0f};
  skySettings.terrainShadow = shadowSampling;
  skySettings.width = kSkyWidth;
  skySettings.height = kSkyHeight;
  skySettings.integrationSteps = 16u;
  skySettings.enabled = true;

  atmosphereRendering::AtmosphereAerialPerspectivePass::Settings
      aerialSettings{};
  aerialSettings.profile = profile;
  aerialSettings.sun = sun;
  aerialSettings.cameraAltitudeMetres = 2.0f;
  aerialSettings.maximumDistanceMetres = 32'000.0f;
  aerialSettings.distributionScaleMetres = 100.0f;
  aerialSettings.localUpWorld = {0.0f, 1.0f, 0.0f};
  aerialSettings.cameraToWorld = identityMatrix();
  aerialSettings.focalPrincipal = {16.0f, 16.0f, 0.0f, 0.0f};
  aerialSettings.terrainShadow = shadowSampling;
  aerialSettings.viewportWidth = kCompositeWidth;
  aerialSettings.viewportHeight = kCompositeHeight;
  aerialSettings.width = kAerialWidth;
  aerialSettings.height = kAerialHeight;
  aerialSettings.depth = kAerialDepth;
  aerialSettings.integrationSteps = 2u;
  aerialSettings.enabled = true;

  atmosphereRendering::AtmosphereAerialTemporalPass::Settings
      aerialTemporalSettings{};
  aerialTemporalSettings.width = kAerialWidth;
  aerialTemporalSettings.height = kAerialHeight;
  aerialTemporalSettings.depth = kAerialDepth;
  aerialTemporalSettings.viewportWidth = kCompositeWidth;
  aerialTemporalSettings.viewportHeight = kCompositeHeight;
  aerialTemporalSettings.previousViewportWidth = kCompositeWidth;
  aerialTemporalSettings.previousViewportHeight = kCompositeHeight;
  aerialTemporalSettings.maximumDistanceMetres = 32'000.0f;
  aerialTemporalSettings.distributionScaleMetres = 100.0f;
  aerialTemporalSettings.historyWeight = 0.88f;
  aerialTemporalSettings.cameraToWorld = identityMatrix();
  aerialTemporalSettings.previousWorldToCamera = identityMatrix();
  aerialTemporalSettings.focalPrincipal = {16.0f, 16.0f, 0.0f, 0.0f};
  aerialTemporalSettings.previousFocalPrincipal =
      aerialTemporalSettings.focalPrincipal;
  aerialTemporalSettings.historyValid = false;

  atmosphereRendering::AtmosphereAmbientPass::Settings ambientSettings{};
  ambientSettings.localUpWorld = {0.0f, 1.0f, 0.0f};
  ambientSettings.directionTowardSunWorld = {0.0f, 1.0f, 0.0f};
  ambientSettings.ambientIntensity = 0.45f;
  ambientSettings.sampleCount = 32u;
  ambientSettings.skyLightIndex = 1u;
  ambientSettings.lightCapacity = initialLights.size();
  ambientSettings.enabled = true;

  atmosphereRendering::AtmosphereLutDebugPass::Settings debugSettings{};
  debugSettings.width = kCompositeWidth;
  debugSettings.height = kCompositeHeight;
  debugSettings.view = atmosphereRendering::AtmosphereLutDebugPass::View::
      AerialPerspectiveTransmittance;
  debugSettings.aerialSlice = 0.75f;

  voxel::svraster::SparseVoxelRasterUniformsGPU raster{};
  raster.worldToCamera = identityMatrix();
  raster.cameraToWorld = identityMatrix();
  raster.focalPrincipal = {16.0f, 16.0f,
      kCompositeWidth * 0.5f, kCompositeHeight * 0.5f};
  raster.viewportTiles = {kCompositeWidth, kCompositeHeight, 1u, 1u};
  svrRendering::SparseVoxelAtmosphereCompositePass::Settings
      compositeSettings{};
  compositeSettings.width = kCompositeWidth;
  compositeSettings.height = kCompositeHeight;
  compositeSettings.enabled = true;
  compositeSettings.profile = profile;
  compositeSettings.sun = sun;
  compositeSettings.cameraAltitudeMetres = 2.0f;
  compositeSettings.aerialMaximumDistanceMetres = 32'000.0f;
  compositeSettings.aerialDistributionScaleMetres = 100.0f;
  compositeSettings.starsEnabled = false;
  compositeSettings.localUpWorld = {0.0f, 1.0f, 0.0f};
  compositeSettings.raster = raster;
  auto disabledCompositeSettings = compositeSettings;
  disabledCompositeSettings.enabled = false;

  svrRendering::SparseVoxelPlanetOverviewAtmospherePass::Settings
      overviewSettings{};
  overviewSettings.width = kOverviewWidth;
  overviewSettings.height = kOverviewHeight;
  overviewSettings.enabled = true;
  overviewSettings.profile = profile;
  overviewSettings.sun = sun;
  overviewSettings.verticalFovRadians = 1.04719755f;
  overviewSettings.cameraDirectionDistance = {0.0f, 0.0f, 1.0f, 2.2f};
  overviewSettings.cameraRight = {1.0f, 0.0f, 0.0f};
  overviewSettings.cameraUp = {0.0f, 1.0f, 0.0f};
  overviewSettings.integrationSamples = 8u;
  overviewSettings.starsEnabled = false;
  auto disabledOverviewSettings = overviewSettings;
  disabledOverviewSettings.enabled = false;

  uint32_t passIndex = 0u;
  auto *terrainPass = graph.registerPass<
      svrRendering::SparseVoxelAtmosphereTerrainShadowPass>(
      "AtmosphereGPU.TerrainPass", passIndex++, terrain,
      terrainVertices(), std::vector<uint32_t>{0u, 1u, 2u, 2u, 1u, 3u},
      terrainSettings);
  auto *transPass = graph.registerPass<
      atmosphereRendering::AtmosphereTransmittancePass>(
      "AtmosphereGPU.TransmittancePass", passIndex++, transmittance,
      transSettings);
  auto *multiplePass = graph.registerPass<
      atmosphereRendering::AtmosphereMultipleScatteringPass>(
      "AtmosphereGPU.MultiplePass", passIndex++, transmittance, multiple,
      multipleSettings);
  auto *skyPass = graph.registerPass<atmosphereRendering::AtmosphereSkyViewPass>(
      "AtmosphereGPU.SkyPass", passIndex++, transmittance, multiple, terrain,
      sky, skySettings);
  auto *aerialPass = graph.registerPass<
      atmosphereRendering::AtmosphereAerialPerspectivePass>(
      "AtmosphereGPU.AerialPass", passIndex++, transmittance, multiple,
      terrain, aerialCurrent, aerialSettings);
  auto *aerialTemporalPass = graph.registerPass<
      atmosphereRendering::AtmosphereAerialTemporalPass>(
      "AtmosphereGPU.AerialTemporalPass", passIndex++, aerialCurrent,
      aerialHistory, aerial, aerialTemporalSettings);
  auto *aerialHistoryCopy = graph.registerPass<
      ::rendering::gpgpu::TextureCopyPass>(
      "AtmosphereGPU.AerialHistoryCopy", passIndex++, aerial, aerialHistory,
      ::rendering::gpgpu::TextureCopyPass::Aspect::Color);
  auto *ambientPass = graph.registerPass<
      atmosphereRendering::AtmosphereAmbientPass>(
      "AtmosphereGPU.AmbientPass", passIndex++, sky, sceneLights,
      ambientSettings);
  auto *debugPass = graph.registerPass<
      atmosphereRendering::AtmosphereLutDebugPass>(
      "AtmosphereGPU.DebugPass", passIndex++,
      atmosphereRendering::AtmosphereLutDebugPass::Inputs{
        transmittance, multiple, sky, aerial}, debug, debugSettings);
  const svrRendering::SparseVoxelAtmosphereCompositePass::Inputs
      compositeInputs{surface, transmittance, sky, aerial, reactive,
        coverageBuffer, depthBuffer};
  auto *compositePass = graph.registerPass<
      svrRendering::SparseVoxelAtmosphereCompositePass>(
      "AtmosphereGPU.CompositePass", passIndex++, compositeInputs,
      composite, compositeSettings);
  auto *disabledCompositePass = graph.registerPass<
      svrRendering::SparseVoxelAtmosphereCompositePass>(
      "AtmosphereGPU.DisabledCompositePass", passIndex++, compositeInputs,
      disabledComposite, disabledCompositeSettings);
  const svrRendering::SparseVoxelPlanetOverviewAtmospherePass::Inputs
      overviewInputs{overviewSource, transmittance, multiple};
  auto *overviewPass = graph.registerPass<
      svrRendering::SparseVoxelPlanetOverviewAtmospherePass>(
      "AtmosphereGPU.OverviewPass", passIndex++, overviewInputs, overview,
      overviewSettings);
  auto *disabledOverviewPass = graph.registerPass<
      svrRendering::SparseVoxelPlanetOverviewAtmospherePass>(
      "AtmosphereGPU.DisabledOverviewPass", passIndex++, overviewInputs,
      disabledOverview, disabledOverviewSettings);
  auto *inspectPass = graph.registerPass<AtmospherePassInspectPass>(
      "AtmosphereGPU.Inspect", passIndex++,
      AtmospherePassInspectPass::Inputs{transmittance, multiple, sky, aerial,
        terrain, debug, composite, disabledComposite, overview,
        disabledOverview}, summaries);
  auto *summaryCopy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "AtmosphereGPU.CopySummaries", passIndex++, summaries, 0u,
      kSummaryCount * sizeof(ImageSummaryGPU), summaryReadback, 0u,
      kSummaryCount * sizeof(ImageSummaryGPU));
  auto *lightCopy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "AtmosphereGPU.CopyLights", passIndex++, sceneLights, 0u,
      initialLights.size() * sizeof(WorldGILightGPU), lightReadback, 0u,
      initialLights.size() * sizeof(WorldGILightGPU));

  runGraph(graph);
  const auto firstSummaries = readSummaries(graph, summaryReadback);
  constexpr std::array<const char *, kSummaryCount> summaryNames{
    "transmittance", "multiple", "sky", "aerial", "terrain", "debug",
    "composite", "composite-disabled", "overview", "overview-disabled"};
  for (uint32_t index = 0u; index < firstSummaries.size(); ++index)
    std::cout << "  " << summaryNames[index] << " mean=("
              << firstSummaries[index].mean[0] << ", "
              << firstSummaries[index].mean[1] << ", "
              << firstSummaries[index].mean[2] << ", "
              << firstSummaries[index].mean[3] << ")\n";
  validateFirstFrame(firstSummaries, readSkyLight(graph, lightReadback));

  // Hillaire-style sub-froxel jitter must actually rotate the integration
  // samples so temporal reconstruction has independent information to
  // accumulate into smooth shafts. It must not materially change the total
  // energy or transmittance of the volume.
  auto jitteredAerialSettings = aerialSettings;
  jitteredAerialSettings.frameIndex = 1u;
  aerialPass->update(jitteredAerialSettings);
  aerialTemporalSettings.frameIndex = 1u;
  aerialTemporalSettings.historyValid = true;
  aerialTemporalPass->update(aerialTemporalSettings);
  runFrame(graph);
  const auto jitteredSummaries = readSummaries(graph, summaryReadback);
  float jitteredAerialDifference = 0.0f;
  for (uint32_t channel = 0u; channel < 3u; ++channel)
    jitteredAerialDifference += std::abs(
        jitteredSummaries[3u].mean[channel] -
        firstSummaries[3u].mean[channel]);
  require(jitteredAerialDifference > 0.000001f &&
          std::abs(jitteredSummaries[3u].mean[3] -
              firstSummaries[3u].mean[3]) < 0.02f,
      "temporal froxel jitter must rotate samples without changing volume energy");

  // A second resolve of the identical current frame with history disabled
  // isolates the temporal pass response. Neighborhood-clipped history should
  // alter noisy scattering while preserving integrated transmittance.
  aerialTemporalSettings.historyValid = false;
  aerialTemporalPass->update(aerialTemporalSettings);
  runFrame(graph);
  const auto unfilteredJitteredSummaries =
      readSummaries(graph, summaryReadback);
  float temporalRadianceDifference = 0.0f;
  for (uint32_t channel = 0u; channel < 3u; ++channel)
    temporalRadianceDifference += std::abs(
        unfilteredJitteredSummaries[3u].mean[channel] -
        jitteredSummaries[3u].mean[channel]);
  require(temporalRadianceDifference > 0.0000001f &&
          std::abs(unfilteredJitteredSummaries[3u].mean[3] -
              jitteredSummaries[3u].mean[3]) < 0.02f,
      "froxel temporal resolve must filter radiance without destabilizing transmittance");

  // History allocations can be undefined after a graph rebuild and an
  // in-flight camera update used to expose that memory through reprojection.
  // NaNs survived clamp/mix, were copied back into history, then the composite
  // converted the corrupted froxels to identity transmittance: moving holes in
  // otherwise continuous fog. Exercise the rotating-camera path explicitly.
  std::vector<uint16_t> corruptAerialHistory(
      static_cast<size_t>(kAerialWidth) * kAerialHeight * kAerialDepth * 4u,
      0x7e00u);
  graph.textureWrite(aerialHistory,
      corruptAerialHistory.size() * sizeof(uint16_t),
      corruptAerialHistory.data());
  auto rotatedFogSettings = aerialSettings;
  rotatedFogSettings.volumetricFogEnabled = true;
  rotatedFogSettings.volumetricFogDensityPerMetre = 0.0004f;
  rotatedFogSettings.volumetricFogHeightFalloffMetres = 2'000.0f;
  rotatedFogSettings.volumetricFogMaximumHeightMetres = 5'000.0f;
  rotatedFogSettings.cameraToWorld = rotationY(0.21f);
  rotatedFogSettings.frameIndex = 2u;
  aerialPass->update(rotatedFogSettings);
  aerialTemporalSettings.cameraToWorld = rotatedFogSettings.cameraToWorld;
  aerialTemporalSettings.previousWorldToCamera = identityMatrix();
  aerialTemporalSettings.frameIndex = 2u;
  aerialTemporalSettings.historyValid = true;
  aerialTemporalPass->update(aerialTemporalSettings);
  runFrame(graph);
  const auto corruptHistorySummaries = readSummaries(graph, summaryReadback);
  require(finite(corruptHistorySummaries[3u]) &&
          rgbSum(corruptHistorySummaries[3u].mean) > 0.0f &&
          corruptHistorySummaries[3u].minimum[3] >= -0.001f &&
          corruptHistorySummaries[3u].maximum[3] <= 1.001f,
      "rotated aerial reprojection must reject corrupt history instead of "
      "creating moving fog holes");

  // Restore the non-temporal baseline used by the remaining independent pass
  // checks below.
  aerialTemporalSettings.historyValid = false;
  aerialTemporalSettings.cameraToWorld = identityMatrix();
  aerialTemporalPass->update(aerialTemporalSettings);

  auto unshadowedSkySettings = skySettings;
  auto unshadowedAerialSettings = aerialSettings;
  unshadowedSkySettings.terrainShadow.strength = 0.0f;
  unshadowedAerialSettings.terrainShadow.strength = 0.0f;
  skyPass->update(unshadowedSkySettings);
  aerialPass->update(unshadowedAerialSettings);
  runFrame(graph);
  const auto unshadowedSummaries = readSummaries(graph, summaryReadback);
  float recoveredDirectLight = 0.0f;
  for (const uint32_t index : {2u, 3u})
    for (uint32_t channel = 0u; channel < 3u; ++channel)
    {
      require(unshadowedSummaries[index].mean[channel] + 0.0001f >=
              firstSummaries[index].mean[channel],
          "terrain shadowing increased direct atmospheric radiance");
      recoveredDirectLight += unshadowedSummaries[index].mean[channel] -
          firstSummaries[index].mean[channel];
    }
  require(recoveredDirectLight > 0.00001f,
      "terrain shadow strength did not affect Sky-View or aerial scattering");

  // Exercise the cheap local participating medium in the same production
  // froxel pass. It must add a visible radiance response and reduce mean
  // transmittance without allocating or dispatching another volume.
  auto fogAerialSettings = unshadowedAerialSettings;
  fogAerialSettings.volumetricFogEnabled = true;
  fogAerialSettings.volumetricFogDensityPerMetre = 0.0004f;
  fogAerialSettings.volumetricFogHeightFalloffMetres = 2'000.0f;
  fogAerialSettings.volumetricFogMaximumHeightMetres = 5'000.0f;
  fogAerialSettings.volumetricFogAlbedo = {0.9f, 0.95f, 1.0f};
  fogAerialSettings.volumetricFogAnisotropy = 0.6f;
  aerialPass->update(fogAerialSettings);
  runFrame(graph);
  const auto fogSummaries = readSummaries(graph, summaryReadback);
  require(fogSummaries[3u].mean[3] + 0.001f <
              unshadowedSummaries[3u].mean[3],
      "volumetric height fog must reduce aerial transmittance");
  require(std::abs(rgbSum(fogSummaries[3u].mean) -
              rgbSum(unshadowedSummaries[3u].mean)) > 0.001f,
      "volumetric height fog must change integrated aerial radiance");

  // The authored local layer must remain bounded instead of integrating into
  // an opaque horizon wall. Removing its distance handoff should measurably
  // lower mean transmittance over the same 32 km volume.
  auto unboundedFogAerialSettings = fogAerialSettings;
  unboundedFogAerialSettings.volumetricFogFullDistanceMetres = 32'000.0f;
  unboundedFogAerialSettings.volumetricFogFadeEndDistanceMetres = 33'000.0f;
  aerialPass->update(unboundedFogAerialSettings);
  runFrame(graph);
  const auto unboundedFogSummaries = readSummaries(graph, summaryReadback);
  require(fogSummaries[3u].mean[3] >
              unboundedFogSummaries[3u].mean[3] + 0.005f,
      "local fog distance handoff did not prevent horizon opacity buildup");

  // atmosphereRadianceScale is the display bridge for physical scattering,
  // not a fog exposure control. Isolate the local medium and prove changing
  // that bridge cannot recreate the former eight-times-brighter fog wall.
  auto isolatedFogAerialSettings = fogAerialSettings;
  isolatedFogAerialSettings.profile.rayleighScatteringPerMetre =
      {0.0f, 0.0f, 0.0f};
  isolatedFogAerialSettings.profile.mieScatteringPerMetre =
      {0.0f, 0.0f, 0.0f};
  isolatedFogAerialSettings.profile.mieAbsorptionPerMetre =
      {0.0f, 0.0f, 0.0f};
  isolatedFogAerialSettings.profile.ozoneAbsorptionPerMetre =
      {0.0f, 0.0f, 0.0f};
  isolatedFogAerialSettings.profile.atmosphereRadianceScale = 1.0f;
  aerialPass->update(isolatedFogAerialSettings);
  runFrame(graph);
  const auto isolatedFogUnitScale =
      readSummaries(graph, summaryReadback);
  isolatedFogAerialSettings.profile.atmosphereRadianceScale = 8.0f;
  aerialPass->update(isolatedFogAerialSettings);
  runFrame(graph);
  const auto isolatedFogDisplayScale =
      readSummaries(graph, summaryReadback);
  for (uint32_t channel = 0u; channel < 4u; ++channel)
    require(near(isolatedFogUnitScale[3u].mean[channel],
                 isolatedFogDisplayScale[3u].mean[channel], 0.0005f),
        "physical atmosphere radiance scale leaked into authored local fog");

  // The local medium must consume terrain visibility too; otherwise the fog
  // would be present but could never display terrain-cast volumetric shafts.
  auto shadowedFogAerialSettings = fogAerialSettings;
  shadowedFogAerialSettings.terrainShadow = aerialSettings.terrainShadow;
  aerialPass->update(shadowedFogAerialSettings);
  runFrame(graph);
  const auto shadowedFogSummaries = readSummaries(graph, summaryReadback);
  float recoveredFogDirectLight = 0.0f;
  for (uint32_t channel = 0u; channel < 3u; ++channel)
  {
    require(fogSummaries[3u].mean[channel] + 0.0001f >=
            shadowedFogSummaries[3u].mean[channel],
        "terrain shadowing increased volumetric fog radiance");
    recoveredFogDirectLight += fogSummaries[3u].mean[channel] -
        shadowedFogSummaries[3u].mean[channel];
  }
  require(recoveredFogDirectLight > 0.00001f,
      "terrain visibility did not produce a volumetric fog shadow response");
  aerialPass->update(unshadowedAerialSettings);

  transSettings.rebuild = false;
  multipleSettings.rebuild = false;
  terrainSettings.rebuild = false;
  transPass->update(transSettings);
  multiplePass->update(multipleSettings);
  terrainPass->update(terrainSettings);
  runFrame(graph);
  const auto retainedSummaries = readSummaries(graph, summaryReadback);
  for (const uint32_t index : {0u, 1u, 4u})
    for (uint32_t channel = 0u; channel < 4u; ++channel)
      require(near(retainedSummaries[index].mean[channel],
                   firstSummaries[index].mean[channel], 0.0001f),
          "persistent LUT/shadow output changed while rebuild was disabled");

  // The sample uses this control as an exposure bridge because its output
  // path has no final tone mapper. Exercise it through every radiance-bearing
  // production pass so a valid-but-nearly-black atmosphere cannot regress
  // unnoticed.
  atmosphere::AtmosphereProfile displayProfile = profile;
  displayProfile.atmosphereRadianceScale = 8.0f;
  auto displaySkySettings = unshadowedSkySettings;
  auto displayAerialSettings = unshadowedAerialSettings;
  auto displayCompositeSettings = compositeSettings;
  auto displayOverviewSettings = overviewSettings;
  displaySkySettings.profile = displayProfile;
  displayAerialSettings.profile = displayProfile;
  displayCompositeSettings.profile = displayProfile;
  displayOverviewSettings.profile = displayProfile;
  skyPass->update(displaySkySettings);
  aerialPass->update(displayAerialSettings);
  compositePass->update(displayCompositeSettings);
  overviewPass->update(displayOverviewSettings);
  runFrame(graph);
  const auto displaySummaries = readSummaries(graph, summaryReadback);
  std::cout << "  calibrated sky mean="
            << rgbSum(displaySummaries[2u].mean)
            << " aerial mean=" << rgbSum(displaySummaries[3u].mean)
            << " composite mean=" << rgbSum(displaySummaries[6u].mean)
            << " overview mean=" << rgbSum(displaySummaries[8u].mean)
            << '\n';
  require(rgbSum(displaySummaries[2u].mean) >
              rgbSum(retainedSummaries[2u].mean) * 4.0f &&
          rgbSum(displaySummaries[3u].mean) >
              rgbSum(retainedSummaries[3u].mean) * 4.0f,
      "atmosphere radiance scale must brighten Sky-View and aerial scattering");
  require(rgbSum(displaySummaries[6u].mean) >
              rgbSum(retainedSummaries[6u].mean) + 0.02f &&
          rgbSum(displaySummaries[8u].mean) >
              rgbSum(retainedSummaries[8u].mean) + 0.02f,
      "display-calibrated radiance must remain visible in local and orbit composites");

  skySettings.enabled = false;
  aerialSettings.enabled = false;
  skyPass->update(skySettings);
  aerialPass->update(aerialSettings);
  runFrame(graph);
  const auto disabledSummaries = readSummaries(graph, summaryReadback);
  require(rgbSum(disabledSummaries[2u].maximum) <= 0.001f &&
          near(disabledSummaries[2u].mean[3], 1.0f),
      "disabled Sky-View must clear radiance and retain identity alpha");
  require(rgbSum(disabledSummaries[3u].maximum) <= 0.001f &&
          near(disabledSummaries[3u].mean[3], 1.0f),
      "disabled aerial perspective must clear scattering and retain identity transmittance");

  delete lightCopy;
  delete summaryCopy;
  delete inspectPass;
  delete disabledOverviewPass;
  delete overviewPass;
  delete disabledCompositePass;
  delete compositePass;
  delete debugPass;
  delete ambientPass;
  delete aerialHistoryCopy;
  delete aerialTemporalPass;
  delete aerialPass;
  delete skyPass;
  delete multiplePass;
  delete transPass;
  delete terrainPass;
}

bool expectedGpuBackendUnavailable(const std::exception &error)
{
  const std::string message = error.what();
#if defined(__APPLE__)
  if (message.find("failed to create instance") != std::string::npos)
    return true;
#endif
  return message.find("Vulkan is not supported on this device") !=
      std::string::npos;
}

} // namespace

int main()
{
  os::Logger::start(100u);
  try
  {
    DeviceRequiredLimits limits{
      .minimumMemory = 32u * 1024u * 1024u,
      .minimumComputeSharedMemory = 0u,
      .minimumComputeWorkGroupInvocations = 64u,
    };
    auto rhi = std::make_unique<
        ::rendering::backend::vulkan::VulkanRHI>(
        ::rendering::backend::vulkan::Vulkan_1_2, limits,
        DeviceFeatures::DeviceFeatures_Compute, std::vector<std::string>{},
        true);
    std::vector<VkSurfaceKHR> surfaces;
    rhi->init(surfaces);
    testAtmospherePassChain(*rhi);
  }
  catch (const std::exception &error)
  {
    if (expectedGpuBackendUnavailable(error))
    {
      os::Logger::warningf(
          "[Test] atmosphere GPU tests skipped: %s", error.what());
      os::Logger::shutdown();
      std::cout << "AtmospherePassGPUTests passed (GPU backend skipped)\n";
      return 0;
    }
    os::Logger::shutdown();
    std::cerr << "AtmospherePassGPUTests failed: " << error.what() << '\n';
    return 1;
  }
  os::Logger::shutdown();
  std::cout << "AtmospherePassGPUTests passed\n";
  return 0;
}
