#include "map_export_pages.wgsl"

struct MapExportTestQuery
{
    coordinate : vec2<f32>,
    field : u32,
    companionField : u32,
    maximumResidentLevel : u32,
    mode : u32,
    reserved : vec2<u32>,
};

struct MapExportTestOutput
{
    value : vec4<f32>,
    ids : vec4<u32>,
    weights : vec4<f32>,
    // Resolved level, sparse count, validity, query mode.
    metadata : vec4<u32>,
};

@group(0) @binding(3) var<storage, read> mapExportTestQueries :
    array<MapExportTestQuery>;
@group(0) @binding(4) var<storage, read_write> mapExportTestOutputs :
    array<MapExportTestOutput>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) invocation : vec3<u32>)
{
    let index = invocation.x;
    if (index >= arrayLength(&mapExportTestQueries) ||
        index >= arrayLength(&mapExportTestOutputs)) { return; }
    let query = mapExportTestQueries[index];
    var output : MapExportTestOutput;
    output.metadata.w = query.mode;
    if (query.mode == 0u)
    {
        let sample = map_export_sample(query.field, query.coordinate,
            query.maximumResidentLevel);
        output.value = sample.value;
        output.metadata.x = sample.level;
        output.metadata.z = sample.valid;
    }
    else if (query.mode == 1u)
    {
        let sample = map_export_sample_nearest_uint(query.field,
            query.coordinate, query.maximumResidentLevel);
        output.ids = sample.value;
        output.metadata.x = sample.level;
        output.metadata.z = sample.valid;
    }
    else if (query.mode == 2u)
    {
        let sample = map_export_sample_sparse_mixture(query.field,
            query.companionField, query.coordinate,
            query.maximumResidentLevel);
        output.ids = sample.ids;
        output.weights = sample.weights;
        output.metadata.x = sample.level;
        output.metadata.y = sample.count;
        output.metadata.z = sample.valid;
    }
    mapExportTestOutputs[index] = output;
}
