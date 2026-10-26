struct BuildUniforms {
    instanceCount : u32,
    cellCount : u32,
    maxRefJobs : u32,
    pageRefEnabled : u32,
    hugeInstanceCellThreshold : u32,
    maxCellsPerRef : u32,
    _padding0 : u32,
    _padding1 : u32,
}

struct SDFSceneGridInfoRW {
    worldBoundsMin : vec4<f32>,
    worldBoundsMaxAndResolution : vec4<f32>,
    cellCount : u32,
    cellCapacity : u32,
    refCapacity : u32,
    globalRefCount : atomic<u32>,
    settings : vec4<u32>,
}

struct SDFSceneGridCellRW {
    offset : u32,
    count : atomic<u32>,
    flags : u32,
    padding : u32,
}

struct SDFSceneGridRef {
    boundsMin : vec4<f32>,
    boundsMax : vec4<f32>,
    kindInstanceAndPage : vec4<u32>,
}

struct SDFInstance {
    localToWorld : mat4x4<f32>,
    worldToLocal : mat4x4<f32>,
    worldBoundsMin : vec4<f32>,
    worldBoundsMaxAndConservativeScale : vec4<f32>,
    idsAndBlas : vec4<u32>,
    materialAndFlags : vec4<u32>,
}

struct SDFBlas {
    localBoundsMin : vec4<f32>,
    localBoundsMaxAndRootVoxelSize : vec4<f32>,
    offsetsAndFormat : vec4<u32>,
    countsAndNarrowBand : vec4<u32>,
}

const TRACE_GRID_RESOLUTION : u32 = 64u;
const TRACE_ROOT_RESOLUTION : u32 = 8u;
const TRACE_ROOT_CELL_COUNT : u32 = TRACE_ROOT_RESOLUTION * TRACE_ROOT_RESOLUTION * TRACE_ROOT_RESOLUTION;
const TRACE_PAGE_RESOLUTION : u32 = 8u;
const TRACE_INVALID_BRICK : u32 = 0xffffffffu;
const SCENE_GRID_REF_INSTANCE : u32 = 1u;
const SCENE_GRID_REF_PAGE : u32 = 2u;

@group(0) @binding(0) var<uniform> uniforms : BuildUniforms;
@group(0) @binding(1) var<storage, read_write> sceneGridInfo : array<SDFSceneGridInfoRW>;
@group(0) @binding(2) var<storage, read_write> sceneGridCells : array<SDFSceneGridCellRW>;
@group(0) @binding(3) var<storage, read_write> sceneGridRefs : array<SDFSceneGridRef>;
@group(0) @binding(4) var<storage, read_write> sceneGridGlobalRefs : array<SDFSceneGridRef>;
@group(0) @binding(5) var<storage, read> instances : array<SDFInstance>;
@group(0) @binding(6) var<storage, read> blases : array<SDFBlas>;
@group(0) @binding(7) var<storage, read> tracePageTables : array<u32>;

fn transform_point(m: mat4x4<f32>, p: vec3<f32>) -> vec3<f32> {
    return (m * vec4<f32>(p, 1.0)).xyz;
}

fn transform_bounds(localMin: vec3<f32>, localMax: vec3<f32>, m: mat4x4<f32>) -> SDFSceneGridRef {
    var outRef : SDFSceneGridRef;
    var bmin = vec3<f32>(1.0e30);
    var bmax = vec3<f32>(-1.0e30);
    for (var i = 0u; i < 8u; i = i + 1u) {
        let p = vec3<f32>(
            select(localMin.x, localMax.x, (i & 1u) != 0u),
            select(localMin.y, localMax.y, (i & 2u) != 0u),
            select(localMin.z, localMax.z, (i & 4u) != 0u));
        let wp = transform_point(m, p);
        bmin = min(bmin, wp);
        bmax = max(bmax, wp);
    }
    outRef.boundsMin = vec4<f32>(bmin, 0.0);
    outRef.boundsMax = vec4<f32>(bmax, 0.0);
    outRef.kindInstanceAndPage = vec4<u32>(0u);
    return outRef;
}

fn trace_root_page_coord(rootPageFlat: u32) -> vec3<u32> {
    return vec3<u32>(
        rootPageFlat % TRACE_ROOT_RESOLUTION,
        (rootPageFlat / TRACE_ROOT_RESOLUTION) % TRACE_ROOT_RESOLUTION,
        rootPageFlat / (TRACE_ROOT_RESOLUTION * TRACE_ROOT_RESOLUTION));
}

fn page_local_bounds(blas: SDFBlas, rootPageFlat: u32) -> SDFSceneGridRef {
    let rootCoord = trace_root_page_coord(rootPageFlat);
    let localMin = blas.localBoundsMin.xyz;
    let localMax = blas.localBoundsMaxAndRootVoxelSize.xyz;
    let cellSize = (localMax - localMin) / vec3<f32>(f32(TRACE_GRID_RESOLUTION));
    let pageMin = localMin + vec3<f32>(rootCoord * vec3<u32>(TRACE_PAGE_RESOLUTION)) * cellSize;
    let pageMax = pageMin + vec3<f32>(f32(TRACE_PAGE_RESOLUTION)) * cellSize;
    var outRef : SDFSceneGridRef;
    outRef.boundsMin = vec4<f32>(pageMin, 0.0);
    outRef.boundsMax = vec4<f32>(pageMax, 0.0);
    outRef.kindInstanceAndPage = vec4<u32>(0u);
    return outRef;
}

fn flatten_grid_cell(cell: vec3<u32>, resolution: u32) -> u32 {
    return cell.x + cell.y * resolution + cell.z * resolution * resolution;
}

fn cell_range_for_bounds(boundsMin: vec3<f32>, boundsMax: vec3<f32>, gridMin: vec3<f32>, gridMax: vec3<f32>, resolution: u32) -> vec4<u32> {
    let extent = max(gridMax - gridMin, vec3<f32>(1.0e-8));
    let scaledMin = floor((boundsMin - gridMin) / extent * f32(resolution));
    let scaledMax = floor((boundsMax - gridMin) / extent * f32(resolution));
    let lo = vec3<u32>(clamp(scaledMin, vec3<f32>(0.0), vec3<f32>(f32(resolution - 1u))));
    let hi = vec3<u32>(clamp(scaledMax, vec3<f32>(0.0), vec3<f32>(f32(resolution - 1u))));
    return vec4<u32>(lo.x, lo.y, lo.z, (hi.x << 20u) | (hi.y << 10u) | hi.z);
}

fn range_hi(range: vec4<u32>) -> vec3<u32> {
    return vec3<u32>((range.w >> 20u) & 0x3ffu, (range.w >> 10u) & 0x3ffu, range.w & 0x3ffu);
}

fn range_cell_count(range: vec4<u32>) -> u32 {
    let hi = range_hi(range);
    return (hi.x - range.x + 1u) * (hi.y - range.y + 1u) * (hi.z - range.z + 1u);
}

fn append_global_ref(gridRef: SDFSceneGridRef) {
    if (arrayLength(&sceneGridInfo) == 0u) {
        return;
    }
    let slot = atomicAdd(&sceneGridInfo[0u].globalRefCount, 1u);
    if (slot < arrayLength(&sceneGridGlobalRefs)) {
        sceneGridGlobalRefs[slot] = gridRef;
    }
}

fn emit_ref_to_grid(gridRef: SDFSceneGridRef) {
    if (arrayLength(&sceneGridInfo) == 0u) {
        return;
    }
    let resolution = u32(sceneGridInfo[0u].worldBoundsMaxAndResolution.w);
    let cellCapacity = sceneGridInfo[0u].cellCapacity;
    if (resolution == 0u || cellCapacity == 0u) {
        return;
    }

    let range = cell_range_for_bounds(
        gridRef.boundsMin.xyz,
        gridRef.boundsMax.xyz,
        sceneGridInfo[0u].worldBoundsMin.xyz,
        sceneGridInfo[0u].worldBoundsMaxAndResolution.xyz,
        resolution);
    if (range_cell_count(range) > max(uniforms.maxCellsPerRef, 1u)) {
        append_global_ref(gridRef);
        return;
    }

    let hi = range_hi(range);
    var overflowed = false;
    for (var z = range.z; z <= hi.z; z = z + 1u) {
        for (var y = range.y; y <= hi.y; y = y + 1u) {
            for (var x = range.x; x <= hi.x; x = x + 1u) {
                let cellIndex = flatten_grid_cell(vec3<u32>(x, y, z), resolution);
                if (cellIndex >= arrayLength(&sceneGridCells)) {
                    continue;
                }
                let slot = atomicAdd(&sceneGridCells[cellIndex].count, 1u);
                if (slot < cellCapacity) {
                    let refIndex = sceneGridCells[cellIndex].offset + slot;
                    if (refIndex < arrayLength(&sceneGridRefs)) {
                        sceneGridRefs[refIndex] = gridRef;
                    }
                } else {
                    sceneGridCells[cellIndex].flags = 1u;
                    overflowed = true;
                }
            }
        }
    }

    if (overflowed) {
        append_global_ref(gridRef);
    }
}

fn instance_cell_count(instance: SDFInstance) -> u32 {
    if (arrayLength(&sceneGridInfo) == 0u) {
        return 0u;
    }
    let range = cell_range_for_bounds(
        instance.worldBoundsMin.xyz,
        instance.worldBoundsMaxAndConservativeScale.xyz,
        sceneGridInfo[0u].worldBoundsMin.xyz,
        sceneGridInfo[0u].worldBoundsMaxAndResolution.xyz,
        max(u32(sceneGridInfo[0u].worldBoundsMaxAndResolution.w), 1u));
    return range_cell_count(range);
}

@compute @workgroup_size(64)
fn clear_scene_grid(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index == 0u && arrayLength(&sceneGridInfo) > 0u) {
        atomicStore(&sceneGridInfo[0u].globalRefCount, 0u);
    }
    if (index >= uniforms.cellCount || index >= arrayLength(&sceneGridCells)) {
        return;
    }
    atomicStore(&sceneGridCells[index].count, 0u);
    sceneGridCells[index].flags = 0u;
}

@compute @workgroup_size(64)
fn emit_scene_grid_refs(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let jobIndex = globalId.x;
    let instanceIndex = jobIndex / TRACE_ROOT_CELL_COUNT;
    let rootPageFlat = jobIndex - instanceIndex * TRACE_ROOT_CELL_COUNT;
    if (instanceIndex >= arrayLength(&instances)) {
        return;
    }

    let instance = instances[instanceIndex];
    let blasIndex = instance.idsAndBlas.y;
    if (blasIndex >= arrayLength(&blases)) {
        return;
    }

    let blas = blases[blasIndex];
    let usePageRefs = uniforms.pageRefEnabled != 0u &&
        instance_cell_count(instance) > max(uniforms.hugeInstanceCellThreshold, 1u);
    if (rootPageFlat == 0u) {
        var gridRef : SDFSceneGridRef;
        gridRef.boundsMin = vec4<f32>(instance.worldBoundsMin.xyz, 0.0);
        gridRef.boundsMax = vec4<f32>(instance.worldBoundsMaxAndConservativeScale.xyz, 0.0);
        gridRef.kindInstanceAndPage = vec4<u32>(SCENE_GRID_REF_INSTANCE, instanceIndex, 0xffffffffu, 0u);
        emit_ref_to_grid(gridRef);
    }
    if (!usePageRefs) {
        return;
    }

    let pageTableIndex = blasIndex * TRACE_ROOT_CELL_COUNT + rootPageFlat;
    if (pageTableIndex >= arrayLength(&tracePageTables) || tracePageTables[pageTableIndex] == TRACE_INVALID_BRICK) {
        return;
    }
    var pageRef = page_local_bounds(blas, rootPageFlat);
    pageRef = transform_bounds(pageRef.boundsMin.xyz, pageRef.boundsMax.xyz, instance.localToWorld);
    pageRef.kindInstanceAndPage = vec4<u32>(SCENE_GRID_REF_PAGE, instanceIndex, rootPageFlat, 0u);
    emit_ref_to_grid(pageRef);
}
