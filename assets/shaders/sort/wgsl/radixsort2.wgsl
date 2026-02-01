//! GPU Radix Sort - Common Constants and Utilities
//!
//! This module defines shared constants for the GPU radix sort implementation.
//! The sort processes 32-bit keys in multiple passes, handling 4 bits per pass
//! using 16 histogram bins.
//!
//! Radix sort overview:
//! - Processes 32-bit keys in 8 passes (4 bits per pass)
//! - Each pass sorts into 16 bins (2^4)
//! - Stable sort (preserves relative order of equal keys)
//! - Sorts both keys and associated values
//!
//! Workgroup configuration:
//! - 256 threads per workgroup
//! - 4 elements per thread
//! - 1024 elements per workgroup (BLOCK_SIZE)
//!
//! Performance: O(k*n) where k=8 passes, linear in practice
//! Memory: O(n) for keys/values + O(workgroups * 16) for histograms
//!
//! Mostly copied from [wgmath](https://github.com/wgmath/wgmath)
//! (Apache-2.0 license).

const OFFSET: u32 = 42;
/// Workgroup size (threads per workgroup).
const WG: u32 = 256;

/// Number of bits processed per radix sort pass.
const BITS_PER_PASS: u32 = 4;
/// Number of histogram bins (2^BITS_PER_PASS).
const BIN_COUNT: u32 = 1u << BITS_PER_PASS;
/// Total histogram size across all threads in a workgroup.
const HISTOGRAM_SIZE: u32 = WG * BIN_COUNT;
/// Number of elements each thread processes.
const ELEMENTS_PER_THREAD: u32 = 4;

/// Total elements processed by one workgroup.
const BLOCK_SIZE = WG * ELEMENTS_PER_THREAD;

/// Integer division with ceiling (rounds up).
fn div_ceil(a: u32, b: u32) -> u32 {
    return (a + b - 1u) / b;
}


//! Radix Sort Count (Histogram) Kernel
//!
//! First pass of radix sort: computes per-workgroup histograms for the current 4-bit digit.
//!
//! Algorithm:
//! 1. Each workgroup processes BLOCK_SIZE (1024) consecutive elements
//! 2. Initialize shared memory histogram to zeros
//! 3. Each thread processes ELEMENTS_PER_THREAD (4) elements
//! 4. Extract 4-bit key from current shift position
//! 5. Atomically increment corresponding histogram bin
//! 6. Write per-workgroup histogram to global memory
//!
//! Output layout: counts[bin * num_workgroups + workgroup_id]
//! This produces num_workgroups separate histograms, one per workgroup.
//!
//! Workgroup size: 256 threads
//! Shared memory: 16 atomic counters (one per bin)

struct Uniforms {
    /// Bit shift amount for this sort pass (0, 4, 8, 12, ..., 28).
    shift: u32,
}

@group(0) @binding(0) var<uniform> config: Uniforms;
@group(0) @binding(1) var<storage, read> num_keys_arr: array<u32>;
@group(0) @binding(2) var<storage, read> src: array<u32>;
@group(0) @binding(3) var<storage, read_write> counts: array<u32>;
@group(0) @binding(4) var<storage, read> values: array<u32>;
@group(0) @binding(5) var<storage, read_write> out: array<u32>;
@group(0) @binding(6) var<storage, read_write> out_values: array<u32>;
@group(0) @binding(7) var<storage, read_write> reduced: array<u32>;
@group(0) @binding(8) var<storage, read_write> debug: array<u32>;

var<workgroup> lds_sums: array<u32, WG>;
var<workgroup> lds_scratch: array<u32, WG>;
var<workgroup> bin_offset_cache: array<u32, WG>;
var<workgroup> local_histogram: array<atomic<u32>, BIN_COUNT>;
var<workgroup> histogram: array<atomic<u32>, BIN_COUNT>;
var<workgroup> sums: array<u32, WG>;
var<workgroup> lds: array<array<u32, WG>, ELEMENTS_PER_THREAD>;


@compute @workgroup_size(WG)
fn clear_counts(
    @builtin(global_invocation_id) gid: vec3<u32>,
) {
    let num_wgs = div_ceil(num_keys_arr[0], BLOCK_SIZE);
    let total = num_wgs * BIN_COUNT;

    let idx = gid.x;
    if (idx < total) {
        counts[idx] = 0u;
    }
}

@compute @workgroup_size(WG, 1, 1)
fn sort_count(
    @builtin(local_invocation_id) local_id: vec3<u32>,
    @builtin(workgroup_id) gid: vec3<u32>,
) {
    debug[0] = config.shift;

    let num_keys = num_keys_arr[0];

    // let num_keys = num_keys_arr[0];
    let num_wgs = div_ceil(num_keys, BLOCK_SIZE);
    let group_id = gid.x;

    if group_id >= num_wgs {
        return;
    }

    if local_id.x < BIN_COUNT {
        histogram[local_id.x] = 0u;
    }

    workgroupBarrier();

    let wg_block_start = BLOCK_SIZE * group_id;
    var block_index = wg_block_start + local_id.x;
    let shift_bit = config.shift;
    var data_index = block_index;

    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        if data_index < num_keys {
            let local_key = (src[data_index] >> shift_bit) & 0xfu;
            atomicAdd(&histogram[local_key], 1u);
        }
        data_index += WG;
    }
    block_index += BLOCK_SIZE;
    workgroupBarrier();
    if local_id.x < BIN_COUNT {
        let num_wgs = div_ceil(num_keys, BLOCK_SIZE);
        counts[local_id.x * num_wgs + group_id] = histogram[local_id.x];
    }
}


@compute @workgroup_size(WG, 1, 1)
fn sort_reduce(
    @builtin(local_invocation_id) local_id: vec3<u32>,
    @builtin(workgroup_id) gid: vec3<u32>,
) {
    let num_keys = num_keys_arr[0];
    let num_wgs = div_ceil(num_keys, BLOCK_SIZE);
    let num_reduce_wgs = BIN_COUNT * div_ceil(num_wgs, BLOCK_SIZE);

    let group_id = gid.x;

    if group_id >= num_reduce_wgs {
        return;
    }

    let num_reduce_wg_per_bin = num_reduce_wgs / BIN_COUNT;
    let bin_id = group_id / num_reduce_wg_per_bin;

    let bin_offset = bin_id * num_wgs;
    let base_index = (group_id % num_reduce_wg_per_bin) * BLOCK_SIZE;
    var sum = 0u;

    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        let data_index = base_index + i * WG + local_id.x;
        if data_index < num_wgs {
            sum += counts[bin_offset + data_index];
        }
    }

    sums[local_id.x] = sum;

    for (var i = 0u; i < 8u; i++) {
        workgroupBarrier();
        if local_id.x < ((WG / 2u) >> i) {
            sum += sums[local_id.x + ((WG / 2u) >> i)];
            sums[local_id.x] = sum;
        }
    }

    if local_id.x == 0u {
        reduced[group_id] = sum;
    }
}


@compute @workgroup_size(WG, 1, 1)
fn sort_scan(
    @builtin(local_invocation_id) local_id: vec3<u32>,
    @builtin(workgroup_id) group_id: vec3<u32>,
) {
    let num_keys = num_keys_arr[0];
    // let num_keys = num_keys_arr[0];
    let num_wgs = div_ceil(num_keys, BLOCK_SIZE);
    let num_reduce_wgs = BIN_COUNT * div_ceil(num_wgs, BLOCK_SIZE);

    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        let data_index = i * WG + local_id.x;
        let col = (i * WG + local_id.x) / ELEMENTS_PER_THREAD;
        let row = (i * WG + local_id.x) % ELEMENTS_PER_THREAD;
        lds[row][col] = reduced[data_index];
    }
    workgroupBarrier();
    var sum = 0u;
    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        let tmp = lds[i][local_id.x];
        lds[i][local_id.x] = sum;
        sum += tmp;
    }
    // workgroup prefix sum
    sums[local_id.x] = sum;
    for (var i = 0u; i < 8u; i++) {
        workgroupBarrier();
        if local_id.x >= (1u << i) {
            sum += sums[local_id.x - (1u << i)];
        }
        workgroupBarrier();
        sums[local_id.x] = sum;
    }
    workgroupBarrier();
    sum = 0u;
    if local_id.x > 0u {
        sum = sums[local_id.x - 1u];
    }
    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        lds[i][local_id.x] += sum;
    }
    // lds now contains exclusive prefix sum
    workgroupBarrier();
    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        let data_index = i * WG + local_id.x;
        let col = (i * WG + local_id.x) / ELEMENTS_PER_THREAD;
        let row = (i * WG + local_id.x) % ELEMENTS_PER_THREAD;
        if data_index < num_reduce_wgs {
            reduced[data_index] = lds[row][col];
        }
    }
}


@compute @workgroup_size(WG, 1, 1)
fn sort_scan_add(
    @builtin(local_invocation_id) local_id: vec3<u32>,
    @builtin(workgroup_id) gid: vec3<u32>,
) {
    let num_keys = num_keys_arr[0];
    // let num_keys = num_keys_arr[0];
    let num_wgs = div_ceil(num_keys, BLOCK_SIZE);
    let num_reduce_wgs = BIN_COUNT * div_ceil(num_wgs, BLOCK_SIZE);

    let group_id = gid.x;

    if group_id >= num_reduce_wgs {
        return;
    }

    let num_reduce_wg_per_bin = num_reduce_wgs / BIN_COUNT;

    let bin_id = group_id / num_reduce_wg_per_bin;
    let bin_offset = bin_id * num_wgs;
    let base_index = (group_id % num_reduce_wg_per_bin) * ELEMENTS_PER_THREAD * WG;

    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        let data_index = base_index + i * WG + local_id.x;
        let col = (i * WG + local_id.x) / ELEMENTS_PER_THREAD;
        let row = (i * WG + local_id.x) % ELEMENTS_PER_THREAD;
        // This is not gated, we let robustness do it for us
        lds[row][col] = counts[bin_offset + data_index];
    }
    workgroupBarrier();
    var sum = 0u;
    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        let tmp = lds[i][local_id.x];
        lds[i][local_id.x] = sum;
        sum += tmp;
    }
    // workgroup prefix sum
    sums[local_id.x] = sum;
    for (var i = 0u; i < 8u; i++) {
        workgroupBarrier();
        if local_id.x >= (1u << i) {
            sum += sums[local_id.x - (1u << i)];
        }
        workgroupBarrier();
        sums[local_id.x] = sum;
    }
    workgroupBarrier();
    sum = reduced[group_id];
    if local_id.x > 0u {
        sum += sums[local_id.x - 1u];
    }
    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        lds[i][local_id.x] += sum;
    }
    // lds now contains exclusive prefix sum
    // Note: storing inclusive might be slightly cheaper here
    workgroupBarrier();
    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        let data_index = base_index + i * WG + local_id.x;
        let col = (i * WG + local_id.x) / ELEMENTS_PER_THREAD;
        let row = (i * WG + local_id.x) % ELEMENTS_PER_THREAD;
        if data_index < num_wgs {
            counts[bin_offset + data_index] = lds[row][col];
        }
    }
}

fn get_workgroup_id(wid: vec3u, num_wgs: vec3u) -> u32 {
    return wid.x + wid.y * num_wgs.x;
}

@compute @workgroup_size(WG, 1, 1)
fn sort_scatter(
    @builtin(local_invocation_id) local_id: vec3<u32>,
    @builtin(workgroup_id) wid: vec3<u32>,
    @builtin(num_workgroups) num_workgroups: vec3<u32>,
) {
    let num_keys = num_keys_arr[0];
    // let num_keys = num_keys_arr[0];
    let num_wgs = div_ceil(num_keys, BLOCK_SIZE);

    let group_id = get_workgroup_id(wid, num_workgroups);

    if group_id >= num_wgs {
        return;
    }

    if local_id.x < BIN_COUNT {
        bin_offset_cache[local_id.x] = counts[local_id.x * num_wgs + group_id];
    }
    workgroupBarrier();
    let wg_block_start = BLOCK_SIZE * group_id;
    let block_index = wg_block_start + local_id.x;
    var data_index = block_index;
    for (var i = 0u; i < ELEMENTS_PER_THREAD; i++) {
        if local_id.x < BIN_COUNT {
            local_histogram[local_id.x] = 0u;
        }
        var local_key = ~0u;
        var local_value = 0u;

        if data_index < num_keys {
            local_key = src[data_index];
            local_value = values[data_index];
        }
        

        for (var bit_shift = 0u; bit_shift < BITS_PER_PASS; bit_shift += 2u) {
            let key_index = (local_key >> config.shift) & 0xfu;
            let bit_key = (key_index >> bit_shift) & 3u;
            var packed_histogram = 1u << (bit_key * 8u);
            // workgroup prefix sum
            var sum = packed_histogram;
            
            lds_scratch[local_id.x] = sum;
            for (var i = 0u; i < 8u; i++) {
                workgroupBarrier();
                if local_id.x >= (1u << i) {
                    sum += lds_scratch[local_id.x - (1u << i)];
                }
                workgroupBarrier();
                lds_scratch[local_id.x] = sum;
            }
               
            debug[local_id.x * ELEMENTS_PER_THREAD + i] += lds_scratch[local_id.x];
            // For some reason this leads to a different result, in the line above we store just zeroes, in the line with += sum, we get meaningful data
            //debug[local_id.x * ELEMENTS_PER_THREAD + i] += sum;
          
            workgroupBarrier();
            packed_histogram = lds_scratch[WG - 1u];

            packed_histogram = (packed_histogram << 8u) + (packed_histogram << 16u) + (packed_histogram << 24u);
            var local_sum = packed_histogram;
            
            if local_id.x > 0u {
                local_sum += lds_scratch[local_id.x - 1u];
            }

            let key_offset = (local_sum >> (bit_key * 8u)) & 0xffu;

            lds_sums[key_offset] = local_key;
            workgroupBarrier();
            local_key = lds_sums[local_id.x];
            workgroupBarrier();

            lds_sums[key_offset] = local_value;
            workgroupBarrier();
            local_value = lds_sums[local_id.x];
            workgroupBarrier();
        }

        let key_index = (local_key >> config.shift) & 0xfu;
        atomicAdd(&local_histogram[key_index], 1u);
        workgroupBarrier();
        var histogram_local_sum = 0u;

        if local_id.x < BIN_COUNT {
            histogram_local_sum = local_histogram[local_id.x];
        }
        // workgroup prefix sum of histogram

        var histogram_prefix_sum = histogram_local_sum;

        if local_id.x < BIN_COUNT {
            lds_scratch[local_id.x] = histogram_prefix_sum;
        }

        for (var i = 0u; i < 4u; i++) {
            workgroupBarrier();
            if local_id.x >= (1u << i) && local_id.x < BIN_COUNT {
                histogram_prefix_sum += lds_scratch[local_id.x - (1u << i)];
            }
            workgroupBarrier();
            if local_id.x < BIN_COUNT {
                lds_scratch[local_id.x] = histogram_prefix_sum;
            }
        }

        let global_offset = bin_offset_cache[key_index];

        workgroupBarrier();

        var local_offset = local_id.x;

        if key_index > 0u {
            local_offset -= lds_scratch[key_index - 1u];
        }

        let total_offset = global_offset + local_offset;

        if total_offset < num_keys {
            out[total_offset] = local_key;
            out_values[total_offset] = local_value;
        }

        if local_id.x < BIN_COUNT {
            bin_offset_cache[local_id.x] += local_histogram[local_id.x];
        }

        workgroupBarrier();
        data_index += WG;
    }
}
