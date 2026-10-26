#ifndef MAP_EXPORT_ADDRESSING_WGSL
#define MAP_EXPORT_ADDRESSING_WGSL

fn map_export_page_table_base(level : u32) -> u32
{
    return ((1u << (level * 2u)) - 1u) / 3u;
}

fn map_export_page_morton(coordinate : vec2<u32>, level : u32) -> u32
{
    var morton = 0u;
    for (var bit = 0u; bit < level; bit++)
    {
        morton |= ((coordinate.x >> bit) & 1u) << (bit * 2u);
        morton |= ((coordinate.y >> bit) & 1u) << (bit * 2u + 1u);
    }
    return morton;
}

#endif
