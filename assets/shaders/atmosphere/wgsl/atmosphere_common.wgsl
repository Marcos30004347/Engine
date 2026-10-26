const ATMOSPHERE_PI : f32 = 3.14159265358979323846;
const ATMOSPHERE_TWO_PI : f32 = 6.28318530717958647692;
const ATMOSPHERE_EPSILON : f32 = 1.0e-5;

struct AtmosphereParameters {
    // x ground radius km, y top radius km, z atmosphere height km, w Mie g.
    radiiMieG : vec4<f32>,
    // rgb km^-1, w scale height km.
    rayleighScatteringScale : vec4<f32>,
    mieScatteringScale : vec4<f32>,
    // rgb km^-1, w ozone centre km.
    mieAbsorptionOzoneCentre : vec4<f32>,
    // rgb km^-1, w ozone half width km.
    ozoneAbsorptionHalfWidth : vec4<f32>,
    // rgb albedo, w engine radiance scale.
    groundAlbedoRadianceScale : vec4<f32>,
    // xyz direction light travels, w intensity.
    sunDirectionIntensity : vec4<f32>,
    // rgb colour, w angular radius radians.
    sunColorAngularRadius : vec4<f32>,
}

struct AtmosphereMediumSample {
    scattering : vec3<f32>,
    extinction : vec3<f32>,
    rayleighScattering : vec3<f32>,
    mieScattering : vec3<f32>,
}

fn atmosphere_safe_sqrt(value : f32) -> f32 {
    return sqrt(max(value, 0.0));
}

fn atmosphere_safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let lengthSquared = dot(value, value);
    return select(fallback, value * inverseSqrt(max(lengthSquared, 1.0e-12)),
        lengthSquared > 1.0e-12);
}

fn atmosphere_ray_sphere(origin : vec3<f32>, direction : vec3<f32>, radius : f32) -> vec2<f32> {
    let b = dot(origin, direction);
    let c = dot(origin, origin) - radius * radius;
    let discriminant = b * b - c;
    if (discriminant < 0.0) {
        return vec2<f32>(1.0e30, -1.0e30);
    }
    let root = atmosphere_safe_sqrt(discriminant);
    return vec2<f32>(-b - root, -b + root);
}

fn atmosphere_nearest_positive(interval : vec2<f32>) -> f32 {
    if (interval.x > ATMOSPHERE_EPSILON) {
        return interval.x;
    }
    return select(-1.0, interval.y, interval.y > ATMOSPHERE_EPSILON);
}

fn atmosphere_distance_to_top(parameters : AtmosphereParameters, radius : f32,
    viewZenithCosine : f32) -> f32 {
    let top = parameters.radiiMieG.y;
    let mu = clamp(viewZenithCosine, -1.0, 1.0);
    return max(-radius * mu + atmosphere_safe_sqrt(
        radius * radius * (mu * mu - 1.0) + top * top), 0.0);
}

fn atmosphere_medium(parameters : AtmosphereParameters, altitudeKm : f32) -> AtmosphereMediumSample {
    let altitude = max(altitudeKm, 0.0);
    if (altitude > parameters.radiiMieG.z) {
        return AtmosphereMediumSample(vec3<f32>(0.0), vec3<f32>(0.0),
            vec3<f32>(0.0), vec3<f32>(0.0));
    }
    let rayleighDensity = exp(-altitude /
        max(parameters.rayleighScatteringScale.w, ATMOSPHERE_EPSILON));
    let mieDensity = exp(-altitude /
        max(parameters.mieScatteringScale.w, ATMOSPHERE_EPSILON));
    let ozoneDensity = max(0.0, 1.0 - abs(altitude -
        parameters.mieAbsorptionOzoneCentre.w) /
        max(parameters.ozoneAbsorptionHalfWidth.w, ATMOSPHERE_EPSILON));
    let rayleigh = parameters.rayleighScatteringScale.xyz * rayleighDensity;
    let mie = parameters.mieScatteringScale.xyz * mieDensity;
    let extinction = rayleigh + mie +
        parameters.mieAbsorptionOzoneCentre.xyz * mieDensity +
        parameters.ozoneAbsorptionHalfWidth.xyz * ozoneDensity;
    return AtmosphereMediumSample(rayleigh + mie, extinction, rayleigh, mie);
}

fn atmosphere_rayleigh_phase(cosine : f32) -> f32 {
    return 3.0 * (1.0 + cosine * cosine) / (16.0 * ATMOSPHERE_PI);
}

fn atmosphere_mie_phase(cosineValue : f32, anisotropy : f32) -> f32 {
    let cosine = clamp(cosineValue, -1.0, 1.0);
    let g = clamp(anisotropy, -0.99, 0.99);
    let g2 = g * g;
    let denominator = max(pow(1.0 + g2 - 2.0 * g * cosine, 1.5), 1.0e-4);
    return 3.0 * (1.0 - g2) * (1.0 + cosine * cosine) /
        (8.0 * ATMOSPHERE_PI * (2.0 + g2) * denominator);
}

fn atmosphere_transmittance_uv(parameters : AtmosphereParameters,
    radius : f32, viewZenithCosine : f32) -> vec2<f32> {
    let ground = parameters.radiiMieG.x;
    let top = parameters.radiiMieG.y;
    let h = atmosphere_safe_sqrt(top * top - ground * ground);
    let r = clamp(radius, ground, top);
    let rho = atmosphere_safe_sqrt(r * r - ground * ground);
    let distance = atmosphere_distance_to_top(parameters, r, viewZenithCosine);
    let minimumDistance = top - r;
    let maximumDistance = rho + h;
    let u = (distance - minimumDistance) /
        max(maximumDistance - minimumDistance, ATMOSPHERE_EPSILON);
    return clamp(vec2<f32>(u, rho / max(h, ATMOSPHERE_EPSILON)),
        vec2<f32>(0.0), vec2<f32>(1.0));
}

fn atmosphere_transmittance_parameters(parameters : AtmosphereParameters,
    uvValue : vec2<f32>) -> vec2<f32> {
    let uv = clamp(uvValue, vec2<f32>(0.0), vec2<f32>(1.0));
    let ground = parameters.radiiMieG.x;
    let top = parameters.radiiMieG.y;
    let h = atmosphere_safe_sqrt(top * top - ground * ground);
    let rho = h * uv.y;
    let radius = atmosphere_safe_sqrt(rho * rho + ground * ground);
    let minimumDistance = top - radius;
    let maximumDistance = rho + h;
    let distance = mix(minimumDistance, maximumDistance, uv.x);
    var cosine = 1.0;
    if (distance > ATMOSPHERE_EPSILON) {
        cosine = (h * h - rho * rho - distance * distance) /
            max(2.0 * radius * distance, ATMOSPHERE_EPSILON);
    }
    return vec2<f32>(radius, clamp(cosine, -1.0, 1.0));
}

fn atmosphere_texture_pixel(uvValue : vec2<f32>, dimensions : vec2<u32>) -> vec2<i32> {
    let maximum = max(dimensions, vec2<u32>(1u)) - vec2<u32>(1u);
    return vec2<i32>(min(vec2<u32>(clamp(uvValue, vec2<f32>(0.0),
        vec2<f32>(1.0)) * vec2<f32>(maximum) + vec2<f32>(0.5)), maximum));
}

fn atmosphere_aerial_distance(normalizedDepth : f32,
    maximumDistanceKm : f32, distributionScaleKm : f32) -> f32 {
    let maximumDistance = max(maximumDistanceKm, 0.0);
    let scale = max(distributionScaleKm, 1.0e-6);
    return scale * (exp(clamp(normalizedDepth, 0.0, 1.0) *
        log(1.0 + maximumDistance / scale)) - 1.0);
}

fn atmosphere_aerial_depth(distanceKm : f32, maximumDistanceKm : f32,
    distributionScaleKm : f32) -> f32 {
    let maximumDistance = max(maximumDistanceKm, 0.0);
    if (maximumDistance <= 0.0) { return 0.0; }
    let scale = max(distributionScaleKm, 1.0e-6);
    return clamp(log(1.0 + clamp(distanceKm, 0.0, maximumDistance) / scale) /
        log(1.0 + maximumDistance / scale), 0.0, 1.0);
}

fn atmosphere_sample_transmittance(lut : texture_2d<f32>,
    parameters : AtmosphereParameters, radius : f32,
    viewZenithCosine : f32) -> vec3<f32> {
    return clamp(textureLoad(lut, atmosphere_texture_pixel(
        atmosphere_transmittance_uv(parameters, radius, viewZenithCosine),
        textureDimensions(lut)), 0).rgb, vec3<f32>(0.0), vec3<f32>(1.0));
}

fn atmosphere_sample_multiple_scattering(lut : texture_2d<f32>,
    parameters : AtmosphereParameters, altitudeKm : f32,
    sunZenithCosine : f32) -> vec3<f32> {
    let uv = vec2<f32>(sunZenithCosine * 0.5 + 0.5,
        altitudeKm / max(parameters.radiiMieG.z, ATMOSPHERE_EPSILON));
    return max(textureLoad(lut,
        atmosphere_texture_pixel(uv, textureDimensions(lut)), 0).rgb,
        vec3<f32>(0.0));
}

fn atmosphere_planet_occludes(position : vec3<f32>, directionTowardSun : vec3<f32>,
    groundRadius : f32) -> bool {
    return atmosphere_nearest_positive(atmosphere_ray_sphere(
        position, directionTowardSun, groundRadius)) > 0.0;
}

fn atmosphere_sky_view_direction(parameters : AtmosphereParameters,
    uvValue : vec2<f32>, sunZenithCosine : f32) -> vec3<f32> {
    let y = clamp(uvValue.y * 2.0 - 1.0, -1.0, 1.0);
    let latitudeNormalized = sign(y) * y * y;
    let elevation = latitudeNormalized * 0.5 * ATMOSPHERE_PI;
    let azimuth = (uvValue.x - 0.5) * ATMOSPHERE_TWO_PI;
    let horizontal = cos(elevation);
    // The projected sun is +X in this local frame.
    return vec3<f32>(horizontal * cos(azimuth), sin(elevation),
        horizontal * sin(azimuth));
}

fn atmosphere_sky_view_uv(direction : vec3<f32>, up : vec3<f32>,
    directionTowardSun : vec3<f32>) -> vec2<f32> {
    let safeUp = atmosphere_safe_normalize(up, vec3<f32>(0.0, 1.0, 0.0));
    let view = atmosphere_safe_normalize(direction, safeUp);
    let mu = clamp(dot(view, safeUp), -1.0, 1.0);
    let latitudeNormalized = asin(mu) / (0.5 * ATMOSPHERE_PI);
    let encodedLatitude = sign(latitudeNormalized) * sqrt(abs(latitudeNormalized));
    var sunTangent = directionTowardSun - safeUp * dot(directionTowardSun, safeUp);
    let fallback = atmosphere_safe_normalize(cross(select(vec3<f32>(1.0, 0.0, 0.0),
        vec3<f32>(0.0, 0.0, 1.0), abs(safeUp.z) < 0.9), safeUp),
        vec3<f32>(1.0, 0.0, 0.0));
    sunTangent = atmosphere_safe_normalize(sunTangent, fallback);
    let bitangent = cross(sunTangent, safeUp);
    let horizontal = view - safeUp * mu;
    let azimuth = atan2(dot(horizontal, bitangent), dot(horizontal, sunTangent));
    return vec2<f32>(fract(azimuth / ATMOSPHERE_TWO_PI + 0.5),
        clamp(encodedLatitude * 0.5 + 0.5, 0.0, 1.0));
}
