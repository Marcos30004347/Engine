#ifndef MAP_EXPORT_PAGES_WGSL
#define MAP_EXPORT_PAGES_WGSL

#include "map_export_addressing.wgsl"

#ifndef MAP_EXPORT_BIND_GROUP
#define MAP_EXPORT_BIND_GROUP 0
#endif
#ifndef MAP_EXPORT_METADATA_BINDING
#define MAP_EXPORT_METADATA_BINDING 0
#endif
#ifndef MAP_EXPORT_PAGE_TABLE_BINDING
#define MAP_EXPORT_PAGE_TABLE_BINDING 1
#endif
#ifndef MAP_EXPORT_PHYSICAL_BINDING
#define MAP_EXPORT_PHYSICAL_BINDING 2
#endif

// These values intentionally match MapExportFormat and MapExportFilter.
const MAP_EXPORT_R8_UINT : u32 = 0u;
const MAP_EXPORT_R16_FLOAT : u32 = 1u;
const MAP_EXPORT_R16_UNORM : u32 = 2u;
const MAP_EXPORT_R32_UINT : u32 = 3u;
const MAP_EXPORT_RG16_FLOAT : u32 = 4u;
const MAP_EXPORT_RGBA8_UINT : u32 = 5u;
const MAP_EXPORT_RGBA8_UNORM : u32 = 6u;
const MAP_EXPORT_FILTER_NEAREST : u32 = 0u;
const MAP_EXPORT_FILTER_LINEAR : u32 = 1u;
const MAP_EXPORT_FILTER_SPARSE_MIXTURE : u32 = 2u;

// Header: eight words. Field descriptor: twelve words (layout, scale, offset).
@group(MAP_EXPORT_BIND_GROUP) @binding(MAP_EXPORT_METADATA_BINDING)
var<storage, read> mapExportMetadata : array<u32>;
@group(MAP_EXPORT_BIND_GROUP) @binding(MAP_EXPORT_PAGE_TABLE_BINDING)
var<storage, read> mapExportPageTable : array<u32>;
@group(MAP_EXPORT_BIND_GROUP) @binding(MAP_EXPORT_PHYSICAL_BINDING)
var<storage, read> mapExportPhysicalWords : array<u32>;

struct MapExportFieldDescriptor
{
    storage : vec4<u32>,
    scale : vec4<f32>,
    offset : vec4<f32>,
};

struct MapExportResolvedPage
{
    physical : u32,
    level : u32,
    coordinate : vec2<u32>,
    valid : u32,
};

struct MapExportSample
{
    value : vec4<f32>,
    level : u32,
    valid : u32,
};

struct MapExportUintSample
{
    value : vec4<u32>,
    level : u32,
    valid : u32,
};

struct MapExportSparseMixture
{
    ids : vec4<u32>,
    weights : vec4<f32>,
    level : u32,
    count : u32,
    valid : u32,
};

fn map_export_page_size() -> u32 { return mapExportMetadata[0u]; }
fn map_export_gutter() -> u32 { return mapExportMetadata[1u]; }
fn map_export_stored_side() -> u32 { return mapExportMetadata[2u]; }
fn map_export_maximum_level() -> u32 { return mapExportMetadata[3u]; }
fn map_export_slot_bytes() -> u32 { return mapExportMetadata[4u]; }
fn map_export_entries_per_field() -> u32 { return mapExportMetadata[5u]; }
fn map_export_field_count() -> u32 { return mapExportMetadata[6u]; }
fn map_export_resolution() -> u32 { return mapExportMetadata[7u]; }

fn map_export_field(field : u32) -> MapExportFieldDescriptor
{
    let base = 8u + field * 12u;
    return MapExportFieldDescriptor(
        vec4<u32>(mapExportMetadata[base], mapExportMetadata[base + 1u],
            mapExportMetadata[base + 2u], mapExportMetadata[base + 3u]),
        vec4<f32>(bitcast<f32>(mapExportMetadata[base + 4u]),
            bitcast<f32>(mapExportMetadata[base + 5u]),
            bitcast<f32>(mapExportMetadata[base + 6u]),
            bitcast<f32>(mapExportMetadata[base + 7u])),
        vec4<f32>(bitcast<f32>(mapExportMetadata[base + 8u]),
            bitcast<f32>(mapExportMetadata[base + 9u]),
            bitcast<f32>(mapExportMetadata[base + 10u]),
            bitcast<f32>(mapExportMetadata[base + 11u])));
}

fn map_export_page_entry(field : u32, level : u32,
    coordinate : vec2<u32>) -> u32
{
    let descriptor = map_export_field(field);
    return mapExportPageTable[descriptor.storage.w +
        map_export_page_table_base(level) +
        map_export_page_morton(coordinate, level)];
}

fn map_export_resolve_page(field : u32, coordinate : vec2<f32>,
    maximumResidentLevel : u32) -> MapExportResolvedPage
{
    if (field >= map_export_field_count())
    {
        return MapExportResolvedPage(0u, 0u, vec2<u32>(0u), 0u);
    }
    let uv = clamp(coordinate, vec2<f32>(0.0), vec2<f32>(1.0));
    var level = min(maximumResidentLevel, map_export_maximum_level());
    var dimension = 1u << level;
    var pageCoordinate = min(vec2<u32>(uv * f32(dimension)),
        vec2<u32>(dimension - 1u));
    loop
    {
        let entry = map_export_page_entry(field, level, pageCoordinate);
        if (entry != 0u)
        {
            return MapExportResolvedPage(entry - 1u, level,
                pageCoordinate, 1u);
        }
        if (level == 0u) { break; }
        level--;
        pageCoordinate /= vec2<u32>(2u);
    }
    return MapExportResolvedPage(0u, 0u, vec2<u32>(0u), 0u);
}

fn map_export_resolve_at(field : u32, page : MapExportResolvedPage)
    -> MapExportResolvedPage
{
    if (field >= map_export_field_count())
    {
        return MapExportResolvedPage(0u, 0u, vec2<u32>(0u), 0u);
    }
    let entry = map_export_page_entry(field, page.level, page.coordinate);
    if (entry == 0u)
    {
        return MapExportResolvedPage(0u, page.level, page.coordinate, 0u);
    }
    return MapExportResolvedPage(entry - 1u, page.level, page.coordinate, 1u);
}

fn map_export_load_bits(page : MapExportResolvedPage,
    descriptor : MapExportFieldDescriptor, local : vec2<u32>) -> u32
{
    let texel = local.y * map_export_stored_side() + local.x;
    let byteOffset = page.physical * map_export_slot_bytes() +
        texel * descriptor.storage.z;
    let word = mapExportPhysicalWords[byteOffset >> 2u];
    if (descriptor.storage.z == 4u) { return word; }
    let shift = (byteOffset & 3u) * 8u;
    let mask = select(0xffu, 0xffffu, descriptor.storage.z == 2u);
    return (word >> shift) & mask;
}

fn map_export_f16_to_f32(value : u32) -> f32
{
    let sign = (value & 0x8000u) << 16u;
    var exponent = (value >> 10u) & 0x1fu;
    var mantissa = value & 0x03ffu;
    if (exponent == 0u)
    {
        if (mantissa == 0u) { return bitcast<f32>(sign); }
        var exponent32 = 113u;
        loop
        {
            if ((mantissa & 0x0400u) != 0u) { break; }
            mantissa <<= 1u;
            exponent32--;
        }
        mantissa &= 0x03ffu;
        return bitcast<f32>(sign | (exponent32 << 23u) | (mantissa << 13u));
    }
    if (exponent == 31u)
    {
        return bitcast<f32>(sign | 0x7f800000u | (mantissa << 13u));
    }
    let exponent32 = exponent + 112u;
    return bitcast<f32>(sign | (exponent32 << 23u) | (mantissa << 13u));
}

fn map_export_decode(bits : u32,
    descriptor : MapExportFieldDescriptor) -> vec4<f32>
{
    var stored = vec4<f32>(0.0);
    switch descriptor.storage.x
    {
        case MAP_EXPORT_R8_UINT:
        { stored.x = f32(bits & 0xffu); }
        case MAP_EXPORT_R16_FLOAT:
        { stored.x = map_export_f16_to_f32(bits); }
        case MAP_EXPORT_R16_UNORM:
        { stored.x = f32(bits & 0xffffu) / 65535.0; }
        case MAP_EXPORT_R32_UINT:
        { stored.x = f32(bits); }
        case MAP_EXPORT_RG16_FLOAT:
        {
            stored.x = map_export_f16_to_f32(bits & 0xffffu);
            stored.y = map_export_f16_to_f32(bits >> 16u);
        }
        case MAP_EXPORT_RGBA8_UINT:
        {
            stored = vec4<f32>(f32(bits & 0xffu),
                f32((bits >> 8u) & 0xffu),
                f32((bits >> 16u) & 0xffu), f32(bits >> 24u));
        }
        case MAP_EXPORT_RGBA8_UNORM:
        {
            stored = vec4<f32>(f32(bits & 0xffu),
                f32((bits >> 8u) & 0xffu),
                f32((bits >> 16u) & 0xffu), f32(bits >> 24u)) / 255.0;
        }
        default: {}
    }
    return stored * descriptor.scale + descriptor.offset;
}

fn map_export_decode_uint(bits : u32,
    descriptor : MapExportFieldDescriptor) -> vec4<u32>
{
    switch descriptor.storage.x
    {
        case MAP_EXPORT_R8_UINT:
        { return vec4<u32>(bits & 0xffu, 0u, 0u, 0u); }
        case MAP_EXPORT_R32_UINT:
        { return vec4<u32>(bits, 0u, 0u, 0u); }
        case MAP_EXPORT_RGBA8_UINT:
        {
            return vec4<u32>(bits & 0xffu, (bits >> 8u) & 0xffu,
                (bits >> 16u) & 0xffu, bits >> 24u);
        }
        default: { return vec4<u32>(0u); }
    }
}

fn map_export_local_texel(page : MapExportResolvedPage,
    global : vec2<u32>) -> vec2<u32>
{
    let origin = page.coordinate * map_export_page_size();
    return vec2<u32>(vec2<i32>(global) - vec2<i32>(origin) +
        vec2<i32>(i32(map_export_gutter())));
}

fn map_export_sample_nearest(field : u32, coordinate : vec2<f32>,
    maximumResidentLevel : u32) -> MapExportSample
{
    let page = map_export_resolve_page(field, coordinate, maximumResidentLevel);
    if (page.valid == 0u) { return MapExportSample(vec4<f32>(0.0), 0u, 0u); }
    let descriptor = map_export_field(field);
    let side = map_export_page_size() * (1u << page.level);
    let global = min(vec2<u32>(clamp(coordinate, vec2<f32>(0.0),
        vec2<f32>(1.0)) * f32(side)), vec2<u32>(side - 1u));
    let bits = map_export_load_bits(page, descriptor,
        map_export_local_texel(page, global));
    return MapExportSample(map_export_decode(bits, descriptor), page.level, 1u);
}

// Exact categorical access. Unlike the physical-value f32 path, this preserves
// every bit of R32Uint region or feature identifiers.
fn map_export_sample_nearest_uint(field : u32, coordinate : vec2<f32>,
    maximumResidentLevel : u32) -> MapExportUintSample
{
    let page = map_export_resolve_page(field, coordinate, maximumResidentLevel);
    if (page.valid == 0u)
    { return MapExportUintSample(vec4<u32>(0u), 0u, 0u); }
    let descriptor = map_export_field(field);
    if (descriptor.storage.x != MAP_EXPORT_R8_UINT &&
        descriptor.storage.x != MAP_EXPORT_R32_UINT &&
        descriptor.storage.x != MAP_EXPORT_RGBA8_UINT)
    { return MapExportUintSample(vec4<u32>(0u), page.level, 0u); }
    let side = map_export_page_size() * (1u << page.level);
    let global = min(vec2<u32>(clamp(coordinate, vec2<f32>(0.0),
        vec2<f32>(1.0)) * f32(side)), vec2<u32>(side - 1u));
    let bits = map_export_load_bits(page, descriptor,
        map_export_local_texel(page, global));
    return MapExportUintSample(map_export_decode_uint(bits, descriptor),
        page.level, 1u);
}

fn map_export_sample_linear(field : u32, coordinate : vec2<f32>,
    maximumResidentLevel : u32) -> MapExportSample
{
    let page = map_export_resolve_page(field, coordinate, maximumResidentLevel);
    if (page.valid == 0u) { return MapExportSample(vec4<f32>(0.0), 0u, 0u); }
    let descriptor = map_export_field(field);
    let side = map_export_page_size() * (1u << page.level);
    let maximum = f32(side - 1u);
    let position = clamp(coordinate * f32(side) - vec2<f32>(0.5),
        vec2<f32>(0.0), vec2<f32>(maximum));
    let low = vec2<u32>(floor(position));
    let high = min(low + vec2<u32>(1u), vec2<u32>(side - 1u));
    let fraction = position - vec2<f32>(low);
    let a = map_export_decode(map_export_load_bits(page, descriptor,
        map_export_local_texel(page, low)), descriptor);
    let b = map_export_decode(map_export_load_bits(page, descriptor,
        map_export_local_texel(page, vec2<u32>(high.x, low.y))), descriptor);
    let c = map_export_decode(map_export_load_bits(page, descriptor,
        map_export_local_texel(page, vec2<u32>(low.x, high.y))), descriptor);
    let d = map_export_decode(map_export_load_bits(page, descriptor,
        map_export_local_texel(page, high)), descriptor);
    return MapExportSample(mix(mix(a, b, fraction.x), mix(c, d, fraction.x),
        fraction.y), page.level, 1u);
}

fn map_export_sample(field : u32, coordinate : vec2<f32>,
    maximumResidentLevel : u32) -> MapExportSample
{
    if (field >= map_export_field_count())
    { return MapExportSample(vec4<f32>(0.0), 0u, 0u); }
    let samplingFilter = map_export_field(field).storage.y;
    if (samplingFilter == MAP_EXPORT_FILTER_NEAREST)
    { return map_export_sample_nearest(field, coordinate, maximumResidentLevel); }
    if (samplingFilter == MAP_EXPORT_FILTER_LINEAR)
    { return map_export_sample_linear(field, coordinate, maximumResidentLevel); }
    // Sparse fields are intentionally invalid alone: use the paired sampler.
    return MapExportSample(vec4<f32>(0.0), 0u, 0u);
}

// Blend categorical terrain generators, never the numeric label itself.
// Smooth spatial weights match the CPU category sampler, including gutters
// and ancestor fallback. Merge equal labels so uniform regions evaluate once.
fn map_export_sample_category(field : u32, coordinate : vec2<f32>,
    maximumResidentLevel : u32) -> MapExportSparseMixture
{
    let page = map_export_resolve_page(field, coordinate, maximumResidentLevel);
    var result = MapExportSparseMixture(vec4<u32>(0u), vec4<f32>(0.0),
        page.level, 0u, 0u);
    if (page.valid == 0u) { return result; }
    let descriptor = map_export_field(field);
    if (descriptor.storage.x != MAP_EXPORT_R8_UINT) { return result; }
    let side = map_export_page_size() * (1u << page.level);
    let position = clamp(coordinate * f32(side) - vec2<f32>(0.5),
        vec2<f32>(0.0), vec2<f32>(f32(side - 1u)));
    let low = vec2<u32>(floor(position));
    let high = min(low + vec2<u32>(1u), vec2<u32>(side - 1u));
    let t = position - vec2<f32>(low);
    let fraction = t * t * (vec2<f32>(3.0) - 2.0 * t);
    let coordinates = array<vec2<u32>, 4>(low,
        vec2<u32>(high.x, low.y), vec2<u32>(low.x, high.y), high);
    let weights = array<f32, 4>((1.0 - fraction.x) * (1.0 - fraction.y),
        fraction.x * (1.0 - fraction.y),
        (1.0 - fraction.x) * fraction.y, fraction.x * fraction.y);
    for (var tap = 0u; tap < 4u; tap++)
    {
        if (weights[tap] <= 0.0) { continue; }
        let id = map_export_load_bits(page, descriptor,
            map_export_local_texel(page, coordinates[tap])) & 0xffu;
        var slot = 0u;
        loop {
            if (slot >= result.count) { break; }
            if (result.ids[slot] == id) { break; }
            slot++;
        }
        if (slot == result.count) {
            result.ids[slot] = id;
            result.count++;
        }
        result.weights[slot] += weights[tap];
    }
    result.valid = 1u;
    return result;
}

fn map_export_sample_sparse_mixture(idField : u32, weightField : u32,
    coordinate : vec2<f32>, maximumResidentLevel : u32)
    -> MapExportSparseMixture
{
    let idPage = map_export_resolve_page(idField, coordinate,
        maximumResidentLevel);
    let weightPage = map_export_resolve_at(weightField, idPage);
    if (idPage.valid == 0u || weightPage.valid == 0u)
    {
        return MapExportSparseMixture(vec4<u32>(0u), vec4<f32>(0.0),
            0u, 0u, 0u);
    }
    let idDescriptor = map_export_field(idField);
    let weightDescriptor = map_export_field(weightField);
    if (idDescriptor.storage.x != MAP_EXPORT_RGBA8_UINT ||
        weightDescriptor.storage.x != MAP_EXPORT_RGBA8_UNORM ||
        idDescriptor.storage.y != MAP_EXPORT_FILTER_SPARSE_MIXTURE ||
        weightDescriptor.storage.y != MAP_EXPORT_FILTER_SPARSE_MIXTURE)
    {
        return MapExportSparseMixture(vec4<u32>(0u), vec4<f32>(0.0),
            0u, 0u, 0u);
    }
    let side = map_export_page_size() * (1u << idPage.level);
    let position = clamp(coordinate * f32(side) - vec2<f32>(0.5),
        vec2<f32>(0.0), vec2<f32>(f32(side - 1u)));
    let low = vec2<u32>(floor(position));
    let high = min(low + vec2<u32>(1u), vec2<u32>(side - 1u));
    let fraction = position - vec2<f32>(low);
    let coordinates = array<vec2<u32>, 4>(low,
        vec2<u32>(high.x, low.y), vec2<u32>(low.x, high.y), high);
    let spatial = array<f32, 4>((1.0 - fraction.x) * (1.0 - fraction.y),
        fraction.x * (1.0 - fraction.y),
        (1.0 - fraction.x) * fraction.y, fraction.x * fraction.y);
    var ids : array<u32, 16>;
    var scores : array<f32, 16>;
    var priorities : array<f32, 16>;
    var candidateCount = 0u;
    for (var tap = 0u; tap < 4u; tap++)
    {
        let local = map_export_local_texel(idPage, coordinates[tap]);
        let packedIds = map_export_load_bits(idPage, idDescriptor, local);
        let packedWeights = map_export_load_bits(weightPage, weightDescriptor,
            local);
        for (var slot = 0u; slot < 4u; slot++)
        {
            let id = (packedIds >> (slot * 8u)) & 0xffu;
            let contribution = spatial[tap] *
                f32((packedWeights >> (slot * 8u)) & 0xffu) / 255.0;
            if (contribution <= 0.0) { continue; }
            var found = candidateCount;
            for (var candidate = 0u; candidate < candidateCount; candidate++)
            {
                if (ids[candidate] == id) { found = candidate; break; }
            }
            if (found == candidateCount)
            {
                ids[candidateCount] = id;
                candidateCount++;
            }
            scores[found] += contribution;
            priorities[found] += contribution * f32(4u - slot);
        }
    }
    var resultIds = vec4<u32>(0u);
    var resultWeights = vec4<f32>(0.0);
    var used : array<u32, 16>;
    var resultCount = 0u;
    var total = 0.0;
    for (var outputSlot = 0u; outputSlot < 4u; outputSlot++)
    {
        var best = 16u;
        for (var candidate = 0u; candidate < candidateCount; candidate++)
        {
            if (used[candidate] != 0u) { continue; }
            if (best == 16u || scores[candidate] > scores[best] ||
                (scores[candidate] == scores[best] &&
                 (priorities[candidate] > priorities[best] ||
                  (priorities[candidate] == priorities[best] &&
                   ids[candidate] < ids[best]))))
            { best = candidate; }
        }
        if (best == 16u) { break; }
        used[best] = 1u;
        resultIds[outputSlot] = ids[best];
        resultWeights[outputSlot] = scores[best];
        total += scores[best];
        resultCount++;
    }
    if (total <= 0.0)
    {
        return MapExportSparseMixture(vec4<u32>(0u), vec4<f32>(0.0),
            idPage.level, 0u, 0u);
    }
    return MapExportSparseMixture(resultIds, resultWeights / total,
        idPage.level, resultCount, 1u);
}

#endif
