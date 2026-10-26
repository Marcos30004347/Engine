// Copy the rasterized brick visibility attachments into the resolver contract.
//
// The draw pass writes its result as render targets so the hardware depth test
// picks the nearest brick per pixel. Writing resolvedSurfaces straight from the
// fragment shader would race between overlapping bricks; reading the attachments
// afterwards is race-free and costs one full-screen pass.
//
// The main attachment carries visibility, primary material and exact identity.
// A compact R32Uint companion carries the bottom RGB resolved from the optional
// eight-bit palette reference. Depth is the only value the hardware insists on
// owning separately. Both colours reach the material G-buffer in the resolved
// surface, so there is no downstream palette dependency.
#include "sparse_voxel_types.wgsl"

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<storage, read_write> resolvedSurfaces : array<ResolvedVoxelSurface>;
@group(0) @binding(2) var<storage, read_write> coverage : array<u32>;
@group(0) @binding(3) var<storage, read_write> linearDepth : array<f32>;
@group(0) @binding(4) var surfaceTexture : texture_2d<u32>;
// The draw pass's per-pixel fragment counter, and where this pass moves it to.
// These bindings and their atomic exchange exist only in the diagnostic
// variant; the shipping resolver has no overdraw resource dependency or
// counter instruction.
#ifdef SPARSE_VOXEL_BRICK_RESOLVE_DIAGNOSTICS
@group(0) @binding(5) var<storage, read_write> overdraw : array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> overdrawResolved : array<u32>;
#endif
@group(0) @binding(7) var bottomAlbedoTexture : texture_2d<u32>;
@group(0) @binding(8) var<storage, read_write> microPayloads : array<u32>;
@group(0) @binding(9) var microPayloadTexture : texture_2d<u32>;
@group(0) @binding(10) var<storage, read_write> materialSampleAddresses :
    array<VoxelMaterialSampleAddress>;
@group(0) @binding(11) var materialSampleAddressTexture : texture_2d<u32>;
@group(0) @binding(12) var<storage, read_write> terrainOwnershipIds : array<u32>;
@group(0) @binding(13) var terrainOwnershipIdTexture : texture_2d<u32>;

const SPARSE_VOXEL_BRICK_SURFACE_SHIFT : u32 = 16u;
// The visibility payload is produced by sparse_voxel_brick_draw.wgsl. Keep
// these two bits local to the resolver rather than including the complete
// micro-geometry implementation in this small copy kernel.
const SPARSE_VOXEL_BRICK_MICRO_PAYLOAD_VALID : u32 = 1u << 28u;
const SPARSE_VOXEL_BRICK_MICRO_PAYLOAD_TERRAIN_SLOPE : u32 = 1u << 31u;
const SPARSE_VOXEL_BRICK_EXACT_MICRO_MATERIAL : u32 = 1u << 31u;

@compute @workgroup_size(8, 8, 1)
fn resolveBrickSurfaces(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    let viewport = uniforms.viewportTiles.xy;
    if (any(pixel >= viewport)) {
        return;
    }
    let pixelIndex = pixel.y * viewport.x + pixel.x;
#ifdef SPARSE_VOXEL_BRICK_RESOLVE_DIAGNOSTICS
    // Draining here publishes this frame's total and arms the counter for the
    // next frame after both draw phases have finished.
    if (pixelIndex < arrayLength(&overdraw) &&
        pixelIndex < arrayLength(&overdrawResolved)) {
        overdrawResolved[pixelIndex] =
            atomicExchange(&overdraw[pixelIndex], 0u);
    }
#endif
    let coordinate = vec2<i32>(pixel);
    let packed = textureLoad(surfaceTexture, coordinate, 0);
    // Only 16..19 are the transient surface tag. Material metadata occupies
    // 20..30 and must not be interpreted as resolver flags.
    let surface =
        (packed.y >> SPARSE_VOXEL_BRICK_SURFACE_SHIFT) & 0x0fu;
    let flags = surface >> 3u;
    if ((flags & SPARSE_VOXEL_SURFACE_HIT) == 0u) {
        resolvedSurfaces[pixelIndex] = ResolvedVoxelSurface(
            0.0, SPARSE_VOXEL_INVALID_INDEX, SPARSE_VOXEL_INVALID_INDEX,
            0u, 0u, 0u, 0u, 0u);
        coverage[pixelIndex] = 0u;
        linearDepth[pixelIndex] = 0.0;
        microPayloads[pixelIndex] = 0u;
        materialSampleAddresses[pixelIndex] =
            VoxelMaterialSampleAddress(vec3<i32>(0), 0u);
        terrainOwnershipIds[pixelIndex] = 0u;
        return;
    }
    let surfaceT = bitcast<f32>(packed.w);
    let microPayload = textureLoad(microPayloadTexture, coordinate, 0).x;
    let exactMicroMaterial = select(
        0u, SPARSE_VOXEL_BRICK_EXACT_MICRO_MATERIAL,
        (microPayload & SPARSE_VOXEL_BRICK_MICRO_PAYLOAD_VALID) != 0u);
    let analyticTerrain = select(
        0u, SPARSE_VOXEL_SURFACE_ANALYTIC_TERRAIN,
        (microPayload & SPARSE_VOXEL_BRICK_MICRO_PAYLOAD_TERRAIN_SLOPE) != 0u &&
        (microPayload & SPARSE_VOXEL_BRICK_MICRO_PAYLOAD_VALID) == 0u);
    // Bricks are built in world space as one implicit instance, so instance 0
    // is the identity transform the G-buffer already applies. Appearance rides
    // in reserved0/reserved1 rather than through a voxel index: the brick
    // hierarchy has its own packed material array and nothing that addresses
    // the scene's.
    //
    // `voxelIndex` is therefore not an index here - the PACKED_MATERIAL flag
    // tells every consumer not to treat it as one - but it still has to be a
    // per-voxel *identity*, because that is what temporal reuse validates
    // history against. See brickSurfaceIdentity() in the draw shader.
    resolvedSurfaces[pixelIndex] = ResolvedVoxelSurface(
        surfaceT,
        0u,
        packed.z,
        surface & 7u,
        flags | SPARSE_VOXEL_SURFACE_PACKED_MATERIAL | analyticTerrain,
        packed.x,
        // Bit 31 in the palette word means that the *parent* material is
        // eligible for micro traversal.  In ResolvedVoxelSurface the same bit
        // means that this particular hit actually resolved an exact micro
        // cell.  Carrying the eligibility bit through made every ordinary GPU
        // terrain voxel masquerade as microgeometry, bypassing the solid-face
        // normal/albedo path and feeding unstable closure normals to shadows.
        // Clear both the transient face nibble and the source eligibility bit;
        // exactMicroMaterial republishes bit 31 only for a real carved hit.
        (packed.y & 0x7ff0ffffu) | exactMicroMaterial,
        // Bottom RGB occupies the low 24 bits; the upper byte carries the
        // normalized terrain context into the material pass.
        textureLoad(bottomAlbedoTexture, coordinate, 0).x);
    coverage[pixelIndex] = 1u;
    linearDepth[pixelIndex] = surfaceT;
    microPayloads[pixelIndex] = microPayload;
    let sampleAddress = textureLoad(
        materialSampleAddressTexture, coordinate, 0);
    materialSampleAddresses[pixelIndex] = VoxelMaterialSampleAddress(
        bitcast<vec3<i32>>(sampleAddress.xyz), sampleAddress.w);
    terrainOwnershipIds[pixelIndex] = textureLoad(
        terrainOwnershipIdTexture, coordinate, 0).x;
}
