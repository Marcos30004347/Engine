#include "os/Logger.hpp"
#include "procedural/world/MapExportGpuCache.hpp"
#include "procedural/world/MapExportSampler.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <array>
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef MAP_EXPORT_PAGES_TEST_SHADER_PATH
#error MAP_EXPORT_PAGES_TEST_SHADER_PATH must name the compiled test shader
#endif

namespace
{

namespace gpu = ::rendering;
using namespace procedural::world;

struct alignas(16) Query
{
  std::array<float, 2> coordinate{};
  uint32_t field = 0u;
  uint32_t companionField = 0u;
  uint32_t maximumResidentLevel = 0u;
  uint32_t mode = 0u;
  std::array<uint32_t, 2> reserved{};
};

struct alignas(16) Output
{
  std::array<float, 4> value{};
  std::array<uint32_t, 4> ids{};
  std::array<float, 4> weights{};
  std::array<uint32_t, 4> metadata{};
};

static_assert(sizeof(Query) == 32u);
static_assert(sizeof(Output) == 64u);

void appendU16(std::vector<uint8_t> &data, const uint16_t value)
{
  data.push_back(static_cast<uint8_t>(value));
  data.push_back(static_cast<uint8_t>(value >> 8u));
}

void appendU32(std::vector<uint8_t> &data, const uint32_t value)
{
  data.push_back(static_cast<uint8_t>(value));
  data.push_back(static_cast<uint8_t>(value >> 8u));
  data.push_back(static_cast<uint8_t>(value >> 16u));
  data.push_back(static_cast<uint8_t>(value >> 24u));
}

MapExportField constantField(const std::string &name, const MapExportFormat format, const MapExportFilter filter, const std::vector<uint8_t> &texel, const std::string &samplingGroup = {})
{
  MapExportField field{};
  field.name = name;
  field.format = format;
  field.filter = filter;
  field.width = 4u;
  field.height = 4u;
  field.available = true;
  field.samplingGroup = samplingGroup;
  MapExportPage page{};
  page.address = {0u, 0u, 0u};
  page.width = 4u;
  page.height = 4u;
  page.data.reserve(16u * texel.size());
  for (uint32_t index = 0u; index < 16u; ++index)
    page.data.insert(page.data.end(), texel.begin(), texel.end());
  field.pages.push_back(std::move(page));
  return field;
}

bool near(const float actual, const float expected, const float tolerance = 1.0e-5f)
{
  return std::abs(actual - expected) <= tolerance;
}

void runGpuSampling(gpu::RHI &rhi)
{
  std::vector<uint8_t> halfScalar;
  appendU16(halfScalar, 0x3e00u); // 1.5
  std::vector<uint8_t> unormScalar;
  appendU16(unormScalar, 32768u);
  std::vector<uint8_t> uintScalar;
  appendU32(uintScalar, 0xf1234567u);
  std::vector<uint8_t> halfPair;
  appendU16(halfPair, 0x3e00u); // 1.5
  appendU16(halfPair, 0xc000u); // -2.0

  MapExportBundle manifest{};
  manifest.resolution = 4u;
  manifest.pageSizePixels = 2u;
  manifest.gutterPixels = 1u;
  manifest.maximumPageLevel = 1u;
  manifest.fields = {
    constantField("r8", MapExportFormat::R8Uint, MapExportFilter::Nearest, {42u}),
    constantField("r16f", MapExportFormat::R16Float, MapExportFilter::Linear, halfScalar),
    constantField("r16unorm", MapExportFormat::R16Unorm, MapExportFilter::Linear, unormScalar),
    constantField("r32", MapExportFormat::R32Uint, MapExportFilter::Nearest, uintScalar),
    constantField("rg16f", MapExportFormat::RG16Float, MapExportFilter::Linear, halfPair),
    constantField("biome_mix_ids", MapExportFormat::RGBA8Uint, MapExportFilter::SparseMixture, {3u, 7u, 3u, 9u}, "biome_mixture"),
    constantField("biome_mix_weights", MapExportFormat::RGBA8Unorm, MapExportFilter::SparseMixture, {128u, 64u, 32u, 31u}, "biome_mixture"),
    constantField("rgba8unorm", MapExportFormat::RGBA8Unorm, MapExportFilter::Linear, {64u, 128u, 192u, 255u}),
  };
  manifest.fields[0].scale[0] = 2.0;
  manifest.fields[0].offset[0] = 1.0;

  MapExportGpuPageCache cache(rhi);
  std::string error;
  assert(cache.initialize(manifest, static_cast<uint32_t>(manifest.fields.size()), &error));
  MapExportPagePublication publication{};
  publication.revision = 1u;
  for (uint32_t field = 0u; field < manifest.fields.size(); ++field)
  {
    const MapExportStreamKey key{manifest.fields[field].name, {0u, 0u, 0u}};
    publication.pageTable.push_back({key, field});
    publication.uploads.push_back({key, field, manifest.fields[field].pages.front().data});
  }
  assert(cache.apply(publication, &error));

  const auto field = [&](const char *name)
  {
    const std::optional<uint32_t> index = cache.layout().fieldIndex(name);
    assert(index.has_value());
    return *index;
  };
  const uint32_t idField = field("biome_mix_ids");
  const uint32_t weightField = field("biome_mix_weights");
  const std::array<float, 2> coordinate{0.5f, 0.5f};
  const std::vector<Query> queries{
    {coordinate, field("r8"), 0u, 1u, 0u, {}},
    {coordinate, field("r16f"), 0u, 1u, 0u, {}},
    {coordinate, field("r16unorm"), 0u, 1u, 0u, {}},
    {coordinate, field("r32"), 0u, 1u, 1u, {}},
    {coordinate, field("rg16f"), 0u, 1u, 0u, {}},
    {coordinate, idField, 0u, 1u, 1u, {}},
    {coordinate, field("rgba8unorm"), 0u, 1u, 0u, {}},
    {coordinate, idField, weightField, 1u, 2u, {}},
  };

  const gpu::BufferId queryBuffer = rhi.createBuffer({
    .name = "mapExport.test.queries",
    .size = queries.size() * sizeof(Query),
    .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Storage | gpu::BufferUsage_Push),
  });
  const gpu::BufferId outputBuffer = rhi.createBuffer({
    .name = "mapExport.test.outputs",
    .size = queries.size() * sizeof(Output),
    .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Storage | gpu::BufferUsage_Pull),
  });
  rhi.bufferWrite(queryBuffer, 0u, queries.size() * sizeof(Query), const_cast<Query *>(queries.data()));

  const gpu::BindingsLayoutId layoutId = rhi.createBindingsLayout({
    .name = "mapExport.test.layout",
    .groups = {gpu::BindingGroupLayout{
      .buffers =
          {
            {.name = "metadata", .binding = 0u, .type = gpu::BufferBindingType_StorageBuffer, .visibility = gpu::BindingVisibility_Compute},
            {.name = "pageTable", .binding = 1u, .type = gpu::BufferBindingType_StorageBuffer, .visibility = gpu::BindingVisibility_Compute},
            {.name = "physical", .binding = 2u, .type = gpu::BufferBindingType_StorageBuffer, .visibility = gpu::BindingVisibility_Compute},
            {.name = "queries", .binding = 3u, .type = gpu::BufferBindingType_StorageBuffer, .visibility = gpu::BindingVisibility_Compute},
            {.name = "outputs", .binding = 4u, .type = gpu::BufferBindingType_StorageBuffer, .visibility = gpu::BindingVisibility_Compute},
          }}},
  });
  const gpu::BindingsLayout layout{.name = "mapExport.test.layout", .id = layoutId};
  const auto view = [](const gpu::BufferId buffer, const uint64_t size, const gpu::AccessPattern access)
  {
    return gpu::BufferView{.resourceId = buffer, .offset = 0u, .size = size, .access = access};
  };
  const gpu::BindingGroupsId groups = rhi.createBindingGroups({
    .name = "mapExport.test.bindings",
    .layout = layout,
    .groups = {gpu::GroupInfo{
      .name = "group0",
      .buffers =
          {
            {.bufferView = view(cache.metadataBuffer(), sizeof(MapExportGpuHeader) + cache.layout().descriptors().size() * sizeof(MapExportGpuFieldDescriptor), gpu::AccessPattern::SHADER_READ), .binding = 0u},
            {.bufferView = view(cache.pageTableBuffer(), cache.layout().entriesPerField() * cache.layout().fields().size() * sizeof(uint32_t), gpu::AccessPattern::SHADER_READ), .binding = 1u},
            {.bufferView = view(cache.physicalBuffer(), cache.layout().physicalBufferBytes(), gpu::AccessPattern::SHADER_READ), .binding = 2u},
            {.bufferView = view(queryBuffer, queries.size() * sizeof(Query), gpu::AccessPattern::SHADER_READ), .binding = 3u},
            {.bufferView = view(outputBuffer, queries.size() * sizeof(Output), gpu::AccessPattern::SHADER_READ_WRITE), .binding = 4u},
          },
    }},
  });

  std::ifstream shaderFile(MAP_EXPORT_PAGES_TEST_SHADER_PATH, std::ios::binary);
  assert(shaderFile);
  const std::string spirv((std::istreambuf_iterator<char>(shaderFile)), std::istreambuf_iterator<char>());
  const gpu::ShaderId shaderId = rhi.createShader({.name = "mapExport.test.shader", .src = spirv, .type = gpu::ShaderType::SpirV, .layout = layout});
  const gpu::ComputePipelineId pipelineId = rhi.createComputePipeline({
    .name = "mapExport.test.pipeline",
    .shader = gpu::Shader{.name = "mapExport.test.shader", .id = shaderId},
    .entry = "main",
    .layout = layout,
  });
  std::vector<gpu::CommandBuffer> commandBuffers = rhi.allocateCommandBuffers(gpu::Queue::Graphics, 1u);
  assert(!commandBuffers.empty());
  rhi.beginCommandBuffer(commandBuffers.front(), true);
  rhi.cmdBindComputePipeline(commandBuffers.front(), gpu::ComputePipeline{.name = "mapExport.test.pipeline", .id = pipelineId});
  rhi.cmdBindBindingGroups(commandBuffers.front(), groups, nullptr, 0u);
  rhi.cmdDispatch(commandBuffers.front(), 1u, 1u, 1u);
  rhi.endCommandBuffer(commandBuffers.front());
  gpu::GPUFuture future = rhi.submit(gpu::Queue::Graphics, commandBuffers.data(), static_cast<uint32_t>(commandBuffers.size()), nullptr, 0u, true);
  rhi.blockUntil(future);

  std::vector<Output> outputs(queries.size());
  bool read = false;
  rhi.bufferRead(
      outputBuffer,
      0u,
      outputs.size() * sizeof(Output),
      [&](const void *data)
      {
        assert(data != nullptr);
        std::memcpy(outputs.data(), data, outputs.size() * sizeof(Output));
        read = true;
      });
  assert(read);
  for (size_t index = 0u; index < outputs.size(); ++index)
    assert(outputs[index].metadata[0] == 0u && outputs[index].metadata[2] == 1u && outputs[index].metadata[3] == queries[index].mode);
  assert(near(outputs[0].value[0], 85.0f));
  assert(near(outputs[1].value[0], 1.5f));
  assert(near(outputs[2].value[0], 32768.0f / 65535.0f));
  assert(outputs[3].ids[0] == 0xf1234567u);
  assert(near(outputs[4].value[0], 1.5f) && near(outputs[4].value[1], -2.0f));
  assert(outputs[5].ids == (std::array<uint32_t, 4>{3u, 7u, 3u, 9u}));
  assert(near(outputs[6].value[0], 64.0f / 255.0f) && near(outputs[6].value[1], 128.0f / 255.0f) && near(outputs[6].value[2], 192.0f / 255.0f) && near(outputs[6].value[3], 1.0f));
  assert(outputs[7].metadata[1] == 3u && outputs[7].ids[0] == 3u && outputs[7].ids[1] == 7u && outputs[7].ids[2] == 9u);
  assert(near(outputs[7].weights[0], 160.0f / 255.0f) && near(outputs[7].weights[1], 64.0f / 255.0f) && near(outputs[7].weights[2], 31.0f / 255.0f));

  const std::array<const char *, 5> continuousFields{"r8", "r16f", "r16unorm", "rg16f", "rgba8unorm"};
  const std::array<size_t, 5> outputIndices{0u, 1u, 2u, 4u, 6u};
  for (size_t fieldIndex = 0u; fieldIndex < continuousFields.size(); ++fieldIndex)
  {
    MapExportFieldSample cpu{};
    assert(sampleMapExportField(manifest, continuousFields[fieldIndex], 0.5, 0.5, 1u, cpu, &error));
    for (uint32_t component = 0u; component < cpu.componentCount; ++component)
      assert(near(outputs[outputIndices[fieldIndex]].value[component], static_cast<float>(cpu.values[component])));
  }
  MapExportFieldSample cpuRegion{};
  assert(sampleMapExportField(manifest, "r32", 0.5, 0.5, 1u, cpuRegion, &error));
  assert(outputs[3].ids[0] == static_cast<uint32_t>(cpuRegion.values[0]));
  MapExportBiomeSample cpuBiome{};
  assert(sampleMapExportBiome(manifest, 0.5, 0.5, 1u, cpuBiome, &error));
  assert(cpuBiome.count == outputs[7].metadata[1]);
  for (uint32_t component = 0u; component < cpuBiome.count; ++component)
  {
    assert(outputs[7].ids[component] == cpuBiome.ids[component]);
    assert(near(outputs[7].weights[component], static_cast<float>(cpuBiome.weights[component])));
  }

  rhi.deleteComputePipeline(pipelineId);
  rhi.deleteShader(shaderId);
  rhi.deleteBindingGroups(groups);
  rhi.deleteBindingsLayout(layoutId);
  rhi.deleteBuffer(outputBuffer);
  rhi.deleteBuffer(queryBuffer);
}

} // namespace

int main()
{
  os::Logger::start();
  gpu::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 64u,
  };
  gpu::backend::vulkan::VulkanRHI rhi(gpu::backend::vulkan::Vulkan_1_2, limits, gpu::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  runGpuSampling(rhi);
  std::cout << "World map streamed-page GPU sampling checks passed\n";
  os::Logger::shutdown();
  return 0;
}
