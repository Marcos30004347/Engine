#define MAP_EXPORT_BIND_GROUP 0
#define MAP_EXPORT_METADATA_BINDING 0
#define MAP_EXPORT_PAGE_TABLE_BINDING 1
#define MAP_EXPORT_PHYSICAL_BINDING 2
#include "map_export_pages.wgsl"

#ifndef TERRAIN_V2_COLUMN_WORKGROUP_X
#define TERRAIN_V2_COLUMN_WORKGROUP_X 8
#endif
#ifndef TERRAIN_V2_COLUMN_WORKGROUP_Z
#define TERRAIN_V2_COLUMN_WORKGROUP_Z 8
#endif

const STATUS_BUILDING : u32 = 1u;
const STATUS_READY : u32 = 2u;
const STATUS_MISSING_PAGE : u32 = 3u;
const STATUS_QUANTIZATION_OVERFLOW : u32 = 5u;
const COLUMN_VALID : u32 = 1u;
const COLUMN_WATER : u32 = 2u;
const INVALID_FIELD : u32 = 0xffffffffu;

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

struct ColumnConfig {
    columnsX : u32,
    columnsZ : u32,
    halo : u32,
    requestedMapLevel : u32,
    tileMinimumCanonicalX : i32,
    tileMinimumCanonicalZ : i32,
    worldOriginCanonicalX : i32,
    worldOriginCanonicalZ : i32,
    canonicalStep : u32,
    worldExtentCanonical : u32,
    heightUnitMetres : f32,
    expectedTerrainGeneration : u32,
    expectedAtlasRevisionLow : u32,
    expectedAtlasRevisionHigh : u32,
    generatorRevision : u32,
    faultCapacity : u32,
    fields : array<u32, 20>,
    seedLow : u32,
    seedHigh : u32,
    bundleSignatureLow : u32,
    bundleSignatureHigh : u32,
    proceduralDetailStrength : f32,
    proceduralTerraceStrength : f32,
    proceduralTerraceHeightMetres : f32,
    proceduralTerraceTransitionFraction : f32,
    reserved : array<u32, 4>,
}

struct HeightSample {
    heightUnits : i32,
    canonicalX : i32,
    canonicalZ : i32,
    flags : u32,
}

struct Column {
    minimumHeightUnits : i32,
    maximumHeightUnits : i32,
    material : u32,
    flags : u32,
}

struct SurfaceSample {
    heightUnits : i32,
    flags : u32,
    heightMetres : f32,
}

struct MapFault {
    field : u32,
    level : u32,
    pageX : u32,
    pageZ : u32,
}

struct ColumnResult {
    status : u32,
    missingSamples : atomic<u32>,
    faultCount : atomic<u32>,
    quantizationOverflows : atomic<u32>,
    heightSampleCount : u32,
    columnCount : u32,
    terrainGeneration : u32,
    writtenColumns : atomic<u32>,
    atlasRevisionLow : u32,
    atlasRevisionHigh : u32,
    generatorRevision : u32,
    selectedWorkgroup : u32,
    minimumHeightUnits : atomic<i32>,
    maximumHeightUnits : atomic<i32>,
    reserved2 : u32,
    reserved3 : u32,
}

@group(0) @binding(3) var<storage, read> configBuffer : array<ColumnConfig>;
@group(0) @binding(4) var<storage, read_write> heightSamples : array<HeightSample>;
@group(0) @binding(5) var<storage, read_write> materialScratch : array<u32>;
@group(0) @binding(6) var<storage, read_write> columns : array<Column>;
@group(0) @binding(7) var<storage, read_write> faults : array<MapFault>;
@group(0) @binding(8) var<storage, read_write> results : array<ColumnResult>;
// City ground (CityGroundOverlay::packGPU); a disabled header when no city.
@group(0) @binding(9) var<storage, read> cityGroundWords : array<u32>;
#include "city_ground_overlay.wgsl"

// Map metres of a canonical lattice position: the frame the city overlay and
// the CPU lane's samples use.
fn canonical_world(uv : vec2<f32>) -> vec2<f32> {
    return uv * f32(config().worldExtentCanonical) * config().heightUnitMetres;
}

var<private> laneIndex : u32;

fn config() -> ColumnConfig { return configBuffer[laneIndex]; }

fn sample_base() -> u32 {
    return laneIndex * config().reserved[1];
}

fn column_base() -> u32 {
    return laneIndex * config().reserved[2];
}

fn fault_base() -> u32 {
    return laneIndex * config().reserved[3];
}

fn sample_dimensions() -> vec2<u32> {
    return vec2<u32>(config().columnsX + config().halo * 2u + 1u,
                     config().columnsZ + config().halo * 2u + 1u);
}

fn column_dimensions() -> vec2<u32> {
    return vec2<u32>(config().columnsX + config().halo * 2u,
                     config().columnsZ + config().halo * 2u);
}

fn canonical_uv(x : f32, z : f32) -> vec2<f32> {
    return vec2<f32>(
        (x - f32(config().worldOriginCanonicalX)) /
            f32(config().worldExtentCanonical),
        (z - f32(config().worldOriginCanonicalZ)) /
            f32(config().worldExtentCanonical));
}

fn record_fault(field : u32, uv : vec2<f32>) {
    atomicAdd(&results[laneIndex].missingSamples, 1u);
    let index = atomicAdd(&results[laneIndex].faultCount, 1u);
    if (index >= config().faultCapacity) { return; }
    let level = min(config().requestedMapLevel, map_export_maximum_level());
    let dimension = 1u << level;
    let page = min(vec2<u32>(clamp(uv, vec2<f32>(0.0),
        vec2<f32>(1.0)) * f32(dimension)), vec2<u32>(dimension - 1u));
    faults[fault_base() + index] = MapFault(field, level, page.x, page.y);
}

fn required_sample(field : u32, uv : vec2<f32>) -> MapExportSample {
    let sampled = map_export_sample(field, uv, config().requestedMapLevel);
    if (sampled.valid == 0u) { record_fault(field, uv); }
    return sampled;
}

fn optional_value(field : u32, uv : vec2<f32>, fallback : vec4<f32>) -> vec4<f32> {
    if (field == INVALID_FIELD) { return fallback; }
    let sampled = map_export_sample(field, uv, config().requestedMapLevel);
    if (sampled.valid == 0u) {
        record_fault(field, uv);
        return fallback;
    }
    return sampled.value;
}

fn hash32(value : u32) -> u32 {
    var x = value;
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

struct U64 { low : u32, high : u32, }

fn u64_xor(a : U64, b : U64) -> U64 {
    return U64(a.low ^ b.low, a.high ^ b.high);
}

fn u64_add(a : U64, b : U64) -> U64 {
    let low = a.low + b.low;
    return U64(low, a.high + b.high + select(0u, 1u, low < a.low));
}

fn u64_shift_right(value : U64, amount : u32) -> U64 {
    if (amount == 0u) { return value; }
    if (amount < 32u) {
        return U64((value.low >> amount) | (value.high << (32u - amount)),
                   value.high >> amount);
    }
    if (amount < 64u) { return U64(value.high >> (amount - 32u), 0u); }
    return U64(0u, 0u);
}

fn multiply32_wide(a : u32, b : u32) -> U64 {
    let a0 = a & 0xffffu;
    let a1 = a >> 16u;
    let b0 = b & 0xffffu;
    let b1 = b >> 16u;
    let first = a0 * b0;
    let middle0 = a1 * b0 + (first >> 16u);
    let middle1 = (middle0 & 0xffffu) + a0 * b1;
    return U64((middle1 << 16u) | (first & 0xffffu),
        a1 * b1 + (middle0 >> 16u) + (middle1 >> 16u));
}

fn u64_multiply(a : U64, b : U64) -> U64 {
    let lowProduct = multiply32_wide(a.low, b.low);
    return U64(lowProduct.low,
        lowProduct.high + a.high * b.low + a.low * b.high);
}

fn mix64(input : U64) -> U64 {
    var value = u64_xor(input, u64_shift_right(input, 30u));
    value = u64_multiply(value, U64(0x1ce4e5b9u, 0xbf58476du));
    value = u64_xor(value, u64_shift_right(value, 27u));
    value = u64_multiply(value, U64(0x133111ebu, 0x94d049bbu));
    return u64_xor(value, u64_shift_right(value, 31u));
}

fn signed_u64(value : i32) -> U64 {
    return U64(bitcast<u32>(value), select(0u, 0xffffffffu, value < 0));
}

fn lattice_noise(cell : vec2<i32>, seed : U64) -> f32 {
    var value = mix64(u64_xor(seed, signed_u64(cell.x)));
    value = mix64(u64_xor(value, u64_add(signed_u64(cell.y),
        U64(0x7f4a7c15u, 0x9e3779b9u))));
    value = mix64(u64_xor(value, U64(0xd192ed03u, 0xd1b54a32u)));
    // The CPU converts the top 53 bits to double.  f32 retains only 24 bits,
    // so the top 24 bits are the exact representable equivalent.
    return f32(value.high >> 8u) / 16777216.0 * 2.0 - 1.0;
}

fn smooth_noise(position : vec2<f32>, wavelength : f32, seed : U64) -> f32 {
    let p = position / wavelength;
    let cell = vec2<i32>(floor(p));
    var f = fract(p);
    f = f * f * (vec2<f32>(3.0) - 2.0 * f);
    return mix(
        mix(lattice_noise(cell, seed),
            lattice_noise(cell + vec2<i32>(1, 0), seed), f.x),
        mix(lattice_noise(cell + vec2<i32>(0, 1), seed),
            lattice_noise(cell + vec2<i32>(1, 1), seed), f.x), f.y);
}

fn octave_seed(base : U64, octave : u32) -> U64 {
    return mix64(u64_add(base, u64_multiply(
        U64(octave, 0u), U64(0x7f4a7c15u, 0x9e3779b9u))));
}

fn ridged(value : f32, power : f32) -> f32 {
    return pow(clamp(1.0 - abs(value), 0.0, 1.0), power) - 0.28;
}

fn smooth_shelf(value : f32) -> f32 {
    let lower = floor(value);
    let t = value - lower;
    return lower + t * t * (3.0 - 2.0 * t);
}

fn terrain_detail(world : vec2<f32>, relief : vec2<f32>,
                  landform : u32, strength : f32,
                  riverDistance : f32) -> f32 {
    if (strength <= 0.0) { return 0.0; }
    let sourceSeed = U64(config().seedLow ^ config().bundleSignatureLow,
        config().seedHigh ^ config().bundleSignatureHigh);
    let baseSeed = mix64(u64_xor(sourceSeed,
        U64(0xf3bcc909u, 0x6a09e667u)));
    let amplitude = max(abs(relief.x), 20.0) / 600.0 *
        (0.8 + 0.4 * clamp(relief.y, 0.0, 1.0));
    let broadNoise = smooth_noise(world, 2400.0, octave_seed(baseSeed, 0u));
    let provinceNoise = smooth_noise(world, 1500.0, octave_seed(baseSeed, 1u));
    let middleNoise = smooth_noise(world, 1050.0, octave_seed(baseSeed, 2u));
    let hillNoise = smooth_noise(world, 720.0, octave_seed(baseSeed, 3u));
    let ridgeNoise = smooth_noise(world, 420.0, octave_seed(baseSeed, 4u));
    let shoulderNoise = smooth_noise(world, 220.0, octave_seed(baseSeed, 5u));
    var broad = broadNoise * 24.0;
    var middle = provinceNoise * 10.0 + middleNoise * 5.0;
    var hill = hillNoise * 11.0;
    var ridge = ridged(ridgeNoise, 3.5) * 8.0;
    var shoulder = shoulderNoise * 4.0;
    if (landform == 5u) {
        broad *= 1.15;
        middle = ridged(provinceNoise, 2.2) * 24.0 +
            ridged(middleNoise, 2.8) * 13.0;
        ridge = ridged(ridgeNoise, 2.5) * 13.0;
    } else if (landform == 6u) {
        middle = smooth_shelf((provinceNoise * 0.65 + hillNoise * 0.35) * 5.0) * 3.6;
        hill *= 0.35; ridge *= 0.30; shoulder *= 0.65;
    } else if (landform == 12u) {
        broad *= 0.85;
        middle = smooth_shelf((provinceNoise * 0.70 + middleNoise * 0.30) * 4.0) * 10.0;
        hill *= 0.22; ridge = ridged(ridgeNoise, 2.4) * 11.0; shoulder *= 0.45;
    } else if (landform == 3u) {
        broad = broadNoise * 14.0;
        middle = provinceNoise * 5.0 + middleNoise * 2.0;
        hill *= 0.18; ridge = 0.0; shoulder *= 0.15;
    } else if (landform == 7u) {
        let axis = clamp(provinceNoise + middleNoise * 0.35, -1.0, 1.0);
        let channel = -pow(clamp(1.0 - abs(axis), 0.0, 1.0), 3.0) * 26.0;
        broad *= 0.75; middle = provinceNoise * 6.0 + channel;
        hill *= 0.30; ridge *= 0.20;
    } else if (landform == 13u) {
        var channelMask = 0.0;
        if (riverDistance >= 0.0) {
            let halfWidth = clamp(max(abs(relief.x), 20.0) * 1.5, 180.0, 1200.0);
            let edge = clamp((halfWidth - riverDistance) / (halfWidth * 0.22), 0.0, 1.0);
            channelMask = edge * edge * (3.0 - 2.0 * edge);
        } else {
            let axis = clamp(provinceNoise + middleNoise * 0.35, -1.0, 1.0);
            channelMask = pow(clamp(1.0 - abs(axis), 0.0, 1.0), 6.0);
        }
        broad *= 0.65; middle = provinceNoise * 5.0 - channelMask * 90.0;
        hill *= 0.18; ridge *= 0.30; shoulder *= 0.35;
    } else if (landform == 4u) {
        middle = provinceNoise * 12.0 + middleNoise * 7.0;
        hill *= 1.25;
    }
    let ground = smooth_noise(world, 48.0, octave_seed(baseSeed, 6u)) * 1.4;
    let fine = smooth_noise(world, 14.0, octave_seed(baseSeed, 7u)) * 0.32;
    return strength * amplitude *
        (broad + middle + hill + ridge + shoulder + ground + fine);
}

fn biome_material(biome : u32) -> u32 {
    if (biome == 0u) { return 0u; }
    if (biome == 1u) { return 1u; }
    if (biome == 2u) { return 2u; }
    if (biome == 3u) { return 8u; }
    if (biome == 4u || biome == 5u || biome == 24u) { return 9u; }
    if (biome == 7u || biome == 10u) { return 12u; }
    if (biome == 8u) { return 16u; }
    if (biome == 9u) { return 15u; }
    if (biome == 16u || biome == 17u || biome == 18u) { return 18u; }
    if (biome == 15u) { return 19u; }
    if (biome == 19u || biome == 20u || biome == 6u) { return 20u; }
    if (biome == 11u || biome == 13u || biome == 14u || biome == 12u) {
        return 13u;
    }
    if (biome == 21u || biome == 22u || biome == 23u) { return 7u; }
    if (biome == 25u) { return 4u; }
    return 3u;
}

fn sample_surface(canonicalX : f32, canonicalZ : f32) -> SurfaceSample {
    let uv = canonical_uv(canonicalX, canonicalZ);
    let water = required_sample(config().fields[FIELD_WATER_TYPE], uv);
    var height = 0.0;
    var flags = COLUMN_VALID;
    if (water.valid != 0u && u32(round(water.value.x)) != 0u) {
        let waterLevel = required_sample(config().fields[FIELD_WATER_LEVEL], uv);
        height = waterLevel.value.x;
        flags |= COLUMN_WATER;
    } else {
        let elevation = required_sample(config().fields[FIELD_ELEVATION], uv);
        let river = required_sample(config().fields[FIELD_RIVER], uv);
        let base = optional_value(config().fields[FIELD_BASE_ELEVATION], uv,
            elevation.value).x;
        let uplift = optional_value(config().fields[FIELD_UPLIFT], uv,
            vec4<f32>(0.0)).x;
        let relief = optional_value(config().fields[FIELD_RELIEF], uv,
            vec4<f32>(0.0)).xy;
        var landforms = MapExportSparseMixture(vec4<u32>(4u, 0u, 0u, 0u),
            vec4<f32>(1.0, 0.0, 0.0, 0.0), 0u, 1u, 1u);
        if (config().fields[FIELD_LANDFORM] != INVALID_FIELD) {
            landforms = map_export_sample_category(config().fields[FIELD_LANDFORM],
                uv, config().requestedMapLevel);
            if (landforms.valid == 0u) { record_fault(config().fields[FIELD_LANDFORM], uv); }
        }
        let coast = optional_value(config().fields[FIELD_COAST], uv,
            vec4<f32>(-1.0, 0.0, 0.0, 0.0)).x;
        let erosion = optional_value(config().fields[FIELD_EROSION], uv,
            vec4<f32>(0.0)).xy;
        let deposition = optional_value(config().fields[FIELD_DEPOSITION], uv,
            vec4<f32>(0.0)).x;
        let roads = optional_value(config().fields[FIELD_ROADS], uv,
            vec4<f32>(0.0)).x;
        let settlement = optional_value(config().fields[FIELD_SETTLEMENT], uv,
            vec4<f32>(0.0)).x;
        let discharge = max(river.value.y, 0.0);
        let halfWidth = clamp(5.0 + sqrt(discharge) * 8.0, 5.0, 120.0);
        let channel = clamp(1.0 - river.value.x / halfWidth, 0.0, 1.0);
        let incision = channel * channel * (3.0 - 2.0 * channel) *
            clamp(log(1.0 + discharge) * 1.8 +
                clamp(erosion.y, 0.0, 1.0) * 4.0, 0.0, 12.0);
        let authored = elevation.value.x - incision;
        let coastMask = select(clamp(1.0 - abs(coast) / 320.0, 0.0, 1.0),
            0.0, coast < 0.0);
        let detailStrength = config().proceduralDetailStrength *
            (1.0 + clamp(abs(uplift) / 0.001, 0.0, 1.0) * 0.35) *
            (1.0 - clamp(erosion.x, 0.0, 1.0) * 0.15) *
            (1.0 - clamp(erosion.y, 0.0, 1.0) * 0.45) *
            (1.0 - clamp(deposition, 0.0, 1.0) * 0.35) *
            (1.0 - coastMask * 0.65);
        let world = uv * f32(config().worldExtentCanonical) *
            config().heightUnitMetres;
        // Each region contributes its height with smooth weights. A nearest
        // landform switch changes relief by metres at a single voxel column.
        for (var region = 0u; region < landforms.count; region++) {
            let landform = min(landforms.ids[region], 13u);
            let rawDetail = terrain_detail(world, relief, landform,
                detailStrength, river.value.x);
            let riverShoulder = max(180.0,
                config().proceduralTerraceHeightMetres * 16.0);
            let riverT = select(clamp(1.0 - river.value.x / riverShoulder,
                0.0, 1.0), 0.0, river.value.x < 0.0);
            let riverSmooth = riverT * riverT * (3.0 - 2.0 * riverT);
            let settlementMask = select(settlement, settlement / 255.0,
                settlement > 1.0);
            let infrastructure = max(select(0.0, 1.0, roads > 0.0),
                clamp(settlementMask, 0.0, 1.0));
            let corridor = max(infrastructure, riverSmooth);
            let detailed = authored + rawDetail *
                (1.0 - max(infrastructure, riverSmooth * 0.90));
            var landformHeight = detailed;
            if (config().proceduralTerraceStrength > 0.0) {
                let terraceHeight = config().proceduralTerraceHeightMetres;
                let warpWavelength = max(4800.0, terraceHeight * 220.0);
                let warpAmplitude = max(700.0, terraceHeight * 42.0);
                let sourceSeed = U64(config().seedLow ^ config().bundleSignatureLow,
                    config().seedHigh ^ config().bundleSignatureHigh);
                let terraceSeed = mix64(u64_xor(sourceSeed,
                    U64(0xade682d1u, 0x510e527fu)));
                let warped = world + vec2<f32>(
                    smooth_noise(world, warpWavelength, mix64(u64_xor(
                        terraceSeed, U64(0xfb41bd6bu, 0x1f83d9abu)))),
                    smooth_noise(world, warpWavelength, mix64(u64_xor(
                        terraceSeed, U64(0x137e2179u, 0x5be0cd19u))))) *
                    warpAmplitude;
                let bandOffset = smooth_noise(warped, warpWavelength * 0.55,
                    mix64(u64_xor(terraceSeed,
                        U64(0x5f1d36f1u, 0xa54ff53au)))) * terraceHeight * 0.48;
                let benchNoise = smooth_noise(warped, warpWavelength * 1.35,
                    mix64(u64_xor(terraceSeed,
                        U64(0x2b3e6c1fu, 0x9b05688cu))));
                let benchT = clamp((benchNoise + 0.15) / 0.55, 0.0, 1.0);
                let benchMask = benchT * benchT * (3.0 - 2.0 * benchT);
                var landformWeight = 0.0;
                var residualDetail = 0.70;
                if (landform == 6u) {
                    landformWeight = 1.0; residualDetail = 0.12;
                } else if (landform == 12u) {
                    landformWeight = 1.0; residualDetail = 0.18;
                } else if (landform == 13u) {
                    landformWeight = 0.78 + benchMask * 0.14;
                    residualDetail = 0.22;
                } else if (landform == 4u) {
                    landformWeight = 0.50 + benchMask * 0.28;
                    residualDetail = 0.35;
                } else if (landform == 5u) {
                    landformWeight = 0.32 + benchMask * 0.24;
                    residualDetail = 0.50;
                } else if (landform == 7u) {
                    landformWeight = 0.10;
                }
                let blend = clamp(config().proceduralTerraceStrength *
                    landformWeight * (1.0 - corridor), 0.0, 1.0);
                let shifted = base + bandOffset;
                let scaled = shifted / terraceHeight;
                let lower = floor(scaled);
                let phase = fract(scaled);
                let transition = config().proceduralTerraceTransitionFraction;
                var riser = 0.0;
                if (transition > 0.0) {
                    let t = clamp((phase - (1.0 - transition * 2.0)) /
                        (transition * 2.0), 0.0, 1.0);
                    riser = t * t * (3.0 - 2.0 * t);
                }
                let terraced = (lower + riser) * terraceHeight +
                    (authored - base) + (detailed - authored) * residualDetail;
                landformHeight = mix(detailed, terraced, blend);
            }
            height += landforms.weights[region] * landformHeight;
        }
    }
    // City ground replaces the map where the overlay covers the point and
    // blends into it across the overlay's margin (the CPU lane does the same
    // in sampleWorldMapSurface).
    let city = city_ground_sample(canonical_world(uv).x, canonical_world(uv).y);
    if (city.weight > 0.0) {
        height += (city.heightMetres + city_ground_height_offset() - height) * min(city.weight, 1.0);
        if (city.weight >= 0.5 && city.paved) {
            flags &= ~COLUMN_WATER;
        }
    }
    let units = height / config().heightUnitMetres;
    if (abs(units) > 2147483000.0 || units != units) {
        atomicAdd(&results[laneIndex].quantizationOverflows, 1u);
        flags = 0u;
    }
    // The payload is occupancy-only: it stores the half-open solid cell below
    // the sampled surface, not a sub-voxel height patch. Rounding to nearest
    // moved every surface in the lower half of a 10 cm interval down one cell,
    // while the production CPU path correctly owns ceil(height / voxel) - 1.
    // Preserve the upper boundary here so the later spatial-LOD division has
    // the same half-open ownership at every level.
    return SurfaceSample(i32(clamp(
        ceil(units), -2147483000.0, 2147483000.0)), flags, height);
}

fn pack_height_sample_flags(flags : u32, heightMetres : f32, heightUnits : i32) -> u32 {
    if ((flags & 0xffu) == 0u || !(config().heightUnitMetres > 0.0)) {
        return flags & 0xffu;
    }
    let units = heightMetres / config().heightUnitMetres;
    let residual = clamp(f32(heightUnits) - units, 0.0, 1.0);
    let encoded = u32(round(residual * 16777215.0));
    return (flags & 0xffu) | (min(encoded, 16777215u) << 8u);
}

@compute @workgroup_size(1, 1, 1)
fn resetTerrainGPUV2Columns(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    results[laneIndex].status = STATUS_BUILDING;
    atomicStore(&results[laneIndex].missingSamples, 0u);
    atomicStore(&results[laneIndex].faultCount, 0u);
    atomicStore(&results[laneIndex].quantizationOverflows, 0u);
    let samples = sample_dimensions();
    let columnSize = column_dimensions();
    results[laneIndex].heightSampleCount = samples.x * samples.y;
    results[laneIndex].columnCount = columnSize.x * columnSize.y;
    results[laneIndex].terrainGeneration = config().expectedTerrainGeneration;
    atomicStore(&results[laneIndex].writtenColumns, 0u);
    results[laneIndex].atlasRevisionLow = config().expectedAtlasRevisionLow;
    results[laneIndex].atlasRevisionHigh = config().expectedAtlasRevisionHigh;
    results[laneIndex].generatorRevision = config().generatorRevision;
    results[laneIndex].selectedWorkgroup = config().reserved[0];
    atomicStore(&results[laneIndex].minimumHeightUnits, 2147483647);
    atomicStore(&results[laneIndex].maximumHeightUnits, -2147483647);
    // Marks the min/max fields as an authored GPU vertical-datum reduction.
    // Synthetic/legacy column fixtures leave this zero and retain the raster
    // config datum for differential tests and ABI compatibility.
    results[laneIndex].reserved2 = 1u;
    // Fine raster consumes the same fixed-capacity column lane and cannot
    // derive its base from a smaller request's local dimensions.
    results[laneIndex].reserved3 = config().reserved[2];
}

@compute @workgroup_size(TERRAIN_V2_COLUMN_WORKGROUP_X,
                         TERRAIN_V2_COLUMN_WORKGROUP_Z, 1)
fn sampleTerrainGPUV2Heights(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let dimensions = sample_dimensions();
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) { return; }
    let canonicalX = config().tileMinimumCanonicalX +
        (i32(gid.x) - i32(config().halo)) * i32(config().canonicalStep);
    let canonicalZ = config().tileMinimumCanonicalZ +
        (i32(gid.y) - i32(config().halo)) * i32(config().canonicalStep);
    let surface = sample_surface(f32(canonicalX), f32(canonicalZ));
    let index = gid.y * dimensions.x + gid.x;
    heightSamples[sample_base() + index] = HeightSample(
        surface.heightUnits, canonicalX, canonicalZ,
        pack_height_sample_flags(
            surface.flags, surface.heightMetres, surface.heightUnits));
    if (surface.flags != 0u) {
        atomicMin(&results[laneIndex].minimumHeightUnits, surface.heightUnits);
        atomicMax(&results[laneIndex].maximumHeightUnits, surface.heightUnits);
    }
}

@compute @workgroup_size(TERRAIN_V2_COLUMN_WORKGROUP_X,
                         TERRAIN_V2_COLUMN_WORKGROUP_Z, 1)
fn sampleTerrainGPUV2Materials(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let dimensions = column_dimensions();
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) { return; }
    let baseCanonicalX = config().tileMinimumCanonicalX +
        (i32(gid.x) - i32(config().halo)) * i32(config().canonicalStep);
    let baseCanonicalZ = config().tileMinimumCanonicalZ +
        (i32(gid.y) - i32(config().halo)) * i32(config().canonicalStep);
    let canonicalX = f32(baseCanonicalX) +
        0.5 * f32(config().canonicalStep);
    let canonicalZ = f32(baseCanonicalZ) +
        0.5 * f32(config().canonicalStep);
    let uv = canonical_uv(canonicalX, canonicalZ);
    let water = required_sample(config().fields[FIELD_WATER_TYPE], uv);
    let substrate = required_sample(config().fields[FIELD_SUBSTRATE], uv);
    let soil = required_sample(config().fields[FIELD_SOIL_DEPTH], uv);
    let river = required_sample(config().fields[FIELD_RIVER], uv);
    let mixture = map_export_sample_sparse_mixture(
        config().fields[FIELD_BIOME_IDS], config().fields[FIELD_BIOME_WEIGHTS],
        uv, config().requestedMapLevel);
    if (mixture.valid == 0u) {
        record_fault(config().fields[FIELD_BIOME_IDS], uv);
        record_fault(config().fields[FIELD_BIOME_WEIGHTS], uv);
    }
    let waterType = u32(round(water.value.x));
    // Select from the authored sparse mixture on the canonical finest lattice.
    // Dominant-biome selection produced page-sized colour blocks and changed
    // character abruptly at map texel boundaries.
    // Match worldMapBiomeForColumn(): the material lattice is relative to the
    // exported world's origin.  Keep the subtraction in integer space so the
    // address remains exact even for very large/coarse pages.  Hashing the
    // absolute canonical address made mixed-biome pages disagree with CPU
    // generation and changed their albedo when the world origin moved.
    let finestX = baseCanonicalX + i32(config().canonicalStep >> 1u) -
        config().worldOriginCanonicalX;
    let finestZ = baseCanonicalZ + i32(config().canonicalStep >> 1u) -
        config().worldOriginCanonicalZ;
    let sourceSeed = U64(config().seedLow ^ config().bundleSignatureLow,
        config().seedHigh ^ config().bundleSignatureHigh);
    let biomeSeed = mix64(u64_xor(sourceSeed,
        U64(0x84caa73bu, 0xbb67ae85u)));
    let random = lattice_noise(vec2<i32>(finestX, finestZ), biomeSeed) *
        0.5 + 0.5;
    var selectedBiome = mixture.ids.x;
    var accumulated = 0.0;
    for (var i = 0u; i < mixture.count; i++) {
        accumulated += mixture.weights[i];
        if (random < accumulated) {
            selectedBiome = mixture.ids[i];
            break;
        }
    }
    var material = biome_material(selectedBiome);
    var flags = COLUMN_VALID;
    if (waterType != 0u) {
        material = select(1u, 0u, waterType == 1u);
        flags |= COLUMN_WATER;
    } else {
        let substrateId = u32(round(substrate.value.x));
        let placement = optional_value(config().fields[FIELD_PLACEMENT], uv,
            vec4<f32>(0.0));
        let layers = optional_value(config().fields[FIELD_SOIL_LAYERS], uv,
            vec4<f32>(0.0));
        let climate = optional_value(config().fields[FIELD_CLIMATE], uv,
            vec4<f32>(20.0, 0.5, 0.0, 0.0));
        if (soil.value.x < 0.15 || substrateId == 1u || substrateId == 2u ||
            (placement.w >= 0.58 && soil.value.x < 0.5) || layers.x >= 0.62) {
            material = 4u;
        }
        if (climate.x <= -2.0 && climate.y >= 0.12) { material = 5u; }
        let discharge = max(river.value.y, 0.0);
        let voxelSize = config().heightUnitMetres * f32(config().canonicalStep);
        let halfWidth = select(0.0, clamp(5.0 + sqrt(discharge) * 8.0,
            max(2.0, voxelSize * 0.75), 120.0), discharge > 0.0);
        if (layers.z + layers.w >= 0.48 && climate.y >= 0.55 &&
            river.value.x <= max(halfWidth * 3.0, 30.0)) { material = 7u; }
        if (discharge > 0.0 && river.value.x <= halfWidth) { material = 6u; }
    }
    let city = city_ground_sample(canonical_world(uv).x, canonical_world(uv).y);
    if (city.weight >= 0.5) {
        if (city.paved) { flags &= ~COLUMN_WATER; }
        if (city.material != 255u) { material = city.material; }
    }
    materialScratch[column_base() + gid.y * dimensions.x + gid.x] =
        (material & 0xffffu) | (flags << 16u);
}

@compute @workgroup_size(TERRAIN_V2_COLUMN_WORKGROUP_X,
                         TERRAIN_V2_COLUMN_WORKGROUP_Z, 1)
fn buildTerrainGPUV2Columns(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    let dimensions = column_dimensions();
    if (gid.x >= dimensions.x || gid.y >= dimensions.y ||
        atomicLoad(&results[laneIndex].missingSamples) != 0u ||
        atomicLoad(&results[laneIndex].quantizationOverflows) != 0u) { return; }
    let samples = sample_dimensions();
    let base = sample_base();
    let a = heightSamples[base + gid.y * samples.x + gid.x];
    let b = heightSamples[base + gid.y * samples.x + gid.x + 1u];
    let c = heightSamples[base + (gid.y + 1u) * samples.x + gid.x];
    let d = heightSamples[base + (gid.y + 1u) * samples.x + gid.x + 1u];
    let packed = materialScratch[column_base() + gid.y * dimensions.x + gid.x];
    var minimumHeight = min(min(a.heightUnits, b.heightUnits),
        min(c.heightUnits, d.heightUnits));
    var maximumHeight = max(max(a.heightUnits, b.heightUnits),
        max(c.heightUnits, d.heightUnits));
    var columnFlags = packed >> 16u;
    // A spatial-LOD boundary can meet two pages one level finer. Endpoints
    // alone do not cover the fine midpoint when the edge crosses a valley or
    // ridge, leaving a visible slit. Match the CPU page contract by sampling
    // that midpoint and conservatively extending only the four outer rows.
    if (config().canonicalStep > 1u) {
        let localX = i32(gid.x) - i32(config().halo);
        let localZ = i32(gid.y) - i32(config().halo);
        let halfStep = i32(config().canonicalStep >> 1u);
        let step = i32(config().canonicalStep);
        var boundary : SurfaceSample;
        if (localX == 0 && localZ >= 0 && localZ < i32(config().columnsZ)) {
            boundary = sample_surface(
                f32(config().tileMinimumCanonicalX),
                f32(config().tileMinimumCanonicalZ + localZ * step + halfStep));
            minimumHeight = min(minimumHeight, boundary.heightUnits);
            maximumHeight = max(maximumHeight, boundary.heightUnits);
            columnFlags = select(0u, columnFlags, boundary.flags != 0u);
        }
        if (localX == i32(config().columnsX) - 1 &&
            localZ >= 0 && localZ < i32(config().columnsZ)) {
            boundary = sample_surface(
                f32(config().tileMinimumCanonicalX + i32(config().columnsX) * step),
                f32(config().tileMinimumCanonicalZ + localZ * step + halfStep));
            minimumHeight = min(minimumHeight, boundary.heightUnits);
            maximumHeight = max(maximumHeight, boundary.heightUnits);
            columnFlags = select(0u, columnFlags, boundary.flags != 0u);
        }
        if (localZ == 0 && localX >= 0 && localX < i32(config().columnsX)) {
            boundary = sample_surface(
                f32(config().tileMinimumCanonicalX + localX * step + halfStep),
                f32(config().tileMinimumCanonicalZ));
            minimumHeight = min(minimumHeight, boundary.heightUnits);
            maximumHeight = max(maximumHeight, boundary.heightUnits);
            columnFlags = select(0u, columnFlags, boundary.flags != 0u);
        }
        if (localZ == i32(config().columnsZ) - 1 &&
            localX >= 0 && localX < i32(config().columnsX)) {
            boundary = sample_surface(
                f32(config().tileMinimumCanonicalX + localX * step + halfStep),
                f32(config().tileMinimumCanonicalZ + i32(config().columnsZ) * step));
            minimumHeight = min(minimumHeight, boundary.heightUnits);
            maximumHeight = max(maximumHeight, boundary.heightUnits);
            columnFlags = select(0u, columnFlags, boundary.flags != 0u);
        }
    }
    columns[column_base() + gid.y * dimensions.x + gid.x] = Column(
        minimumHeight, maximumHeight, packed & 0xffffu, columnFlags);
    atomicAdd(&results[laneIndex].writtenColumns, 1u);
}

@compute @workgroup_size(1, 1, 1)
fn finalizeTerrainGPUV2Columns(@builtin(global_invocation_id) gid : vec3<u32>) {
    laneIndex = gid.z;
    if (atomicLoad(&results[laneIndex].missingSamples) != 0u) {
        results[laneIndex].status = STATUS_MISSING_PAGE;
    } else if (atomicLoad(&results[laneIndex].quantizationOverflows) != 0u) {
        results[laneIndex].status = STATUS_QUANTIZATION_OVERFLOW;
    } else {
        results[laneIndex].status = STATUS_READY;
    }
}
