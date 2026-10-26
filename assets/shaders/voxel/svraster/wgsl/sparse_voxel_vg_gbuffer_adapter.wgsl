#include "sparse_voxel_types.wgsl"

struct SparseVoxelVGAdapterSettings {
    dimensions : vec4<u32>,
    // viewDepth = y / (deviceDepth + x). For the sample's reverse-Z infinite
    // projection this is near / deviceDepth.
    depthConversion : vec4<f32>,
}

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> settings : SparseVoxelVGAdapterSettings;
@group(0) @binding(2) var<storage, read_write> resolvedSurfaces : array<ResolvedVoxelSurface>;
@group(0) @binding(3) var<storage, read_write> coverage : array<u32>;
@group(0) @binding(4) var<storage, read_write> linearDepth : array<f32>;
@group(0) @binding(5) var<storage, read_write> microPayloads : array<u32>;
@group(0) @binding(6) var<storage, read_write> materialSampleAddresses : array<VoxelMaterialSampleAddress>;
@group(0) @binding(7) var<storage, read_write> terrainOwnershipIds : array<u32>;
@group(0) @binding(8) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(9) var packedGeometryIdsLoTexture : texture_2d<u32>;
@group(0) @binding(10) var packedGeometryIdsHiTexture : texture_2d<u32>;
@group(0) @binding(11) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(12) var baseColorTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(13) var materialPropertiesTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(14) var emissiveTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(15) var normalTexture : texture_storage_2d<rgba16float, write>;
// Per-pixel software-raster resolve from the decoded-VG merge kernel:
// payload, reverse-Z device depth (0 = no hit), oct-encoded world normal,
// dense instance. A one-element dummy disables the merge.
struct SparseVoxelVGSwPixel {
    payload : u32,
    depthBits : u32,
    normalOct : u32,
    denseInstance : u32,
}
@group(0) @binding(16) var<storage, read> swResolvedPayloads : array<SparseVoxelVGSwPixel>;
// Exact city voxel colour written by the VG visibility resolve; alpha 0 where
// the pixel is not a city voxel. A texture of another size is a placeholder.
@group(0) @binding(17) var cityAlbedoTexture : texture_2d<f32>;
// City ground materials by world position, for terrain pixels (the VG terrain
// mesh carries no per-voxel material). Words: [enabled, width, height, 0,
// originX, originZ, texelMetres (f32 bits, renderer frame), 0, palette x32
// (RGBA8, R in the low byte), material ids packed four per word]. 255 = none.
@group(0) @binding(18) var<storage, read> cityGround : array<u32>;

fn vg_adapter_city_ground(world : vec3<f32>) -> vec4<f32> {
    if (arrayLength(&cityGround) < 41u || cityGround[0] == 0u) {
        return vec4<f32>(0.0);
    }
    let width = cityGround[1];
    let height = cityGround[2];
    let texel = bitcast<f32>(cityGround[6]);
    let local = (world.xz - vec2<f32>(bitcast<f32>(cityGround[4]), bitcast<f32>(cityGround[5]))) / texel;
    if (local.x < 0.0 || local.y < 0.0 || local.x >= f32(width) || local.y >= f32(height)) {
        return vec4<f32>(0.0);
    }
    let index = u32(local.y) * width + u32(local.x);
    let word = 40u + index / 4u;
    if (word >= arrayLength(&cityGround)) {
        return vec4<f32>(0.0);
    }
    let material = (cityGround[word] >> ((index % 4u) * 8u)) & 255u;
    if (material >= 32u) {
        return vec4<f32>(0.0);
    }
    return vec4<f32>(unpack4x8unorm(cityGround[8u + material]).rgb, 1.0);
}

fn vg_adapter_camera_direction(pixel : vec2<f32>) -> vec3<f32> {
    return vec3<f32>(
        (pixel.x + 0.5 - raster.focalPrincipal.z) /
            max(raster.focalPrincipal.x, 1.0e-6),
        (pixel.y + 0.5 - raster.focalPrincipal.w) /
            max(raster.focalPrincipal.y, 1.0e-6),
        1.0);
}

// The low geometry ID word is the full instance index and the high word is
// the visibility payload: (globalCluster << 7) | triangle, the encoding
// shared with the software rasterizer (packVisibilityPayload in
// virtualgeometrydata.wgsl).
const VG_ADAPTER_PAYLOAD_TRIANGLE_BITS : u32 = 7u;
const VG_ADAPTER_PAYLOAD_TRIANGLE_MASK : u32 = 0x7fu;

fn vg_adapter_oct_decode(oct : u32) -> vec3<f32> {
    let sx = f32(i32(oct << 16u) >> 16u) / 32767.0;
    let sy = f32(i32(oct) >> 16u) / 32767.0;
    var n = vec3<f32>(sx, sy, 1.0 - abs(sx) - abs(sy));
    if (n.z < 0.0) {
        let ox = n.x;
        let oy = n.y;
        n.x = (1.0 - abs(oy)) * select(-1.0, 1.0, ox >= 0.0);
        n.y = (1.0 - abs(ox)) * select(-1.0, 1.0, oy >= 0.0);
    }
    let len = length(n);
    return select(vec3<f32>(0.0, 0.0, 1.0), n / len, len > 1.0e-6);
}

fn vg_adapter_pack_rgb8(color : vec3<f32>, roughness : f32) -> u32 {
    let c = vec3<u32>(round(clamp(color, vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0));
    let r = u32(round(clamp(roughness, 0.0, 1.0) * 255.0));
    return c.r | (c.g << 8u) | (c.b << 16u) | (r << 24u);
}

@compute @workgroup_size(8, 8, 1)
fn adaptVirtualGeometryGBuffer(@builtin(global_invocation_id) gid : vec3<u32>) {
    let pixel = gid.xy;
    let dimensions = max(settings.dimensions.xy, vec2<u32>(1u));
    if (any(pixel >= dimensions)) {
        return;
    }

    let pixelIndex = pixel.y * dimensions.x + pixel.x;
    let pixelI = vec2<i32>(pixel);
    var deviceDepth = textureLoad(sceneDepthTexture, pixelI, 0);

    // Software-raster merge: the compute raster resolves its winning payload
    // per pixel; reverse-Z, so the larger device depth is the closer surface.
    var swWins = false;
    var swPixel = SparseVoxelVGSwPixel(0u, 0u, 0u, 0xffffffffu);
    if (pixelIndex < arrayLength(&swResolvedPayloads)) {
        let candidate = swResolvedPayloads[pixelIndex];
        let swDepth = f32(candidate.depthBits) * (1.0 / 4294967295.0);
        if (candidate.depthBits != 0u &&
            candidate.denseInstance != 0xffffffffu &&
            swDepth > deviceDepth) {
            swWins = true;
            swPixel = candidate;
            deviceDepth = swDepth;
        }
    }

    if (deviceDepth <= raster.gbufferClearDepth) {
        resolvedSurfaces[pixelIndex] = ResolvedVoxelSurface(
            0.0,
            SPARSE_VOXEL_INVALID_INDEX,
            SPARSE_VOXEL_INVALID_INDEX,
            0u, 0u, 0u, 0u, 0u);
        coverage[pixelIndex] = 0u;
        linearDepth[pixelIndex] = 0.0;
        microPayloads[pixelIndex] = 0u;
        materialSampleAddresses[pixelIndex] =
            VoxelMaterialSampleAddress(vec3<i32>(0), 0u);
        terrainOwnershipIds[pixelIndex] = 0u;
        textureStore(baseColorTexture, pixelI, vec4<f32>(0.0));
        textureStore(materialPropertiesTexture, pixelI, vec4<f32>(1.0, 0.0, 0.0, 0.0));
        textureStore(emissiveTexture, pixelI, vec4<f32>(0.0));
        textureStore(normalTexture, pixelI, vec4<f32>(0.0, 0.0, 1.0, 0.0));
        return;
    }

    var idsLo = textureLoad(packedGeometryIdsLoTexture, pixelI, 0).x;
    var idsHi = textureLoad(packedGeometryIdsHiTexture, pixelI, 0).x;
    var geometryNormal = textureLoad(geometryNormalTexture, pixelI, 0);
    if (swWins) {
        idsLo = swPixel.denseInstance;
        idsHi = swPixel.payload;
        geometryNormal = vec4<f32>(vg_adapter_oct_decode(swPixel.normalOct), 1.0);
    }
    let instanceIndex = idsLo;
    let triangleId = idsHi & VG_ADAPTER_PAYLOAD_TRIANGLE_MASK;
    // Real per-voxel materials are still to be wired through the mesh path;
    // until then a fixed terrain albedo keeps lighting comparable with the
    // brick route instead of hashing cluster ids into noise. City voxels carry
    // their exact colour from the visibility resolve. The software raster
    // winner has no resolve colour, so it keeps the fixed albedo.
    var debugColor = vec3<f32>(0.301, 0.271, 0.232);
    var roughness = 0.92;
    var cityVoxel = false;
    if (!swWins && all(textureDimensions(cityAlbedoTexture) == dimensions)) {
        let city = textureLoad(cityAlbedoTexture, pixelI, 0);
        if (city.a > 0.5) {
            debugColor = city.rgb;
            roughness = 0.85;
            cityVoxel = true;
        }
    }
    if (!cityVoxel) {
        let viewDepthForGround = settings.depthConversion.y /
            max(deviceDepth + settings.depthConversion.x, 1.0e-8);
        let viewPoint = vg_adapter_camera_direction(vec2<f32>(pixel)) * viewDepthForGround;
        let world = (raster.cameraToWorld * vec4<f32>(viewPoint, 1.0)).xyz;
        let ground = vg_adapter_city_ground(world);
        if (ground.a > 0.5) {
            debugColor = ground.rgb;
        }
    }

    let cameraDirection = vg_adapter_camera_direction(vec2<f32>(pixel));
    let viewDepth = settings.depthConversion.y /
        max(deviceDepth + settings.depthConversion.x, 1.0e-8);
    let rayDistance = viewDepth * length(cameraDirection);

    resolvedSurfaces[pixelIndex] = ResolvedVoxelSurface(
        rayDistance,
        instanceIndex,
        idsHi,
        triangleId,
        SPARSE_VOXEL_SURFACE_HIT | SPARSE_VOXEL_SURFACE_PACKED_MATERIAL,
        vg_adapter_pack_rgb8(debugColor, roughness),
        0u,
        idsLo);
    coverage[pixelIndex] = 1u;
    linearDepth[pixelIndex] = rayDistance;
    microPayloads[pixelIndex] = 0u;
    materialSampleAddresses[pixelIndex] =
        VoxelMaterialSampleAddress(vec3<i32>(0), 0u);
    // Ownership must be stable for a surface across frames; the instance is
    // the chunk, which is exactly the ownership the terrain passes expect.
    terrainOwnershipIds[pixelIndex] = instanceIndex;
    textureStore(baseColorTexture, pixelI, vec4<f32>(debugColor, 1.0));
    textureStore(materialPropertiesTexture, pixelI, vec4<f32>(roughness, 0.0, 0.0, 1.0));
    textureStore(emissiveTexture, pixelI, vec4<f32>(0.0));
    textureStore(normalTexture, pixelI, geometryNormal);
}
