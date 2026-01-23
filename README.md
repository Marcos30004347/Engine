TODO:
add allocator classes
optmize job allocation in async
add App class
add basic trinagle rendering
add basic compute shaders

- Example compile wgsl into spirv
./scripts/wgsl2spirv.py ./assets/shaders/sort/wgsl/radixsort.wgsl -o ./build/tests/assets/shaders/spirv/radixsort.spirv

- Example compile wgsl into hlsl
./scripts/wgsl2hlsl.py ./assets/shaders/sort/wgsl/radixsort.wgsl -o ./build/tests/assets/shaders/hlsl/radix_sort