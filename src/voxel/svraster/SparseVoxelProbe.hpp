#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster
{

// Screen-space probe direct illumination.
//
// The per-pixel path spends one shadow ray per receiver per frame and then
// tries to recover a usable estimate from a single binary visibility sample.
// That is the wrong place to fight variance for a low-frequency signal: soft
// shadows and sky occlusion vary far more slowly across the screen than the
// per-pixel sampling noise does.
//
// A probe instead owns a whole screen tile, traces a full hemisphere of
// directions in one frame, and stores the result in a small octahedral map.
// Every pixel in the neighbourhood then reconstructs its own cosine-weighted
// irradiance from those directions. One probe with 64 directions costs a
// fraction of 256 pixels with one ray each, and it produces an estimate whose
// variance is already low before any denoiser runs.

// Output pixels per probe along each axis. Twelve is the production balance:
// adaptive refinement restores probes at depth/normal discontinuities, while
// broad corridor walls and floors no longer pay the 8-pixel high-quality
// density during camera motion. The renderer converts this footprint to render
// pixels, so TSR scale changes do not silently change the screen-space quality.
// A cold uniform grid therefore costs 64 / (12 * 12) = 0.44 traced directions
// per output pixel. Eight remains available through --probe-tile for
// high-quality captures.
constexpr uint32_t kSparseVoxelProbeDefaultTileSize = 12u;
// Hemi-octahedral resolution of one probe's radiance map. 8x8 = 64 directions
// covering the hemisphere above the probe's normal. The mapping is hemispherical
// rather than spherical because every cell must be a direction the probe can
// actually see: a full-sphere map spends half its cells below the surface, and
// folding those back destroys the equal-solid-angle property the estimator
// depends on.
constexpr uint32_t kSparseVoxelProbeOctahedralSize = 8u;
// Second-order spherical harmonics: nine coefficients per probe, RGB packed
// into a vec4 each so the GPU side is one aligned array.
constexpr uint32_t kSparseVoxelProbeSHCount = 9u;

constexpr uint32_t kSparseVoxelProbeDirectionCount =
    kSparseVoxelProbeOctahedralSize * kSparseVoxelProbeOctahedralSize;

// One probe record. Kept to 16 words so the placement pass writes a single
// aligned block per probe and the interpolation pass reads one cache line.
struct alignas(16) SparseVoxelProbeGPU
{
  // World position of the probe's representative surface point, w is the
  // linear depth it was placed at.
  std::array<float, 4> positionDepth{};
  // World geometry normal, w is the world-space radius the probe covers. The
  // radius bounds how far a pixel may be from the probe and still accept it.
  std::array<float, 4> normalRadius{};
  // Stable instance and voxel identity of the representative surface, then the
  // accumulated sample count and a validity flag.
  std::array<uint32_t, 4> identityState{};
  // Screen pixel the probe was placed at, the frame it was last updated, and
  // the previous-frame probe index it reprojected from (0xffffffff if none).
  std::array<uint32_t, 4> pixelFrameHistory{};
};
static_assert(sizeof(SparseVoxelProbeGPU) == 64u);

constexpr uint32_t kSparseVoxelProbeStateValid = 1u << 0u;
// The probe reprojected from a previous-frame probe on the same surface, so
// its octahedral radiance history is meaningful.
constexpr uint32_t kSparseVoxelProbeStateReprojected = 1u << 1u;
// The probe was placed by adaptive refinement rather than by owning a tile.
constexpr uint32_t kSparseVoxelProbeStateAdaptive = 1u << 2u;
// Log2 of the screen-grid stride used for this probe (0 = every tile, 1 = one
// in 2x2 tiles, 2 = one in 4x4 tiles). Keeping it in the probe record lets
// sharing and filtering find the next live neighbour without another density
// texture or a second placement list.
constexpr uint32_t kSparseVoxelProbeStateDensityShift = 3u;
constexpr uint32_t kSparseVoxelProbeStateDensityMask = 3u << 3u;

// Adaptive probe refinement, following Lumen's screen probe placement.
//
// One probe per tile is the right density for a surface that is roughly planar
// across the tile, and the wrong density for anything else. A tile straddling
// a depth discontinuity - a pillar edge against a distant wall, a railing, a
// stair nosing - has no single representative point, so whichever pixel wins
// the spawn election describes one of the surfaces and misrepresents the rest.
// The interpolation then rejects that probe for the pixels on the other
// surface, and those pixels fall back to whatever is nearest. That is the tile
// structure visible in the output: it is not a filtering artefact, it is the
// placement being unable to express the tile.
//
// Refinement adds probes exactly where that test fails. A tile that its own
// probe already represents costs nothing extra; a tile that needs two or three
// surfaces described gets them. The refined probes live in an overflow range
// of the same probe arrays, so ray generation, sharing, resolve and the SH
// projection all treat them as ordinary probes and need no changes at all.
// Voxel surfaces have at most three simultaneously visible cube faces, so the
// tile owns one primary probe plus two refinement slots. Keeping that bound
// structural makes the gather predictable and prevents one noisy tile from
// consuming the refinement budget intended for its neighbours.
constexpr uint32_t kSparseVoxelProbeAdaptivePerTile = 2u;

// Words in the refinement allocation buffer: one counter, an eight-bin demand
// histogram, and the priority cutoff derived from the previous frame.
constexpr uint32_t kSparseVoxelProbeAllocationWords = 12u;

// Two refinement slots per uniform tile guarantee that a 90-degree face change
// can keep both faces resident while the third visible cube face receives the
// remaining slot. This is capacity rather than steady-state ray cost: planar
// tiles allocate neither slot, and cached probes use reduced scheduled ray
// counts after convergence.
constexpr uint32_t kSparseVoxelProbeDefaultAdaptivePercent = 200u;

constexpr uint32_t sparseVoxelProbeCountAxis(
    const uint32_t extent,
    const uint32_t tileSize = kSparseVoxelProbeDefaultTileSize)
{
  const uint32_t size = std::max(tileSize, 1u);
  return (std::max(extent, 1u) + size - 1u) / size;
}

constexpr uint32_t sparseVoxelProbeCount(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelProbeDefaultTileSize)
{
  return sparseVoxelProbeCountAxis(width, tileSize) *
         sparseVoxelProbeCountAxis(height, tileSize);
}

// How many refined probes may exist. Rounded up to a whole workgroup so the
// pass that invalidates the unallocated tail never needs a bounds test.
constexpr uint32_t sparseVoxelProbeAdaptiveCapacity(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelProbeDefaultTileSize,
    const uint32_t adaptivePercent = kSparseVoxelProbeDefaultAdaptivePercent)
{
  if (adaptivePercent == 0u) return 0u;
  const uint64_t uniform = sparseVoxelProbeCount(width, height, tileSize);
  const uint32_t budget = static_cast<uint32_t>(
      std::min<uint64_t>(uniform * adaptivePercent / 100u, 0xffff0000ull));
  return ((budget + 63u) / 64u) * 64u;
}

// Every array indexed by probe carries the uniform grid first and the refined
// overflow after it, so a refined probe is an ordinary probe with a higher
// index and no downstream pass has to know the difference.
constexpr uint32_t sparseVoxelProbeTotalCount(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelProbeDefaultTileSize,
    const uint32_t adaptivePercent = kSparseVoxelProbeDefaultAdaptivePercent)
{
  return sparseVoxelProbeCount(width, height, tileSize) +
         sparseVoxelProbeAdaptiveCapacity(
             width, height, tileSize, adaptivePercent);
}

constexpr uint64_t sparseVoxelProbeBufferSize(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelProbeDefaultTileSize,
    const uint32_t adaptivePercent = kSparseVoxelProbeDefaultAdaptivePercent)
{
  return uint64_t{sparseVoxelProbeTotalCount(
             width, height, tileSize, adaptivePercent)} *
         sizeof(SparseVoxelProbeGPU);
}

// Per-tile registry of the refined probes that tile spawned. Interpolation is
// the only consumer, and it only ever looks at the tiles it already gathers
// uniform probes from, so a small fixed capacity per tile beats a compacted
// list: no prefix sum, no indirection, and a bounded inner loop.
constexpr uint64_t sparseVoxelProbeAdaptiveTileBufferSize(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelProbeDefaultTileSize)
{
  return uint64_t{sparseVoxelProbeCount(width, height, tileSize)} *
         (kSparseVoxelProbeAdaptivePerTile + 1u) * sizeof(uint32_t);
}

// Ray slots reserved per probe. This is a *capacity*, not a per-frame cost: a
// probe that traces owns a contiguous 64-slot range, but scheduling means only
// a fraction of probes trace on any given frame. The ray, hit and active-index
// buffers are still sized for the worst case, because the canonical ray index
// of a probe cell is `probeIndex * 64 + cell` and that mapping has to stay
// valid whether or not the probe traced.
constexpr uint32_t sparseVoxelProbeRayCount(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelProbeDefaultTileSize,
    const uint32_t adaptivePercent = kSparseVoxelProbeDefaultAdaptivePercent)
{
  return sparseVoxelProbeTotalCount(
             width, height, tileSize, adaptivePercent) *
         kSparseVoxelProbeDirectionCount;
}

// Per-probe scheduling state, carried across frames alongside the probe record.
//
// Tracing every probe's whole direction set every frame is the single largest
// cost in the probe path, and most of it is spent re-measuring hemispheres that
// have not changed. This block is what lets a probe stop paying for that: it
// records how converged the probe's own irradiance estimate is, and which
// lights were affecting it when that estimate was made.
//
// It is a separate array rather than four more words in SparseVoxelProbeGPU so
// the probe record stays at the 64 bytes its single-cache-line read depends on.
struct alignas(16) SparseVoxelProbeScheduleGPU
{
  // Hash of the lights whose influence volume contains the probe, mixing each
  // one's stable handle with its content revision. A light that never changes
  // never changes this value, which is what makes "skip static lights" fall out
  // for free instead of needing a static flag of its own.
  uint32_t lightSignature{0u};
  // Running mean and second moment of the probe's mean irradiance, updated only
  // on frames the probe actually traced. Their ratio is the relative standard
  // error the convergence test thresholds against.
  float meanLuminance{0.0f};
  float secondMomentLuminance{0.0f};
  // stableRuns | wakeFrames | lastInterval, one byte each.
  uint32_t counters{0u};
};
static_assert(sizeof(SparseVoxelProbeScheduleGPU) == 16u);

// A probe's update rate has two dials, and they do different jobs.
//
// The *ray count* is how much of the hemisphere one update measures. Reducing
// it keeps every probe updating every frame, so a change is picked up on the
// next frame and the estimate moves smoothly; the cost is that one update is a
// partial hemisphere, and the directions it skipped are carried by the sharing
// pass until their turn comes round.
//
// The *interval* is how many frames pass between updates at all. It is the
// cheaper of the two per unit of work saved - a skipped frame costs no rays,
// no compaction slots and no resolve arithmetic - but a probe cannot react to
// anything until its turn arrives, so it is only appropriate once a probe is
// thoroughly settled.
//
// The defaults spend the ray count first, then stagger probes only after their
// estimate has converged. The phase is keyed on the probe's tile, so the probes
// that trace on a given frame are spread across the screen rather than pulsing
// together. Newly placed, disoccluded, or light-invalidated probes remain at
// interval one and receive a fresh awake direction slice every frame.
//
// Next-event contributions are deliberately *not* written into the radiance
// map: they are pdf-weighted estimator contributions, not directional radiance
// samples. The resolve pass carries them in a separate SH history, so an
// interval may safely skip both tracing and resolving a finite emitter without
// making the light disappear on the skipped frame.
constexpr uint32_t kSparseVoxelProbeIntervalAwake = 1u;
constexpr uint32_t kSparseVoxelProbeIntervalConverged = 2u;
constexpr uint32_t kSparseVoxelProbeIntervalAsleep = 4u;

// Directions traced per update in each tier. A reduced count is split between
// the stratified and next-event estimators in the same proportion the full set
// uses; see sparse_voxel_probe_cell_schedule for why that split has to be
// explicit rather than falling out of a stride.
// Half the map per frame covers all directions in two consecutive awake
// updates. This retains the every-frame response needed at disocclusions while
// avoiding a one-frame 64-ray burst when camera jitter exposes many tiny faces
// at once. Probe sharing and the orientation-independent fallback cover the
// unmeasured half during that first frame.
constexpr uint32_t kSparseVoxelProbeRaysAwake =
    kSparseVoxelProbeDirectionCount / 2u;
// A probe with a useful directional prior only needs to validate a quarter of
// that map itself instead of paying the full cold-start burst. This covers a
// new probe seeded from compatible neighbours and a carried probe whose motion
// gate temporarily returned it to the awake tier. Truly cold probes (first
// frame or a disocclusion with no compatible neighbours) still use
// kSparseVoxelProbeRaysAwake.
constexpr uint32_t kSparseVoxelProbeRaysSeeded = 16u;
constexpr uint32_t kSparseVoxelProbeRaysConverged = 8u;
constexpr uint32_t kSparseVoxelProbeRaysAsleep = 4u;

// Traced updates a probe must have accumulated before it may leave the awake
// tier at all, whatever its error says. A probe that has only been measured
// twice can report a small relative error purely by chance.
constexpr uint32_t kSparseVoxelProbeMinimumUpdates = 4u;
// Relative standard error at or below which a traced update counts as stable.
constexpr float kSparseVoxelProbeMaximumError = 0.15f;
// Consecutive stable updates required to reach each tier.
constexpr uint32_t kSparseVoxelProbeConvergedRuns = 2u;
constexpr uint32_t kSparseVoxelProbeAsleepRuns = 6u;
// Frames a probe stays awake after its light signature changes. One frame would
// be enough to re-measure, but not to re-converge: the temporal blend needs a
// few full updates before the new answer has replaced the old one.
constexpr uint32_t kSparseVoxelProbeWakeFrames = 8u;

// How far a probe may move off the plane it was describing between frames, as a
// fraction of the world radius it serves, before it is held in the awake tier
// regardless of how converged its statistics look.
//
// Convergence alone cannot carry this decision. The statistics only compare
// frames the probe traced, so a sleeping probe's samples are many frames apart
// and a steady drift is absorbed by the running mean rather than reported as
// error - which lets a probe that has moved onto different geometry keep its
// tier and then jump every time its turn comes round. The per-pixel scheduler
// gates on motion separately for exactly this reason.
//
// The measure is the displacement along the probe's own normal, not the raw
// distance: placement re-elects the probe's pixel inside its tile every frame,
// so in-plane sliding is constant even under a static camera and is exactly
// what the sharing pass's parallax reprojection already handles.
constexpr float kSparseVoxelProbeMotionRadiusFraction = 0.25f;

// Automatic distance LODs are expressed as fractions of the probe trace
// distance unless the caller supplies explicit world distances. Near geometry
// keeps one probe per tile, the middle tier keeps one per 2x2 tiles, and the
// far tier one per 4x4 tiles.
constexpr float kSparseVoxelProbeDensityHalfDistanceScale = 0.35f;
constexpr float kSparseVoxelProbeDensityQuarterDistanceScale = 0.65f;
constexpr float kSparseVoxelProbeLocalLightCullStartScale = 0.55f;
// Past this point no traversal rays are emitted. Resolve projects the analytic
// sky and directional lights directly into the probe SH, intentionally trading
// far-field shadows for a large reduction in ray work.
constexpr float kSparseVoxelProbeSkyOnlyDistanceScale = 0.70f;
constexpr uint32_t kSparseVoxelProbeMaximumDensityStride = 4u;

// Resolve user-facing world distances once on the CPU so every probe pass sees
// the same ordered thresholds. Non-positive values request automatic fractions
// of the trace extent; the analytical cutoff must follow the fade start.
constexpr std::array<float, 4> sparseVoxelProbeDistanceThresholds(
    const float maximumDistance, const float densityHalfDistance,
    const float densityQuarterDistance,
    const float localLightCullStartDistance, const float skyOnlyDistance)
{
  const float extent = std::max(maximumDistance, 1.0e-3f);
  const float half = densityHalfDistance > 0.0f
      ? densityHalfDistance
      : extent * kSparseVoxelProbeDensityHalfDistanceScale;
  const float quarter = std::max(
      densityQuarterDistance > 0.0f
          ? densityQuarterDistance
          : extent * kSparseVoxelProbeDensityQuarterDistanceScale,
      half);
  const float localStart = localLightCullStartDistance > 0.0f
      ? localLightCullStartDistance
      : extent * kSparseVoxelProbeLocalLightCullStartScale;
  const float skyOnly = std::max(
      skyOnlyDistance > 0.0f
          ? skyOnlyDistance
          : extent * kSparseVoxelProbeSkyOnlyDistanceScale,
      localStart + 1.0e-3f);
  return {half, quarter, localStart, skyOnly};
}

constexpr uint64_t sparseVoxelProbeScheduleBufferSize(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelProbeDefaultTileSize,
    const uint32_t adaptivePercent = kSparseVoxelProbeDefaultAdaptivePercent)
{
  return uint64_t{sparseVoxelProbeTotalCount(
             width, height, tileSize, adaptivePercent)} *
         sizeof(SparseVoxelProbeScheduleGPU);
}

// The directional radiance map: one RGBA record per direction cell per probe,
// holding the radiance arriving along that cell and the distance the ray
// travelled to find it.
//
// The distance is the reason this exists at all. Without it a cell is only a
// radiance in a direction, which is useless to any probe but its owner. With it
// the cell identifies a *point*, and a neighbouring probe can re-aim that
// sample from its own position - which is what turns 64 traced rays per probe
// into up to 576 usable samples. It costs one screen-sized RGBA32F buffer per
// history slot, and that cost buys the entire ray-sharing scheme.
constexpr uint64_t sparseVoxelProbeRadianceBufferSize(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelProbeDefaultTileSize,
    const uint32_t adaptivePercent = kSparseVoxelProbeDefaultAdaptivePercent)
{
  return uint64_t{sparseVoxelProbeTotalCount(
             width, height, tileSize, adaptivePercent)} *
         kSparseVoxelProbeDirectionCount * 4u * sizeof(float);
}

constexpr uint64_t sparseVoxelProbeSHBufferSize(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelProbeDefaultTileSize,
    const uint32_t adaptivePercent = kSparseVoxelProbeDefaultAdaptivePercent)
{
  return uint64_t{sparseVoxelProbeTotalCount(
             width, height, tileSize, adaptivePercent)} *
         kSparseVoxelProbeSHCount * 4u * sizeof(float);
}

// Which direct-illumination path the renderer builds. The two are exclusive:
// selecting one removes the other's passes from the graph entirely rather than
// leaving them resident and idle.
enum class SparseVoxelDIMode : uint32_t
{
  // One ReSTIR-resampled shadow ray per pixel, accumulated in the
  // full-resolution light field.
  PerPixel = 0u,
  // One screen-space probe per tile, full hemisphere per probe, interpolated
  // to full resolution.
  Probe = 1u,
};

} // namespace voxel::svraster
