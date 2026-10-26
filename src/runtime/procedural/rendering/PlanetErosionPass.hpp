#pragma once

#include "procedural/PlanetCubeField.hpp"
#include "procedural/PlanetGeology.hpp"
#include "procedural/SphericalTerrain.hpp"
#include "rendering/gpu/RHI.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::rendering
{

// Parameters of the stream power solver.
//
// Defaults follow the reference GLSL (StreamPowerErosion) rather than its
// WebGPU port: the port drops the slope exponent from 2 to 1, which makes
// incision linear in slope and materially weakens valley deepening, and it
// compensates with a 100x larger k and a 25x smaller timestep. Those are
// adaptations to its 8-bit height field, not better defaults.
struct PlanetStreamPowerSettings
{
  // 0 stream power only, 1 adds hillslope diffusion, 2 adds the debris term.
  uint32_t erosionMode = 1u;
  // Cells per cube-face edge. Zero matches the atlas ground resolution.
  uint32_t faceSide = 0u;
  // Zero derives the count from the domain: drainage area advances exactly one
  // cell per iteration, so a field of side N needs O(N) iterations before its
  // downstream half has seen its true catchment.
  uint32_t iterations = 0u;
  uint32_t iterationsPerBatch = 64u;
  // Total simulated time the run integrates, in the reference solver's time
  // units. The per-iteration step is this divided by the iteration count, which
  // is what makes the result resolution-independent: iteration count controls
  // how far drainage information propagates and how accurate the integration
  // is, not how much rock the run removes. Using a fixed per-iteration step
  // instead makes a higher-resolution bake erode proportionally more, which
  // moved the channel-regime exponent from -0.50 to -1.12 between a 256- and a
  // 1024-iteration run of the same planet.
  double simulatedTime = 12800.0;
  // Metres of uplift per unit tectonics per unit time. This is what incision
  // works against, and the pair (upliftRate, streamPowerK) sets the equilibrium
  // channel slope, since stream power balances uplift at S = (U/(k A^m))^(1/n).
  //
  // Calibrated on the reference planet: uplift near zero lets the channels
  // flatten the authored relief, while uplift high enough to preserve all of it
  // leaves the network under-organised at a fixed iteration count. Resistant
  // rock amplifies the effect, so this dropped from 0.10 to 0.05 once lithology
  // landed — at 0.10 the strata drove peak relief into the encoding ceiling.
  double upliftRate = 0.05;
  // Erodibility in E = k A^m S^n with A in square metres. The reference's
  // k = 0.0005 belongs to a domain of a few hundred cells across whose drainage
  // is measured in cell diagonals; carrying that number over unchanged makes
  // incision four orders of magnitude too strong.
  double streamPowerK = 2.5e-5;
  double hillslopeK = 2.0;
  double debrisK = 10.0;
  double areaExponent = 0.8;  // m in E = k A^m S^n
  double slopeExponent = 2.0; // n in E = k A^m S^n
  // Soil production rate in metres per unit time at bare rock, scaled by the
  // rock's own erodibility. Weathering is what supplies the sediment that
  // fluvial transport then moves.
  double weatheringRate = 4.0e-4;
  // Depth at which existing regolith has armoured the rock beneath it to 1/e of
  // the bare-rock rate. This is what makes soil depth self-limit.
  double weatheringDecayMetres = 2.5;
  // Transport-limited sediment routing: weathering into regolith, fluvial
  // transport and deposition. Off by default because it is implemented but not
  // yet calibrated — see the phase 3 section of planet-generator-baseline.md.
  // The detachment-limited path is the one whose slope-area gate is measured.
  bool sedimentTransport = false;
  // Sediment transport capacity coefficient in Qc = kt A^m S^n. Erosion is
  // capacity minus load, so this replaces the detachment-limited k as the dial
  // that sets how much material the rivers move.
  double transportK = 2.5e-5;
  // Fraction of the excess load dropped per unit time when over capacity.
  // McDonald & Cordonnier's Appendix C gives 0.05 as a starting coefficient.
  double depositionK = 0.05;
  // False forces every rock type to erodibility 1, which is the before side of
  // the differential-erosion comparison rather than a supported world.
  bool differentialErosion = true;
  // Treats every cell as land, removing the ocean outlet. Nothing can leave the
  // system, so bedrock lost plus uplift must equal regolith gained: it is the
  // only way to check conservation independently, since with an outlet the
  // export term can only be inferred from the very difference it explains.
  bool closedSystem = false;
};

struct PlanetStreamPowerStatistics
{
  uint32_t faceSide = 0u;
  uint32_t iterations = 0u;
  uint64_t cellCount = 0u;
  // Per-iteration step actually used: simulatedTime / iterations.
  double timeStep = 0.0;
  double projectionMilliseconds = 0.0;
  double solveMilliseconds = 0.0;
  double readbackMilliseconds = 0.0;
  double totalMilliseconds = 0.0;
  // Land relief before and after, in metres, for the honest before/after line.
  double inputReliefMetres = 0.0;
  double outputReliefMetres = 0.0;
  // Normalized uplift actually delivered to the solver, so an inert uplift term
  // can be told apart from an empty uplift field.
  double meanUplift = 0.0;
  double peakUplift = 0.0;
  // Mass balance, in cubic metres. A leak here is invisible in a render and
  // lethal to a long run, so it is measured rather than assumed.
  //
  // With an ocean outlet the export term is inferred, not measured: it is
  // whatever bedrock loss plus uplift did not end up as regolith. Run with
  // closedSystem to check conservation for real, where export must be zero and
  // massBalanceError is then a genuine residual.
  double bedrockLostCubicMetres = 0.0;
  double regolithGainedCubicMetres = 0.0;
  double upliftedCubicMetres = 0.0;
  // Rock the monotonicity clamp invented where no sediment was available to
  // fill a depression. This is the solver's only mass source besides uplift.
  double injectedCubicMetres = 0.0;
  // Sediment entrained but not yet deposited when the run stopped. It is real
  // material in the books, not a leak, and omitting it makes the balance blame
  // the difference on export.
  double inTransitCubicMetres = 0.0;
  double oceanExportCubicMetres = 0.0;
  double massBalanceError = 0.0;
  // Regolith depth over land, in metres.
  double meanRegolithMetres = 0.0;
  double maximumRegolithMetres = 0.0;
};

// GPU stream power erosion over the six cube faces of the planet.
//
// The pass is synchronous: it owns its dispatch loop and blocks in batches,
// because it runs inside an offline bake rather than a frame. Batching between
// fences follows TerrainPreviewHydraulicErosionPass, which the sample's
// progress UI already speaks.
class PlanetStreamPowerSolver final : public SphericalTerrainFluvialSolver
{
public:
  PlanetStreamPowerSolver(::rendering::RHI &rhi, std::string shaderPath, PlanetStreamPowerSettings settings = {});
  ~PlanetStreamPowerSolver() override;

  PlanetStreamPowerSolver(const PlanetStreamPowerSolver &) = delete;
  PlanetStreamPowerSolver &operator=(const PlanetStreamPowerSolver &) = delete;

  bool solve(const Request &request, std::string *error) override;

  const PlanetStreamPowerStatistics &statistics() const
  {
    return statistics_;
  }

  // The solved field on its own domain, before the projection back to the
  // atlas. Diagnostics should measure this rather than the resampled atlas: the
  // round trip through the equirectangular grid softens exactly the channels
  // the solver just cut.
  const PlanetCubeField &solvedField() const
  {
    return elevationField_;
  }

  // Accumulated drainage area in square metres, the solver's own discharge
  // field. It is a strictly better river signal than the re-routed mask the
  // generator still computes on the CPU.
  const PlanetCubeField &drainageField() const
  {
    return drainageField_;
  }

  // Regolith thickness in metres on the solver's domain.
  const PlanetCubeField &regolithField() const
  {
    return regolithField_;
  }

private:
  // Per-cell state, matching PlanetCell in planet_stream_power.wgsl. One struct
  // instead of a buffer per field: it mirrors the reference solver's own vec4
  // Read/Write pair, makes each cell a single contiguous fetch, and keeps the
  // binding set down to two state buffers plus the receiver directions.
  struct PlanetCellGPU
  {
    float bedrock = 0.0f;
    float regolith = 0.0f;
    float drainage = 0.0f;
    float sediment = 0.0f;
    float uplift = 0.0f;
    float injected = 0.0f;
    uint32_t flags = 0u;
  };

  static constexpr uint32_t kOceanFlag = 1u;
  // Rock suite occupies bits 8..11 and the bedding phase bits 16..23, so all of
  // the geology rides along without growing the 16-byte cell.
  static constexpr uint32_t kSuiteShift = 8u;
  static constexpr uint32_t kBedPhaseShift = 16u;

  struct alignas(16) Uniforms
  {
    uint32_t side = 0u;
    uint32_t cellCount = 0u;
    uint32_t erosionMode = 0u;
    uint32_t reserved = 0u;
    float radiusMetres = 0.0f;
    float timeStep = 0.0f;
    float upliftRate = 0.0f;
    float streamPowerK = 0.0f;
    float hillslopeK = 0.0f;
    float debrisK = 0.0f;
    float areaExponent = 0.0f;
    float slopeExponent = 0.0f;
    float maximumElevationMetres = 0.0f;
    float sourceAreaScale = 0.0f;
    float minimumLandElevationMetres = 0.0f;
    float stratumThicknessMetres = 0.0f;
    float weatheringRate = 0.0f;
    float weatheringDecayMetres = 0.0f;
    float transportK = 0.0f;
    float depositionK = 0.0f;
    float maximumDrainageSquareMetres = 0.0f;
    uint32_t sedimentTransport = 0u;
    // The WGSL array<vec4<f32>, 2> below needs 16-byte alignment, which the 88
    // bytes of scalars above do not reach. Without this padding the shader read
    // the array from offset 96 while C++ wrote it at 88, so every rock took
    // another rock's erodibility and the two hardest — metamorphic and granite,
    // the rocks that hold mountains up — read struct padding instead.
    //
    // Phase 2 was correct by luck: the scalars totalled exactly 64 bytes then.
    // Phase 3 added six more and silently broke the alignment.
    std::array<uint32_t, 2> alignmentPadding{};
    // Erodibility per PlanetLithology, matching the WGSL uniform's vec4-packed
    // array. Eight rock types fill exactly two lanes.
    std::array<float, kPlanetLithologyCount> erodibility{};
  };

  // The uniform layout is shared with planet_stream_power.wgsl and there is no
  // compiler that checks the two agree, so assert the part that drifted.
  static_assert(sizeof(Uniforms) == 128u, "stream power uniforms must stay 128 bytes");
  static_assert(offsetof(Uniforms, erodibility) == 96u,
      "erodibility must sit at the 16-byte boundary WGSL places it on");

  void createResources(
      const PlanetCubeField &elevation,
      const PlanetCubeField &uplift,
      const PlanetCubeField &oceanMask,
      const PlanetCubeField &rockSuite,
      const PlanetCubeField &bedPhase);
  void createPipelines();
  void submitBatch(uint32_t iterations);
  bool readElevation(std::string *error);
  void destroy();

  ::rendering::RHI &rhi_;
  std::string shaderPath_;
  PlanetStreamPowerSettings settings_{};
  PlanetStreamPowerStatistics statistics_{};
  PlanetCubeField elevationField_{};
  PlanetCubeField drainageField_{};
  PlanetCubeField regolithField_{};
  uint32_t side_ = 0u;
  uint32_t cellCount_ = 0u;
  double radiusMetres_ = 0.0;
  double reliefMetres_ = 0.0;
  double timeStep_ = 0.0;
  // Ground area of every cell, in square metres. Needed to turn a thickness in
  // metres into a volume for the mass balance.
  std::vector<double> cellAreaMetres_;
  // Per-cell invented rock, read back with the rest of the state.
  std::vector<float> injectedField_;
  std::vector<float> sedimentField_;
  // Which of the two ping-pong slots currently holds the live field.
  uint32_t current_ = 0u;
  ::rendering::BufferId uniformBuffer_ = ::rendering::BufferId::Invalid;
  std::array<::rendering::BufferId, 2> stateBuffers_{::rendering::BufferId::Invalid, ::rendering::BufferId::Invalid};
  ::rendering::BufferId receiverBuffer_ = ::rendering::BufferId::Invalid;
  ::rendering::BindingsLayoutId bindingsLayoutId_ = ::rendering::BindingsLayoutId::Invalid;
  // One group per ping-pong parity.
  std::array<::rendering::BindingGroupsId, 2> bindingGroupIds_{::rendering::BindingGroupsId::Invalid, ::rendering::BindingGroupsId::Invalid};
  ::rendering::ShaderId shaderId_ = ::rendering::ShaderId::Invalid;
  std::array<::rendering::ComputePipelineId, 3> pipelineIds_{
      ::rendering::ComputePipelineId::Invalid,
      ::rendering::ComputePipelineId::Invalid,
      ::rendering::ComputePipelineId::Invalid};
};

} // namespace procedural::rendering
