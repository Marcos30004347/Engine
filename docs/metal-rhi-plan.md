# Metal RHI plan

Status: proposed
Date: 2026-07-30
Scope: `src/runtime/rendering/gpu` (RHI interface, new Metal backend), `src/runtime/window`, `src/runtime/softwarebvh` (acceleration-structure bridge), `wgslc-naga`, `scripts/`, `cmake/`, every `*.cmake` that calls `wgsl2spirv`, and the RHI construction sites in `tests/` and `samples/`

## Executive decision

Add a second `rendering::RHI` implementation backed by Metal, selected by default on Apple platforms, with MoltenVK kept alive as a switchable A/B backend for the whole migration. The WGSL sources stay the single shader authority: `wgslc-naga` grows an MSL output path that emits a `.metallib` plus a sidecar binding-map JSON, and the Metal backend consumes that pair at runtime.

Through P6 the parity work changes nothing about the render graph, the passes, or the WGSL — it is confined to one interface implementation, one compiler output mode, one build-system function, and the handful of places that name Vulkan directly. P7 is the deliberate exception: hardware ray tracing adds an acceleration-structure resource type to the RHI and a new class of WGSL, and it is the one capability the Metal backend gains that the MoltenVK path can never have.

```text
*.wgsl → wgsl_pp.py → naga IR ─┬→ spv-out  → .spirv        → VulkanRHI
                               └→ msl-out  → .metal        → metallib → MetalRHI
                                           → .bindings.json ┘
```

## Why this is worth doing

The engine already runs on macOS through MoltenVK, and [docs/xcode-gpu-profiling.md](docs/xcode-gpu-profiling.md) documents the cost of that: Vulkan timestamps measure Vulkan pass boundaries, not Metal encoders, so translation-layer stalls get billed to the wrong pass and a capture is the only way to read the frame. A native backend removes that indirection entirely — encoders are ours, they carry our pass names, the shader source in the capture is the MSL we compiled, and the Metal debugger's shader profiler, geometry viewer, and dependency graph all attach to objects the engine created rather than to MoltenVK's synthesized ones. It also removes the `libMoltenVK.dylib` link and the `VULKAN_SDK` environment dependency currently hard-coded in [CMakeLists.txt:104-113](CMakeLists.txt#L104-L113), and it is the only route to iOS.

## Current state

Facts this plan builds on, all verified in the tree:

- `rendering::RHI` ([src/runtime/rendering/gpu/RHI.hpp](src/runtime/rendering/gpu/RHI.hpp)) is already the abstraction the render graph talks to — 60-odd virtuals, no Vulkan types in the signatures. `VulkanRHI` is a plain subclass. This is the seam.
- The binding model is WebGPU-shaped (`BindingGroupLayout` with separate buffer/sampler/texture/storage-texture entry lists, [Types.hpp:914-979](src/runtime/rendering/gpu/Types.hpp#L914-L979)), which maps onto Metal's separate buffer/texture/sampler argument tables without a translation layer.
- Shaders are loaded as opaque bytes: passes call `renderGraph->createShader({.src = os::io::readRelativeFile("assets/shaders/spirv/foo.spirv"), .type = ShaderType::SpirV, ...})`. `ShaderType` already has a `WGSL` enumerator that nothing uses, so adding `MetalLib` costs nothing.
- 56 `wgsl2spirv()` invocations across [src/runtime/CMakeLists.txt](src/runtime/CMakeLists.txt) and the per-test `.cmake` files produce every shader; the function itself is 80 lines in [cmake/WGSL2SpirV.cmake](cmake/WGSL2SpirV.cmake).
- `VulkanRHI` is constructed directly at ~10 call sites in `tests/` and `samples/`, and `window::Window` ([src/runtime/window/window.hpp](src/runtime/window/window.hpp)) includes `vulkan/vulkan.h` unconditionally and exposes `getVulkanSurface(VkInstance)`. Both need a factory/abstraction pass.
- `DeviceFeatures_DrawIndirectCount` is already optional, and the virtual-geometry passes carry a fully-implemented no-indirect-count path with separate shader variants ([VirtualGeometryCullingMultipleDispatchesPass.hpp:424-425](src/runtime/virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp#L424-L425)). Metal can ship phase 1 without indirect count and lose nothing.
- No shader in `assets/shaders` requests 64-bit atomics or texture atomics — no `--atomic-u64*` or `--texture-int64-atomic` flag appears in any `.cmake`. The largest single-shader capability gap therefore does not exist.

## Backend selection

Build-time: `ENGINE_RHI_BACKEND` cache variable, values `auto|vulkan|metal|both`, default `auto` → `metal` when `APPLE`, `vulkan` otherwise. `both` is what macOS developers use during the migration. Each enabled backend defines `ENGINE_RHI_METAL` / `ENGINE_RHI_VULKAN` and pulls its own sources and frameworks (`Metal`, `MetalKit`, `QuartzCore`, `Foundation`) or `Vulkan::Vulkan` + MoltenVK.

Run-time: a new `rendering::createRHI(const RHIDesc &)` factory in `src/runtime/rendering/gpu/backend/RHIFactory.hpp`. `RHIDesc` carries what the two constructors need in common (limits, requested features, validation flag, window handle). When both backends are compiled it honours `--rhi metal|vulkan` and `ENGINE_RHI=`, defaulting to Metal. Every `new vulkan::VulkanRHI(...)` in `tests/` and `samples/` becomes `createRHI(...)`.

The window abstraction gains `WindowSurface_Metal` and `void *getMetalLayer()` (a `CAMetalLayer *`), and `window.hpp` moves its `vulkan.h` include behind `ENGINE_RHI_VULKAN`. `SDL3Window` gets the `SDL_Metal_CreateView` path; SDL3 supports it natively.

## Shader toolchain

### `wgslc-naga`

Add `msl-out` to the naga feature list and a `--msl` mode alongside the existing `--hlsl`. Per input it writes three artifacts:

1. `<name>.metal` — MSL source from `naga::back::msl::Writer`.
2. `<name>.metallib` — produced by `xcrun -sdk macosx metal` + `metallib` (the Python driver invokes these; `wgslc-naga` itself stays a pure translator).
3. `<name>.bindings.json` — the piece that makes the runtime work.

The JSON is required, not a convenience, for three reasons: naga **mangles entry point names** (`TranslationInfo::entry_point_names` is the only source of the emitted MSL function name), naga's MSL backend does **not** derive slot assignments — the caller supplies them through `msl::Options::per_entry_point_map`, and runtime-sized arrays need a **sizes buffer** whose slot only the compiler knows. Schema:

```json
{
  "entry_points": [{
    "name": "main",                 // WGSL name, what ComputePipelineInfo::entry holds
    "msl_name": "main_",            // mangled, what newFunctionWithName: takes
    "stage": "compute",
    "workgroup_size": [64, 1, 1],
    "sizes_buffer_slot": 30,
    "bindings": [
      { "group": 0, "binding": 3, "kind": "storage-buffer", "slot": 2, "mutable": true },
      { "group": 1, "binding": 0, "kind": "texture",        "slot": 0 }
    ]
  }]
}
```

Slot assignment is computed in `wgslc-naga` from the module's globals with a stable rule — buffers, textures and samplers each numbered in `(group, binding)` order within their own table — and is deterministic, so a rebuild never silently reshuffles slots under a cached `.metallib`.

### Build integration

`cmake/WGSL2SpirV.cmake` becomes `cmake/WGSLCompile.cmake` exporting `wgsl_shader(NAME SRC DST_BASE ...)`, which fans out to the SPIR-V rule, the MSL rule, or both, according to `ENGINE_RHI_BACKEND`. The preprocessing step (`scripts/wgsl_pp.py`) and its depfile are shared and run once. `wgsl2spirv()` stays as a thin wrapper so the 56 existing call sites can migrate incrementally rather than in one commit; the `DST` argument keeps meaning the SPIR-V path and the MSL artifacts land beside it under `tests/assets/shaders/metal/`.

`scripts/wgsl2msl.py` mirrors `scripts/wgsl2spirv.py`: same locking, same tmp-then-rename, same validity check (`metallib` magic instead of SPIR-V magic), plus the `xcrun metal` invocation. Debug builds pass `-frecord-sources -gline-tables-only` so Xcode shows original MSL in captures; release builds do not.

### Runtime loading

`ShaderType::MetalLib` is added, and passes select their path the same way they already select indirect-count variants — one helper, `rendering::shaderPath("virtualgeometry-culling-multipass")`, returning the `.spirv` or `.metallib` path for the active backend, so individual passes stop hard-coding `assets/shaders/spirv/`. `MetalRHI::createShader` loads the `.metallib` into an `MTLLibrary` and the neighbouring `.bindings.json` into the shader record; `createComputePipeline` looks the entry point up by its **WGSL** name in that map, instantiates `MTLFunction` by the mangled name, and stores the binding table on the pipeline for `cmdBindBindingGroups` to consume.

## RHI → Metal mapping

| RHI concept | Metal |
| --- | --- |
| `CommandBuffer` | `MTLCommandBuffer` + the currently open encoder |
| `Queue::{Graphics,Compute,Transfer}` | one `MTLCommandQueue`; `canonicalQueue()` collapses all three to `Graphics` (the same choice `preferUnifiedQueueExecution` already makes on Apple hardware) |
| `submit` / `GPUFuture` | `commit` + `addCompletedHandler`, wrapped in a `MetalAsyncHandler` for the existing `EventLoop` |
| cross-submit waits | `MTLSharedEvent` with a per-queue monotonic value, mirroring `QueueTimelineState` |
| `SwapChain` | `CAMetalLayer`; `getCurrentSwapChainTextureView` wraps `nextDrawable`, `present` calls `presentDrawable` |
| `cmdBeginRenderPass` | `MTLRenderPassDescriptor` — `LoadOp`/`StoreOp` map 1:1 |
| `GraphicsPipeline` | `MTLRenderPipelineState` + a cached `MTLDepthStencilState` (depth/stencil is separate state in Metal and must be re-applied per encoder) |
| `BindingGroups` | direct `setBuffer:/setTexture:/setSamplerState:` at the slots from the bindings JSON |
| dynamic offsets | `setBufferOffset:atIndex:` |
| `cmdDispatch` | `dispatchThreadgroups:threadsPerThreadgroup:` using the JSON workgroup size |
| `cmdDispatchIndirect` | `dispatchThreadgroupsWithIndirectBuffer:` (argument layout matches Vulkan's) |
| `cmdDrawIndirect` / `cmdDrawIndexedIndirect` | `drawPrimitives:indirectBuffer:` — `MTLDrawIndexedPrimitivesIndirectArguments` matches `VkDrawIndexedIndirectCommand` field for field, including `baseVertex` and `baseInstance` |
| `cmdDrawIndirectCount` | not in phase 1 (feature bit off); ICB in phase 6 |
| `Timer` | `MTLCounterSampleBuffer` (`MTLCounterSetTimestamp`) attached to pass descriptors |
| acceleration structures (P7) | `MTLPrimitiveAccelerationStructure` / `MTLInstanceAccelerationStructure`, built on `MTLAccelerationStructureCommandEncoder`; bound with `setAccelerationStructure:atBufferIndex:` |
| buffer/texture creation | `MTLBuffer`/`MTLTexture`; `BufferUsage_Push` → `MTLStorageModeShared`, device-local → `MTLStorageModePrivate` (`Managed` on Intel Macs) |

## The four problems that decide the schedule

**1. Buffer argument table overflow — the only hard blocker.** Metal allows 31 buffer bindings per stage. Two shaders exceed it today: `gpusigneddistancefields-build.wgsl` (41 buffer bindings) and `gpusigneddistancefields-debug-trace.wgsl` (40); `software-raytrace.wgsl` sits exactly at 31 and the sizes buffer pushes it over. Naga's MSL backend emits resources as flat function arguments and has no argument-buffer mode, so this cannot be waved away by a compiler flag. Three responses, in order of preference:

- Report the budget at build time. `wgslc-naga --msl` fails loudly with the count when a shader exceeds 31 buffers, so the constraint is visible the moment a shader crosses it rather than at runtime on a device.
- For shaders over budget, merge buffers in the WGSL. Most of the SDF shaders' bindings are small parameter blocks that belong in one struct anyway; this is engine work, not backend work, and it also helps Vulkan descriptor pressure.
- If merging proves impractical, generate an argument-buffer wrapper: `wgslc-naga` knows the exact emitted signature, so it can rename naga's `kernel` entry point and emit a wrapper taking a single `device Args &` argument-buffer struct that unpacks into the original call. This is a real workstream, not a footnote — schedule it only if step 2 fails.

The SDF and software-BVH shaders are off the sparse-voxel critical path, so this does not gate the milestone that matters most.

**2. The sizes buffer.** 113 shader files use runtime-sized `array<T>`. Naga's MSL output resolves `arrayLength()` against a buffer of `uint` lengths bound at `sizes_buffer_slot`. `MetalRHI` must build and bind that buffer whenever a binding group is bound — sizes come from the `BufferView` ranges already present in `BindingBuffer`, so the data is available; it needs a small per-bind-group scratch allocator with frame-lifetime recycling. This is required for phase 1, not deferrable.

**3. Barriers.** Metal's default hazard tracking is automatic for non-heap resources, so `cmdBufferBarrier`/`cmdImageBarrier` reduce to encoder-boundary decisions rather than emitted commands. Phase 1 takes the conservative route: a barrier with differing stages ends the open encoder, and Metal's tracking orders the rest. Within one compute encoder, `memoryBarrierWithScope:` covers the dispatch-to-dispatch case without a costly encoder split. Layout transitions have no Metal equivalent and become no-ops. Optimising this (heaps + `MTLFence`, untracked resources) is phase 7 and should be driven by capture data, not guessed at.

**4. Timestamps.** Apple GPUs support counter sampling at *stage* boundaries only — not at draw or dispatch boundaries. The RHI's `cmdStartTimer`/`cmdStopTimer` therefore cannot emit a command mid-encoder; they record intent, and the sample buffer is attached to the enclosing pass descriptor when the encoder opens. Passes that time a sub-range of an encoder will read the whole encoder instead. Because a Metal-native backend is partly *about* getting honest timings, this needs to be flagged in the timer results rather than silently approximated, and it argues for one encoder per render-graph pass — which the barrier strategy above already produces.

## Hardware ray tracing

Metal is the only backend the engine can reach that offers hardware ray traversal — MoltenVK does not implement `VK_KHR_ray_query`, so on Apple hardware this capability is unreachable without the native backend. It is the strongest single argument for the whole plan, and it is why P7 exists.

### It is not a ray tracing pipeline

Metal has no DXR-style ray tracing pipeline: no raygen/miss/closest-hit stages, no shader binding table, no `MTLRayTracingPipelineState`. What Metal 3 provides is *inline* ray tracing — a `metal::raytracing::intersector` instantiated inside an ordinary compute or fragment shader, traversing an acceleration structure and returning the intersection directly. (Intersection function tables exist for procedural geometry, but they are a table of visible functions attached to a compute pipeline, not a separate pipeline type.)

This is good news for the RHI: no new pipeline object, no shader-table lifetime problem, no new command types. The additions are one resource kind and one feature bit. It also means ray tracing slots into the existing pass structure — an RT shadow or GI pass is a compute pass that happens to trace, dispatched by the render graph like any other.

### WGSL authoring works end to end

Verified against the vendored naga 29.0.3, not assumed:

- The WGSL front end accepts `acceleration_structure`, the `ray_query` type, and `rayQueryInitialize` / `rayQueryProceed` / `rayQueryGetCommittedIntersection`, gated behind `Capabilities::RAY_QUERY` in the validator — one more flag next to the `--int64` family already in [wgslc-naga/src/main.rs](wgslc-naga/src/main.rs).
- The MSL backend emits `metal::raytracing::intersector<instancing, triangle_data, world_space_data>` and requires `lang_version >= (2, 4)`, so the MSL target version must be raised for RT shaders.
- An `acceleration_structure` global lands in `BindTarget::buffer` — it consumes a **buffer slot**, counting against the 31-slot budget alongside the sizes buffer. The bindings JSON gains an `acceleration-structure` kind so `MetalRHI` calls `setAccelerationStructure:atBufferIndex:` rather than `setBuffer:`.
- `spv-out` supports ray queries too, so the same WGSL later runs on desktop Vulkan with `VK_KHR_ray_query`. The shaders written here are not Metal-only.

### The trap, and it is a silent one

`RAY_QUERY_MODERN_SUPPORT` is a hardcoded `false` in naga 29.0.3's MSL writer. The consequences are specific and they dictate how RT shaders must be written:

- `rayQueryInitialize` lowers to a **one-shot** `intersector.intersect(...)` with `assume_geometry_type(triangle)` hardcoded. Ray flags map correctly — `TERMINATE_ON_FIRST_HIT` becomes `accept_any_intersection`, the opaque/no-opaque flags become cull and force-opacity modes.
- `rayQueryProceed` returns a stored `ready` flag rather than advancing traversal. The usual `while (rayQueryProceed(&rq)) { ... }` candidate loop **does not terminate** under this lowering. The only correct idiom is initialize → proceed once → `rayQueryGetCommittedIntersection`.
- `rayQueryGenerateIntersection` and `rayQueryConfirmIntersection` emit **nothing at all** — a `log::warn!` on the compiler's stderr and no MSL. Any shader relying on them compiles cleanly and returns wrong answers.

So the supported envelope is committed-only queries against opaque triangles: closest hit, and any-hit through the terminate-on-first-hit flag. Alpha-tested candidate handling and procedural AABB geometry are out until naga is patched. Because the failure mode is silence, `wgslc-naga --msl` must **hard-error** on any module containing `GenerateIntersection` or `ConfirmIntersection` instead of letting naga warn and continue. That check is small and belongs in P0 with the buffer-budget check, even though the rest of RT is much later.

### What feeds the acceleration structures

The scene side already exists and is already GPU-resident, which is most of why this phase is tractable. [SoftwareBLASManager](src/runtime/softwarebvh/SoftwareBLASManager.hpp) maintains triangle BLASes as vertex-position and index buffers with per-BLAS offsets — exactly the inputs `MTLAccelerationStructureTriangleGeometryDescriptor` wants. [SoftwareTLASManager](src/runtime/softwarebvh/SoftwareTLASManager.hpp) maintains instances as 4x4 transforms plus scene instance IDs, which convert to `MTLAccelerationStructureInstanceDescriptor` (a 4x3 transform, a mask, and a BLAS index). The bridge builds Metal structures from the same source data the software builders consume, so the two paths trace the same scene by construction and can be compared frame for frame.

Refit maps too: `SoftwareTLASManager::refit` already has a dirty-instance path, which becomes `refitAccelerationStructure` on the Metal encoder — no full rebuild for a moved object.

### RHI surface

```cpp
enum class AccelerationStructureId : uintptr_t;          // Types.hpp, alongside BufferId
DeviceFeatures_RayQuery = 1 << 22;                       // reported only by MetalRHI, for now

virtual AccelerationStructureId createBlas(const BlasInfo &) = 0;   // geometry buffers + flags
virtual AccelerationStructureId createTlas(const TlasInfo &) = 0;   // instance buffer + BLAS list
virtual void cmdBuildAccelerationStructure(CommandBuffer, AccelerationStructureId, const BufferView &scratch) = 0;
virtual void cmdRefitAccelerationStructure(CommandBuffer, AccelerationStructureId, const BufferView &scratch) = 0;
virtual void deleteAccelerationStructure(AccelerationStructureId) = 0;
```

Plus `BindingGroupLayoutAccelerationStructureEntry` in `BindingGroupLayout` and a matching list in `GroupInfo`, mirroring how storage textures are already handled. The base class default-throws exactly like `textureWrite` does today, so `VulkanRHI` needs no changes until someone wants `VK_KHR_ray_query` on desktop.

Two Metal-specific obligations the render graph must learn: acceleration structures and every buffer they reference need explicit residency (`useResource:`/`useHeap:`) on the encoder that traces, and the build encoder is a third encoder type alongside render and compute, so the barrier logic in P4 has to admit it.

### How it lands in the engine

Not as a replacement. The software CWBVH path stays the cross-platform default and becomes the correctness reference — the same relationship the engine already has with `DeviceFeatures_DrawIndirectCount`, where passes select between two shader variants on a feature bit. RT passes carry a `-rayquery` WGSL variant next to the software one, selected on `DeviceFeatures_RayQuery`, and the acceptance gate is a frame-signature match against the software trace on the same scene.

One caveat worth stating plainly: **this does not accelerate the voxel tracer.** Tree64 and occupancy DDA traversal are not triangle geometry, and reaching them through hardware traversal would need procedural AABB primitives — precisely what naga's MSL ray-query lowering cannot express. Hardware RT applies to the triangle scene: the software-BVH consumers in GI, shadows, and the screen-space trace fallback. Extending it to voxels means patching naga's intersector emission to support `bounding_box_data` and intersection function tables, which is a real project and is not in this plan.

## Phases

Each phase has a gate that is an existing test or sample, so progress is measured by something that already knows what correct looks like.

**P0 — Audit and toolchain skeleton.** `--msl` in `wgslc-naga` behind the existing CLI, `scripts/wgsl2msl.py`, `wgsl_shader()` in CMake, the buffer-budget check, and the ray-query rejection check described above. Gate: every shader in the tree compiles to `.metallib` + `.bindings.json`, or fails with a named budget error. No C++ yet. *This phase alone tells us whether the shader corpus is viable, so it comes first and nothing else starts until it passes.*

**P1 — Selection plumbing.** `ENGINE_RHI_BACKEND`, `createRHI` factory, `WindowSurface_Metal`, `.mm` compilation with ARC, framework links, `shaderPath()` helper. Gate: `both` and `vulkan` builds are byte-identical in behaviour to today; `metal` builds and links against an RHI that throws on every call.

**P2 — Compute path.** Device, buffers, shaders, bindings layouts, binding groups, sizes buffer, compute pipelines, command buffers, dispatch, submit/fence, `bufferRead`/`bufferWrite`. Gate: `ComputeAddTests` and `ComputeSortTests` pass on Metal. *This is the phase where the binding-map design either works or doesn't, and it is reachable in isolation — no swapchain, no render pass.*

**P3 — Graphics path.** Textures, samplers, texture views, render passes, graphics pipelines, depth-stencil state, vertex/index binding, draws, swapchain and present. Gate: `TriangleExample` and `TriangleReverseZ` render correctly on Metal.

**P4 — Render-graph integration.** Barriers as encoder boundaries, queue collapsing, indirect dispatch and indirect draw, timers, `textureWrite`, `cmdCopyBuffer`/`cmdCopyImage` via blit encoders. Gate: `SparseVoxelBinningTests` and the Tree64 GPU test suite pass; the render graph's own barrier dispatchers run unmodified.

**P5 — Sample cutover.** ImGui's Metal backend replaces `imgui_impl_vulkan` behind the same selection switch; `SparseVoxelRendererSample` runs on Metal end to end. Gate: frame signature matches the Vulkan run (`--test-frame-signatures`), which is the strongest correctness check available and the reason the A/B backend was kept.

**P6 — Feature parity.** Indirect command buffers for `cmdDrawIndirectCount`, then flip `DeviceFeatures_DrawIndirectCount` on and drop the fallback shader variants on Metal. Gate: virtual-geometry renderer signature match with indirect count enabled.

**P7 — Hardware ray tracing.** `AccelerationStructureId` and the five RHI entry points, the BLAS/TLAS bridge off the existing software managers, build/refit encoders, residency, the `acceleration-structure` binding kind, `Capabilities::RAY_QUERY` and MSL 2.4 in the compiler. Start with one consumer — the shadow ray path is the smallest and has the clearest reference — before touching GI. Gate: the `-rayquery` variant frame-signature-matches the software CWBVH trace on the same scene, and beats it on time. *Independent of P6 and can run in parallel with it; both depend only on P5.*

**P8 — Xcode and performance.** `MTLCaptureManager` programmatic capture replacing the `MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE` path, encoder/resource debug labels wired from the existing `info.name` fields (every RHI info struct already carries one), `pushDebugGroup` per render-graph pass, `make gputrace` retargeted, `docs/xcode-gpu-profiling.md` rewritten. Then heaps, untracked resources and `MTLFence` where captures show encoder splits cost real time. Gate: Metal frame time at or below the MoltenVK path on the same scene.

**P9 — iOS.** Only after P8. Touch/no-window entry point, memoryless attachments for tile memory, `MTLStorageModeShared` everywhere, feature-set gating. Deliberately last and deliberately unscoped here.

## Risks

- **The 31-buffer limit** (P0) is the one risk that can change the shape of the plan rather than its schedule. It is why P0 produces the full-corpus compile report before any backend code is written.
- **Naga MSL output quality** for this corpus is unproven — subgroup ops, `textureStore`, atomics on storage buffers, and reverse-Z depth all need spot-checking. The SPIR-V path has years of shaking-out behind it; the MSL path has none. P2 and P3 gates are chosen to surface this early on small shaders.
- **Dual-backend drift.** Keeping both alive costs discipline: every new pass must resolve its shader through `shaderPath()`, and the CI build should compile `both` on macOS so a Vulkan-only shader addition fails the build rather than the device.
- **Naga's ray-query lowering** (P7) is the least mature part of the toolchain — a hardcoded `false` and two statements that compile to warnings. The P0 rejection check converts the silent failures into build errors, but it does not make the missing features appear, and any RT work that needs candidate loops or procedural geometry turns into naga patches. Budget for that possibility before committing to an RT consumer that needs alpha testing.
- **Scope creep into the render graph.** If a Metal limitation seems to require changing `RHI.hpp`, that is a signal to re-examine the mapping first. The interface is already backend-neutral; the exceptions this plan accepts in advance are the timer-granularity flag in P4 and the acceleration-structure resource type in P7.

## Out of scope

Removing MoltenVK or the Vulkan backend on macOS; changing WGSL authoring outside the ray-tracing shaders in P7; the HLSL output path; mesh-shader APIs; DXR-style ray tracing pipelines and Metal intersection function tables; hardware traversal of voxel structures; `VK_KHR_ray_query` on the Vulkan backend; the second, older `rhi::Device` hierarchy under [src/runtime/rhi/](src/runtime/rhi/), which only `samples/triangle` still uses and which should be deleted rather than ported.
