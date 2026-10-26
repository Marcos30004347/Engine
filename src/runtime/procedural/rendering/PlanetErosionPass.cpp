#include "procedural/rendering/PlanetErosionPass.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace procedural::rendering
{
namespace
{

namespace gpu = ::rendering;

using Clock = std::chrono::steady_clock;

double elapsedMilliseconds(const Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// encodedLandElevation in SphericalTerrain.cpp starts land at 0.025 of the
// encoded range, and decodedLandElevation maps anything below that back to
// zero, which material classification reads as ocean. The solver has to respect
// the same pedestal or it silently drowns the coast it just eroded.
constexpr double kLandPedestal = 0.025;

} // namespace

PlanetStreamPowerSolver::PlanetStreamPowerSolver(gpu::RHI &rhi, std::string shaderPath, PlanetStreamPowerSettings settings) :
    rhi_(rhi), shaderPath_(std::move(shaderPath)), settings_(settings)
{
}

PlanetStreamPowerSolver::~PlanetStreamPowerSolver()
{
  destroy();
}

bool PlanetStreamPowerSolver::solve(const Request &request, std::string *error)
{
  const auto fail = [error](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (request.heightmap == nullptr || request.oceanMask == nullptr || request.tectonics == nullptr)
    return fail("planet erosion requires a height field, an ocean mask and an uplift field");
  const size_t cellCount = static_cast<size_t>(request.width) * request.height;
  if (request.width < 8u || request.height < 4u || request.heightmap->size() != cellCount ||
      request.oceanMask->size() != cellCount || request.tectonics->size() != cellCount)
    return fail("planet erosion received inconsistent atlas dimensions");
  if (!(request.planetRadiusMetres > 0.0) || !(request.reliefMetres > 0.0))
    return fail("planet erosion requires a positive planet radius and relief");

  const Clock::time_point totalStart = Clock::now();
  statistics_ = {};
  radiusMetres_ = request.planetRadiusMetres;
  reliefMetres_ = request.reliefMetres;

  side_ = settings_.faceSide != 0u ? settings_.faceSide : planetCubeFieldSideForEquirect(request.width);
  cellCount_ = static_cast<uint32_t>(PlanetCubeField::cellCount(side_));

  // Project the atlas onto the solver's domain. Height is bilinear; the ocean
  // mask is nearest, because an interpolated mask invents cells that are
  // neither land nor sea and the outlet boundary condition keys off it.
  const Clock::time_point projectionStart = Clock::now();
  PlanetCubeField elevation{};
  PlanetCubeField oceanMask{};
  PlanetCubeField uplift{};
  PlanetCubeField rockSuite{};
  PlanetCubeField bedPhase{};
  if (!planetCubeFieldFromEquirect(request.width, request.height, *request.heightmap, side_, elevation) ||
      !planetCubeFieldFromEquirect(request.width, request.height, *request.oceanMask, side_, oceanMask, true) ||
      !planetCubeFieldFromEquirect(request.width, request.height, *request.tectonics, side_, uplift))
    return fail("could not project the planet atlas onto cube faces");
  // The suite is a class index, so it resamples nearest: an interpolated suite
  // would invent values that are not in the table. The bedding phase is
  // continuous and interpolates, which keeps bedding planes smooth across the
  // resample instead of stair-stepping at atlas texel boundaries.
  const bool hasGeology = request.rockSuite != nullptr && request.bedPhase != nullptr &&
      request.rockSuite->size() == cellCount && request.bedPhase->size() == cellCount;
  if (hasGeology)
  {
    if (!planetCubeFieldFromEquirect(request.width, request.height, *request.rockSuite, side_, rockSuite, true) ||
        !planetCubeFieldFromEquirect(request.width, request.height, *request.bedPhase, side_, bedPhase))
      return fail("could not project the planet geology onto cube faces");
  }
  else
  {
    rockSuite.side = side_;
    rockSuite.values.assign(PlanetCubeField::cellCount(side_), static_cast<float>(PlanetRockSuite::Platform));
    bedPhase.side = side_;
    bedPhase.values.assign(PlanetCubeField::cellCount(side_), 0.0f);
  }

  // Normalized encoding to metres, and ocean pinned to the datum. Bilinear
  // height sampling across a coastline pulls land toward zero, so the mask has
  // the final say on which cells are sea.
  const double heightScale = request.reliefMetres / 65535.0;
  for (size_t cell = 0u; cell < elevation.values.size(); ++cell)
  {
    const bool sea = oceanMask.values[cell] > 127.0f;
    elevation.values[cell] = sea ? 0.0f : static_cast<float>(elevation.values[cell] * heightScale);
    oceanMask.values[cell] = sea ? 1.0f : 0.0f;
    uplift.values[cell] = static_cast<float>(uplift.values[cell] / 255.0);
  }
  statistics_.projectionMilliseconds = elapsedMilliseconds(projectionStart);
  {
    double upliftSum = 0.0;
    double upliftPeak = 0.0;
    for (const float value : uplift.values)
    {
      upliftSum += value;
      upliftPeak = std::max(upliftPeak, static_cast<double>(value));
    }
    statistics_.meanUplift = uplift.values.empty() ? 0.0 : upliftSum / uplift.values.size();
    statistics_.peakUplift = upliftPeak;
  }
  statistics_.inputReliefMetres = elevation.values.empty() ? 0.0 : *std::max_element(elevation.values.begin(), elevation.values.end());

  // Ground area per cell, from the same chord metric the shader uses. Cube
  // cells vary about twofold in size from face centre to corner, so a single
  // nominal area would bias the mass balance toward the face centres.
  cellAreaMetres_.assign(cellCount_, 0.0);
  for (uint32_t face = 0u; face < kPlanetPackageCubeFaceCount; ++face)
    for (uint32_t y = 0u; y < side_; ++y)
      for (uint32_t x = 0u; x < side_; ++x)
      {
        const auto span = [&](const int32_t fromX, const int32_t fromY, const int32_t toX, const int32_t toY)
        {
          const PlanetCubeCell a = planetCubeFieldNeighbour(side_, face, fromX, fromY);
          const PlanetCubeCell b = planetCubeFieldNeighbour(side_, face, toX, toY);
          const SpherePoint da = planetCubeFieldDirection(side_, a.face, static_cast<int32_t>(a.x), static_cast<int32_t>(a.y));
          const SpherePoint db = planetCubeFieldDirection(side_, b.face, static_cast<int32_t>(b.x), static_cast<int32_t>(b.y));
          const double dx = da.x - db.x;
          const double dy = da.y - db.y;
          const double dz = da.z - db.z;
          return std::sqrt(dx * dx + dy * dy + dz * dz) * radiusMetres_ * 0.5;
        };
        const int32_t cellX = static_cast<int32_t>(x);
        const int32_t cellY = static_cast<int32_t>(y);
        const double width = span(cellX + 1, cellY, cellX - 1, cellY);
        const double height = span(cellX, cellY + 1, cellX, cellY - 1);
        cellAreaMetres_[PlanetCubeField::index(side_, face, x, y)] = width * height;
      }

  // Drainage area advances one cell per iteration, so the iteration count is
  // what convergence costs; the per-iteration work is trivial by comparison.
  const uint32_t iterations = settings_.iterations != 0u ? settings_.iterations : side_ * 2u;
  timeStep_ = settings_.simulatedTime / std::max(1u, iterations);
  statistics_.faceSide = side_;
  statistics_.timeStep = timeStep_;
  statistics_.iterations = iterations;
  statistics_.cellCount = cellCount_;

  const Clock::time_point solveStart = Clock::now();
  try
  {
    createResources(elevation, uplift, oceanMask, rockSuite, bedPhase);
    createPipelines();
    uint32_t completed = 0u;
    const uint32_t batch = std::max(1u, settings_.iterationsPerBatch);
    while (completed < iterations)
    {
      const uint32_t count = std::min(batch, iterations - completed);
      submitBatch(count);
      completed += count;
    }
  }
  catch (const std::exception &exception)
  {
    destroy();
    return fail(std::string("planet erosion failed: ") + exception.what());
  }
  statistics_.solveMilliseconds = elapsedMilliseconds(solveStart);

  const Clock::time_point readbackStart = Clock::now();
  if (!readElevation(error))
  {
    destroy();
    return false;
  }
  statistics_.readbackMilliseconds = elapsedMilliseconds(readbackStart);

  statistics_.outputReliefMetres = elevationField_.values.empty() ? 0.0 : *std::max_element(elevationField_.values.begin(), elevationField_.values.end());

  // Close the mass balance. Rock that left the bedrock has to be sitting in
  // regolith somewhere or have been exported through the coast; a leak here is
  // invisible in a render and compounds over a long run, so it is measured.
  //
  // Uplift is a source and has to be counted as one, otherwise every run looks
  // like it is creating rock from nothing.
  {
    double bedrockLost = 0.0;
    double regolithGained = 0.0;
    double uplifted = 0.0;
    double injected = 0.0;
    double inTransit = 0.0;
    const double totalTime = timeStep_ * iterations;
    for (uint32_t cell = 0u; cell < cellCount_; ++cell)
    {
      const bool sea = !settings_.closedSystem && oceanMask.values[cell] > 0.5f;
      if (sea)
        continue;
      const double area = cellAreaMetres_[cell];
      const double bedrock = static_cast<double>(elevationField_.values[cell]) - static_cast<double>(regolithField_.values[cell]);
      bedrockLost += (static_cast<double>(elevation.values[cell]) - bedrock) * area;
      regolithGained += static_cast<double>(regolithField_.values[cell]) * area;
      uplifted += settings_.upliftRate * totalTime * static_cast<double>(uplift.values[cell]) * area;
      injected += static_cast<double>(injectedField_[cell]) * area;
      inTransit += static_cast<double>(sedimentField_[cell]) * timeStep_ * area;
    }
    // Uplift is a source: without it every run looks like it created rock.
    statistics_.bedrockLostCubicMetres = bedrockLost;
    statistics_.upliftedCubicMetres = uplifted;
    statistics_.injectedCubicMetres = injected;
    statistics_.inTransitCubicMetres = inTransit;
    statistics_.regolithGainedCubicMetres = regolithGained;
    statistics_.oceanExportCubicMetres = bedrockLost + uplifted + injected - regolithGained - inTransit;
    // Only meaningful in a closed system, where export must be zero and this is
    // a real residual rather than a restatement of its own definition.
    const double scale = std::max(std::abs(bedrockLost) + std::abs(uplifted) + std::abs(injected), 1.0);
    statistics_.massBalanceError = settings_.closedSystem ?
        std::abs(statistics_.oceanExportCubicMetres) / scale : 0.0;
  }

  // Back to the atlas, and back to the normalized encoding the rest of the
  // generator speaks. Ocean texels stay zero: the height field holds solid
  // terrain only, and classification still owns the ocean mask channel.
  std::vector<float> atlas;
  if (!planetEquirectFromCubeField(elevationField_, request.width, request.height, atlas))
  {
    destroy();
    return fail("could not project the solved field back onto the atlas");
  }
  std::vector<uint16_t> &heightmap = *request.heightmap;
  const std::vector<uint8_t> &ocean = *request.oceanMask;
  // The rock finally exposed, resolved from the eroded height. This is the same
  // query the shader ran every iteration, answered once more on the surface the
  // solver actually left behind.
  const bool writeLithology = request.exposedLithology != nullptr && hasGeology;
  if (writeLithology)
    request.exposedLithology->assign(cellCount, static_cast<uint8_t>(PlanetLithology::Sedimentary));
  for (size_t cell = 0u; cell < cellCount; ++cell)
  {
    if (writeLithology)
    {
      const PlanetRockSuite suite = static_cast<PlanetRockSuite>(std::min<uint8_t>(
          (*request.rockSuite)[cell], static_cast<uint8_t>(PlanetRockSuite::Count) - 1u));
      const float phase = static_cast<float>((*request.bedPhase)[cell]) / 255.0f * kPlanetStratumThicknessMetres;
      const float surface = std::isfinite(atlas[cell]) ? static_cast<float>(atlas[cell]) : 0.0f;
      (*request.exposedLithology)[cell] = static_cast<uint8_t>(planetStratumLithology(suite, surface, phase));
    }
    if (ocean[cell] != 0u)
    {
      heightmap[cell] = 0u;
      continue;
    }
    const double metres = std::isfinite(atlas[cell]) ? atlas[cell] : 0.0;
    // Land keeps the encoding's pedestal: below it the pipeline reads sea, and
    // a channel cut past the datum would punch a hole in the coastline.
    const double normalized = std::clamp(metres / request.reliefMetres, kLandPedestal, 1.0);
    heightmap[cell] = static_cast<uint16_t>(std::lround(normalized * 65535.0));
  }

  destroy();
  statistics_.totalMilliseconds = elapsedMilliseconds(totalStart);
  return true;
}

void PlanetStreamPowerSolver::createResources(
    const PlanetCubeField &elevation,
    const PlanetCubeField &uplift,
    const PlanetCubeField &oceanMask,
    const PlanetCubeField &rockSuite,
    const PlanetCubeField &bedPhase)
{
  const uint64_t stateBytes = static_cast<uint64_t>(cellCount_) * sizeof(PlanetCellGPU);
  const uint64_t indexBytes = static_cast<uint64_t>(cellCount_) * sizeof(uint32_t);
  const auto createStorage = [this](const std::string &name, const uint64_t size)
  {
    return rhi_.createBuffer({
        .name = name,
        .size = size,
        .usage = static_cast<gpu::BufferUsage>(
            gpu::BufferUsage_Storage | gpu::BufferUsage_Push | gpu::BufferUsage_Pull |
            gpu::BufferUsage_CopySrc | gpu::BufferUsage_CopyDst),
    });
  };

  // f32 state, never rgba8unorm: 256 height levels planet-wide is not a usable
  // range, and the reference's WebGPU port only used it because that was the
  // sole storage format WebGPU offered.
  stateBuffers_[0] = createStorage("planet.erosion.stateA", stateBytes);
  stateBuffers_[1] = createStorage("planet.erosion.stateB", stateBytes);
  receiverBuffer_ = createStorage("planet.erosion.receiver", indexBytes);

  std::vector<PlanetCellGPU> cells(cellCount_);
  for (uint32_t cell = 0u; cell < cellCount_; ++cell)
  {
    const bool sea = oceanMask.values[cell] > 0.5f;
    const uint32_t suite = std::min<uint32_t>(
        static_cast<uint32_t>(std::lround(rockSuite.values[cell])),
        static_cast<uint32_t>(PlanetRockSuite::Count) - 1u);
    const uint32_t phase = std::min<uint32_t>(
        static_cast<uint32_t>(std::lround(bedPhase.values[cell])), 255u);
    // The initial surface is all bedrock: whatever soil the world starts with
    // is what the run's own weathering produces, so the mass balance has a
    // known starting point.
    cells[cell].bedrock = elevation.values[cell];
    cells[cell].regolith = 0.0f;
    cells[cell].drainage = 0.0f;
    cells[cell].sediment = 0.0f;
    cells[cell].uplift = uplift.values[cell];
    cells[cell].injected = 0.0f;
    const bool outlet = sea && !settings_.closedSystem;
    cells[cell].flags = (outlet ? kOceanFlag : 0u) | (suite << kSuiteShift) | (phase << kBedPhaseShift);
  }
  std::vector<uint32_t> initialReceiver(cellCount_, 8u);

  // Both slots start identical, so a run of any length reads back a valid
  // field regardless of which parity it ends on.
  rhi_.bufferWrite(stateBuffers_[0], 0u, stateBytes, cells.data());
  rhi_.bufferWrite(stateBuffers_[1], 0u, stateBytes, cells.data());
  rhi_.bufferWrite(receiverBuffer_, 0u, indexBytes, initialReceiver.data());

  Uniforms uniforms{};
  uniforms.side = side_;
  uniforms.cellCount = cellCount_;
  uniforms.erosionMode = std::min(settings_.erosionMode, 2u);
  uniforms.radiusMetres = static_cast<float>(radiusMetres_);
  uniforms.timeStep = static_cast<float>(timeStep_);
  uniforms.upliftRate = static_cast<float>(settings_.upliftRate);
  uniforms.streamPowerK = static_cast<float>(settings_.streamPowerK);
  uniforms.hillslopeK = static_cast<float>(settings_.hillslopeK);
  uniforms.debrisK = static_cast<float>(settings_.debrisK);
  uniforms.areaExponent = static_cast<float>(settings_.areaExponent);
  uniforms.slopeExponent = static_cast<float>(settings_.slopeExponent);
  uniforms.maximumElevationMetres = static_cast<float>(reliefMetres_);
  uniforms.sourceAreaScale = 1.0f;
  // With no ocean there is no coastline to protect, and holding every cell above
  // the land pedestal would invent rock in what used to be sea floor — which is
  // precisely the leak the closed-system check exists to detect.
  uniforms.minimumLandElevationMetres = settings_.closedSystem ? 0.0f : static_cast<float>(kLandPedestal * reliefMetres_);
  uniforms.stratumThicknessMetres = kPlanetStratumThicknessMetres;
  uniforms.weatheringRate = static_cast<float>(settings_.weatheringRate);
  uniforms.weatheringDecayMetres = static_cast<float>(settings_.weatheringDecayMetres);
  uniforms.transportK = static_cast<float>(settings_.transportK);
  uniforms.depositionK = static_cast<float>(settings_.depositionK);
  double landArea = 0.0;
  for (uint32_t cell = 0u; cell < cellCount_; ++cell)
    if (oceanMask.values[cell] <= 0.5f)
      landArea += cellAreaMetres_[cell];
  uniforms.maximumDrainageSquareMetres = static_cast<float>(std::max(landArea, 1.0));
  uniforms.sedimentTransport = settings_.sedimentTransport ? 1u : 0u;
  for (uint32_t rock = 0u; rock < kPlanetLithologyCount; ++rock)
    uniforms.erodibility[rock] = settings_.differentialErosion ? kPlanetLithologyTable[rock].erodibility : 1.0f;
  uniformBuffer_ = rhi_.createBuffer({
      .name = "planet.erosion.uniforms",
      .size = sizeof(Uniforms),
      .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Uniform | gpu::BufferUsage_Push),
  });
  rhi_.bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
}

void PlanetStreamPowerSolver::createPipelines()
{
  const gpu::BindingsLayoutInfo layoutInfo{
      .name = "planet.erosion.layout",
      .groups = {gpu::BindingGroupLayout{
          .buffers = {
              {.name = "uniforms", .binding = 0u, .type = gpu::BufferBindingType_UniformBuffer, .visibility = gpu::BindingVisibility_Compute, .isDynamic = false},
              {.name = "stateIn", .binding = 1u, .type = gpu::BufferBindingType_StorageBuffer, .visibility = gpu::BindingVisibility_Compute, .isDynamic = false},
              {.name = "stateOut", .binding = 2u, .type = gpu::BufferBindingType_StorageBuffer, .visibility = gpu::BindingVisibility_Compute, .isDynamic = false},
              {.name = "receiver", .binding = 3u, .type = gpu::BufferBindingType_StorageBuffer, .visibility = gpu::BindingVisibility_Compute, .isDynamic = false},
          },
      }},
  };
  bindingsLayoutId_ = rhi_.createBindingsLayout(layoutInfo);
  const gpu::BindingsLayout layout{.name = "planet.erosion.layout", .id = bindingsLayoutId_};

  const uint64_t stateBytes = static_cast<uint64_t>(cellCount_) * sizeof(PlanetCellGPU);
  const uint64_t indexBytes = static_cast<uint64_t>(cellCount_) * sizeof(uint32_t);
  const auto view = [](const gpu::BufferId buffer, const uint64_t size, const gpu::AccessPattern access)
  {
    return gpu::BufferView{.resourceId = buffer, .offset = 0u, .size = size, .access = access};
  };
  for (uint32_t parity = 0u; parity < 2u; ++parity)
  {
    const uint32_t other = 1u - parity;
    bindingGroupIds_[parity] = rhi_.createBindingGroups({
        .name = "planet.erosion.parity" + std::to_string(parity),
        .layout = layout,
        .groups = {gpu::GroupInfo{
            .name = "group0",
            .buffers = {
                {.bufferView = view(uniformBuffer_, sizeof(Uniforms), gpu::AccessPattern::SHADER_READ), .binding = 0u},
                {.bufferView = view(stateBuffers_[parity], stateBytes, gpu::AccessPattern::SHADER_READ), .binding = 1u},
                {.bufferView = view(stateBuffers_[other], stateBytes, gpu::AccessPattern::SHADER_READ_WRITE), .binding = 2u},
                {.bufferView = view(receiverBuffer_, indexBytes, gpu::AccessPattern::SHADER_READ_WRITE), .binding = 3u},
            },
        }},
    });
  }

  std::ifstream shaderFile(shaderPath_, std::ios::binary);
  if (!shaderFile)
    throw std::runtime_error("could not open the stream power shader: " + shaderPath_);
  const std::string spirv((std::istreambuf_iterator<char>(shaderFile)), std::istreambuf_iterator<char>());
  shaderId_ = rhi_.createShader({.name = "planet.erosion.shader", .src = spirv, .type = gpu::ShaderType::SpirV, .layout = layout});
  const gpu::Shader shader{.name = "planet.erosion.shader", .id = shaderId_};
  const std::array<const char *, 3> entries{"receiver_main", "accumulate_main", "incise_main"};
  for (uint32_t stage = 0u; stage < entries.size(); ++stage)
    pipelineIds_[stage] = rhi_.createComputePipeline({
        .name = std::string("planet.erosion.") + entries[stage],
        .shader = shader,
        .entry = entries[stage],
        .layout = layout,
    });
}

void PlanetStreamPowerSolver::submitBatch(const uint32_t iterations)
{
  std::vector<gpu::CommandBuffer> commandBuffers = rhi_.allocateCommandBuffers(gpu::Queue::Graphics, 1u);
  if (commandBuffers.empty())
    throw std::runtime_error("could not allocate an erosion command buffer");
  const gpu::CommandBuffer commandBuffer = commandBuffers.front();
  rhi_.beginCommandBuffer(commandBuffer, true);
  const uint32_t groupsX = (side_ + 7u) / 8u;
  const uint32_t groupsY = (side_ * 6u + 7u) / 8u;
  const std::array<gpu::ComputePipeline, 3> pipelines{
      gpu::ComputePipeline{.name = "receiver", .id = pipelineIds_[0]},
      gpu::ComputePipeline{.name = "accumulate", .id = pipelineIds_[1]},
      gpu::ComputePipeline{.name = "incise", .id = pipelineIds_[2]},
  };
  for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
  {
    for (const gpu::ComputePipeline &pipeline : pipelines)
    {
      rhi_.cmdBindComputePipeline(commandBuffer, pipeline);
      rhi_.cmdBindBindingGroups(commandBuffer, bindingGroupIds_[current_], nullptr, 0u);
      rhi_.cmdDispatch(commandBuffer, groupsX, groupsY, 1u);
      // Each stage reads what the previous one wrote, so every dispatch is a
      // dependency rather than an independent pass.
      rhi_.cmdMemoryBarrier(
          commandBuffer, gpu::PipelineStage::COMPUTE_SHADER, gpu::PipelineStage::COMPUTE_SHADER,
          gpu::AccessPattern::SHADER_WRITE, gpu::AccessPattern::SHADER_READ);
    }
    current_ = 1u - current_;
  }
  rhi_.endCommandBuffer(commandBuffer);
  gpu::GPUFuture future = rhi_.submit(gpu::Queue::Graphics, commandBuffers.data(), static_cast<uint32_t>(commandBuffers.size()), nullptr, 0u, true);
  rhi_.blockUntil(future);
}

bool PlanetStreamPowerSolver::readElevation(std::string *error)
{
  elevationField_.side = side_;
  elevationField_.values.assign(cellCount_, 0.0f);
  drainageField_.side = side_;
  drainageField_.values.assign(cellCount_, 0.0f);
  regolithField_.side = side_;
  regolithField_.values.assign(cellCount_, 0.0f);
  const uint64_t bytes = static_cast<uint64_t>(cellCount_) * sizeof(PlanetCellGPU);
  bool read = false;
  injectedField_.assign(cellCount_, 0.0f);
  sedimentField_.assign(cellCount_, 0.0f);
  double regolithSum = 0.0;
  double regolithPeak = 0.0;
  uint32_t landCells = 0u;
  rhi_.bufferRead(stateBuffers_[current_], 0u, bytes, [&](const void *data)
  {
    if (data == nullptr)
      return;
    const PlanetCellGPU *cells = static_cast<const PlanetCellGPU *>(data);
    for (uint32_t cell = 0u; cell < cellCount_; ++cell)
    {
      const float bedrock = std::isfinite(cells[cell].bedrock) ? cells[cell].bedrock : 0.0f;
      const float regolith = std::isfinite(cells[cell].regolith) ? std::max(0.0f, cells[cell].regolith) : 0.0f;
      elevationField_.values[cell] = bedrock + regolith;
      regolithField_.values[cell] = regolith;
      drainageField_.values[cell] = std::isfinite(cells[cell].drainage) ? cells[cell].drainage : 0.0f;
      injectedField_[cell] = std::isfinite(cells[cell].injected) ? cells[cell].injected : 0.0f;
      sedimentField_[cell] = std::isfinite(cells[cell].sediment) ? cells[cell].sediment : 0.0f;
      if ((cells[cell].flags & kOceanFlag) == 0u)
      {
        regolithSum += regolith;
        regolithPeak = std::max(regolithPeak, static_cast<double>(regolith));
        ++landCells;
      }
    }
    read = true;
  });
  if (!read)
  {
    if (error != nullptr)
      *error = "could not read the solved state buffer";
    return false;
  }
  statistics_.meanRegolithMetres = landCells > 0u ? regolithSum / landCells : 0.0;
  statistics_.maximumRegolithMetres = regolithPeak;
  return true;
}

void PlanetStreamPowerSolver::destroy()
{
  for (gpu::BindingGroupsId &group : bindingGroupIds_)
  {
    if (group != gpu::BindingGroupsId::Invalid)
      rhi_.deleteBindingGroups(group);
    group = gpu::BindingGroupsId::Invalid;
  }
  for (gpu::ComputePipelineId &pipeline : pipelineIds_)
  {
    if (pipeline != gpu::ComputePipelineId::Invalid)
      rhi_.deleteComputePipeline(pipeline);
    pipeline = gpu::ComputePipelineId::Invalid;
  }
  if (shaderId_ != gpu::ShaderId::Invalid)
    rhi_.deleteShader(shaderId_);
  shaderId_ = gpu::ShaderId::Invalid;
  if (bindingsLayoutId_ != gpu::BindingsLayoutId::Invalid)
    rhi_.deleteBindingsLayout(bindingsLayoutId_);
  bindingsLayoutId_ = gpu::BindingsLayoutId::Invalid;
  const auto deleteBuffer = [this](gpu::BufferId &buffer)
  {
    if (buffer != gpu::BufferId::Invalid)
      rhi_.deleteBuffer(buffer);
    buffer = gpu::BufferId::Invalid;
  };
  deleteBuffer(uniformBuffer_);
  for (gpu::BufferId &buffer : stateBuffers_)
    deleteBuffer(buffer);
  deleteBuffer(receiverBuffer_);
  current_ = 0u;
}

} // namespace procedural::rendering
