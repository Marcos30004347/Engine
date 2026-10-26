// City ground on the GPU: the same evaluation as CityGroundOverlay::sample,
// from the word buffer CityGroundOverlay::packGPU writes. The includer
// declares `cityGroundWords : array<u32>` (read-only storage).
//
// Header words: 0 enabled, 1-3 ground width/height/spacing mm, 4-5 ground
// origin mm (site-local), 6-7 site origin (world metres, f32), 8 height
// offset (f32), 9-11 paint width/height/spacing mm, 12-16 prism bins (min x,
// min z, count x, count z, size mm), 17 ground offset, 18 weight bytes, 19
// paint bytes, 20 bin firsts, 21 bin prisms, 22 prisms (8 words each), 23
// rings (first vertex, count), 24 vertices (x, z mm), 25 prism count.

struct CityGroundSample {
    heightMetres : f32,
    weight : f32,
    material : u32,  // 255: keep the map's material.
    paved : bool,
}

fn city_ground_i32(index : u32) -> i32 { return bitcast<i32>(cityGroundWords[index]); }
fn city_ground_f32(index : u32) -> f32 { return bitcast<f32>(cityGroundWords[index]); }
fn city_ground_byte(section : u32, index : u32) -> u32 {
    return (cityGroundWords[section + index / 4u] >> (8u * (index % 4u))) & 255u;
}

// Even-odd over every ring of one prism, in site-local millimetres.
fn city_ground_inside(prism : u32, x : f32, z : f32) -> bool {
    let record = cityGroundWords[22u] + prism * 8u;
    let firstRing = cityGroundWords[record + 6u];
    let ringCount = cityGroundWords[record + 7u];
    var inside = false;
    for (var r = 0u; r < ringCount; r++) {
        let ring = cityGroundWords[23u] + (firstRing + r) * 2u;
        let first = cityGroundWords[ring];
        let count = cityGroundWords[ring + 1u];
        var j = count - 1u;
        for (var i = 0u; i < count; i++) {
            let vi = cityGroundWords[24u] + (first + i) * 2u;
            let vj = cityGroundWords[24u] + (first + j) * 2u;
            let xi = f32(city_ground_i32(vi));
            let zi = f32(city_ground_i32(vi + 1u));
            let xj = f32(city_ground_i32(vj));
            let zj = f32(city_ground_i32(vj + 1u));
            if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) {
                inside = !inside;
            }
            j = i;
        }
    }
    return inside;
}

// `worldX`/`worldZ` in map metres (the terrain's world frame).
fn city_ground_sample(worldX : f32, worldZ : f32) -> CityGroundSample {
    var result = CityGroundSample(0.0, 0.0, 255u, false);
    if (arrayLength(&cityGroundWords) < 32u || cityGroundWords[0] == 0u) {
        return result;
    }
    let x = worldX - city_ground_f32(6u);
    let z = worldZ - city_ground_f32(7u);
    // Graded ground, bilinear, with its blend weight.
    let width = cityGroundWords[1];
    let height = cityGroundWords[2];
    let spacing = f32(cityGroundWords[3]);
    let gx = (x * 1000.0 - f32(city_ground_i32(4u))) / spacing;
    let gz = (z * 1000.0 - f32(city_ground_i32(5u))) / spacing;
    if (!(gx >= 0.0 && gz >= 0.0 && gx <= f32(width - 1u) && gz <= f32(height - 1u))) {
        return result;
    }
    let x0 = min(u32(gx), width - 2u);
    let z0 = min(u32(gz), height - 2u);
    let fx = gx - f32(x0);
    let fz = gz - f32(z0);
    let a = z0 * width + x0;
    let b = a + 1u;
    let c = a + width;
    let d = c + 1u;
    let ground = cityGroundWords[17u];
    let weights = cityGroundWords[18u];
    let ha = f32(city_ground_i32(ground + a));
    let hb = f32(city_ground_i32(ground + b));
    let hc = f32(city_ground_i32(ground + c));
    let hd = f32(city_ground_i32(ground + d));
    result.heightMetres = ((ha * (1.0 - fx) + hb * fx) * (1.0 - fz) + (hc * (1.0 - fx) + hd * fx) * fz) * 0.001;
    let wa = f32(city_ground_byte(weights, a));
    let wb = f32(city_ground_byte(weights, b));
    let wc = f32(city_ground_byte(weights, c));
    let wd = f32(city_ground_byte(weights, d));
    result.weight = ((wa * (1.0 - fx) + wb * fx) * (1.0 - fz) + (wc * (1.0 - fx) + wd * fx) * fz) / 255.0;
    if (result.weight <= 0.0) {
        return result;
    }
    // Unpaved paint.
    let paintWidth = cityGroundWords[9];
    let paintHeight = cityGroundWords[10];
    if (paintWidth != 0u) {
        let paintSpacing = f32(cityGroundWords[11]);
        let px = i32(floor((x * 1000.0 - f32(city_ground_i32(4u))) / paintSpacing));
        let pz = i32(floor((z * 1000.0 - f32(city_ground_i32(5u))) / paintSpacing));
        if (px >= 0 && pz >= 0 && u32(px) < paintWidth && u32(pz) < paintHeight) {
            result.material = city_ground_byte(cityGroundWords[19u], u32(pz) * paintWidth + u32(px));
        }
    }
    // The highest paving prism over the point.
    let binsX = cityGroundWords[14];
    let binsZ = cityGroundWords[15];
    if (binsX == 0u) {
        return result;
    }
    let mx = x * 1000.0;
    let mz = z * 1000.0;
    let binSize = f32(cityGroundWords[16]);
    let bx = i32(floor(mx / binSize)) - city_ground_i32(12u);
    let bz = i32(floor(mz / binSize)) - city_ground_i32(13u);
    if (bx < 0 || bz < 0 || u32(bx) >= binsX || u32(bz) >= binsZ) {
        return result;
    }
    let bin = u32(bz) * binsX + u32(bx);
    let firsts = cityGroundWords[20u];
    let prisms = cityGroundWords[21u];
    var best = -3.0e38;
    var found = false;
    var topMaterial = 0u;
    for (var k = cityGroundWords[firsts + bin]; k < cityGroundWords[firsts + bin + 1u]; k++) {
        let prism = cityGroundWords[prisms + k];
        let record = cityGroundWords[22u] + prism * 8u;
        if (mx < f32(city_ground_i32(record)) || mz < f32(city_ground_i32(record + 1u)) ||
            mx > f32(city_ground_i32(record + 2u)) || mz > f32(city_ground_i32(record + 3u))) {
            continue;
        }
        if (!city_ground_inside(prism, mx, mz)) {
            continue;
        }
        let flags = cityGroundWords[record + 5u];
        let top = f32(city_ground_i32(record + 4u)) * 0.001 +
            select(0.0, result.heightMetres, (flags & 1u) != 0u);
        if (top > best) {
            best = top;
            found = true;
            topMaterial = (flags >> 8u) & 255u;
        }
    }
    if (found) {
        result.heightMetres = best;
        result.material = topMaterial;
        result.paved = true;
    }
    return result;
}

// The renderer-frame city height: the overlay's height plus the datum offset.
fn city_ground_height_offset() -> f32 {
    return city_ground_f32(8u);
}
