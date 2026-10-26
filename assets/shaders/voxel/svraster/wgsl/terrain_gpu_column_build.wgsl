#define MAP_EXPORT_BIND_GROUP 0
#define MAP_EXPORT_METADATA_BINDING 0
#define MAP_EXPORT_PAGE_TABLE_BINDING 1
#define MAP_EXPORT_PHYSICAL_BINDING 2
#include "map_export_pages.wgsl"

const INVALID_FIELD : u32 = 0xffffffffu;
const STATUS_BUILDING : u32 = 1u;
const STATUS_READY : u32 = 2u;
const STATUS_MISSING_PAGE : u32 = 4u;
const STATUS_VERTICAL_OVERFLOW : u32 = 5u;
const STATUS_VALIDATION_FAILED : u32 = 6u;
const EXTERIOR_SHELL : u32 = 1u;
const RESOLVE_PAGE_DATUM : u32 = 2u;
const COLUMN_EXTERIOR_SHELL : u32 = 1u;
const COLUMN_BOUNDARY_STITCHED : u32 = 2u;
const HEIGHT_UNITS_PER_VOXEL : u32 = 16383u;

const FIELD_ELEVATION : u32 = 0u;
const FIELD_BASE_ELEVATION : u32 = 1u;
const FIELD_UPLIFT : u32 = 2u;
const FIELD_RELIEF : u32 = 3u;
const FIELD_LANDFORM : u32 = 4u;
const FIELD_WATER_TYPE : u32 = 5u;
const FIELD_WATER_LEVEL : u32 = 6u;
const FIELD_RIVER : u32 = 7u;
const FIELD_COAST : u32 = 8u;
const FIELD_EROSION : u32 = 9u;
const FIELD_DEPOSITION : u32 = 10u;
const FIELD_SUBSTRATE : u32 = 11u;
const FIELD_SOIL_DEPTH : u32 = 12u;
const FIELD_BIOME_IDS : u32 = 13u;
const FIELD_BIOME_WEIGHTS : u32 = 14u;
const FIELD_CLIMATE : u32 = 15u;
const FIELD_PLACEMENT : u32 = 16u;
const FIELD_SOIL_LAYERS : u32 = 17u;
const FIELD_ROADS : u32 = 18u;
const FIELD_SETTLEMENT : u32 = 19u;

const CONTEXT_INTERIOR : u32 = 0u;
const CONTEXT_FLAT : u32 = 1u;
const CONTEXT_NEGATIVE_X : u32 = 2u;
const CONTEXT_POSITIVE_X : u32 = 3u;
const CONTEXT_NEGATIVE_Z : u32 = 4u;
const CONTEXT_POSITIVE_Z : u32 = 5u;

struct TerrainColumnConfig {
    side : u32,
    halo : u32,
    maximumVerticalVoxels : u32,
    requestedLevel : u32,
    internalLOD : u32,
    boundaryStitchSubdivisions : u32,
    surfaceShellDepthVoxels : u32,
    flags : u32,
    expectedTerrainGeneration : u32,
    generatorRevisionLow : u32,
    generatorRevisionHigh : u32,
    expectedAtlasRevisionLow : u32,
    expectedAtlasRevisionHigh : u32,
    seedLow : u32,
    seedHigh : u32,
    bundleSignatureLow : u32,
    bundleSignatureHigh : u32,
    minimumU : f32,
    minimumV : f32,
    maximumU : f32,
    maximumV : f32,
    extentMetres : f32,
    edgeMetres : f32,
    voxelSizeMetres : f32,
    finestVoxelSizeMetres : f32,
    heightUnitMetres : f32,
    elevationOriginMetres : f32,
    proceduralDetailStrength : f32,
    proceduralTerraceStrength : f32,
    proceduralTerraceHeightMetres : f32,
    proceduralTerraceTransitionFraction : f32,
    fields : array<u32, 20>,
    foundationDepthVoxels : u32,
    reserved : array<u32, 4>,
}

struct TerrainColumn { topY : u32, firstSolidY : u32,
    packedMaterialContext : u32, packedFlags : u32, }
struct TerrainWall { firstSolidYWithoutBoundaryStitch : u32,
    lowestNeighbourY : u32, packedContexts : u32, reserved : u32, }
struct RasterColumn { column : TerrainColumn, wall : TerrainWall, }
struct Height64 { low : u32, high : u32, }
struct TerrainColumnResult {
    status : u32,
    validationErrors : atomic<u32>,
    missingSamples : atomic<u32>,
    verticalOverflows : atomic<u32>,
    columnCount : u32,
    cornerCount : u32,
    boundaryCount : u32,
    terrainGeneration : u32,
    atlasRevisionLow : u32,
    atlasRevisionHigh : u32,
    generatorRevisionLow : u32,
    generatorRevisionHigh : u32,
    semanticHashLow : u32,
    semanticHashHigh : u32,
    elevationOriginBits : atomic<u32>,
    occupiedVoxelCount : atomic<u32>,
}
struct StageUniform { stage : u32, reserved0 : u32,
    reserved1 : u32, reserved2 : u32, }

@group(0) @binding(3) var<storage, read> config : TerrainColumnConfig;
@group(0) @binding(4) var<storage, read_write> cornerHeights : array<f32>;
@group(0) @binding(5) var<storage, read_write> materials : array<u32>;
@group(0) @binding(6) var<storage, read_write> tops : array<u32>;
@group(0) @binding(7) var<storage, read_write> columns : array<TerrainColumn>;
@group(0) @binding(8) var<storage, read_write> walls : array<TerrainWall>;
@group(0) @binding(9) var<storage, read_write> rasterColumns : array<RasterColumn>;
@group(0) @binding(10) var<storage, read_write> canonicalCorners : array<Height64>;
@group(0) @binding(11) var<storage, read_write> boundaryMinimums : array<Height64>;
@group(0) @binding(12) var<storage, read_write> boundaryMaximums : array<Height64>;
@group(0) @binding(13) var<storage, read_write> result : TerrainColumnResult;
@group(0) @binding(14) var<uniform> stageUniform : StageUniform;

fn hash32(value : u32) -> u32 {
    var x = value;
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}
fn lattice_noise(cell : vec2<i32>, salt : u32) -> f32 {
    let key = u32(cell.x) * 0x9e3779b9u ^ u32(cell.y) * 0x85ebca6bu ^
        config.seedLow ^ config.bundleSignatureHigh ^ salt;
    return f32(hash32(key) & 0x00ffffffu) / 8388607.5 - 1.0;
}
fn smooth_noise(position : vec2<f32>, wavelength : f32, salt : u32) -> f32 {
    let p = position / wavelength;
    let cell = vec2<i32>(floor(p));
    var f = fract(p); f = f * f * (vec2<f32>(3.0) - 2.0 * f);
    return mix(mix(lattice_noise(cell, salt),
                   lattice_noise(cell + vec2<i32>(1, 0), salt), f.x),
               mix(lattice_noise(cell + vec2<i32>(0, 1), salt),
                   lattice_noise(cell + vec2<i32>(1, 1), salt), f.x), f.y);
}
fn optional_sample(field : u32, uv : vec2<f32>, fallback : vec4<f32>) -> vec4<f32> {
    if (field == INVALID_FIELD) { return fallback; }
    let sampled = map_export_sample(field, uv, config.requestedLevel);
    if (sampled.valid == 0u) { atomicAdd(&result.missingSamples, 1u); return fallback; }
    return sampled.value;
}
fn required_sample(field : u32, uv : vec2<f32>) -> vec4<f32> {
    let sampled = map_export_sample(field, uv, config.requestedLevel);
    if (sampled.valid == 0u) { atomicAdd(&result.missingSamples, 1u); }
    return sampled.value;
}
fn surface_height(uv : vec2<f32>) -> f32 {
    let waterType = u32(round(required_sample(config.fields[FIELD_WATER_TYPE], uv).x));
    if (waterType != 0u) {
        return required_sample(config.fields[FIELD_WATER_LEVEL], uv).x;
    }
    let elevation = required_sample(config.fields[FIELD_ELEVATION], uv).x;
    let river = required_sample(config.fields[FIELD_RIVER], uv);
    let erosion = optional_sample(config.fields[FIELD_EROSION], uv, vec4<f32>(0.0));
    let deposition = optional_sample(config.fields[FIELD_DEPOSITION], uv, vec4<f32>(0.0)).x;
    let uplift = optional_sample(config.fields[FIELD_UPLIFT], uv, vec4<f32>(0.0)).x;
    let relief = optional_sample(config.fields[FIELD_RELIEF], uv, vec4<f32>(0.0));
    let coast = optional_sample(config.fields[FIELD_COAST], uv, vec4<f32>(-1.0)).x;
    let landform = u32(round(optional_sample(config.fields[FIELD_LANDFORM], uv,
        vec4<f32>(4.0, 0.0, 0.0, 0.0)).x));
    let roads = optional_sample(config.fields[FIELD_ROADS], uv, vec4<f32>(0.0)).x;
    let settlement = optional_sample(config.fields[FIELD_SETTLEMENT], uv, vec4<f32>(0.0)).x;
    let discharge = max(river.y, 0.0);
    let halfWidth = clamp(5.0 + sqrt(discharge) * 8.0, 5.0, 120.0);
    let channelT = clamp(1.0 - river.x / halfWidth, 0.0, 1.0);
    let incision = channelT * channelT * (3.0 - 2.0 * channelT) *
        clamp(log(1.0 + discharge) * 1.8 + clamp(erosion.y, 0.0, 1.0) * 4.0,
              0.0, 12.0);
    let authored = elevation - incision;
    if (config.proceduralDetailStrength <= 0.0) { return authored; }
    let world = uv * config.extentMetres;
    var character = 1.0;
    if (landform == 3u) { character = 0.25; }
    if (landform == 5u) { character = 1.4; }
    if (landform == 6u || landform == 12u) { character = 0.8; }
    if (landform == 13u) { character = 1.1; }
    let coastMask = select(clamp(1.0 - abs(coast) / 320.0, 0.0, 1.0), 0.0, coast < 0.0);
    let signal = config.proceduralDetailStrength *
        (1.0 + clamp(abs(uplift) / 0.001, 0.0, 1.0) * 0.35) *
        (1.0 - clamp(erosion.x, 0.0, 1.0) * 0.15) *
        (1.0 - clamp(erosion.y, 0.0, 1.0) * 0.45) *
        (1.0 - clamp(deposition, 0.0, 1.0) * 0.35) *
        (1.0 - coastMask * 0.65);
    var detail = (smooth_noise(world, 720.0, 3u) * 11.0 +
        smooth_noise(world, 220.0, 5u) * 4.0 +
        smooth_noise(world, 48.0, 6u) * 1.4 +
        smooth_noise(world, 14.0, 7u) * 0.32) *
        signal * character * clamp(relief.x / 100.0 + relief.y, 0.1, 4.0);
    let riverShoulder = max(180.0, config.proceduralTerraceHeightMetres * 16.0);
    let riverSmooth = clamp(1.0 - river.x / riverShoulder, 0.0, 1.0);
    let infrastructure = max(select(0.0, 1.0, roads > 0.0),
        clamp(select(settlement, settlement / 255.0, settlement > 1.0), 0.0, 1.0));
    detail *= 1.0 - max(infrastructure, riverSmooth * riverSmooth * (3.0 - 2.0 * riverSmooth) * 0.9);
    let detailed = authored + detail;
    if (config.proceduralTerraceStrength <= 0.0) { return detailed; }
    let base = optional_sample(config.fields[FIELD_BASE_ELEVATION], uv,
        vec4<f32>(elevation, 0.0, 0.0, 0.0)).x;
    let scaled = base / config.proceduralTerraceHeightMetres;
    let lower = floor(scaled);
    let phase = fract(scaled);
    let transition = config.proceduralTerraceTransitionFraction;
    let t = select(0.0, clamp((phase - (1.0 - transition * 2.0)) /
        (transition * 2.0), 0.0, 1.0), transition > 0.0);
    let terraced = (lower + t * t * (3.0 - 2.0 * t)) *
        config.proceduralTerraceHeightMetres + (authored - base) + detail * 0.35;
    let corridor = max(infrastructure, riverSmooth);
    let landformWeight = select(0.0, 1.0,
        landform == 6u || landform == 12u || landform == 13u);
    let blend = clamp(config.proceduralTerraceStrength * landformWeight *
        (1.0 - corridor), 0.0, 1.0);
    return mix(detailed, terraced, blend);
}
fn biome_material(biome : u32) -> u32 {
    if (biome == 0u) { return 0u; } if (biome == 1u) { return 1u; }
    if (biome == 2u) { return 2u; } if (biome == 3u) { return 8u; }
    if (biome == 4u || biome == 5u || biome == 24u) { return 9u; }
    if (biome == 7u || biome == 10u) { return 12u; }
    if (biome == 8u) { return 16u; } if (biome == 9u) { return 15u; }
    if (biome == 16u || biome == 17u || biome == 18u) { return 18u; }
    if (biome == 15u) { return 19u; }
    if (biome == 19u || biome == 20u || biome == 6u) { return 20u; }
    if (biome == 11u || biome == 13u || biome == 14u || biome == 12u) { return 13u; }
    if (biome == 21u || biome == 22u || biome == 23u) { return 7u; }
    if (biome == 25u) { return 4u; } if (biome == 12u) { return 17u; }
    return 3u;
}
fn select_material(uv : vec2<f32>, column : vec2<u32>) -> u32 {
    let water = u32(round(required_sample(config.fields[FIELD_WATER_TYPE], uv).x));
    if (water == 1u) { return 0u; } if (water == 2u) { return 1u; }
    let mixture = map_export_sample_sparse_mixture(config.fields[FIELD_BIOME_IDS],
        config.fields[FIELD_BIOME_WEIGHTS], uv, config.requestedLevel);
    if (mixture.valid == 0u) { atomicAdd(&result.missingSamples, 1u); return 3u; }
    let random = f32(hash32(column.x * 0x9e3779b9u ^ column.y * 0x85ebca6bu ^
        config.seedHigh) & 0x00ffffffu) / 16777216.0;
    var biome = mixture.ids.x; var accumulated = 0.0;
    for (var i = 0u; i < mixture.count; i++) {
        accumulated += mixture.weights[i];
        if (random < accumulated) { biome = mixture.ids[i]; break; }
    }
    var material = biome_material(biome);
    let substrate = u32(round(required_sample(config.fields[FIELD_SUBSTRATE], uv).x));
    let soil = required_sample(config.fields[FIELD_SOIL_DEPTH], uv).x;
    let placement = optional_sample(config.fields[FIELD_PLACEMENT], uv, vec4<f32>(0.0));
    let layers = optional_sample(config.fields[FIELD_SOIL_LAYERS], uv, vec4<f32>(0.0));
    let climate = optional_sample(config.fields[FIELD_CLIMATE], uv, vec4<f32>(20.0, 0.5, 0.0, 0.0));
    let river = required_sample(config.fields[FIELD_RIVER], uv);
    if (soil < 0.15 || substrate == 1u || substrate == 2u ||
        (placement.w >= 0.58 && soil < 0.5) || layers.x >= 0.62) { material = 4u; }
    if (climate.x <= -2.0 && climate.y >= 0.12) { material = 5u; }
    let width = select(0.0, clamp(5.0 + sqrt(max(river.y, 0.0)) * 8.0,
        max(2.0, config.voxelSizeMetres * 0.75), 120.0), river.y > 0.0);
    if (layers.z + layers.w >= 0.48 && climate.y >= 0.55 &&
        river.x <= max(width * 3.0, 30.0)) { material = 7u; }
    if (river.y > 0.0 && river.x <= width) { material = 6u; }
    return material;
}
fn quantized_height(height : f32) -> Height64 {
    let value = i32(round(height / config.heightUnitMetres));
    return Height64(bitcast<u32>(value), select(0u, 0xffffffffu, value < 0));
}
fn resolved_elevation_origin() -> f32 {
    return bitcast<f32>(atomicLoad(&result.elevationOriginBits));
}
fn relative_units(height : f32) -> i32 {
    return i32(round((height - resolved_elevation_origin()) /
        config.voxelSizeMetres * f32(HEIGHT_UNITS_PER_VOXEL)));
}
fn ordered_float(value : f32) -> u32 {
    let bits = bitcast<u32>(value);
    return select(bits ^ 0x80000000u, ~bits,
        (bits & 0x80000000u) != 0u);
}
fn ordered_to_float(value : u32) -> f32 {
    let bits = select(value ^ 0x80000000u, ~value,
        (value & 0x80000000u) == 0u);
    return bitcast<f32>(bits);
}
fn owned_top(height : f32) -> u32 {
    let relative = max(relative_units(height), 0);
    if (relative == 0) { return 0u; }
    return (u32(relative) + HEIGHT_UNITS_PER_VOXEL - 1u) /
        HEIGHT_UNITS_PER_VOXEL - 1u;
}
fn floor_cell(height : f32) -> u32 {
    return u32(max(relative_units(height), 0)) / HEIGHT_UNITS_PER_VOXEL;
}
fn corner_index(x : i32, z : i32) -> u32 {
    let cornerSide = config.side + config.halo * 2u + 1u;
    return u32(z + i32(config.halo)) * cornerSide + u32(x + i32(config.halo));
}
fn top_index(x : i32, z : i32) -> u32 {
    let topSide = config.side + config.halo * 2u;
    return u32(z + i32(config.halo)) * topSide + u32(x + i32(config.halo));
}
fn local_uv(x : f32, z : f32) -> vec2<f32> {
    let local = vec2<f32>(x, z) / f32(config.side);
    return mix(vec2<f32>(config.minimumU, config.minimumV),
        vec2<f32>(config.maximumU, config.maximumV), local);
}

fn reset_stage() {
    result.status = STATUS_BUILDING; atomicStore(&result.validationErrors, 0u);
    atomicStore(&result.missingSamples, 0u); atomicStore(&result.verticalOverflows, 0u);
    result.columnCount = config.side * config.side;
    result.cornerCount = (config.side + 1u) * (config.side + 1u);
    result.boundaryCount = 4u * config.side;
    result.terrainGeneration = config.expectedTerrainGeneration;
    result.atlasRevisionLow = config.expectedAtlasRevisionLow;
    result.atlasRevisionHigh = config.expectedAtlasRevisionHigh;
    result.generatorRevisionLow = config.generatorRevisionLow;
    result.generatorRevisionHigh = config.generatorRevisionHigh;
    result.semanticHashLow = 0u; result.semanticHashHigh = 0u;
    atomicStore(&result.elevationOriginBits, select(
        bitcast<u32>(config.elevationOriginMetres), 0xffffffffu,
        (config.flags & RESOLVE_PAGE_DATUM) != 0u));
    atomicStore(&result.occupiedVoxelCount, 0u);
}
fn sample_stage(worker : u32) {
    let cornerSide = config.side + config.halo * 2u + 1u;
    for (var index = worker; index < cornerSide * cornerSide; index += 64u) {
        let x = i32(index % cornerSide) - i32(config.halo);
        let z = i32(index / cornerSide) - i32(config.halo);
        let height = surface_height(local_uv(f32(x), f32(z)));
        cornerHeights[index] = height;
        if ((config.flags & RESOLVE_PAGE_DATUM) != 0u) {
            atomicMin(&result.elevationOriginBits, ordered_float(height));
        }
    }
    for (var index = worker; index < config.side * config.side; index += 64u) {
        let x = index % config.side; let z = index / config.side;
        materials[index] = select_material(local_uv(f32(x) + 0.5, f32(z) + 0.5),
            vec2<u32>(x, z));
    }
}
fn resolve_datum_stage() {
    if ((config.flags & RESOLVE_PAGE_DATUM) == 0u) { return; }
    let minimumHeight = ordered_to_float(
        atomicLoad(&result.elevationOriginBits));
    // Every page datum must lie on that page's voxel lattice. Snapping only
    // to the finest spacing lets a coarse page start at a fractional coarse
    // voxel; adjacent/parent pages then describe different world-space face
    // planes and visibly overlap or crack during replacement.
    let origin = floor(minimumHeight / config.voxelSizeMetres) *
        config.voxelSizeMetres - f32(config.foundationDepthVoxels) *
        config.voxelSizeMetres;
    atomicStore(&result.elevationOriginBits, bitcast<u32>(origin));
}
fn top_stage(worker : u32) {
    let topSide = config.side + config.halo * 2u;
    for (var index = worker; index < topSide * topSide; index += 64u) {
        let x = i32(index % topSide) - i32(config.halo);
        let z = i32(index / topSide) - i32(config.halo);
        let height = max(max(cornerHeights[corner_index(x, z)],
            cornerHeights[corner_index(x + 1, z)]), max(
            cornerHeights[corner_index(x, z + 1)],
            cornerHeights[corner_index(x + 1, z + 1)]));
        let top = owned_top(height); tops[index] = min(top, config.maximumVerticalVoxels - 1u);
        if (top >= config.maximumVerticalVoxels) { atomicAdd(&result.verticalOverflows, 1u); }
    }
    let canonicalSide = config.side + 1u;
    for (var index = worker; index < canonicalSide * canonicalSide; index += 64u) {
        let x = i32(index % canonicalSide); let z = i32(index / canonicalSide);
        canonicalCorners[index] = quantized_height(cornerHeights[corner_index(x, z)]);
    }
    for (var index = worker; index < 4u * config.side; index += 64u) {
        let face = index / config.side; let segment = index % config.side;
        var a = 0.0; var b = 0.0;
        if (face == 0u) { a = cornerHeights[corner_index(0, i32(segment))]; b = cornerHeights[corner_index(0, i32(segment + 1u))]; }
        if (face == 1u) { a = cornerHeights[corner_index(i32(config.side), i32(segment))]; b = cornerHeights[corner_index(i32(config.side), i32(segment + 1u))]; }
        if (face == 2u) { a = cornerHeights[corner_index(i32(segment), 0)]; b = cornerHeights[corner_index(i32(segment + 1u), 0)]; }
        if (face == 3u) { a = cornerHeights[corner_index(i32(segment), i32(config.side))]; b = cornerHeights[corner_index(i32(segment + 1u), i32(config.side))]; }
        var minimum = min(a, b); var maximum = max(a, b);
        for (var subdivision = 1u; subdivision < config.boundaryStitchSubdivisions; subdivision++) {
            let along = f32(segment) + f32(subdivision) / f32(config.boundaryStitchSubdivisions);
            var uv = local_uv(0.0, along);
            if (face == 1u) { uv = local_uv(f32(config.side), along); }
            if (face == 2u) { uv = local_uv(along, 0.0); }
            if (face == 3u) { uv = local_uv(along, f32(config.side)); }
            let sampled = surface_height(uv); minimum = min(minimum, sampled); maximum = max(maximum, sampled);
        }
        boundaryMinimums[index] = quantized_height(minimum);
        boundaryMaximums[index] = quantized_height(maximum);
    }
    workgroupBarrier();
    for (var index = worker; index < config.side * config.side; index += 64u) {
        let x = index % config.side; let z = index / config.side;
        var maximum = -3.402823466e+38;
        if (x == 0u) { maximum = max(maximum, f32(bitcast<i32>(boundaryMaximums[z].low)) * config.heightUnitMetres); }
        if (x + 1u == config.side) { maximum = max(maximum, f32(bitcast<i32>(boundaryMaximums[config.side + z].low)) * config.heightUnitMetres); }
        if (z == 0u) { maximum = max(maximum, f32(bitcast<i32>(boundaryMaximums[2u * config.side + x].low)) * config.heightUnitMetres); }
        if (z + 1u == config.side) { maximum = max(maximum, f32(bitcast<i32>(boundaryMaximums[3u * config.side + x].low)) * config.heightUnitMetres); }
        if (maximum > -3.0e+38) {
            let boundaryTop = owned_top(maximum);
            if (boundaryTop >= config.maximumVerticalVoxels) { atomicAdd(&result.verticalOverflows, 1u); }
            tops[top_index(i32(x), i32(z))] = max(tops[top_index(i32(x), i32(z))],
                min(boundaryTop, config.maximumVerticalVoxels - 1u));
        }
    }
}
fn column_stage(worker : u32) {
    for (var index = worker; index < config.side * config.side; index += 64u) {
        let x = i32(index % config.side); let z = i32(index / config.side);
        let top = tops[top_index(x, z)];
        var threshold = top; var wallContext = CONTEXT_INTERIOR;
        let nx = tops[top_index(x - 1, z)]; if (nx < threshold) { threshold = nx; wallContext = CONTEXT_NEGATIVE_X; }
        let px = tops[top_index(x + 1, z)]; if (px < threshold) { threshold = px; wallContext = CONTEXT_POSITIVE_X; }
        let nz = tops[top_index(x, z - 1)]; if (nz < threshold) { threshold = nz; wallContext = CONTEXT_NEGATIVE_Z; }
        let pz = tops[top_index(x, z + 1)]; if (pz < threshold) { threshold = pz; wallContext = CONTEXT_POSITIVE_Z; }
        var first = 0u;
        if ((config.flags & EXTERIOR_SHELL) != 0u) {
            let exposed = select(top, threshold + 1u, threshold < top);
            first = select(top, select(0u, exposed - config.surfaceShellDepthVoxels,
                exposed > config.surfaceShellDepthVoxels), threshold < top);
            let minimumCorner = min(min(cornerHeights[corner_index(x, z)], cornerHeights[corner_index(x + 1, z)]),
                min(cornerHeights[corner_index(x, z + 1)], cornerHeights[corner_index(x + 1, z + 1)]));
            first = min(first, min(floor_cell(minimumCorner), config.maximumVerticalVoxels - 1u));
        } else if (config.surfaceShellDepthVoxels != 0u && top > config.surfaceShellDepthVoxels) {
            first = min(top - config.surfaceShellDepthVoxels, threshold);
        }
        let unstitched = first;
        if ((config.flags & EXTERIOR_SHELL) != 0u && config.boundaryStitchSubdivisions > 1u) {
            var minimum = 3.402823466e+38;
            if (x == 0) { minimum = min(minimum, f32(bitcast<i32>(boundaryMinimums[u32(z)].low)) * config.heightUnitMetres); }
            if (x + 1 == i32(config.side)) { minimum = min(minimum, f32(bitcast<i32>(boundaryMinimums[config.side + u32(z)].low)) * config.heightUnitMetres); }
            if (z == 0) { minimum = min(minimum, f32(bitcast<i32>(boundaryMinimums[2u * config.side + u32(x)].low)) * config.heightUnitMetres); }
            if (z + 1 == i32(config.side)) { minimum = min(minimum, f32(bitcast<i32>(boundaryMinimums[3u * config.side + u32(x)].low)) * config.heightUnitMetres); }
            if (minimum < 3.0e+38) {
                let boundaryTop = min(floor_cell(minimum), config.maximumVerticalVoxels - 1u);
                let stitchFloor = select(0u, boundaryTop - config.surfaceShellDepthVoxels,
                    boundaryTop > config.surfaceShellDepthVoxels);
                first = min(first, stitchFloor);
            }
        }
        var boundaryContext = CONTEXT_INTERIOR;
        if (x == 0) { boundaryContext = CONTEXT_NEGATIVE_X; }
        else if (x + 1 == i32(config.side)) { boundaryContext = CONTEXT_POSITIVE_X; }
        else if (z == 0) { boundaryContext = CONTEXT_NEGATIVE_Z; }
        else if (z + 1 == i32(config.side)) { boundaryContext = CONTEXT_POSITIVE_Z; }
        var packedFlags = select(0u, COLUMN_EXTERIOR_SHELL, (config.flags & EXTERIOR_SHELL) != 0u);
        if (first < unstitched) { packedFlags |= COLUMN_BOUNDARY_STITCHED; }
        let column = TerrainColumn(top, first, (materials[index] & 0xffu) | (CONTEXT_FLAT << 8u), packedFlags);
        let wall = TerrainWall(unstitched, threshold, wallContext | (boundaryContext << 8u), 0u);
        columns[index] = column; walls[index] = wall; rasterColumns[index] = RasterColumn(column, wall);
        atomicAdd(&result.occupiedVoxelCount, top - first + 1u);
    }
}
fn validate_stage() {
    if (atomicLoad(&result.missingSamples) != 0u) { result.status = STATUS_MISSING_PAGE; return; }
    if (atomicLoad(&result.verticalOverflows) != 0u) { result.status = STATUS_VERTICAL_OVERFLOW; return; }
    var low = 2166136261u; var high = 2166136261u ^ 0x9e3779b9u;
    for (var index = 0u; index < config.side * config.side; index++) {
        let column = columns[index]; let wall = walls[index];
        if (column.firstSolidY > column.topY || wall.firstSolidYWithoutBoundaryStitch < column.firstSolidY ||
            wall.firstSolidYWithoutBoundaryStitch > column.topY || wall.lowestNeighbourY > column.topY) {
            atomicAdd(&result.validationErrors, 1u);
        }
        low = (low ^ column.topY) * 16777619u; high = (high ^ column.firstSolidY ^ low) * 16777619u;
        low = (low ^ column.packedMaterialContext) * 16777619u; high = (high ^ wall.packedContexts ^ low) * 16777619u;
    }
    result.semanticHashLow = low; result.semanticHashHigh = high;
    result.status = select(STATUS_READY, STATUS_VALIDATION_FAILED,
        atomicLoad(&result.validationErrors) != 0u);
}

@compute @workgroup_size(64)
fn runTerrainColumnStage(@builtin(local_invocation_index) worker : u32,
    @builtin(workgroup_id) group : vec3<u32>) {
    if (group.x != 0u || group.y != 0u || group.z != 0u) { return; }
    if (stageUniform.stage == 0u && worker == 0u) { reset_stage(); }
    else if (stageUniform.stage == 1u) { sample_stage(worker); }
    else if (stageUniform.stage == 2u && worker == 0u) { resolve_datum_stage(); }
    else if (stageUniform.stage == 3u) { top_stage(worker); }
    else if (stageUniform.stage == 4u) { column_stage(worker); }
    else if (stageUniform.stage == 5u && worker == 0u) { validate_stage(); }
}
