// Header: depth width, height, words per tile, enabled. Exact bitsets have no
// per-tile light overflow. Missing/incompatible lists fall back to all lights.
fn vsm_next_tile_light(start: u32, pixel: vec2<u32>, dims: vec2<u32>, lightCount: u32) -> u32 {
    if (start >= lightCount) { return lightCount; }
    if (lightTiles[3] == 0u || lightTiles[0] != dims.x || lightTiles[1] != dims.y) { return start; }
    let words = lightTiles[2];
    if (words < (lightCount+31u)/32u) { return start; }
    let tile = (pixel.y/16u)*((dims.x+15u)/16u) + pixel.x/16u;
    var word = start/32u;
    var bits = lightTiles[4u+tile*words+word] & (0xffffffffu << (start%32u));
    loop {
        if (bits != 0u) { return min(word*32u + firstTrailingBit(bits),lightCount); }
        word += 1u;
        if (word >= (lightCount+31u)/32u) { return lightCount; }
        bits = lightTiles[4u+tile*words+word];
    }
    return lightCount;
}
