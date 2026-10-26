// T0 instrument for the terrain mesh-proxy plan. Rasterizes the resident
// terrain height field as a triangle mesh under the *scene* camera and counts
// fragment invocations per pixel, so the mesh proxy's overdraw can be compared
// against the brick proxy's measured 15.163x on the same framing.
//
// The mesh is drawn from the same per-column height raster the voxel columns
// were built from - not the overview atlas the background planet displaces
// from - because a mesh that is not a superset of the voxel surface produces
// holes rather than seams under the proposed scheme.
//
// Geometry is pulled, not fetched: one instance per resident chunk, six
// vertices per mesh quad. Six rather than an indexed corner grid so a quad can
// be removed by collapsing it, which is how the two culls below are applied
// without a fragment-side `discard` - a discard would still be counted, and
// would push the whole pass to late-Z and corrupt the very number being
// measured.
//
// Both culls exist to make this comparable to the brick raster's 15.163x:
//   - clipmap overlap. Coarse pages extend under the finer levels; the brick
//     packer clips each coarse brick against the finer-owned rectangle, so a
//     mesh that draws the overlap is drawing geometry the incumbent does not.
//   - the 25 km far plane. Terrain is resident out to 48 km, but brick culling
//     is frustum culling, so everything past the far plane is already gone
//     before rasterization and the planet background mesh owns those pixels.
// Without them the probe covered all 2,073,600 pixels against the brick path's
// 1,193,491, which is not the same picture.

struct TerrainMeshProbeUniforms {
    worldToClip : mat4x4<f32>,
    // xy viewport, z counting enabled, w chunk count.
    viewportCounting : vec4<u32>,
    // x halo raster side, y halo width, z columns per chunk side, w vertex
    // stride in columns.
    rasterLayout : vec4<u32>,
    // x world Y bias (the set elevation origin), y far plane in metres,
    // z extra envelope lift in voxels, w unused.
    heightBiasFar : vec4<f32>,
    // x samples written per chunk, y quad step between samples, zw unused.
    sampleLayout : vec4<u32>,
    // Camera basis for the contract variant, in the brick raster's own terms:
    // surfaceT has to be measured along the ray the G-buffer reconstructs for
    // this pixel, not along the ray through the fragment's own surface point.
    cameraToWorld : mat4x4<f32>,
    cameraWorldPositionNear : vec4<f32>,
    focalPrincipal : vec4<f32>,
}

// One resident chunk. Placement is metric and independent of the packed brick
// arena, so a page can move slots without moving its mesh.
struct TerrainMeshProbeChunk {
    // xy world centre, z chunk edge, w column size, all metres.
    centreEdgeVoxel : vec4<f32>,
    // World rectangle owned by finer clipmap levels: minX, minZ, maxX, maxZ.
    // Empty (min >= max) on the finest level.
    finerBounds : vec4<f32>,
    // x first float of this chunk's height raster, y clipmap LOD,
    // z elevation origin in metres (bitcast), w first word of its material
    // raster.
    rasterOffsetLod : vec4<u32>,
}

@group(0) @binding(0) var<uniform> uniforms : TerrainMeshProbeUniforms;
@group(0) @binding(1) var<storage, read> chunks : array<TerrainMeshProbeChunk>;
@group(0) @binding(2) var<storage, read> heights : array<f32>;
@group(0) @binding(3) var<storage, read_write> overdraw : array<atomic<u32>>;
// T1's gate: the heights the shader reads, published so the CPU can compare
// them against what it uploaded. Written from a compute entry rather than
// from the vertex stage, which cannot store to buffers on this device -
// `vertexPipelineStoresAndAtomics` is not among the enabled features and the
// writes are silently dropped.
@group(0) @binding(4) var<storage, read_write> probeSamples : array<f32>;
// Two packed words per interior column: the appearance the column's top voxel
// carries. Indexed without a halo, so a fragment reads the page it belongs to.
@group(0) @binding(5) var<storage, read> columnMaterials : array<u32>;
@group(0) @binding(6) var<storage, read> envelope : array<f32>;

struct TerrainMeshProbeVertexOutput {
    @builtin(position) position : vec4<f32>,
#ifdef SPARSE_VOXEL_TERRAIN_MESH_CONTRACT
    // Flat, because all three vertices of a triangle compute them identically
    // and the fragment needs the plane, not a per-vertex sample of it.
    @location(0) @interpolate(flat) planeNormal : vec3<f32>,
    @location(1) @interpolate(flat) planePoint : vec3<f32>,
    @location(2) @interpolate(flat) chunkIndex : u32,
#endif
}

#ifdef SPARSE_VOXEL_TERRAIN_MESH_CONTRACT
#include "sparse_voxel_types.wgsl"

const TERRAIN_MESH_SURFACE_SHIFT : u32 = 16u;
const TERRAIN_MESH_TRANSIENT_MASK : u32 = 0x0fu << TERRAIN_MESH_SURFACE_SHIFT;
// Where the build parked the micro context byte, in bits the palette metadata
// would otherwise occupy.
const TERRAIN_MESH_CONTEXT_SHIFT : u32 = 20u;

struct TerrainMeshFragmentOutput {
    @builtin(frag_depth) depth : f32,
    @location(0) surface : vec4<u32>,
    @location(1) bottomAlbedo : u32,
    @location(2) microPayload : u32,
    @location(3) materialSampleAddress : vec4<u32>,
}

// The world cell this surface point falls in, at the page's own voxel size.
// Deliberately a function of world position alone: anything derived from the
// mesh - a triangle index, a quad, a tessellation level - changes identity
// whenever the mesh is retessellated, and temporal reuse validates history
// against this value. A page that changes clipmap level does change identity,
// which is correct: it is a different surface.
fn terrainMeshSurfaceIdentity(cell : vec3<i32>, voxelSize : f32) -> u32 {
    var hash = bitcast<u32>(cell.x) * 0x9e3779b9u;
    hash = (hash ^ bitcast<u32>(cell.y)) * 0x85ebca6bu;
    hash = (hash ^ bitcast<u32>(cell.z)) * 0xc2b2ae35u;
    hash = (hash ^ bitcast<u32>(voxelSize)) * 0x27d4eb2fu;
    return hash ^ (hash >> 16u);
}

fn terrainMeshCameraRayDirection(fragmentPosition : vec4<f32>) -> vec3<f32> {
    let viewX = (fragmentPosition.x - uniforms.focalPrincipal.z) /
        uniforms.focalPrincipal.x;
    let viewY = (fragmentPosition.y - uniforms.focalPrincipal.w) /
        uniforms.focalPrincipal.y;
    return normalize(
        uniforms.cameraToWorld[0].xyz * viewX +
        uniforms.cameraToWorld[1].xyz * viewY +
        uniforms.cameraToWorld[2].xyz);
}
#endif

fn terrainMeshProbeRasterIndex(
    chunk : TerrainMeshProbeChunk, column : vec2<i32>) -> u32 {
    let side = i32(uniforms.rasterLayout.x);
    let halo = i32(uniforms.rasterLayout.y);
    let clamped = clamp(column + vec2<i32>(halo), vec2<i32>(0), vec2<i32>(side - 1));
    return chunk.rasterOffsetLod.x + u32(clamped.y * side + clamped.x);
}

fn terrainMeshProbeHeight(chunk : TerrainMeshProbeChunk, column : vec2<i32>) -> f32 {
    let index = terrainMeshProbeRasterIndex(chunk, column);
    if (index >= arrayLength(&heights)) {
        return 0.0;
    }
    return heights[index];
}

#ifdef SPARSE_VOXEL_TERRAIN_MESH_CONTRACT
// Capped in metres as well as scaled by the page's voxel, because a lift meant
// only to break a depth tie must not move a 200 m coarse page by 400 m and
// change which terrain is visible at all - that would confound the measurement
// it exists to make.
fn terrainMeshProbeLift(chunk : TerrainMeshProbeChunk) -> f32 {
    return min(
        uniforms.heightBiasFar.z * chunk.centreEdgeVoxel.w,
        uniforms.heightBiasFar.w);
}

// The conservative envelope: the largest voxel top face in the column's 3x3
// neighbourhood. Displacing by this rather than by the height field is what
// makes the mesh a strict superset of the voxel surface, measured at T0 as
// 0 of 2,278,400 columns outside against 100% for the height field itself.
fn terrainMeshProbeEnvelope(
    chunk : TerrainMeshProbeChunk, column : vec2<i32>) -> f32 {
    let index = terrainMeshProbeRasterIndex(chunk, column);
    if (index >= arrayLength(&envelope)) {
        return 0.0;
    }
    return envelope[index];
}
#endif

@vertex
fn drawTerrainMeshProbeVertex(
    @builtin(vertex_index) vertexIndex : u32,
    @builtin(instance_index) instanceIndex : u32)
    -> TerrainMeshProbeVertexOutput {
    var output : TerrainMeshProbeVertexOutput;
    output.position = vec4<f32>(0.0);
    if (instanceIndex >= uniforms.viewportCounting.w) {
        return output;
    }
    let chunk = chunks[instanceIndex];
    let columns = uniforms.rasterLayout.z;
    let stride = max(uniforms.rasterLayout.w, 1u);
    let quadsPerSide = columns / stride;
    let quad = vertexIndex / 6u;
    if (quad >= quadsPerSide * quadsPerSide) {
        return output;
    }
    let quadIndex = vec2<u32>(quad % quadsPerSide, quad / quadsPerSide);
    // Two triangles per quad: (0,0) (1,0) (0,1) and (0,1) (1,0) (1,1).
    let vertex = vertexIndex % 6u;
    let step = vec2<u32>(
        select(0u, 1u, vertex == 1u || vertex == 4u || vertex == 5u),
        select(0u, 1u, vertex == 2u || vertex == 3u || vertex == 5u));
    // The far corner row lands on column `columns`, which is the neighbouring
    // page's first column. Its height comes from the halo, so two pages at the
    // same clipmap level meet on one shared value rather than on two
    // independently sampled ones.
    let column = vec2<i32>(
        min((quadIndex + step) * stride, vec2<u32>(columns)));

    let voxelSize = chunk.centreEdgeVoxel.w;
    let minimum = chunk.centreEdgeVoxel.xy - vec2<f32>(chunk.centreEdgeVoxel.z * 0.5);
    // Heights are defined at column centres, so the mesh is the dual grid of
    // the voxel columns and overhangs each page edge by half a column.
    let horizontal = minimum + (vec2<f32>(column) + vec2<f32>(0.5)) * voxelSize;
#ifdef SPARSE_VOXEL_TERRAIN_MESH_CONTRACT
    // The lift exists to separate two publishers that would otherwise tie. On
    // flat ground the envelope equals the column's own voxel top, so the mesh
    // and the terrain bricks - which are both still drawn at T2 - are coplanar,
    // and which of them wins a pixel can change frame to frame. They publish
    // different identities, so an alternating pixel loses its history every
    // frame. Lifting the mesh keeps containment (it only moves further outside
    // the voxel solid) while making the winner deterministic.
    let sampled = terrainMeshProbeEnvelope(chunk, column) +
        terrainMeshProbeLift(chunk);
#else
    let sampled = terrainMeshProbeHeight(chunk, column);
#endif
    let height = sampled - uniforms.heightBiasFar.x;
    let world = vec3<f32>(horizontal.x, height, horizontal.y);

    // Quad-granular culls, decided identically by all six vertices so a
    // rejected quad collapses to a point instead of leaving a torn triangle.
    let quadCentreColumn = vec2<f32>(quadIndex * stride) + vec2<f32>(f32(stride) * 0.5);
    let quadCentre = minimum + (quadCentreColumn + vec2<f32>(0.5)) * voxelSize;
    if (quadCentre.x >= chunk.finerBounds.x && quadCentre.x <= chunk.finerBounds.z &&
        quadCentre.y >= chunk.finerBounds.y && quadCentre.y <= chunk.finerBounds.w) {
        return output;
    }
    let centreHeight = terrainMeshProbeHeight(
        chunk, vec2<i32>(quadIndex * stride + stride / 2u)) -
        uniforms.heightBiasFar.x;
    let centreClip = uniforms.worldToClip *
        vec4<f32>(quadCentre.x, centreHeight, quadCentre.y, 1.0);
    // w is view depth for this projection. A quad whose centre is a full quad
    // diagonal past the far plane cannot survive frustum culling, which is
    // what removes the resident terrain beyond 25 km from the brick path.
    let quadDiagonal = f32(stride) * voxelSize * 1.5;
    if (centreClip.w > uniforms.heightBiasFar.y + quadDiagonal) {
        return output;
    }

    let clip = uniforms.worldToClip * vec4<f32>(world, 1.0);
    // Same two corrections the brick proxy raster applies: this renderer's
    // clip +Y is the top of the image while the rasterizer takes NDC +Y as
    // downward, and the 25 km far plane is smaller than the resident terrain
    // footprint, so depth clipping would delete the far pages outright.
    let clippedZ = select(clip.z, clamp(clip.z, 0.0, clip.w), clip.w > 0.0);
    output.position = vec4<f32>(clip.x, -clip.y, clippedZ, clip.w);
#ifdef SPARSE_VOXEL_TERRAIN_MESH_CONTRACT
    // The whole triangle, recomputed identically by each of its three
    // vertices. The fragment needs the plane rather than its own interpolated
    // point: surfaceT has to be measured along the pixel's camera ray, and
    // that ray does not pass through the interpolated position once jitter is
    // involved.
    let firstTriangle = vertex < 3u;
    let cornerA = vec2<i32>(min(quadIndex * stride, vec2<u32>(columns)));
    let cornerB = vec2<i32>(min(
        (quadIndex + vec2<u32>(1u, 0u)) * stride, vec2<u32>(columns)));
    let cornerC = vec2<i32>(min(
        (quadIndex + vec2<u32>(0u, 1u)) * stride, vec2<u32>(columns)));
    let cornerD = vec2<i32>(min(
        (quadIndex + vec2<u32>(1u, 1u)) * stride, vec2<u32>(columns)));
    let triangleColumns = array<vec2<i32>, 3>(
        select(cornerC, cornerA, firstTriangle),
        select(cornerB, cornerB, firstTriangle),
        select(cornerD, cornerC, firstTriangle));
    var positions : array<vec3<f32>, 3>;
    for (var corner = 0u; corner < 3u; corner = corner + 1u) {
        let cornerColumn = triangleColumns[corner];
        let cornerHorizontal =
            minimum + (vec2<f32>(cornerColumn) + vec2<f32>(0.5)) * voxelSize;
        positions[corner] = vec3<f32>(
            cornerHorizontal.x,
            terrainMeshProbeEnvelope(chunk, cornerColumn) +
                terrainMeshProbeLift(chunk) - uniforms.heightBiasFar.x,
            cornerHorizontal.y);
    }
    let planeNormal = cross(
        positions[1] - positions[0], positions[2] - positions[0]);
    // Terrain is a height field, so the outward normal is the +Y one. A
    // degenerate triangle (a flat quad's two identical corners) falls back to
    // straight up rather than to a zero vector the fragment would divide by.
    let orientedNormal = select(-planeNormal, planeNormal, planeNormal.y > 0.0);
    output.planeNormal = select(
        vec3<f32>(0.0, 1.0, 0.0), normalize(orientedNormal),
        dot(orientedNormal, orientedNormal) > 1.0e-20);
    output.planePoint = positions[0];
    output.chunkIndex = instanceIndex;
#endif
    return output;
}

// One invocation per published sample. It reaches the raster through the same
// terrainMeshProbeHeight() the vertex stage uses, so what the gate compares is
// the addressing the mesh is actually built from, not a second copy of it.
@compute @workgroup_size(64)
fn sampleTerrainMeshProbeHeights(
    @builtin(global_invocation_id) id : vec3<u32>) {
    let sampleCount = uniforms.sampleLayout.x;
    let sampleStep = max(uniforms.sampleLayout.y, 1u);
    if (sampleCount == 0u) {
        return;
    }
    let chunkIndex = id.x / sampleCount;
    let sampleIndex = id.x % sampleCount;
    if (chunkIndex >= uniforms.viewportCounting.w) {
        return;
    }
    let columns = uniforms.rasterLayout.z;
    let stride = max(uniforms.rasterLayout.w, 1u);
    let quadsPerSide = columns / stride;
    let quad = sampleIndex * sampleStep;
    if (quad >= quadsPerSide * quadsPerSide) {
        return;
    }
    let column = vec2<i32>(vec2<u32>(
        (quad % quadsPerSide) * stride, (quad / quadsPerSide) * stride));
    probeSamples[id.x] = terrainMeshProbeHeight(chunks[chunkIndex], column);
}

#ifdef SPARSE_VOXEL_TERRAIN_MESH_CONTRACT
// Drop-in producer of what SparseVoxelBrickResolvePass publishes. It writes the
// same four attachments the brick raster writes, into the same targets, so the
// existing resolver copies them into the resolver contract unchanged and every
// downstream consumer - material G-buffer, probes, SVGF, TAA, shadows - is
// untouched. The two paths compose through the shared depth buffer, which is
// also how non-terrain brick geometry keeps working while this draws.
//
// T2 deliberately publishes the *smooth* envelope with no voxel carve, so an
// integration failure here is a contract bug and not a march bug.
@fragment
fn drawTerrainMeshContractFragment(
    input : TerrainMeshProbeVertexOutput) -> TerrainMeshFragmentOutput {
    var output : TerrainMeshFragmentOutput;
    let origin = uniforms.cameraWorldPositionNear.xyz;
    let direction = terrainMeshCameraRayDirection(input.position);
    let denominator = dot(input.planeNormal, direction);
    // A ray parallel to the plane has no defined intersection; there is no
    // surface to publish for this pixel and the brick path behind it, if any,
    // keeps the pixel.
    if (abs(denominator) < 1.0e-9) {
        discard;
    }
    let nearDistance = max(uniforms.cameraWorldPositionNear.w, 1.0e-4);
    let surfaceT =
        dot(input.planeNormal, input.planePoint - origin) / denominator;
    if (surfaceT < nearDistance) {
        discard;
    }
    let world = origin + direction * surfaceT;

    // Same depth construction as the brick raster: two rows of the projection,
    // clamped rather than rejected, because discarding out-of-range depth reads
    // as the far plane having moved in.
    let clipZ = dot(
        vec4<f32>(
            uniforms.worldToClip[0].z, uniforms.worldToClip[1].z,
            uniforms.worldToClip[2].z, uniforms.worldToClip[3].z),
        vec4<f32>(world, 1.0));
    let clipW = dot(
        vec4<f32>(
            uniforms.worldToClip[0].w, uniforms.worldToClip[1].w,
            uniforms.worldToClip[2].w, uniforms.worldToClip[3].w),
        vec4<f32>(world, 1.0));
    if (clipW <= 0.0) {
        discard;
    }
    let depth = clipZ / clipW;
    if (!(depth == depth)) {
        discard;
    }
    output.depth = clamp(depth, 0.0, 1.0);

    let chunk = chunks[input.chunkIndex];
    let voxelSize = chunk.centreEdgeVoxel.w;
    let columns = uniforms.rasterLayout.z;
    let minimum =
        chunk.centreEdgeVoxel.xy - vec2<f32>(chunk.centreEdgeVoxel.z * 0.5);
    // The column this surface point belongs to, not the one its triangle
    // corner came from: appearance follows the voxel column, so it stays put
    // when the mesh is retessellated.
    let columnIndex = vec2<u32>(clamp(
        vec2<i32>(floor((world.xz - minimum) / voxelSize)),
        vec2<i32>(0), vec2<i32>(i32(columns) - 1)));
    let materialAddress = chunk.rasterOffsetLod.w +
        2u * (columnIndex.y * columns + columnIndex.x);
    var materialX = 0u;
    var materialY = 0u;
    if (materialAddress + 1u < arrayLength(&columnMaterials)) {
        materialX = columnMaterials[materialAddress];
        materialY = columnMaterials[materialAddress + 1u];
    }
    let context = (materialY >> TERRAIN_MESH_CONTEXT_SHIFT) & 0xffu;

    // Face in the brick raster's own packing: axis in the low bits, bit 2 set
    // when the ray entered through the positive face. A smooth mesh has no cube
    // face, so this is the plane's dominant axis - which is what T3's slab test
    // will produce once the carve is in.
    let absoluteNormal = abs(input.planeNormal);
    var axis = 0u;
    if (absoluteNormal.y >= absoluteNormal.x &&
        absoluteNormal.y >= absoluteNormal.z) {
        axis = 1u;
    } else if (absoluteNormal.z >= absoluteNormal.x) {
        axis = 2u;
    }
    let face = axis | select(0u, 4u, direction[axis] < 0.0);

    // Identity hashes the world voxel cell, never anything the tessellation
    // produced: if it flickered with the mesh, temporal reuse would throw away
    // history on every retessellation and the image would boil under motion.
    let cell = vec3<i32>(floor(world / voxelSize));
    output.surface = vec4<u32>(
        materialX,
        (materialY & ~TERRAIN_MESH_TRANSIENT_MASK) |
            ((face | (SPARSE_VOXEL_SURFACE_HIT << 3u)) <<
                TERRAIN_MESH_SURFACE_SHIFT),
        terrainMeshSurfaceIdentity(cell, voxelSize),
        bitcast<u32>(surfaceT));
    output.bottomAlbedo =
        (materialX & SPARSE_VOXEL_RESOLVED_BOTTOM_ALBEDO_MASK) |
        (context << SPARSE_VOXEL_RESOLVED_CONTEXT_SHIFT);
    // No exact micro geometry on this path: the height field is the geometry.
    // Zero is what the resolver reads as "legacy parent hit", which is the
    // behaviour every downstream consumer already handles.
    output.microPayload = 0u;
    output.materialSampleAddress = vec4<u32>(0u);
    return output;
}
#else
@fragment
fn drawTerrainMeshProbeFragment(
    input : TerrainMeshProbeVertexOutput) -> @location(0) vec4<f32> {
    // Counted before the depth test, exactly as the brick raster's overdraw
    // view counts: the question is how much work the rasterizer was asked to
    // do per pixel. Early-Z rejection happens before this shader runs, so both
    // numerators are "invocations that survived early-Z".
    if (uniforms.viewportCounting.z != 0u) {
        let viewport = max(uniforms.viewportCounting.xy, vec2<u32>(1u));
        let pixel = vec2<u32>(input.position.xy);
        if (all(pixel < viewport)) {
            let pixelIndex = pixel.y * viewport.x + pixel.x;
            if (pixelIndex < arrayLength(&overdraw)) {
                atomicAdd(&overdraw[pixelIndex], 1u);
            }
        }
    }
    return vec4<f32>(1.0, 1.0, 1.0, 1.0);
}
#endif
