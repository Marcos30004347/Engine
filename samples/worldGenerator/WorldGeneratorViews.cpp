#include "WorldGeneratorViews.hpp"

#include "procedural/world/MapEdges.hpp"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace worldgen
{

using procedural::Point2;
using procedural::world::BlendSettings;
using procedural::world::BlendWeight;
using procedural::world::buildEdgeSet;
using procedural::world::cellBoundary;
using procedural::world::CellType;
using procedural::world::CoastPolyline;
using procedural::world::ContourPolyline;
using procedural::world::ContourSet;
using procedural::world::EdgeRelaxSettings;
using procedural::world::MapBoundaryEdge;
using procedural::world::MapEdgeSet;
using procedural::world::MapGraph;
using procedural::world::MapNode;
using procedural::world::MapRegion;
using procedural::world::RegionMap;
using procedural::world::relaxEdges;
using procedural::world::RiverNetwork;
using procedural::world::RiverSegment;
using procedural::world::shapeBlendByHeight;
using procedural::world::applyCellInfluenceWeights;
using procedural::world::restrictBlendToSurface;
using procedural::world::SiteIndex;

constexpr uint32_t kInvalid = procedural::world::kInvalidNode;

// Same quantisation the noisy edges use, so an edge is identified the same way
// on both sides.
uint64_t positionKey(const Point2 &point)
{
  uint64_t value = (static_cast<uint64_t>(static_cast<int64_t>(std::llround(point.x))) << 32u) ^ (static_cast<uint64_t>(static_cast<int64_t>(std::llround(point.y))) & 0xffffffffull);
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33u;
  return value;
}

vkbackend::VulkanTextureView WorldMapRHI::createDepthTextureView(const gpu::TextureId texture)
{
  gpu::TextureView view{};
  view.resourceId = texture;
  view.flags = gpu::ImageAspectFlags::Depth;
  view.baseMipLevel = 0u;
  view.levelCount = 1u;
  view.baseArrayLayer = 0u;
  view.layerCount = 1u;
  view.access = gpu::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ | gpu::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE;
  view.layout = gpu::ResourceLayout::DEPTH_STENCIL_ATTACHMENT;
  return createTextureView(view);
}

void WorldMapRHI::destroyDepthTextureView(vkbackend::VulkanTextureView &view)
{
  if (view.view != VK_NULL_HANDLE)
  {
    destroyTextureView(view);
    view = {};
  }
}

VkBuffer WorldMapRHI::bufferHandle(const gpu::BufferId buffer)
{
  return getVulkanBuffer(buffer).buffer;
}

WorldMapOverlayPass::WorldMapOverlayPass(WorldMapRHI &rhi) : rhi_(rhi)
{
}

WorldMapOverlayPass::~WorldMapOverlayPass()
{
  destroy();
}

void WorldMapOverlayPass::initialize(const gpu::SwapChain swapChain)
{
  destroy();
  renderPass_ = createRenderPass(rhi_.getDevice(), rhi_.getSwapChainVkFormat(swapChain));
}

void WorldMapOverlayPass::destroy()
{
  destroyFramebuffers();
  if (renderPass_ != VK_NULL_HANDLE)
  {
    vkDestroyRenderPass(rhi_.getDevice(), renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
  }
}

void WorldMapOverlayPass::destroyFramebuffers()
{
  for (auto &[_, resources] : framebuffers_)
  {
    if (resources.framebuffer != VK_NULL_HANDLE)
      vkDestroyFramebuffer(rhi_.getDevice(), resources.framebuffer, nullptr);
    rhi_.destroyDepthTextureView(resources.depthView);
    if (resources.depthTexture != gpu::TextureId::Invalid)
      rhi_.deleteTexture(resources.depthTexture);
  }
  framebuffers_.clear();
}

void WorldMapOverlayPass::onSwapChainRecreated(const gpu::SwapChain swapChain)
{
  destroyFramebuffers();
  if (renderPass_ == VK_NULL_HANDLE)
    renderPass_ = createRenderPass(rhi_.getDevice(), rhi_.getSwapChainVkFormat(swapChain));
}

void WorldMapOverlayPass::render(gpu::SwapChain &swapChain, const gpu::TextureView &swapChainView, ImDrawData *drawData, const std::function<void(VkCommandBuffer, VkExtent2D)> &beforeImGui)
{
  rhi_.presentWithOverlay(
      swapChain,
      swapChainView.resourceId,
      gpu::ResourceLayout::UNDEFINED,
      [this, drawData, beforeImGui, framebufferKey = static_cast<uintptr_t>(swapChainView.resourceId)](VkCommandBuffer commandBuffer, VkImageView imageView, VkExtent2D extent)
      {
        FramebufferResources &resources = framebuffers_[framebufferKey];
        if (resources.framebuffer == VK_NULL_HANDLE)
        {
          resources.depthTexture = rhi_.createTexture({
            .name = "worldGenerator.overlayDepth",
            .format = gpu::Format::Format_Depth32Float,
            .memoryProperties = gpu::BufferUsage_None,
            .usage = gpu::ImageUsage_DepthStencilAttachment,
            .width = extent.width,
            .height = extent.height,
            .depth = 1u,
            .arrayLayers = 1u,
            .mipLevels = 1u,
            .initialLayout = gpu::ResourceLayout::UNDEFINED,
          });
          resources.depthView = rhi_.createDepthTextureView(resources.depthTexture);
          const std::array<VkImageView, 2> attachments{imageView, resources.depthView.view};
          VkFramebufferCreateInfo framebufferInfo{};
          framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
          framebufferInfo.renderPass = renderPass_;
          framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
          framebufferInfo.pAttachments = attachments.data();
          framebufferInfo.width = extent.width;
          framebufferInfo.height = extent.height;
          framebufferInfo.layers = 1u;
          checkVkResult(vkCreateFramebuffer(rhi_.getDevice(), &framebufferInfo, nullptr, &resources.framebuffer));
        }

        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = {{0.043f, 0.047f, 0.059f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0u};
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass_;
        renderPassInfo.framebuffer = resources.framebuffer;
        renderPassInfo.renderArea.extent = extent;
        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        if (beforeImGui)
          beforeImGui(commandBuffer, extent);
        if (drawData != nullptr && drawData->CmdListsCount > 0)
          ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
        vkCmdEndRenderPass(commandBuffer);
      });
}

void WorldMapOverlayPass::checkVkResult(const VkResult result)
{
  if (result != VK_SUCCESS)
    throw std::runtime_error("World generator Vulkan pass failed with VkResult=" + std::to_string(static_cast<int>(result)));
}

VkRenderPass WorldMapOverlayPass::createRenderPass(const VkDevice device, const VkFormat format)
{
  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = format;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentDescription depthAttachment{};
  depthAttachment.format = VK_FORMAT_D32_SFLOAT;
  depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorReference{};
  colorReference.attachment = 0u;
  colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depthReference{};
  depthReference.attachment = 1u;
  depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1u;
  subpass.pColorAttachments = &colorReference;
  subpass.pDepthStencilAttachment = &depthReference;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0u;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dependency.dstStageMask = dependency.srcStageMask;
  dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  const std::array<VkAttachmentDescription, 2> attachments{colorAttachment, depthAttachment};

  VkRenderPassCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = static_cast<uint32_t>(attachments.size());
  info.pAttachments = attachments.data();
  info.subpassCount = 1u;
  info.pSubpasses = &subpass;
  info.dependencyCount = 1u;
  info.pDependencies = &dependency;

  VkRenderPass renderPass = VK_NULL_HANDLE;
  checkVkResult(vkCreateRenderPass(device, &info, nullptr, &renderPass));
  return renderPass;
}

// ---------------------------------------------------------------------------

std::array<float, 3> cellTypeColour(const CellType type)
{
  // Chosen to read as a map rather than as a data visualisation: the water
  // ramp is cool and low contrast so the eye reads the land as figure, and the
  // land types separate by hue rather than by brightness so a hillshade can be
  // laid over them later without fighting the fill.
  switch (type)
  {
  case CellType::Ocean:
    return {0.086f, 0.184f, 0.318f};
  case CellType::Lake:
    return {0.204f, 0.376f, 0.522f};
  case CellType::Coast:
    return {0.831f, 0.776f, 0.596f};
  case CellType::Plain:
    return {0.549f, 0.639f, 0.400f};
  case CellType::Hill:
    return {0.545f, 0.549f, 0.325f};
  case CellType::Mountain:
    return {0.514f, 0.455f, 0.404f};
  case CellType::Plateau:
    return {0.624f, 0.514f, 0.337f};
  case CellType::Valley:
    return {0.443f, 0.573f, 0.396f};
  case CellType::Wood:
    return {0.278f, 0.435f, 0.278f};
  case CellType::Swamp:
    return {0.365f, 0.435f, 0.325f};
  case CellType::Desert:
    return {0.800f, 0.706f, 0.478f};
  case CellType::Rocks:
    return {0.612f, 0.588f, 0.541f};
  case CellType::Cliff:
    return {0.333f, 0.306f, 0.278f};
  case CellType::Canyon:
    return {0.557f, 0.318f, 0.247f};
  case CellType::Count:
    break;
  }
  return {1.0f, 0.0f, 1.0f};
}

const char *mapLayerName(const MapLayer layer)
{
  switch (layer)
  {
  case MapLayer::Content:
    return "content";
  case MapLayer::Landform:
    return "landform";
  case MapLayer::Biome:
    return "biome";
  case MapLayer::Heightmap:
    return "heightmap";
  case MapLayer::Region:
    return "region";
  case MapLayer::Count:
    return "unknown";
  case MapLayer::Elevation:
    return "elevation";
  case MapLayer::Uplift:
    return "uplift";
  }
  return "unknown";
}

namespace
{

std::array<float, 3> rampColour(const std::vector<std::pair<double, std::array<float, 3>>> &stops, const double t)
{
  if (stops.empty())
    return {1.0f, 0.0f, 1.0f};
  if (t <= stops.front().first)
    return stops.front().second;
  for (size_t index = 1u; index < stops.size(); ++index)
  {
    if (t > stops[index].first)
      continue;
    const double span = stops[index].first - stops[index - 1u].first;
    const float f = span <= 0.0 ? 0.0f : static_cast<float>((t - stops[index - 1u].first) / span);
    std::array<float, 3> colour{};
    for (size_t channel = 0u; channel < 3u; ++channel)
      colour[channel] = stops[index - 1u].second[channel] + (stops[index].second[channel] - stops[index - 1u].second[channel]) * f;
    return colour;
  }
  return stops.back().second;
}

// Hypsometric tinting, the convention every physical atlas uses: greens low,
// buff and brown through the middle, grey and white at the top. It is read
// fluently by anyone who has seen a school atlas, which a linear grey ramp is
// not.
std::array<float, 3> elevationColour(const double metresAboveDatum, const double reliefMetres)
{
  static const std::vector<std::pair<double, std::array<float, 3>>> land{
    {0.00, {0.451f, 0.588f, 0.412f}},
    {0.08, {0.573f, 0.647f, 0.412f}},
    {0.22, {0.741f, 0.729f, 0.482f}},
    {0.42, {0.769f, 0.663f, 0.463f}},
    {0.62, {0.671f, 0.545f, 0.427f}},
    {0.80, {0.612f, 0.588f, 0.573f}},
    {1.00, {0.949f, 0.957f, 0.969f}}};
  static const std::vector<std::pair<double, std::array<float, 3>>> sea{{0.00, {0.043f, 0.098f, 0.196f}}, {0.45, {0.098f, 0.208f, 0.361f}}, {0.80, {0.184f, 0.353f, 0.522f}}, {1.00, {0.353f, 0.545f, 0.663f}}};

  const double relief = std::max(1.0, reliefMetres);
  if (metresAboveDatum >= 0.0)
    return rampColour(land, std::clamp(metresAboveDatum / relief, 0.0, 1.0));
  // Shallow at the top of the sea ramp, so the shelf reads as shelf.
  return rampColour(sea, std::clamp(1.0 + metresAboveDatum / (relief * 0.35), 0.0, 1.0));
}

} // namespace

std::array<float, 3> biomeColour(const procedural::world::Biome biome)
{
  using procedural::world::Biome;
  switch (biome)
  {
  case Biome::Ocean:
    return {0.086f, 0.184f, 0.318f};
  case Biome::Lake:
    return {0.243f, 0.435f, 0.588f};
  case Biome::Shore:
    return {0.847f, 0.796f, 0.627f};
  case Biome::Glacier:
    return {0.937f, 0.957f, 0.973f};
  case Biome::Tundra:
    return {0.678f, 0.694f, 0.635f};
  case Biome::ColdDesert:
    return {0.741f, 0.714f, 0.620f};
  case Biome::SaltFlat:
    return {0.820f, 0.804f, 0.718f};
  case Biome::Taiga:
    return {0.278f, 0.400f, 0.353f};
  case Biome::TemperateForest:
    return {0.290f, 0.475f, 0.267f};
  case Biome::TemperateRainforest:
    return {0.165f, 0.373f, 0.259f};
  case Biome::MontaneForest:
    return {0.227f, 0.365f, 0.239f};
  case Biome::Mediterranean:
    return {0.478f, 0.557f, 0.302f};
  case Biome::Grassland:
    return {0.663f, 0.694f, 0.412f};
  case Biome::Steppe:
    return {0.714f, 0.663f, 0.416f};
  case Biome::Shrubland:
    return {0.686f, 0.639f, 0.400f};
  case Biome::Savanna:
    return {0.788f, 0.722f, 0.408f};
  case Biome::TropicalSeasonalForest:
    return {0.400f, 0.561f, 0.259f};
  case Biome::TropicalRainforest:
    return {0.118f, 0.396f, 0.208f};
  case Biome::SubtropicalForest:
    return {0.220f, 0.475f, 0.290f};
  case Biome::Desert:
    return {0.855f, 0.769f, 0.545f};
  case Biome::SandDesert:
    return {0.902f, 0.800f, 0.510f};
  case Biome::Wetland:
    return {0.373f, 0.478f, 0.400f};
  case Biome::Swamp:
    return {0.247f, 0.412f, 0.325f};
  case Biome::Mangrove:
    return {0.173f, 0.365f, 0.290f};
  case Biome::AlpineMeadow:
    return {0.588f, 0.639f, 0.502f};
  case Biome::BareRock:
    return {0.596f, 0.580f, 0.545f};
  case Biome::Count:
    break;
  }
  return {1.0f, 0.0f, 1.0f};
}

std::array<float, 3> regionColour(const uint32_t region)
{
  if (region == procedural::world::kInvalidNode)
    return {0.055f, 0.063f, 0.078f};
  // Golden-ratio hue stepping, so adjacent region ids never land on adjacent
  // hues and the partition stays legible however many regions there are.
  const double hue = std::fmod(static_cast<double>(region) * 0.6180339887, 1.0);
  const double sector = hue * 6.0;
  const double fractional = sector - std::floor(sector);
  const float high = 0.78f;
  const float low = 0.34f;
  const float rise = static_cast<float>(low + (high - low) * fractional);
  const float fall = static_cast<float>(high - (high - low) * fractional);
  switch (static_cast<int>(sector))
  {
  case 0:
    return {high, rise, low};
  case 1:
    return {fall, high, low};
  case 2:
    return {low, high, rise};
  case 3:
    return {low, fall, high};
  case 4:
    return {rise, low, high};
  default:
    return {high, low, fall};
  }
}

double blendedElevation(
    const procedural::world::MapSurfaceField &surface,
    const Point2 position)
{
  return surface.sample(position).elevationMetres;
}

double hillshade(const double dzdx, const double dzdy, const MapShading &shading)
{
  const double bearing = shading.sunBearingDegrees * 3.14159265358979 / 180.0;
  const double altitude = shading.sunAltitudeDegrees * 3.14159265358979 / 180.0;
  // Bearing is measured clockwise from north, which is the cartographic
  // convention and not the mathematical one.
  const double lightX = std::sin(bearing) * std::cos(altitude);
  const double lightY = std::cos(bearing) * std::cos(altitude);
  const double lightZ = std::sin(altitude);
  const double nx = -dzdx * shading.verticalExaggeration;
  const double ny = -dzdy * shading.verticalExaggeration;
  const double inverse = 1.0 / std::sqrt(nx * nx + ny * ny + 1.0);
  return std::clamp((nx * lightX + ny * lightY + lightZ) * inverse, 0.0, 1.0);
}

std::array<float, 3> nodeColour(const MapNode &node, const MapShading &shading)
{
  if (shading.layer == MapLayer::Biome)
    return biomeColour(procedural::world::biomeOf(node));
  if (shading.layer == MapLayer::Region)
    return node.land ? regionColour(node.region) : std::array<float, 3>{0.055f, 0.063f, 0.078f};
  if (shading.layer == MapLayer::Elevation || shading.layer == MapLayer::Heightmap)
    return elevationColour(node.intent.elevationMetres - shading.seaLevelMetres, shading.reliefMetres);
  if (shading.layer == MapLayer::Uplift)
  {
    if (!node.land)
      return {0.055f, 0.063f, 0.078f};
    const double peak = std::max(1.0e-12, shading.peakUpliftMetresPerYear);
    const double t = std::clamp(node.intent.upliftRate / peak, 0.0, 1.0);
    static const std::vector<std::pair<double, std::array<float, 3>>> ramp{{0.00, {0.110f, 0.114f, 0.145f}}, {0.35, {0.310f, 0.208f, 0.267f}}, {0.70, {0.686f, 0.353f, 0.243f}}, {1.00, {0.988f, 0.855f, 0.522f}}};
    return rampColour(ramp, t);
  }

  std::array<float, 3> colour = cellTypeColour(shading.layer == MapLayer::Landform ? node.type : node.content);
  if (!shading.shadeShoreAndShelf)
    return colour;

  const auto mix = [&colour](const std::array<float, 3> &target, const double amount)
  {
    const float t = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    for (size_t channel = 0u; channel < 3u; ++channel)
      colour[channel] += (target[channel] - colour[channel]) * t;
  };
  if (node.land && node.coastal)
    mix(cellTypeColour(CellType::Coast), shading.shoreStrength);
  else if (!node.land && node.shallow)
    mix({0.318f, 0.510f, 0.612f}, shading.shelfStrength);
  return colour;
}

namespace
{

uint32_t packColour(const std::array<float, 3> &colour)
{
  const auto channel = [](const float value)
  {
    return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
  };
  return IM_COL32(channel(colour[0]), channel(colour[1]), channel(colour[2]), 255);
}

float riverWidthFactor(const double dischargeCumecs, const double largestDischargeCumecs)
{
  const double share = std::clamp(
      dischargeCumecs / std::max(1.0e-9, largestDischargeCumecs), 0.0, 1.0);
  // Hydraulic geometry gives width ~= Q^0.5, but a slightly steeper display
  // exponent separates country-scale trunks from headwaters more clearly. The
  // floor keeps a selected small river visible as a one-pixel hairline.
  return static_cast<float>(std::max(0.16, std::pow(share, 0.65)));
}

double localHeightVariation(
    const procedural::world::MapSurfaceField &surface,
    const Point2 position,
    const double centreElevation)
{
  const double radius = std::max(1.0, surface.spacingMetres());
  double lowest = centreElevation;
  double highest = centreElevation;
  for (const Point2 offset : {Point2(radius, 0.0), Point2(-radius, 0.0),
           Point2(0.0, radius), Point2(0.0, -radius)})
  {
    const double elevation = surface.sample(
        Point2(position.x + offset.x, position.y + offset.y))
                                 .elevationMetres;
    lowest = std::min(lowest, elevation);
    highest = std::max(highest, elevation);
  }
  return highest - lowest;
}

uint32_t thresholdedColour(
    const MapGraph &graph,
    const std::vector<BlendWeight> &weights,
    const MapShading &shading)
{
  struct ClassScore
  {
    uint32_t key = 0u;
    uint32_t representative = kInvalid;
    double total = 0.0;
    double representativeWeight = -1.0;
  };
  std::vector<ClassScore> classes;
  classes.reserve(weights.size());
  const auto classOf = [&shading](const MapNode &node)
  {
    switch (shading.layer)
    {
    case MapLayer::Content:
      return static_cast<uint32_t>(node.content);
    case MapLayer::Landform:
      return static_cast<uint32_t>(node.type);
    case MapLayer::Biome:
      return static_cast<uint32_t>(node.biome);
    case MapLayer::Region:
      return node.region;
    default:
      return node.id;
    }
  };
  for (const BlendWeight &weight : weights)
  {
    if (weight.node >= graph.nodeCount() || !(weight.weight > 0.0))
      continue;
    const uint32_t key = classOf(graph.node(weight.node));
    auto found = std::find_if(
        classes.begin(), classes.end(),
        [key](const ClassScore &entry) { return entry.key == key; });
    if (found == classes.end())
    {
      classes.push_back(ClassScore{key, weight.node, weight.weight, weight.weight});
      continue;
    }
    found->total += weight.weight;
    if (weight.weight > found->representativeWeight ||
        (weight.weight == found->representativeWeight &&
            weight.node < found->representative))
    {
      found->representative = weight.node;
      found->representativeWeight = weight.weight;
    }
  }
  if (classes.empty())
    return IM_COL32(255, 0, 255, 255);
  const ClassScore *winner = &classes.front();
  for (const ClassScore &entry : classes)
    if (entry.total > winner->total ||
        (entry.total == winner->total && entry.key < winner->key))
      winner = &entry;
  return packColour(nodeColour(graph.node(winner->representative), shading));
}

constexpr size_t kBiomeSlots = 4u;

uint32_t packBytes(const std::array<uint8_t, kBiomeSlots> &bytes)
{
  return static_cast<uint32_t>(bytes[0]) |
      static_cast<uint32_t>(bytes[1]) << 8u |
      static_cast<uint32_t>(bytes[2]) << 16u |
      static_cast<uint32_t>(bytes[3]) << 24u;
}

std::array<uint8_t, kBiomeSlots> unpackBytes(const uint32_t packed)
{
  return {static_cast<uint8_t>(packed),
      static_cast<uint8_t>(packed >> 8u),
      static_cast<uint8_t>(packed >> 16u),
      static_cast<uint8_t>(packed >> 24u)};
}

void encodeBiomeScores(
    const std::array<double, 256> &scores,
    uint32_t &packedIds,
    uint32_t &packedWeights)
{
  struct Candidate
  {
    uint8_t id = 0u;
    double score = 0.0;
    double remainder = 0.0;
  };
  std::vector<Candidate> candidates;
  for (uint32_t id = 0u;
       id < static_cast<uint32_t>(procedural::world::Biome::Count); ++id)
    if (scores[id] > 0.0 && std::isfinite(scores[id]))
      candidates.push_back(
          {static_cast<uint8_t>(id), scores[id], 0.0});
  std::stable_sort(candidates.begin(), candidates.end(),
      [](const Candidate &left, const Candidate &right)
      {
        if (left.score != right.score)
          return left.score > right.score;
        return left.id < right.id;
      });
  if (candidates.size() > kBiomeSlots)
    candidates.resize(kBiomeSlots);
  std::array<uint8_t, kBiomeSlots> ids{};
  std::array<uint8_t, kBiomeSlots> weights{};
  if (candidates.empty())
  {
    ids[0] = static_cast<uint8_t>(procedural::world::Biome::Ocean);
    weights[0] = 255u;
    packedIds = packBytes(ids);
    packedWeights = packBytes(weights);
    return;
  }
  double total = 0.0;
  for (const Candidate &candidate : candidates)
    total += candidate.score;
  uint32_t quantized = 0u;
  for (size_t slot = 0u; slot < candidates.size(); ++slot)
  {
    ids[slot] = candidates[slot].id;
    const double exact = candidates[slot].score / total * 255.0;
    weights[slot] = static_cast<uint8_t>(std::floor(exact));
    candidates[slot].remainder = exact - std::floor(exact);
    quantized += weights[slot];
  }
  std::vector<size_t> remainderOrder(candidates.size());
  for (size_t slot = 0u; slot < candidates.size(); ++slot)
    remainderOrder[slot] = slot;
  std::stable_sort(remainderOrder.begin(), remainderOrder.end(),
      [&](const size_t left, const size_t right)
      {
        if (candidates[left].remainder != candidates[right].remainder)
          return candidates[left].remainder > candidates[right].remainder;
        return candidates[left].id < candidates[right].id;
      });
  for (uint32_t value = quantized; value < 255u; ++value)
    ++weights[remainderOrder[(value - quantized) % remainderOrder.size()]];
  packedIds = packBytes(ids);
  packedWeights = packBytes(weights);
}

void biomeMixture(
    const MapGraph &graph,
    const std::vector<BlendWeight> &weights,
    uint32_t &packedIds,
    uint32_t &packedWeights)
{
  std::array<double, 256> scores{};
  for (const BlendWeight &weight : weights)
    if (weight.node < graph.nodeCount() && weight.weight > 0.0)
      scores[graph.node(weight.node).biome] += weight.weight;
  encodeBiomeScores(scores, packedIds, packedWeights);
}

uint32_t mixtureColour(const uint32_t packedIds, const uint32_t packedWeights)
{
  const std::array<uint8_t, kBiomeSlots> ids = unpackBytes(packedIds);
  const std::array<uint8_t, kBiomeSlots> weights = unpackBytes(packedWeights);
  std::array<float, 3> colour{};
  for (size_t slot = 0u; slot < kBiomeSlots; ++slot)
  {
    if (ids[slot] >= static_cast<uint8_t>(procedural::world::Biome::Count))
      continue;
    const std::array<float, 3> contribution =
        biomeColour(static_cast<procedural::world::Biome>(ids[slot]));
    const float weight = weights[slot] / 255.0f;
    for (size_t channel = 0u; channel < colour.size(); ++channel)
      colour[channel] += contribution[channel] * weight;
  }
  return packColour(colour);
}

uint32_t majority4(const std::array<uint32_t, 4> &values)
{
  uint32_t selected = values[0];
  uint32_t selectedCount = 0u;
  for (const uint32_t candidate : values)
  {
    uint32_t count = 0u;
    for (const uint32_t value : values)
      count += value == candidate ? 1u : 0u;
    if (count > selectedCount ||
        (count == selectedCount && candidate < selected))
    {
      selected = candidate;
      selectedCount = count;
    }
  }
  return selected;
}

std::vector<MapMaterialSample> downsampleMaterialLevel(
    const std::vector<MapMaterialSample> &source, const uint32_t sourceSide)
{
  const uint32_t targetSide = sourceSide / 2u;
  std::vector<MapMaterialSample> target(
      static_cast<size_t>(targetSide) * targetSide);
  for (uint32_t y = 0u; y < targetSide; ++y)
    for (uint32_t x = 0u; x < targetSide; ++x)
    {
      std::array<const MapMaterialSample *, 4> children{};
      for (uint32_t childY = 0u; childY < 2u; ++childY)
        for (uint32_t childX = 0u; childX < 2u; ++childX)
          children[childY * 2u + childX] =
              &source[static_cast<size_t>(y * 2u + childY) * sourceSide +
                  x * 2u + childX];
      MapMaterialSample &parent =
          target[static_cast<size_t>(y) * targetSide + x];
      parent.contentColour = majority4({children[0]->contentColour,
          children[1]->contentColour, children[2]->contentColour,
          children[3]->contentColour});
      parent.landformColour = majority4({children[0]->landformColour,
          children[1]->landformColour, children[2]->landformColour,
          children[3]->landformColour});
      parent.regionColour = majority4({children[0]->regionColour,
          children[1]->regionColour, children[2]->regionColour,
          children[3]->regionColour});
      parent.surfaceClass = majority4({children[0]->surfaceClass,
          children[1]->surfaceClass, children[2]->surfaceClass,
          children[3]->surfaceClass});
      std::array<double, 256> scores{};
      for (const MapMaterialSample *child : children)
      {
        const std::array<uint8_t, kBiomeSlots> ids =
            unpackBytes(child->biomeIds);
        const std::array<uint8_t, kBiomeSlots> weights =
            unpackBytes(child->biomeWeights);
        for (size_t slot = 0u; slot < kBiomeSlots; ++slot)
          if (ids[slot] <
              static_cast<uint8_t>(procedural::world::Biome::Count))
            scores[ids[slot]] += weights[slot];
      }
      encodeBiomeScores(scores, parent.biomeIds, parent.biomeWeights);
      const std::array<uint8_t, kBiomeSlots> ids =
          unpackBytes(parent.biomeIds);
      parent.biomeColour = packColour(biomeColour(
          static_cast<procedural::world::Biome>(ids[0])));
      parent.biomeBlendColour =
          mixtureColour(parent.biomeIds, parent.biomeWeights);
    }
  return target;
}

uint32_t materialPageTableBase(const uint32_t level)
{
  return ((uint32_t{1u} << (level * 2u)) - 1u) / 3u;
}

uint32_t materialPageMorton(
    const uint32_t level, const uint32_t x, const uint32_t y)
{
  uint32_t morton = 0u;
  for (uint32_t bit = 0u; bit < level; ++bit)
  {
    morton |= ((x >> bit) & 1u) << (bit * 2u);
    morton |= ((y >> bit) & 1u) << (bit * 2u + 1u);
  }
  return morton;
}

void buildMaterialPages(MapMesh &mesh)
{
  mesh.materialPageTable.clear();
  mesh.materialPages.clear();
  if (mesh.materialField.empty() || mesh.materialPageSize == 0u ||
      mesh.materialFieldSide < mesh.materialPageSize ||
      (mesh.materialFieldSide % mesh.materialPageSize) != 0u)
    return;
  uint32_t dimension = mesh.materialFieldSide / mesh.materialPageSize;
  uint32_t maximumLevel = 0u;
  while (dimension > 1u && (dimension & 1u) == 0u)
  {
    dimension >>= 1u;
    ++maximumLevel;
  }
  if (dimension != 1u || maximumLevel > 15u)
    return;
  mesh.materialMaximumPageLevel = maximumLevel;
  mesh.materialPageStoredSide =
      mesh.materialPageSize + mesh.materialPageGutter * 2u;
  const uint32_t tableSize = materialPageTableBase(maximumLevel + 1u);
  mesh.materialPageTable.assign(tableSize, 0u);
  std::vector<std::vector<MapMaterialSample>> levels(maximumLevel + 1u);
  levels[maximumLevel] = mesh.materialField;
  uint32_t levelSide = mesh.materialFieldSide;
  for (uint32_t level = maximumLevel; level > 0u; --level)
  {
    levels[level - 1u] =
        downsampleMaterialLevel(levels[level], levelSide);
    levelSide /= 2u;
  }
  const size_t pageTexels = static_cast<size_t>(mesh.materialPageStoredSide) *
      mesh.materialPageStoredSide;
  for (uint32_t level = 0u; level <= maximumLevel; ++level)
  {
    const uint32_t levelDimension = uint32_t{1u} << level;
    const uint32_t rasterSide = mesh.materialPageSize * levelDimension;
    for (uint32_t pageY = 0u; pageY < levelDimension; ++pageY)
      for (uint32_t pageX = 0u; pageX < levelDimension; ++pageX)
      {
        const uint32_t physical = static_cast<uint32_t>(
            mesh.materialPages.size() / pageTexels);
        mesh.materialPageTable[materialPageTableBase(level) +
            materialPageMorton(level, pageX, pageY)] = physical + 1u;
        const size_t pageOffset = mesh.materialPages.size();
        mesh.materialPages.resize(pageOffset + pageTexels);
        for (uint32_t storedY = 0u;
             storedY < mesh.materialPageStoredSide; ++storedY)
          for (uint32_t storedX = 0u;
               storedX < mesh.materialPageStoredSide; ++storedX)
          {
            const int64_t globalX =
                static_cast<int64_t>(pageX) * mesh.materialPageSize +
                storedX - mesh.materialPageGutter;
            const int64_t globalY =
                static_cast<int64_t>(pageY) * mesh.materialPageSize +
                storedY - mesh.materialPageGutter;
            const uint32_t sourceX = static_cast<uint32_t>(
                std::clamp<int64_t>(globalX, 0, rasterSide - 1u));
            const uint32_t sourceY = static_cast<uint32_t>(
                std::clamp<int64_t>(globalY, 0, rasterSide - 1u));
            mesh.materialPages[pageOffset +
                static_cast<size_t>(storedY) * mesh.materialPageStoredSide +
                storedX] = levels[level][
                static_cast<size_t>(sourceY) * rasterSide + sourceX];
          }
      }
  }
}

uint32_t bilinearBiomeColour(
    const MapMesh &mesh, const double u, const double v)
{
  if (mesh.materialFieldSide == 0u || mesh.materialField.empty())
    return IM_COL32(255, 0, 255, 255);
  const double maximum = static_cast<double>(mesh.materialFieldSide - 1u);
  const double fx = std::clamp(
      u * static_cast<double>(mesh.materialFieldSide) - 0.5, 0.0, maximum);
  const double fy = std::clamp(
      v * static_cast<double>(mesh.materialFieldSide) - 0.5, 0.0, maximum);
  const uint32_t x0 = static_cast<uint32_t>(std::floor(fx));
  const uint32_t y0 = static_cast<uint32_t>(std::floor(fy));
  const uint32_t x1 = std::min(x0 + 1u, mesh.materialFieldSide - 1u);
  const uint32_t y1 = std::min(y0 + 1u, mesh.materialFieldSide - 1u);
  const double tx = fx - static_cast<double>(x0);
  const double ty = fy - static_cast<double>(y0);
  const auto at = [&mesh](const uint32_t x, const uint32_t y)
  {
    return &mesh.materialField[static_cast<size_t>(y) *
                                   mesh.materialFieldSide +
                               x];
  };
  const std::array<const MapMaterialSample *, 4> samples{
      at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1)};
  const std::array<double, 4> weights{
      (1.0 - tx) * (1.0 - ty), tx * (1.0 - ty),
      (1.0 - tx) * ty, tx * ty};
  double landAmount = 0.0;
  for (size_t sample = 0u; sample < samples.size(); ++sample)
    landAmount += weights[sample] * samples[sample]->surfaceClass;
  const uint32_t surfaceClass = landAmount >= 0.5 ? 1u : 0u;
  double matchingWeight = 0.0;
  for (size_t sample = 0u; sample < samples.size(); ++sample)
    if (samples[sample]->surfaceClass == surfaceClass)
      matchingWeight += weights[sample];
  if (!(matchingWeight > 0.0))
    return IM_COL32(255, 0, 255, 255);
  std::array<double, 256> scores{};
  std::array<double, 256> priorities{};
  for (size_t sample = 0u; sample < samples.size(); ++sample)
  {
    if (samples[sample]->surfaceClass != surfaceClass)
      continue;
    const std::array<uint8_t, kBiomeSlots> ids =
        unpackBytes(samples[sample]->biomeIds);
    const std::array<uint8_t, kBiomeSlots> biomeWeights =
        unpackBytes(samples[sample]->biomeWeights);
    for (size_t slot = 0u; slot < kBiomeSlots; ++slot)
    {
      const double contribution = weights[sample] *
          (biomeWeights[slot] / 255.0);
      scores[ids[slot]] += contribution;
      priorities[ids[slot]] +=
          contribution * static_cast<double>(kBiomeSlots - slot);
    }
  }
  struct Candidate
  {
    uint8_t id = 0u;
    double score = 0.0;
    double priority = 0.0;
  };
  std::vector<Candidate> candidates;
  for (uint32_t id = 0u;
       id < static_cast<uint32_t>(procedural::world::Biome::Count); ++id)
    if (scores[id] > 0.0)
      candidates.push_back(
          {static_cast<uint8_t>(id), scores[id], priorities[id]});
  std::stable_sort(candidates.begin(), candidates.end(),
      [](const Candidate &left, const Candidate &right)
      {
        if (left.score != right.score)
          return left.score > right.score;
        if (left.priority != right.priority)
          return left.priority > right.priority;
        return left.id < right.id;
      });
  if (candidates.size() > kBiomeSlots)
    candidates.resize(kBiomeSlots);
  std::array<float, 3> colour{};
  double selectedTotal = 0.0;
  for (const Candidate &candidate : candidates)
  {
    const std::array<float, 3> contribution = biomeColour(
        static_cast<procedural::world::Biome>(candidate.id));
    for (size_t channel = 0u; channel < colour.size(); ++channel)
      colour[channel] +=
          static_cast<float>(candidate.score) * contribution[channel];
    selectedTotal += candidate.score;
  }
  if (!(selectedTotal > 0.0))
    return IM_COL32(255, 0, 255, 255);
  for (float &channel : colour)
    channel /= static_cast<float>(selectedTotal);
  return packColour(colour);
}

// ImGui indices are 16 bit and PrimReserve can only start a fresh vertex
// offset between reservations, so a batch must stay under that. Left well
// clear of the limit because a batch is only closed on a cell boundary.
constexpr size_t kMaximumBatchVertices = 60000u;
static_assert(sizeof(MapVertex) == 32u && offsetof(MapVertex, contentColour) == 8u && offsetof(MapVertex, elevationMetres) == 28u, "WorldGeneratorMap.wgsl must match MapVertex's packed GPU layout");
static_assert(sizeof(MapMaterialSample) == 32u, "WorldGeneratorMap.wgsl must match the material-field layout");
static_assert(static_cast<uint8_t>(MapLayer::Content) == 0u && static_cast<uint8_t>(MapLayer::Landform) == 1u && static_cast<uint8_t>(MapLayer::Elevation) == 2u && static_cast<uint8_t>(MapLayer::Uplift) == 3u && static_cast<uint8_t>(MapLayer::Biome) == 4u && static_cast<uint8_t>(MapLayer::Heightmap) == 5u && static_cast<uint8_t>(MapLayer::Region) == 6u, "WorldGeneratorMap.wgsl layer switch must match MapLayer");

} // namespace

MapMesh buildMapMesh(
    const MapGraph &graph,
    const std::vector<CoastPolyline> &coastline,
    const RiverNetwork &rivers,
    const RegionMap &regions,
    const ContourSet &contours,
    const procedural::world::MapSurfaceField &surface,
    const SiteIndex &index,
    const BlendSettings &settings,
    const MapShading &shading)
{
  MapMesh mesh{};
  mesh.coastline = coastline;
  mesh.contours = contours.lines;
  mesh.largestDischargeCumecs = rivers.largestDischargeCumecs;

  // Drainage geometry stays faithful to the generated graph. Catmull-Rom is
  // reserved for scalar contour presentation; applying it here could swing a
  // river outside its valley or across a categorical boundary.
  mesh.rivers = rivers.segments;
  for (const procedural::world::Waterfall &waterfall : rivers.waterfalls)
  {
    if (waterfall.node >= graph.nodeCount() || waterfall.node >= rivers.nodes.size())
      continue;
    const uint32_t downstream = rivers.nodes[waterfall.node].downstream;
    if (downstream == kInvalid || downstream >= graph.nodeCount())
      continue;
    const Point2 from = graph.node(waterfall.node).site;
    const Point2 to = graph.node(downstream).site;
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double span = std::hypot(dx, dy);
    if (!(span > 0.0))
      continue;
    const double half = span * 0.16;
    const double nx = -dy / span;
    const double ny = dx / span;
    mesh.waterfallMarks.push_back({
        static_cast<float>(from.x - nx * half),
        static_cast<float>(from.y - ny * half),
        static_cast<float>(from.x + nx * half),
        static_cast<float>(from.y + ny * half)});
  }

  for (const MapRegion &region : regions.regions)
    if (!region.name.empty())
      mesh.labels.push_back(MapLabel{region.centroid, region.name, 1u, region.areaSquareMetres});
  // Sorted by rank then weight, so the collision pass at draw time keeps the
  // labels that matter. Done once here rather than every frame.
  for (const procedural::world::NamedRiver &river : regions.rivers)
  {
    if (river.stem.empty() || river.name.empty())
      continue;
    // A third of the way up from the mouth: far enough off the coast not to
    // collide with the shore, still on the part of the course that is wide
    // enough to be worth labelling.
    const uint32_t anchor = river.stem[river.stem.size() / 3u];
    mesh.labels.push_back(MapLabel{graph.node(anchor).site, river.name, 0u, river.dischargeCumecs});
  }
  std::stable_sort(
      mesh.labels.begin(),
      mesh.labels.end(),
      [](const MapLabel &lhs, const MapLabel &rhs)
      {
        if (lhs.rank != rhs.rank)
          return lhs.rank > rhs.rank;
        return lhs.weight > rhs.weight;
      });
  if (graph.nodeCount() == 0u || index.empty())
    return mesh;

  // Boundaries as a shared structure, then warped once. Both cells reading a
  // boundary read the same points, so nothing has to be reconciled.
  MapEdgeSet boundaries = buildEdgeSet(graph, shading.weldToleranceMetres);
  EdgeRelaxSettings relax{};
  relax.segmentMetres = shading.edgeSegmentMetres;
  relax.amplitudeMetres = shading.edgeAmplitudeMetres;
  relax.wavelengthMetres = shading.edgeWavelengthMetres;
  relax.classificationScale = shading.boundaryNoiseScale;
  relax.seed = graph.brief().seed;
  relaxEdges(boundaries, relax);

  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || node.area.size() < 3u)
      continue;
    MapCellShape shape{};
    shape.site = node.site;
    shape.node = node.id;
    shape.colour = packColour(nodeColour(node, shading));
    shape.boundary = cellBoundary(boundaries, node.id);
    if (shape.boundary.size() >= 3u)
      mesh.cells.push_back(std::move(shape));
  }

  mesh.batches.emplace_back();
  MapShading contentShading = shading;
  contentShading.layer = MapLayer::Content;
  MapShading landformShading = shading;
  landformShading.layer = MapLayer::Landform;
  MapShading biomeShading = shading;
  biomeShading.layer = MapLayer::Biome;
  MapShading regionShading = shading;
  regionShading.layer = MapLayer::Region;

  // A separate categorical field, analogous to the continuous elevation
  // field. Original cell/region picks contribute smooth influence, elevation
  // variation reshapes it, and arg-max resolves a hard material at each texel.
  // This creates natural threshold contours without interpolating colours.
  mesh.materialFieldSide = surface.gridSide() > 512u ? 1024u : 512u;
  mesh.materialFieldExtentMetres = graph.brief().extentMetres;
  mesh.materialField.resize(
      static_cast<size_t>(mesh.materialFieldSide) * mesh.materialFieldSide);
  std::vector<BlendWeight> fieldWeights;
  for (uint32_t y = 0u; y < mesh.materialFieldSide; ++y)
    for (uint32_t x = 0u; x < mesh.materialFieldSide; ++x)
    {
      const Point2 position(
          (static_cast<double>(x) + 0.5) /
              static_cast<double>(mesh.materialFieldSide) *
              mesh.materialFieldExtentMetres,
          (static_cast<double>(y) + 0.5) /
              static_cast<double>(mesh.materialFieldSide) *
              mesh.materialFieldExtentMetres);
      const procedural::world::MapSurfaceSample scalar = surface.sample(position);
      blendAt(graph, index, position, settings, fieldWeights);
      if (fieldWeights.empty())
        continue;
      const uint32_t fallback = fieldWeights.front().node;
      bool land = scalar.elevationMetres >= shading.seaLevelMetres;
      if (fallback < graph.nodeCount() &&
          graph.node(fallback).type == CellType::Lake)
        land = false;
      restrictBlendToSurface(graph, land, fieldWeights);
      applyCellInfluenceWeights(
          graph, shading.heightBlend.cellWeightStrength, fieldWeights);
      shapeBlendByHeight(
          graph,
          scalar.elevationMetres,
          localHeightVariation(surface, position, scalar.elevationMetres),
          shading.reliefMetres,
          shading.heightBlend,
          fieldWeights);
      // Keep both products of the same biome votes: a categorical winner for
      // simulation/debugging, and a continuous colour for map presentation.
      uint32_t biomeIds = 0u;
      uint32_t biomeWeights = 0u;
      biomeMixture(graph, fieldWeights, biomeIds, biomeWeights);
      mesh.materialField[static_cast<size_t>(y) * mesh.materialFieldSide + x] =
          MapMaterialSample{
              thresholdedColour(graph, fieldWeights, contentShading),
              thresholdedColour(graph, fieldWeights, landformShading),
              thresholdedColour(graph, fieldWeights, biomeShading),
              mixtureColour(biomeIds, biomeWeights),
              thresholdedColour(graph, fieldWeights, regionShading),
              land ? 1u : 0u,
              biomeIds,
              biomeWeights};
    }
  buildMaterialPages(mesh);

  const auto vertexAt = [&](const Point2 position, const MapNode &owner)
  {
    const procedural::world::MapSurfaceSample scalar = surface.sample(position);
    std::array<float, 3> content{};
    std::array<float, 3> landform{};
    std::array<float, 3> uplift{};
    std::array<float, 3> biome{};
    std::array<float, 3> region{};
    const auto accumulate = [&](const MapNode &node, const double weight)
    {
      const std::array<float, 3> contentContribution = nodeColour(node, contentShading);
      const std::array<float, 3> landformContribution = nodeColour(node, landformShading);
      const std::array<float, 3> biomeContribution = nodeColour(node, biomeShading);
      const std::array<float, 3> regionContribution = nodeColour(node, regionShading);
      for (size_t channel = 0u; channel < 3u; ++channel)
      {
        const float amount = static_cast<float>(weight);
        content[channel] += amount * contentContribution[channel];
        landform[channel] += amount * landformContribution[channel];
        biome[channel] += amount * biomeContribution[channel];
        region[channel] += amount * regionContribution[channel];
      }
    };
    // Categorical GPU layers read the threshold field. The owner colours remain
    // useful for the raw-cell diagnostic and the non-GPU fallback.
    accumulate(owner, 1.0);
    const double peak = std::max(1.0e-12, shading.peakUpliftMetresPerYear);
    const double upliftT = std::clamp(
        scalar.upliftRateMetresPerYear / peak, 0.0, 1.0);
    static const std::vector<std::pair<double, std::array<float, 3>>>
        upliftRamp{{0.00, {0.110f, 0.114f, 0.145f}},
            {0.35, {0.310f, 0.208f, 0.267f}},
            {0.70, {0.686f, 0.353f, 0.243f}},
            {1.00, {0.988f, 0.855f, 0.522f}}};
    uplift = rampColour(upliftRamp, upliftT);
    return MapVertex{
      static_cast<float>(position.x),
      static_cast<float>(position.y),
      packColour(content),
      packColour(landform),
      packColour(uplift),
      packColour(biome),
      packColour(region),
      static_cast<float>(scalar.elevationMetres),
    };
  };

  for (const MapCellShape &shape : mesh.cells)
  {
    MapBatch *batch = &mesh.batches.back();
    if (batch->vertices.size() + 1u + shape.boundary.size() > kMaximumBatchVertices)
    {
      mesh.batches.emplace_back();
      batch = &mesh.batches.back();
    }

    const uint16_t centre = static_cast<uint16_t>(batch->vertices.size());
    const MapNode &owner = graph.node(shape.node);
    batch->vertices.push_back(vertexAt(shape.site, owner));
    const size_t ring = batch->vertices.size();
    for (const Point2 &point : shape.boundary)
      batch->vertices.push_back(vertexAt(point, owner));
    for (size_t step = 0u; step < shape.boundary.size(); ++step)
    {
      batch->indices.push_back(centre);
      batch->indices.push_back(static_cast<uint16_t>(ring + step));
      batch->indices.push_back(static_cast<uint16_t>(ring + (step + 1u) % shape.boundary.size()));
      ++mesh.triangleCount;
    }
  }

  // Debug cell edges use the same shared geometry as the fill. Categorical
  // boundaries are intentionally not copied into an outline overlay: their
  // improved geometry is visible through the filled layers themselves.
  for (const MapBoundaryEdge &edge : boundaries.edges)
  {
    for (size_t point = 0u; point + 1u < edge.path.size(); ++point)
    {
      const std::array<float, 4> segment{static_cast<float>(edge.path[point].x), static_cast<float>(edge.path[point].y), static_cast<float>(edge.path[point + 1u].x), static_cast<float>(edge.path[point + 1u].y)};
      mesh.cellEdges.push_back(segment);
    }
  }

  return mesh;
}

MapView fitView(const double extentMetres, const float widthPixels, const float heightPixels)
{
  MapView view{};
  view.centreX = extentMetres * 0.5;
  view.centreY = extentMetres * 0.5;
  const double width = std::max(1.0, static_cast<double>(widthPixels));
  const double height = std::max(1.0, static_cast<double>(heightPixels));
  // A margin, so the world border is visible rather than flush with the panel.
  view.metresPerPixel = extentMetres / std::min(width, height) * 1.06;
  return view;
}

WorldMapGpuPass::WorldMapGpuPass(WorldMapRHI &rhi, std::string shaderPath) : rhi_(rhi), shaderPath_(std::move(shaderPath))
{
}

WorldMapGpuPass::~WorldMapGpuPass()
{
  destroy();
}

bool WorldMapGpuPass::ready() const
{
  return pipeline_ != VK_NULL_HANDLE && descriptorSet_ != VK_NULL_HANDLE &&
      vertexBuffer_ != gpu::BufferId::Invalid &&
      indexBuffer_ != gpu::BufferId::Invalid &&
      materialPageTableBuffer_ != gpu::BufferId::Invalid &&
      materialPageBuffer_ != gpu::BufferId::Invalid && indexCount_ != 0u;
}

bool WorldMapGpuPass::linesReady() const
{
  return linePipeline_ != VK_NULL_HANDLE && descriptorSet_ != VK_NULL_HANDLE && lineInstanceBuffer_ != gpu::BufferId::Invalid && lineInstanceCount_ != 0u;
}

void WorldMapGpuPass::initialize(const VkRenderPass renderPass)
{
  destroy();
  uniformBuffer_ = rhi_.createBuffer({
    .name = "worldGenerator.map.uniforms",
    .size = sizeof(Uniforms),
    .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Uniform | gpu::BufferUsage_Push),
  });
  createDescriptors();
  createPipeline(renderPass);
}

void WorldMapGpuPass::destroyMesh()
{
  if (materialPageBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(materialPageBuffer_);
    materialPageBuffer_ = gpu::BufferId::Invalid;
  }
  if (materialPageTableBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(materialPageTableBuffer_);
    materialPageTableBuffer_ = gpu::BufferId::Invalid;
  }
  if (lineInstanceBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(lineInstanceBuffer_);
    lineInstanceBuffer_ = gpu::BufferId::Invalid;
  }
  if (indexBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(indexBuffer_);
    indexBuffer_ = gpu::BufferId::Invalid;
  }
  if (vertexBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(vertexBuffer_);
    vertexBuffer_ = gpu::BufferId::Invalid;
  }
  vertexCount_ = 0u;
  indexCount_ = 0u;
  lineInstanceCount_ = 0u;
  materialPageCount_ = 0u;
  materialPageBytes_ = 0u;
  materialFieldSide_ = 0u;
  materialFieldExtentMetres_ = 0.0f;
  materialPageSize_ = 0u;
  materialPageGutter_ = 0u;
  materialPageStoredSide_ = 0u;
  materialMaximumPageLevel_ = 0u;
  cellLines_ = {};
  riverLines_ = {};
  minorContourLines_ = {};
  majorContourLines_ = {};
  coastLines_ = {};
}

void WorldMapGpuPass::destroy()
{
  const VkDevice device = rhi_.getDevice();
  destroyMesh();
  if (uniformBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(uniformBuffer_);
    uniformBuffer_ = gpu::BufferId::Invalid;
  }
  if (pipeline_ != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(device, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
  }
  if (linePipeline_ != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(device, linePipeline_, nullptr);
    linePipeline_ = VK_NULL_HANDLE;
  }
  if (pipelineLayout_ != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    pipelineLayout_ = VK_NULL_HANDLE;
  }
  if (shaderModule_ != VK_NULL_HANDLE)
  {
    vkDestroyShaderModule(device, shaderModule_, nullptr);
    shaderModule_ = VK_NULL_HANDLE;
  }
  if (descriptorPool_ != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
  }
  if (descriptorSetLayout_ != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
    descriptorSetLayout_ = VK_NULL_HANDLE;
  }
}

bool WorldMapGpuPass::upload(const MapMesh &mesh, std::string *error)
{
  std::vector<MapVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<LineInstance> lines;
  size_t totalVertices = 0u;
  size_t totalIndices = 0u;
  for (const MapBatch &batch : mesh.batches)
  {
    totalVertices += batch.vertices.size();
    totalIndices += batch.indices.size();
  }
  vertices.reserve(totalVertices);
  indices.reserve(totalIndices);
  for (const MapBatch &batch : mesh.batches)
  {
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    vertices.insert(vertices.end(), batch.vertices.begin(), batch.vertices.end());
    for (const uint16_t index : batch.indices)
      indices.push_back(base + index);
  }
  if (vertices.empty() || indices.empty())
  {
    if (error != nullptr)
      *error = "map mesh has no fill geometry";
    return false;
  }
  if (mesh.materialField.empty() || mesh.materialFieldSide == 0u ||
      mesh.materialField.size() !=
          static_cast<size_t>(mesh.materialFieldSide) * mesh.materialFieldSide)
  {
    if (error != nullptr)
      *error = "map mesh has no thresholded material field";
    return false;
  }
  if (mesh.materialPageTable.empty() || mesh.materialPages.empty() ||
      mesh.materialPageSize == 0u || mesh.materialPageStoredSide == 0u)
  {
    if (error != nullptr)
      *error = "map mesh has no GPU material page pyramid";
    return false;
  }

  const auto beginRange = [&lines]()
  {
    return LineRange{static_cast<uint32_t>(lines.size()), 0u};
  };
  const auto finishRange = [&lines](LineRange &range)
  {
    range.count = static_cast<uint32_t>(lines.size()) - range.firstInstance;
  };
  const auto appendLine = [&lines](const Point2 &from, const Point2 &to, const uint32_t colour, const float widthFactor, const uint32_t style)
  {
    lines.push_back(LineInstance{
      static_cast<float>(from.x),
      static_cast<float>(from.y),
      static_cast<float>(to.x),
      static_cast<float>(to.y),
      colour,
      widthFactor,
      style,
      0u,
    });
  };
  const auto appendSegments = [&appendLine](const std::vector<Point2> &points, const bool closed, const uint32_t colour, const float widthFactor, const uint32_t style)
  {
    for (size_t point = 0u; point + 1u < points.size(); ++point)
      appendLine(points[point], points[point + 1u], colour, widthFactor, style);
    if (closed && points.size() > 2u)
      appendLine(points.back(), points.front(), colour, widthFactor, style);
  };

  LineRange cellLines = beginRange();
  for (const std::array<float, 4> &edge : mesh.cellEdges)
    appendLine(Point2(edge[0], edge[1]), Point2(edge[2], edge[3]), IM_COL32(255, 255, 255, 36), 1.0f, 0u);
  finishRange(cellLines);

  LineRange riverLines = beginRange();
  for (const RiverSegment &river : mesh.rivers)
  {
    const float widthFactor = riverWidthFactor(river.dischargeCumecs, mesh.largestDischargeCumecs);
    appendLine(river.from, river.to, IM_COL32(64, 120, 168, 255), widthFactor, 1u);
  }
  for (const std::array<float, 4> &waterfall : mesh.waterfallMarks)
    appendLine(Point2(waterfall[0], waterfall[1]), Point2(waterfall[2], waterfall[3]), IM_COL32(196, 226, 242, 255), 0.55f, 1u);
  finishRange(riverLines);

  LineRange minorContourLines = beginRange();
  for (const ContourPolyline &contour : mesh.contours)
    if (!contour.major)
      appendSegments(contour.points, contour.closed, IM_COL32(38, 32, 25, 105), 1.0f, 2u);
  finishRange(minorContourLines);

  LineRange majorContourLines = beginRange();
  for (const ContourPolyline &contour : mesh.contours)
    if (contour.major)
      appendSegments(contour.points, contour.closed, IM_COL32(30, 24, 18, 155), 1.0f, 3u);
  finishRange(majorContourLines);

  LineRange coastLines = beginRange();
  for (const CoastPolyline &coast : mesh.coastline)
    appendSegments(coast.points, coast.closed, IM_COL32(16, 22, 30, 235), 1.0f, 4u);
  finishRange(coastLines);

  try
  {
    // Replacement is rare (generation or a layer rebuild) and old buffers may
    // still be referenced by an in-flight frame. Waiting here keeps lifetime
    // explicit without imposing a stall on pan/zoom frames.
    rhi_.waitIdle();
    destroyMesh();
    vertexBuffer_ = rhi_.createBuffer({
      .name = "worldGenerator.map.vertices",
      .size = vertices.size() * sizeof(MapVertex),
      .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Vertex | gpu::BufferUsage_Push),
    });
    indexBuffer_ = rhi_.createBuffer({
      .name = "worldGenerator.map.indices",
      .size = indices.size() * sizeof(uint32_t),
      .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Index | gpu::BufferUsage_Push),
    });
    materialPageTableBuffer_ = rhi_.createBuffer({
      .name = "worldGenerator.map.materialPageTable",
      .size = mesh.materialPageTable.size() * sizeof(uint32_t),
      .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Storage | gpu::BufferUsage_Push),
    });
    materialPageBuffer_ = rhi_.createBuffer({
      .name = "worldGenerator.map.materialPages",
      .size = mesh.materialPages.size() * sizeof(MapMaterialSample),
      .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Storage | gpu::BufferUsage_Push),
    });
    if (!lines.empty())
      lineInstanceBuffer_ = rhi_.createBuffer({
        .name = "worldGenerator.map.lines",
        .size = lines.size() * sizeof(LineInstance),
        .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Vertex | gpu::BufferUsage_Push),
      });
    rhi_.bufferWrite(vertexBuffer_, 0u, vertices.size() * sizeof(MapVertex), vertices.data());
    rhi_.bufferWrite(indexBuffer_, 0u, indices.size() * sizeof(uint32_t), indices.data());
    rhi_.bufferWrite(materialPageTableBuffer_, 0u,
        mesh.materialPageTable.size() * sizeof(uint32_t),
        const_cast<uint32_t *>(mesh.materialPageTable.data()));
    rhi_.bufferWrite(materialPageBuffer_, 0u,
        mesh.materialPages.size() * sizeof(MapMaterialSample),
        const_cast<MapMaterialSample *>(mesh.materialPages.data()));
    if (lineInstanceBuffer_ != gpu::BufferId::Invalid)
      rhi_.bufferWrite(lineInstanceBuffer_, 0u, lines.size() * sizeof(LineInstance), lines.data());
    vertexCount_ = vertices.size();
    indexCount_ = indices.size();
    lineInstanceCount_ = lines.size();
    materialFieldSide_ = mesh.materialFieldSide;
    materialFieldExtentMetres_ =
        static_cast<float>(mesh.materialFieldExtentMetres);
    materialPageSize_ = mesh.materialPageSize;
    materialPageGutter_ = mesh.materialPageGutter;
    materialPageStoredSide_ = mesh.materialPageStoredSide;
    materialMaximumPageLevel_ = mesh.materialMaximumPageLevel;
    const size_t pageTexels = static_cast<size_t>(materialPageStoredSide_) *
        materialPageStoredSide_;
    materialPageCount_ = pageTexels == 0u
        ? 0u
        : mesh.materialPages.size() / pageTexels;
    materialPageBytes_ =
        mesh.materialPages.size() * sizeof(MapMaterialSample) +
        mesh.materialPageTable.size() * sizeof(uint32_t);
    std::array<VkDescriptorBufferInfo, 2u> materialInfos{};
    materialInfos[0].buffer = rhi_.bufferHandle(materialPageTableBuffer_);
    materialInfos[0].range =
        mesh.materialPageTable.size() * sizeof(uint32_t);
    materialInfos[1].buffer = rhi_.bufferHandle(materialPageBuffer_);
    materialInfos[1].range =
        mesh.materialPages.size() * sizeof(MapMaterialSample);
    std::array<VkWriteDescriptorSet, 2u> materialWrites{};
    for (uint32_t binding = 0u; binding < materialWrites.size(); ++binding)
    {
      materialWrites[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      materialWrites[binding].dstSet = descriptorSet_;
      materialWrites[binding].dstBinding = binding + 1u;
      materialWrites[binding].descriptorCount = 1u;
      materialWrites[binding].descriptorType =
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      materialWrites[binding].pBufferInfo = &materialInfos[binding];
    }
    vkUpdateDescriptorSets(rhi_.getDevice(),
        static_cast<uint32_t>(materialWrites.size()), materialWrites.data(),
        0u, nullptr);
    cellLines_ = cellLines;
    riverLines_ = riverLines;
    minorContourLines_ = minorContourLines;
    majorContourLines_ = majorContourLines;
    coastLines_ = coastLines;
    return true;
  }
  catch (const std::exception &exception)
  {
    destroyMesh();
    if (error != nullptr)
      *error = exception.what();
    return false;
  }
}

void WorldMapGpuPass::render(
    const VkCommandBuffer commandBuffer,
    const VkExtent2D framebufferExtent,
    const MapView &view,
    const ImVec2 origin,
    const ImVec2 size,
    const MapShading &shading,
    const MapOverlayStyle &style,
    const bool drawFill,
    const bool drawLines)
{
  const bool canDrawFill = drawFill && ready();
  const bool canDrawLines = drawLines && linesReady();
  if ((!canDrawFill && !canDrawLines) || framebufferExtent.width == 0u || framebufferExtent.height == 0u || !(view.metresPerPixel > 0.0) || !(size.x > 0.0f) || !(size.y > 0.0f))
    return;

  const double inverseMetresPerPixel = 1.0 / view.metresPerPixel;
  Uniforms uniforms{};
  uniforms.transform = {
    static_cast<float>(2.0 * inverseMetresPerPixel / static_cast<double>(framebufferExtent.width)),
    static_cast<float>(-2.0 * inverseMetresPerPixel / static_cast<double>(framebufferExtent.height)),
    static_cast<float>(2.0 * (static_cast<double>(origin.x + size.x * 0.5f) - view.centreX * inverseMetresPerPixel) / static_cast<double>(framebufferExtent.width) - 1.0),
    static_cast<float>(2.0 * (static_cast<double>(origin.y + size.y * 0.5f) + view.centreY * inverseMetresPerPixel) / static_cast<double>(framebufferExtent.height) - 1.0),
  };
  uniforms.viewport = {
    static_cast<float>(framebufferExtent.width),
    static_cast<float>(framebufferExtent.height),
    1.0f / static_cast<float>(framebufferExtent.width),
    1.0f / static_cast<float>(framebufferExtent.height),
  };
  uniforms.lineWidths0 = {1.0f, style.riverWidthScale, style.contourThickness, style.majorContourThickness};
  uniforms.lineWidths1 = {style.coastlineThickness, 0.0f, 0.0f, 0.0f};
  const double bearing = shading.sunBearingDegrees * 3.14159265358979 / 180.0;
  const double altitude = shading.sunAltitudeDegrees * 3.14159265358979 / 180.0;
  uniforms.shading = {
    static_cast<float>(shading.seaLevelMetres),
    static_cast<float>(std::max(1.0, shading.reliefMetres)),
    static_cast<float>(shading.verticalExaggeration),
    static_cast<float>(view.metresPerPixel),
  };
  uniforms.light = {
    static_cast<float>(std::sin(bearing) * std::cos(altitude)),
    static_cast<float>(std::cos(bearing) * std::cos(altitude)),
    static_cast<float>(std::sin(altitude)),
    materialFieldExtentMetres_,
  };
  uniforms.options = {static_cast<uint32_t>(shading.layer),
      materialFieldSide_, shading.flatCells ? 1u : 0u,
      shading.materialResidentLevel};
  uniforms.paging = {materialPageSize_, materialPageGutter_,
      materialMaximumPageLevel_, materialPageStoredSide_};
  rhi_.bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), static_cast<void *>(&uniforms));

  VkViewport viewport{};
  viewport.width = static_cast<float>(framebufferExtent.width);
  viewport.height = static_cast<float>(framebufferExtent.height);
  viewport.maxDepth = 1.0f;
  const int32_t left = std::clamp(static_cast<int32_t>(std::floor(origin.x)), 0, static_cast<int32_t>(framebufferExtent.width));
  const int32_t top = std::clamp(static_cast<int32_t>(std::floor(origin.y)), 0, static_cast<int32_t>(framebufferExtent.height));
  const int32_t right = std::clamp(static_cast<int32_t>(std::ceil(origin.x + size.x)), 0, static_cast<int32_t>(framebufferExtent.width));
  const int32_t bottom = std::clamp(static_cast<int32_t>(std::ceil(origin.y + size.y)), 0, static_cast<int32_t>(framebufferExtent.height));
  if (right <= left || bottom <= top)
    return;
  VkRect2D scissor{};
  scissor.offset = {left, top};
  scissor.extent = {static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)};

  vkCmdSetViewport(commandBuffer, 0u, 1u, &viewport);
  vkCmdSetScissor(commandBuffer, 0u, 1u, &scissor);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0u, 1u, &descriptorSet_, 0u, nullptr);
  const VkDeviceSize offset = 0u;
  if (canDrawFill)
  {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkBuffer vertexBuffer = rhi_.bufferHandle(vertexBuffer_);
    vkCmdBindVertexBuffers(commandBuffer, 0u, 1u, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(commandBuffer, rhi_.bufferHandle(indexBuffer_), 0u, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(indexCount_), 1u, 0u, 0, 0u);
  }
  if (canDrawLines)
  {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);
    const VkBuffer lineBuffer = rhi_.bufferHandle(lineInstanceBuffer_);
    vkCmdBindVertexBuffers(commandBuffer, 0u, 1u, &lineBuffer, &offset);
    const auto drawRange = [commandBuffer](const LineRange &range)
    {
      if (range.count != 0u)
        vkCmdDraw(commandBuffer, 6u, range.count, 0u, range.firstInstance);
    };
    if (style.showCellEdges)
      drawRange(cellLines_);
    if (style.showRivers)
      drawRange(riverLines_);
    if (style.showContours)
    {
      drawRange(minorContourLines_);
      drawRange(majorContourLines_);
    }
    if (style.showCoastline)
      drawRange(coastLines_);
  }
}

std::string WorldMapGpuPass::readBinaryFile(const std::string &path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("Could not open world map shader: " + path);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

VkShaderModule WorldMapGpuPass::createShaderModule(const VkDevice device, const std::string &spirv)
{
  if (spirv.empty() || (spirv.size() % sizeof(uint32_t)) != 0u)
    throw std::runtime_error("World map shader is not valid SPIR-V");
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = spirv.size();
  info.pCode = reinterpret_cast<const uint32_t *>(spirv.data());
  VkShaderModule module = VK_NULL_HANDLE;
  WorldMapOverlayPass::checkVkResult(vkCreateShaderModule(device, &info, nullptr, &module));
  return module;
}

void WorldMapGpuPass::createDescriptors()
{
  static_assert(sizeof(Uniforms) == 128u, "WorldGeneratorMap.wgsl must match the GPU uniform layout");
  std::array<VkDescriptorSetLayoutBinding, 3u> bindings{};
  bindings[0].binding = 0u;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  bindings[0].descriptorCount = 1u;
  bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[1].binding = 1u;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[1].descriptorCount = 1u;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[2].binding = 2u;
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[2].descriptorCount = 1u;
  bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();
  WorldMapOverlayPass::checkVkResult(vkCreateDescriptorSetLayout(rhi_.getDevice(), &layoutInfo, nullptr, &descriptorSetLayout_));

  std::array<VkDescriptorPoolSize, 2u> poolSizes{};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[0].descriptorCount = 1u;
  poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSizes[1].descriptorCount = 2u;
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1u;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  WorldMapOverlayPass::checkVkResult(vkCreateDescriptorPool(rhi_.getDevice(), &poolInfo, nullptr, &descriptorPool_));

  VkDescriptorSetAllocateInfo allocateInfo{};
  allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocateInfo.descriptorPool = descriptorPool_;
  allocateInfo.descriptorSetCount = 1u;
  allocateInfo.pSetLayouts = &descriptorSetLayout_;
  WorldMapOverlayPass::checkVkResult(vkAllocateDescriptorSets(rhi_.getDevice(), &allocateInfo, &descriptorSet_));

  VkDescriptorBufferInfo bufferInfo{};
  bufferInfo.buffer = rhi_.bufferHandle(uniformBuffer_);
  bufferInfo.range = sizeof(Uniforms);
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = descriptorSet_;
  write.dstBinding = 0u;
  write.descriptorCount = 1u;
  write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  write.pBufferInfo = &bufferInfo;
  vkUpdateDescriptorSets(rhi_.getDevice(), 1u, &write, 0u, nullptr);
}

void WorldMapGpuPass::createPipeline(const VkRenderPass renderPass)
{
  shaderModule_ = createShaderModule(rhi_.getDevice(), readBinaryFile(shaderPath_));
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1u;
  pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
  WorldMapOverlayPass::checkVkResult(vkCreatePipelineLayout(rhi_.getDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout_));

  std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};
  shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shaderStages[0].module = shaderModule_;
  shaderStages[0].pName = "vs_main";
  shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shaderStages[1].module = shaderModule_;
  shaderStages[1].pName = "fs_main";

  VkVertexInputBindingDescription vertexBinding{};
  vertexBinding.binding = 0u;
  vertexBinding.stride = sizeof(MapVertex);
  vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 7> attributes{};
  attributes[0] = {0u, 0u, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(MapVertex, x))};
  attributes[1] = {1u, 0u, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(MapVertex, contentColour))};
  attributes[2] = {2u, 0u, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(MapVertex, landformColour))};
  attributes[3] = {3u, 0u, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(MapVertex, upliftColour))};
  attributes[4] = {4u, 0u, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(MapVertex, biomeColour))};
  attributes[5] = {5u, 0u, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(MapVertex, regionColour))};
  attributes[6] = {6u, 0u, VK_FORMAT_R32_SFLOAT, static_cast<uint32_t>(offsetof(MapVertex, elevationMetres))};
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1u;
  vertexInput.pVertexBindingDescriptions = &vertexBinding;
  vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
  vertexInput.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1u;
  viewportState.scissorCount = 1u;
  std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
  dynamicState.pDynamicStates = dynamicStates.data();
  VkPipelineRasterizationStateCreateInfo rasterization{};
  rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization.cullMode = VK_CULL_MODE_NONE;
  rasterization.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_FALSE;
  depthStencil.depthWriteEnable = VK_FALSE;
  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1u;
  blend.pAttachments = &blendAttachment;

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
  pipelineInfo.pStages = shaderStages.data();
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterization;
  pipelineInfo.pMultisampleState = &multisample;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &blend;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = pipelineLayout_;
  pipelineInfo.renderPass = renderPass;
  pipelineInfo.subpass = 0u;
  WorldMapOverlayPass::checkVkResult(vkCreateGraphicsPipelines(rhi_.getDevice(), VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &pipeline_));

  static_assert(sizeof(LineInstance) == 32u);
  static_assert(offsetof(LineInstance, colour) == 16u);
  static_assert(offsetof(LineInstance, widthFactor) == 20u);
  static_assert(offsetof(LineInstance, style) == 24u);
  shaderStages[0].pName = "line_vs";
  shaderStages[1].pName = "line_fs";

  VkVertexInputBindingDescription lineVertexBinding{};
  lineVertexBinding.binding = 0u;
  lineVertexBinding.stride = sizeof(LineInstance);
  lineVertexBinding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
  std::array<VkVertexInputAttributeDescription, 5> lineAttributes{};
  lineAttributes[0] = {0u, 0u, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(LineInstance, fromX))};
  lineAttributes[1] = {1u, 0u, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(LineInstance, toX))};
  lineAttributes[2] = {2u, 0u, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(LineInstance, colour))};
  lineAttributes[3] = {3u, 0u, VK_FORMAT_R32_SFLOAT, static_cast<uint32_t>(offsetof(LineInstance, widthFactor))};
  lineAttributes[4] = {4u, 0u, VK_FORMAT_R32_UINT, static_cast<uint32_t>(offsetof(LineInstance, style))};
  vertexInput.pVertexBindingDescriptions = &lineVertexBinding;
  vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(lineAttributes.size());
  vertexInput.pVertexAttributeDescriptions = lineAttributes.data();

  blendAttachment.blendEnable = VK_TRUE;
  blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
  WorldMapOverlayPass::checkVkResult(vkCreateGraphicsPipelines(rhi_.getDevice(), VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &linePipeline_));
}

namespace
{

// A 5x7 uppercase bitmap font, one byte per row, low five bits.
//
// Self-contained on purpose. The headless render is the only way this work gets
// looked at without a display, and a map with no place names cannot be judged
// as a map; ImGui's atlas is not reachable from a process with no context, and
// pulling in a font library for four dozen glyphs would be worse than this
// table.
constexpr uint32_t kGlyphWidth = 5u;
constexpr uint32_t kGlyphHeight = 7u;
const char *kGlyphOrder = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.,'";
const uint8_t kGlyphs[][kGlyphHeight] = {
  {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, // A
  {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}, // B
  {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}, // C
  {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}, // D
  {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, // E
  {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}, // F
  {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}, // G
  {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, // H
  {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}, // I
  {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c}, // J
  {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
  {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}, // L
  {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}, // M
  {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // N
  {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, // O
  {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}, // P
  {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}, // Q
  {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}, // R
  {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, // S
  {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
  {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, // U
  {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}, // V
  {0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11}, // W
  {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}, // X
  {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}, // Y
  {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}, // Z
  {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, // 0
  {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}, // 1
  {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, // 2
  {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}, // 3
  {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, // 4
  {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}, // 5
  {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}, // 6
  {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
  {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, // 8
  {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}, // 9
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, //
  {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}, // -
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}, // .
  {0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08}, // ,
  {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}, // apostrophe
};

int glyphIndex(char character)
{
  if (character >= 'a' && character <= 'z')
    character = static_cast<char>(character - 'a' + 'A');
  for (int index = 0; kGlyphOrder[index] != '\0'; ++index)
    if (kGlyphOrder[index] == character)
      return index;
  return -1;
}

uint32_t textWidthPixels(const std::string &text, const uint32_t scale)
{
  if (text.empty())
    return 0u;
  return static_cast<uint32_t>(text.size()) * (kGlyphWidth + 1u) * scale - scale;
}

} // namespace

namespace
{

ImVec2 worldToScreen(const MapView &view, const ImVec2 origin, const ImVec2 size, const float worldX, const float worldY)
{
  const double scale = 1.0 / view.metresPerPixel;
  return ImVec2(origin.x + size.x * 0.5f + static_cast<float>((static_cast<double>(worldX) - view.centreX) * scale), origin.y + size.y * 0.5f - static_cast<float>((static_cast<double>(worldY) - view.centreY) * scale));
}

} // namespace

Point2 screenToWorld(const MapView &view, const ImVec2 origin, const ImVec2 size, const ImVec2 screen)
{
  return Point2(view.centreX + static_cast<double>(screen.x - origin.x - size.x * 0.5f) * view.metresPerPixel, view.centreY - static_cast<double>(screen.y - origin.y - size.y * 0.5f) * view.metresPerPixel);
}

void drawMapMesh(ImDrawList *drawList, const MapMesh &mesh, const MapView &view, const ImVec2 origin, const ImVec2 size, const MapShading &shading, const MapOverlayStyle &style, const double extentMetres, const bool drawFill, const bool drawGpuLines)
{
  if (drawList == nullptr)
    return;

  // The window's draw list already has the font atlas bound, so the fill can
  // reuse it by sampling the atlas's white texel rather than pushing a texture
  // of its own.
  const ImVec2 uv = ImGui::GetIO().Fonts->TexUvWhitePixel;
  const auto vertexColour = [&shading](const MapVertex &vertex)
  {
    switch (shading.layer)
    {
    case MapLayer::Content:
      return vertex.contentColour;
    case MapLayer::Landform:
      return vertex.landformColour;
    case MapLayer::Elevation:
    case MapLayer::Heightmap:
      return packColour(elevationColour(static_cast<double>(vertex.elevationMetres) - shading.seaLevelMetres, shading.reliefMetres));
    case MapLayer::Uplift:
      return vertex.upliftColour;
    case MapLayer::Biome:
      return vertex.biomeColour;
    case MapLayer::Region:
      return vertex.regionColour;
    case MapLayer::Count:
      break;
    }
    return IM_COL32(255, 0, 255, 255);
  };
  const auto materialColour = [&shading](const MapMaterialSample &sample)
  {
    switch (shading.layer)
    {
    case MapLayer::Content:
      return sample.contentColour;
    case MapLayer::Landform:
      return sample.landformColour;
    case MapLayer::Biome:
      return sample.biomeColour;
    case MapLayer::Region:
      return sample.regionColour;
    default:
      return IM_COL32(255, 0, 255, 255);
    }
  };
  const bool thresholdLayer = !shading.flatCells &&
      (shading.layer == MapLayer::Content ||
          shading.layer == MapLayer::Landform ||
          shading.layer == MapLayer::Biome ||
          shading.layer == MapLayer::Region) &&
      mesh.materialFieldSide > 0u &&
      mesh.materialField.size() ==
          static_cast<size_t>(mesh.materialFieldSide) * mesh.materialFieldSide;
  if (drawFill && thresholdLayer)
  {
    if (shading.layer == MapLayer::Biome)
    {
      // The CPU fallback tessellates a modest presentation grid and lets ImGui
      // interpolate its corner colours. Sampling the continuous biome channel
      // at the corners avoids exposing the 512x512 storage texels.
      const uint32_t displaySide =
          std::min(128u, mesh.materialFieldSide);
      const double step = mesh.materialFieldExtentMetres /
          static_cast<double>(displaySide);
      for (uint32_t y = 0u; y < displaySide; ++y)
        for (uint32_t x = 0u; x < displaySide; ++x)
        {
          const double u0 = static_cast<double>(x) / displaySide;
          const double u1 = static_cast<double>(x + 1u) / displaySide;
          const double v0 = static_cast<double>(y) / displaySide;
          const double v1 = static_cast<double>(y + 1u) / displaySide;
          const ImVec2 topLeft = worldToScreen(
              view, origin, size, static_cast<float>(x * step),
              static_cast<float>((y + 1u) * step));
          const ImVec2 bottomRight = worldToScreen(
              view, origin, size, static_cast<float>((x + 1u) * step),
              static_cast<float>(y * step));
          drawList->AddRectFilledMultiColor(
              topLeft,
              bottomRight,
              bilinearBiomeColour(mesh, u0, v1),
              bilinearBiomeColour(mesh, u1, v1),
              bilinearBiomeColour(mesh, u1, v0),
              bilinearBiomeColour(mesh, u0, v0));
        }
    }
    else
    {
      // Hard categorical layers use horizontal runs of identical material
      // texels, avoiding one draw per texel in large uniform regions.
      const double step = mesh.materialFieldExtentMetres /
          static_cast<double>(mesh.materialFieldSide);
      for (uint32_t y = 0u; y < mesh.materialFieldSide; ++y)
      {
        uint32_t from = 0u;
        while (from < mesh.materialFieldSide)
        {
          const uint32_t colour = materialColour(
              mesh.materialField[static_cast<size_t>(y) *
                                     mesh.materialFieldSide +
                                 from]);
          uint32_t to = from + 1u;
          while (to < mesh.materialFieldSide &&
              materialColour(mesh.materialField[static_cast<size_t>(y) *
                                                     mesh.materialFieldSide +
                                                 to]) == colour)
            ++to;
          const ImVec2 topLeft = worldToScreen(
              view, origin, size, static_cast<float>(from * step),
              static_cast<float>((y + 1u) * step));
          const ImVec2 bottomRight = worldToScreen(
              view, origin, size, static_cast<float>(to * step),
              static_cast<float>(y * step));
          drawList->AddRectFilled(topLeft, bottomRight, colour);
          from = to;
        }
      }
    }
  }
  else if (drawFill)
    for (const MapBatch &batch : mesh.batches)
    {
      if (batch.vertices.empty() || batch.indices.empty())
        continue;
      drawList->PrimReserve(static_cast<int>(batch.indices.size()), static_cast<int>(batch.vertices.size()));
      const unsigned int base = drawList->_VtxCurrentIdx;
      for (const MapVertex &vertex : batch.vertices)
      {
        drawList->PrimWriteVtx(worldToScreen(view, origin, size, vertex.x, vertex.y), uv, vertexColour(vertex));
      }
      for (const uint16_t index : batch.indices)
        drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
    }

  if (!drawGpuLines && style.showCellEdges)
  {
    const ImU32 colour = IM_COL32(255, 255, 255, 36);
    for (const std::array<float, 4> &edge : mesh.cellEdges)
      drawList->AddLine(worldToScreen(view, origin, size, edge[0], edge[1]), worldToScreen(view, origin, size, edge[2], edge[3]), colour, 1.0f);
  }

  if (!drawGpuLines && style.showRivers && !mesh.rivers.empty())
  {
    // Width follows discharge, with enough display contrast that trunks,
    // tributaries and headwaters read as three different scales.
    const ImU32 colour = IM_COL32(64, 120, 168, 255);
    for (const RiverSegment &segment : mesh.rivers)
    {
      const float width = std::max(1.0f, riverWidthFactor(segment.dischargeCumecs, mesh.largestDischargeCumecs) * style.riverWidthScale);
      drawList->AddLine(
          worldToScreen(view, origin, size, static_cast<float>(segment.from.x), static_cast<float>(segment.from.y)),
          worldToScreen(view, origin, size, static_cast<float>(segment.to.x), static_cast<float>(segment.to.y)),
          colour,
          width);
    }
    for (const std::array<float, 4> &waterfall : mesh.waterfallMarks)
      drawList->AddLine(
          worldToScreen(view, origin, size, waterfall[0], waterfall[1]),
          worldToScreen(view, origin, size, waterfall[2], waterfall[3]),
          IM_COL32(196, 226, 242, 255),
          std::max(1.0f, style.riverWidthScale * 0.55f));
  }

  if (!drawGpuLines && style.showContours)
  {
    const ImU32 minor = IM_COL32(38, 32, 25, 105);
    const ImU32 major = IM_COL32(30, 24, 18, 155);
    std::vector<ImVec2> points;
    for (const ContourPolyline &line : mesh.contours)
    {
      if (line.points.size() < 2u)
        continue;
      points.clear();
      points.reserve(line.points.size());
      for (const Point2 &point : line.points)
        points.push_back(worldToScreen(view, origin, size, static_cast<float>(point.x), static_cast<float>(point.y)));
      drawList->AddPolyline(
          points.data(), static_cast<int>(points.size()), line.major ? major : minor, line.closed ? ImDrawFlags_Closed : ImDrawFlags_None, line.major ? style.majorContourThickness : style.contourThickness);
    }
  }

  if (!drawGpuLines && style.showCoastline)
  {
    const ImU32 colour = IM_COL32(16, 22, 30, 235);
    std::vector<ImVec2> points;
    for (const CoastPolyline &line : mesh.coastline)
    {
      if (line.points.size() < 2u)
        continue;
      points.clear();
      points.reserve(line.points.size());
      for (const Point2 &point : line.points)
        points.push_back(worldToScreen(view, origin, size, static_cast<float>(point.x), static_cast<float>(point.y)));
      drawList->AddPolyline(points.data(), static_cast<int>(points.size()), colour, line.closed ? ImDrawFlags_Closed : ImDrawFlags_None, style.coastlineThickness);
    }
  }

  if (style.showLabels)
  {
    // Greedy collision rejection, most important first. Without it the map is
    // a wall of overlapping text and unreadable, which is the whole thing a
    // map render is judged on.
    std::vector<std::array<float, 4>> placed;
    for (const MapLabel &label : mesh.labels)
    {
      const ImVec2 at = worldToScreen(view, origin, size, static_cast<float>(label.position.x), static_cast<float>(label.position.y));
      if (at.x < origin.x || at.y < origin.y || at.x > origin.x + size.x || at.y > origin.y + size.y)
        continue;
      const ImVec2 extent = ImGui::CalcTextSize(label.text.c_str());
      const ImVec2 corner(at.x - extent.x * 0.5f, at.y - extent.y * 0.5f);
      const std::array<float, 4> box{corner.x - 3.0f, corner.y - 2.0f, corner.x + extent.x + 3.0f, corner.y + extent.y + 2.0f};
      bool clashes = false;
      for (const std::array<float, 4> &other : placed)
        clashes = clashes || (box[0] < other[2] && other[0] < box[2] && box[1] < other[3] && other[1] < box[3]);
      if (clashes)
        continue;
      placed.push_back(box);
      // Drawn twice: a dark offset copy first, so a name stays readable over
      // both a pale desert and a dark forest without a halo box.
      const ImU32 shadow = IM_COL32(12, 14, 18, 190);
      drawList->AddText(ImVec2(corner.x + 1.0f, corner.y + 1.0f), shadow, label.text.c_str());
      drawList->AddText(corner, label.rank == 0u ? IM_COL32(196, 226, 248, 235) : IM_COL32(250, 246, 236, 245), label.text.c_str());
    }
  }

  if (style.showWorldBorder)
  {
    const ImVec2 topLeft = worldToScreen(view, origin, size, 0.0f, static_cast<float>(extentMetres));
    const ImVec2 bottomRight = worldToScreen(view, origin, size, static_cast<float>(extentMetres), 0.0f);
    drawList->AddRect(topLeft, bottomRight, IM_COL32(255, 255, 255, 60), 0.0f, ImDrawFlags_None, 1.0f);
  }
}

std::vector<uint8_t> renderMapImage(
    const MapGraph &graph,
    const std::vector<CoastPolyline> &coastline,
    const RiverNetwork &rivers,
    const MapMesh &mesh,
    const procedural::world::MapSurfaceField &surface,
    const SiteIndex &index,
    const BlendSettings &settings,
    const MapShading &shading,
    const uint32_t width,
    const uint32_t height,
    const double extentMetres,
    const MapOverlayStyle &style)
{
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u, static_cast<uint8_t>(0u));
  if (width == 0u || height == 0u || index.empty())
    return rgba;
  // Open sea behind everything, so a pixel no cell covers is not a hole.
  {
    const std::array<float, 3> sea = cellTypeColour(procedural::world::CellType::Ocean);
    for (size_t pixel = 0u; pixel < rgba.size(); pixel += 4u)
    {
      for (size_t channel = 0u; channel < 3u; ++channel)
        rgba[pixel + channel] = static_cast<uint8_t>(std::clamp(sea[channel], 0.0f, 1.0f) * 255.0f + 0.5f);
      rgba[pixel + 3u] = 255u;
    }
  }

  // Scanline-filled from the same noisy cell shapes the window draws, rather
  // than resolved per pixel by nearest site. Two reasons: a nearest-site fill
  // cannot show a noisy boundary at all, and having one geometry for both
  // renderers is what stops the exported image and the window disagreeing.
  const auto toPixel = [&](const Point2 &point)
  {
    return Point2(point.x / extentMetres * static_cast<double>(width) - 0.5, (1.0 - point.y / extentMetres) * static_cast<double>(height) - 0.5);
  };
  std::vector<double> crossings;
  std::vector<Point2> projected;
  for (const MapCellShape &shape : mesh.cells)
  {
    if (shape.boundary.size() < 3u)
      continue;
    projected.clear();
    double lowY = 1e30;
    double highY = -1e30;
    for (const Point2 &point : shape.boundary)
    {
      projected.push_back(toPixel(point));
      lowY = std::min(lowY, projected.back().y);
      highY = std::max(highY, projected.back().y);
    }
    const uint32_t red = shape.colour & 0xffu;
    const uint32_t green = (shape.colour >> 8u) & 0xffu;
    const uint32_t blue = (shape.colour >> 16u) & 0xffu;
    const int32_t firstRow = std::max(0, static_cast<int32_t>(std::floor(lowY)));
    const int32_t lastRow = std::min(static_cast<int32_t>(height) - 1, static_cast<int32_t>(std::ceil(highY)));
    for (int32_t row = firstRow; row <= lastRow; ++row)
    {
      const double scan = static_cast<double>(row);
      crossings.clear();
      for (size_t index = 0u; index < projected.size(); ++index)
      {
        const Point2 &a = projected[index];
        const Point2 &b = projected[(index + 1u) % projected.size()];
        if ((a.y > scan) == (b.y > scan))
          continue;
        crossings.push_back(a.x + (scan - a.y) / (b.y - a.y) * (b.x - a.x));
      }
      std::sort(crossings.begin(), crossings.end());
      for (size_t span = 0u; span + 1u < crossings.size(); span += 2u)
      {
        const int32_t from = std::max(0, static_cast<int32_t>(std::ceil(crossings[span])));
        const int32_t to = std::min(static_cast<int32_t>(width) - 1, static_cast<int32_t>(std::floor(crossings[span + 1u])));
        for (int32_t column = from; column <= to; ++column)
        {
          const size_t pixel = (static_cast<size_t>(row) * width + column) * 4u;
          rgba[pixel + 0u] = static_cast<uint8_t>(red);
          rgba[pixel + 1u] = static_cast<uint8_t>(green);
          rgba[pixel + 2u] = static_cast<uint8_t>(blue);
          rgba[pixel + 3u] = 255u;
        }
      }
    }
  }

  const bool categoricalLayer = shading.layer == MapLayer::Content ||
      shading.layer == MapLayer::Landform ||
      shading.layer == MapLayer::Biome || shading.layer == MapLayer::Region;
  if (categoricalLayer && !shading.flatCells &&
      mesh.materialFieldSide > 0u &&
      mesh.materialField.size() ==
          static_cast<size_t>(mesh.materialFieldSide) * mesh.materialFieldSide)
  {
    const auto colourOf = [&shading](const MapMaterialSample &sample)
    {
      switch (shading.layer)
      {
      case MapLayer::Content:
        return sample.contentColour;
      case MapLayer::Landform:
        return sample.landformColour;
      case MapLayer::Biome:
        return sample.biomeColour;
      case MapLayer::Region:
        return sample.regionColour;
      default:
        return IM_COL32(255, 0, 255, 255);
      }
    };
    for (uint32_t row = 0u; row < height; ++row)
      for (uint32_t column = 0u; column < width; ++column)
      {
        const uint32_t fieldX = std::min(
            static_cast<uint32_t>(
                (static_cast<uint64_t>(column) * mesh.materialFieldSide) /
                std::max(1u, width)),
            mesh.materialFieldSide - 1u);
        const uint32_t fieldY = std::min(
            static_cast<uint32_t>(
                (static_cast<uint64_t>(height - row - 1u) *
                    mesh.materialFieldSide) /
                std::max(1u, height)),
            mesh.materialFieldSide - 1u);
        const uint32_t colour = shading.layer == MapLayer::Biome
            ? bilinearBiomeColour(
                  mesh,
                  (static_cast<double>(column) + 0.5) /
                      static_cast<double>(std::max(1u, width)),
                  (static_cast<double>(height - row) - 0.5) /
                      static_cast<double>(std::max(1u, height)))
            : colourOf(
                  mesh.materialField[static_cast<size_t>(fieldY) *
                                         mesh.materialFieldSide +
                                     fieldX]);
        const size_t pixel =
            (static_cast<size_t>(row) * width + column) * 4u;
        rgba[pixel + 0u] = static_cast<uint8_t>(colour & 0xffu);
        rgba[pixel + 1u] = static_cast<uint8_t>((colour >> 8u) & 0xffu);
        rgba[pixel + 2u] = static_cast<uint8_t>((colour >> 16u) & 0xffu);
        rgba[pixel + 3u] = 255u;
      }
  }

  if (shading.layer == MapLayer::Heightmap)
  {
    // Shaded from the raster's own finite differences rather than from four
    // extra field samples per pixel: same surface, a fraction of the cost, and
    // the gradient is measured at exactly the scale being drawn.
    std::vector<double> elevation(static_cast<size_t>(width) * height, 0.0);
    std::vector<BlendWeight> weights;
    for (uint32_t y = 0u; y < height; ++y)
      for (uint32_t x = 0u; x < width; ++x)
      {
        const double worldY = extentMetres * (1.0 - (static_cast<double>(y) + 0.5) / static_cast<double>(height));
        const double worldX = extentMetres * ((static_cast<double>(x) + 0.5) / static_cast<double>(width));
        elevation[static_cast<size_t>(y) * width + x] =
            blendedElevation(surface, Point2(worldX, worldY));
      }
    const double stepX = extentMetres / static_cast<double>(width);
    const double stepY = extentMetres / static_cast<double>(height);
    for (uint32_t y = 0u; y < height; ++y)
      for (uint32_t x = 0u; x < width; ++x)
      {
        const uint32_t left = x > 0u ? x - 1u : x;
        const uint32_t right = x + 1u < width ? x + 1u : x;
        const uint32_t up = y > 0u ? y - 1u : y;
        const uint32_t down = y + 1u < height ? y + 1u : y;
        const double dzdx = (elevation[static_cast<size_t>(y) * width + right] - elevation[static_cast<size_t>(y) * width + left]) / ((right - left) * stepX);
        // Row index increases southward, so the sign flips to put the gradient
        // back in world space.
        const double dzdy = (elevation[static_cast<size_t>(up) * width + x] - elevation[static_cast<size_t>(down) * width + x]) / ((down - up) * stepY);
        const double shade = 0.25 + 0.95 * hillshade(dzdx, dzdy, shading);
        const size_t pixel = (static_cast<size_t>(y) * width + x) * 4u;
        for (size_t channel = 0u; channel < 3u; ++channel)
          rgba[pixel + channel] = static_cast<uint8_t>(std::clamp(static_cast<double>(rgba[pixel + channel]) * shade, 0.0, 255.0));
      }
  }

  const auto plot = [&](const int32_t x, const int32_t y, const std::array<uint8_t, 3> &colour)
  {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(width) || y >= static_cast<int32_t>(height))
      return;
    const size_t pixel = (static_cast<size_t>(y) * width + static_cast<uint32_t>(x)) * 4u;
    for (size_t channel = 0u; channel < 3u; ++channel)
      rgba[pixel + channel] = colour[channel];
  };
  const auto project = [&](const Point2 &point, double &x, double &y)
  {
    x = point.x / extentMetres * static_cast<double>(width) - 0.5;
    y = (1.0 - point.y / extentMetres) * static_cast<double>(height) - 0.5;
  };
  // Half-width in pixels, so a trunk river is visible at map scale. Same
  // square-root-of-discharge rule as the interactive view.
  const auto stroke = [&](const Point2 &from, const Point2 &to, const std::array<uint8_t, 3> &colour, const double radius)
  {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    project(from, x0, y0);
    project(to, x1, y1);
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const int32_t steps = static_cast<int32_t>(std::ceil(std::max(std::abs(dx), std::abs(dy))));
    const int32_t span = static_cast<int32_t>(std::floor(radius));
    for (int32_t step = 0; step <= steps; ++step)
    {
      const double t = steps == 0 ? 0.0 : static_cast<double>(step) / steps;
      const int32_t px = static_cast<int32_t>(std::lround(x0 + dx * t));
      const int32_t py = static_cast<int32_t>(std::lround(y0 + dy * t));
      for (int32_t oy = -span; oy <= span; ++oy)
        for (int32_t ox = -span; ox <= span; ++ox)
          if (static_cast<double>(ox * ox + oy * oy) <= radius * radius)
            plot(px + ox, py + oy, colour);
    }
  };

  if (style.showRivers)
  {
    for (const RiverSegment &segment : rivers.segments)
    {
      const double width = riverWidthFactor(segment.dischargeCumecs, rivers.largestDischargeCumecs);
      stroke(segment.from, segment.to, {64u, 120u, 168u}, 0.35 + width * style.riverWidthScale * 0.5);
    }
    for (const std::array<float, 4> &waterfall : mesh.waterfallMarks)
      stroke(Point2(waterfall[0], waterfall[1]), Point2(waterfall[2], waterfall[3]), {196u, 226u, 242u}, std::max(0.5, style.riverWidthScale * 0.275));
  }
  if (style.showContours)
    for (const ContourPolyline &line : mesh.contours)
    {
      for (size_t i = 0u; i + 1u < line.points.size(); ++i)
        stroke(
            line.points[i], line.points[i + 1u], line.major ? std::array<uint8_t, 3>{42u, 34u, 25u} : std::array<uint8_t, 3>{67u, 57u, 44u}, line.major ? style.majorContourThickness * 0.5 : style.contourThickness * 0.5);
      if (line.closed && line.points.size() >= 3u)
        stroke(
            line.points.back(), line.points.front(), line.major ? std::array<uint8_t, 3>{42u, 34u, 25u} : std::array<uint8_t, 3>{67u, 57u, 44u}, line.major ? style.majorContourThickness * 0.5 : style.contourThickness * 0.5);
    }
  if (style.showCoastline)
    for (const CoastPolyline &line : coastline)
      for (size_t i = 0u; i + 1u < line.points.size(); ++i)
        stroke(line.points[i], line.points[i + 1u], {20u, 26u, 34u}, 0.0);

  if (style.showLabels)
  {
    // Scaled to the image, so a 512 px inspection render and a 2048 px one are
    // equally readable rather than one being unreadable and the other coarse.
    const uint32_t scale = std::max(1u, static_cast<uint32_t>(std::min(width, height) / 512u));
    std::vector<std::array<int32_t, 4>> placed;
    for (const MapLabel &label : mesh.labels)
    {
      double cx = 0.0;
      double cy = 0.0;
      project(label.position, cx, cy);
      const int32_t textWidth = static_cast<int32_t>(textWidthPixels(label.text, scale));
      const int32_t textHeight = static_cast<int32_t>(kGlyphHeight * scale);
      const int32_t left = static_cast<int32_t>(std::lround(cx)) - textWidth / 2;
      const int32_t top = static_cast<int32_t>(std::lround(cy)) - textHeight / 2;
      const int32_t pad = static_cast<int32_t>(scale) * 2;
      const std::array<int32_t, 4> box{left - pad, top - pad, left + textWidth + pad, top + textHeight + pad};
      // A label that runs off the edge reads as a bug rather than as a map, so
      // it is dropped whole rather than clipped mid-word.
      if (box[0] < 0 || box[1] < 0 || box[2] > static_cast<int32_t>(width) || box[3] > static_cast<int32_t>(height))
        continue;
      bool clashes = false;
      for (const std::array<int32_t, 4> &other : placed)
        clashes = clashes || (box[0] < other[2] && other[0] < box[2] && box[1] < other[3] && other[1] < box[3]);
      if (clashes)
        continue;
      placed.push_back(box);
      const std::array<uint8_t, 3> ink = label.rank == 0u ? std::array<uint8_t, 3>{196u, 226u, 248u} : std::array<uint8_t, 3>{250u, 246u, 236u};
      const std::array<uint8_t, 3> shadow{12u, 14u, 18u};
      for (int pass = 0; pass < 2; ++pass)
      {
        const std::array<uint8_t, 3> &colour = pass == 0 ? shadow : ink;
        const int32_t offset = pass == 0 ? static_cast<int32_t>(scale) : 0;
        int32_t pen = left + offset;
        for (const char character : label.text)
        {
          const int index = glyphIndex(character);
          if (index >= 0)
            for (uint32_t row = 0u; row < kGlyphHeight; ++row)
              for (uint32_t column = 0u; column < kGlyphWidth; ++column)
                if ((kGlyphs[index][row] >> (kGlyphWidth - 1u - column)) & 1u)
                  for (uint32_t sy = 0u; sy < scale; ++sy)
                    for (uint32_t sx = 0u; sx < scale; ++sx)
                      plot(pen + static_cast<int32_t>(column * scale + sx), top + offset + static_cast<int32_t>(row * scale + sy), colour);
          pen += static_cast<int32_t>((kGlyphWidth + 1u) * scale);
        }
      }
    }
  }
  return rgba;
}

bool writePng(const std::string &path, const uint32_t width, const uint32_t height, const std::vector<uint8_t> &rgba, std::string &error)
{
  if (width == 0u || height == 0u || rgba.size() != static_cast<size_t>(width) * height * 4u)
  {
    error = "PNG dimensions do not match the pixel buffer";
    return false;
  }
  size_t length = 0u;
  void *encoded = tdefl_write_image_to_png_file_in_memory(rgba.data(), static_cast<int>(width), static_cast<int>(height), 4, &length);
  if (encoded == nullptr)
  {
    error = "PNG encoding failed";
    return false;
  }
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
  {
    mz_free(encoded);
    error = "Could not open " + path + " for writing";
    return false;
  }
  const bool written = std::fwrite(encoded, 1u, length, file) == length;
  std::fclose(file);
  mz_free(encoded);
  if (!written)
  {
    error = "Could not write " + path;
    return false;
  }
  return true;
}

} // namespace worldgen
