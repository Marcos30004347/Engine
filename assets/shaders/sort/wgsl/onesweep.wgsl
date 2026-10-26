#ifdef WGSL_ENABLE_SUBGROUPS
enable subgroups;
#endif

@diagnostic(off, subgroup_uniformity)
fn unsafeSubgroupInclusiveAdd(x: u32) -> u32 { return subgroupInclusiveAdd(x); }

@diagnostic(off, subgroup_uniformity)
fn unsafeSubgroupExclusiveAdd(x: u32) -> u32 { return subgroupExclusiveAdd(x); }

@diagnostic(off, subgroup_uniformity)
fn unsafeSubgroupShuffle(x: u32, source: u32) -> u32 { return subgroupShuffle(x, source); }

@diagnostic(off, subgroup_uniformity)
fn unsafeSubgroupBallot(pred: bool) -> vec4<u32> { return subgroupBallot(pred); }

@diagnostic(off, subgroup_uniformity)
fn unsafeSubgroupAdd(x: u32) -> u32 { return subgroupAdd(x); }

struct InfoStruct
{
    size: u32,
    shift: u32,
    thread_blocks: u32,
    padding0: u32,
};

@group(0) @binding(0) var<uniform> info : InfoStruct;
@group(0) @binding(1) var<storage, read_write> bump: array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> sort: array<u32>;
@group(0) @binding(3) var<storage, read_write> alt: array<u32>;
@group(0) @binding(4) var<storage, read_write> payload: array<u32>;
@group(0) @binding(5) var<storage, read_write> alt_payload: array<u32>;
@group(0) @binding(6) var<storage, read_write> hist: array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> pass_hist: array<atomic<u32>>;
@group(0) @binding(8) var<storage, read_write> status: array<u32>;

const SORT_PASSES: u32 = 4u;
const STATUS_ERR_GLOBAL_HIST: u32 = 0u;
const STATUS_ERR_SCAN: u32 = 1u;
const STATUS_ERR_PASS: u32 = 2u;
const STATUS_ENTRY_COUNT: u32 = 3u;

const FLAG_NOT_READY: u32 = 0u;
const FLAG_REDUCTION: u32 = 1u;
const FLAG_INCLUSIVE: u32 = 2u;
const FLAG_MASK: u32 = 3u;

const BLOCK_DIM: u32 = 256u;
const REDUCE_BLOCK_DIM: u32 = 128u;
const MIN_SUBGROUP_SIZE: u32 = 16u;

const RADIX: u32 = 256u;
const ALL_RADIX: u32 = RADIX * SORT_PASSES;
const RADIX_MASK: u32 = 255u;
const RADIX_LOG: u32 = 8u;

const KEYS_PER_THREAD: u32 = 15u;
const PART_SIZE: u32 = KEYS_PER_THREAD * BLOCK_DIM;

const REDUCE_KEYS_PER_THREAD: u32 = 30u;
const REDUCE_HIST_SIZE: u32 = (REDUCE_BLOCK_DIM / MIN_SUBGROUP_SIZE) * ALL_RADIX;
const REDUCE_PART_SIZE: u32 = REDUCE_KEYS_PER_THREAD * REDUCE_BLOCK_DIM;

const MAX_SUBGROUPS_PER_BLOCK: u32 = BLOCK_DIM / MIN_SUBGROUP_SIZE;
const WARP_HIST_CAPACITY: u32 = MAX_SUBGROUPS_PER_BLOCK * RADIX;
const SCAN_MEM_SIZE: u32 = RADIX / MIN_SUBGROUP_SIZE;

var<workgroup> wg_globalHist: array<atomic<u32>, REDUCE_HIST_SIZE>;
var<workgroup> wg_scan: array<u32, SCAN_MEM_SIZE>;
var<workgroup> wg_subgroupHist: array<atomic<u32>, WARP_HIST_CAPACITY>;
var<workgroup> wg_localHist: array<u32, RADIX>;
var<workgroup> wg_broadcast: u32;

fn lowMask(bits: u32) -> u32
{
    if (bits == 0u)
    {
        return 0u;
    }

    if (bits >= 32u)
    {
        return 0xffffffffu;
    }

    return (1u << bits) - 1u;
}

fn laneMaskLessThan(laneid: u32) -> vec4<u32>
{
    if (laneid >= 32u)
    {
        return vec4<u32>(0xffffffffu, lowMask(laneid - 32u), 0u, 0u);
    }

    return vec4<u32>(lowMask(laneid), 0u, 0u, 0u);
}

fn subgroupMaskForSize(size: u32) -> vec4<u32>
{
    if (size <= 32u)
    {
        return vec4<u32>(lowMask(size), 0u, 0u, 0u);
    }

    return vec4<u32>(0xffffffffu, lowMask(size - 32u), 0u, 0u);
}

fn maskAnd(a: vec4<u32>, b: vec4<u32>) -> vec4<u32>
{
    return vec4<u32>(a.x & b.x, a.y & b.y, 0u, 0u);
}

fn maskFilter(ballot: vec4<u32>, pred: bool) -> vec4<u32>
{
    let keep = vec4<u32>(ballot.x, ballot.y, 0u, 0u);
    let reject = vec4<u32>(~ballot.x, ~ballot.y, 0u, 0u);
    let cond = vec4<bool>(pred, pred, pred, pred);
    return select(reject, keep, cond);
}

fn maskBitCount(mask: vec4<u32>) -> u32
{
    return countOneBits(mask.x) + countOneBits(mask.y);
}

fn maskHasBits(mask: vec4<u32>) -> bool
{
    return (mask.x | mask.y) != 0u;
}

fn maskHighestLane(mask: vec4<u32>) -> u32
{
    if (mask.y != 0u)
    {
        return 32u + (31u - countLeadingZeros(mask.y));
    }

    return 31u - countLeadingZeros(mask.x);
}

fn WLMS(key: u32, shift: u32, laneid: u32, lane_count: u32, s_offset: u32, key_valid: bool) -> u32
{
    var eq_mask = vec4<u32>(0xffffffffu, 0xffffffffu, 0u, 0u);
    for (var k = 0u; k < RADIX_LOG; k = k + 1u)
    {
        let curr_bit = 1u << (k + shift);
        let pred = key_valid && ((key & curr_bit) != 0u);
        let ballot = unsafeSubgroupBallot(pred);
        eq_mask = maskAnd(eq_mask, maskFilter(ballot, pred));
    }

    if (!key_valid)
    {
        eq_mask = vec4<u32>(0u);
    }

    eq_mask = maskAnd(eq_mask, subgroupMaskForSize(lane_count));
    let lane_mask_lt = laneMaskLessThan(laneid);
    var out = maskBitCount(maskAnd(eq_mask, lane_mask_lt));
    let has_peers = maskHasBits(eq_mask);
    let highest_rank_peer = select(lane_count - 1u, maskHighestLane(eq_mask), has_peers);
    var pre_inc = 0u;
    if (key_valid && has_peers && laneid == highest_rank_peer)
    {
        pre_inc = atomicAdd(&wg_subgroupHist[((key >> shift) & RADIX_MASK) + s_offset], out + 1u);
    }

    workgroupBarrier();

    var broadcast = 0u;
    if (has_peers)
    {
        broadcast = unsafeSubgroupShuffle(pre_inc, highest_rank_peer);
    }

    out = out + broadcast;
    return select(0u, out, key_valid);
}

@compute @workgroup_size(BLOCK_DIM, 1, 1)
fn init_onesweep(@builtin(global_invocation_id) gid: vec3<u32>)
{
    let index = gid.x;
    let pass_hist_size = info.thread_blocks * ALL_RADIX;

    if (index < pass_hist_size)
    {
        let block = (index / RADIX) % info.thread_blocks;
        atomicStore(&pass_hist[index], select(0u, FLAG_INCLUSIVE, block == 0u));
    }

    if (index < ALL_RADIX)
    {
        atomicStore(&hist[index], 0u);
    }

    if (index < SORT_PASSES)
    {
        atomicStore(&bump[index], 0u);
    }

    if (index < STATUS_ENTRY_COUNT)
    {
        status[index] = 0u;
    }
}

@compute @workgroup_size(REDUCE_BLOCK_DIM, 1, 1)
fn global_hist(
    @builtin(local_invocation_id) threadid: vec3<u32>,
    @builtin(subgroup_invocation_id) laneid: u32,
    @builtin(subgroup_size) lane_count: u32,
    @builtin(workgroup_id) wgid: vec3<u32>)
{
    if (lane_count < MIN_SUBGROUP_SIZE || (REDUCE_BLOCK_DIM % lane_count) != 0u)
    {
        if (threadid.x == 0u)
        {
            status[STATUS_ERR_GLOBAL_HIST] = 0xDEAD0001u;
        }
        return;
    }

    let sid = threadid.x / lane_count;

    for (var i = threadid.x; i < REDUCE_HIST_SIZE; i = i + REDUCE_BLOCK_DIM)
    {
        atomicStore(&wg_globalHist[i], 0u);
    }
    workgroupBarrier();

    let hist_offset = sid * ALL_RADIX;
    var index = threadid.x + wgid.x * REDUCE_PART_SIZE;
    if (wgid.x < info.thread_blocks - 1u)
    {
        for (var k = 0u; k < REDUCE_KEYS_PER_THREAD; k = k + 1u)
        {
            let key = sort[index];
            atomicAdd(&wg_globalHist[(key & RADIX_MASK) + hist_offset], 1u);
            atomicAdd(&wg_globalHist[((key >> 8u) & RADIX_MASK) + hist_offset + 256u], 1u);
            atomicAdd(&wg_globalHist[((key >> 16u) & RADIX_MASK) + hist_offset + 512u], 1u);
            atomicAdd(&wg_globalHist[((key >> 24u) & RADIX_MASK) + hist_offset + 768u], 1u);
            index = index + REDUCE_BLOCK_DIM;
        }
    }
    else
    {
        for (var k = 0u; k < REDUCE_KEYS_PER_THREAD; k = k + 1u)
        {
            if (index < info.size)
            {
                let key = sort[index];
                atomicAdd(&wg_globalHist[(key & RADIX_MASK) + hist_offset], 1u);
                atomicAdd(&wg_globalHist[((key >> 8u) & RADIX_MASK) + hist_offset + 256u], 1u);
                atomicAdd(&wg_globalHist[((key >> 16u) & RADIX_MASK) + hist_offset + 512u], 1u);
                atomicAdd(&wg_globalHist[((key >> 24u) & RADIX_MASK) + hist_offset + 768u], 1u);
            }
            index = index + REDUCE_BLOCK_DIM;
        }
    }
    workgroupBarrier();

    let subgroup_histograms = REDUCE_BLOCK_DIM / lane_count;
    for (var i = threadid.x; i < RADIX; i = i + REDUCE_BLOCK_DIM)
    {
        var reduction0 = atomicLoad(&wg_globalHist[i]);
        var reduction1 = atomicLoad(&wg_globalHist[i + 256u]);
        var reduction2 = atomicLoad(&wg_globalHist[i + 512u]);
        var reduction3 = atomicLoad(&wg_globalHist[i + 768u]);

        for (var h = 1u; h < subgroup_histograms; h = h + 1u)
        {
            let hist_index = h * ALL_RADIX;
            reduction0 = reduction0 + atomicLoad(&wg_globalHist[i + hist_index]);
            reduction1 = reduction1 + atomicLoad(&wg_globalHist[i + 256u + hist_index]);
            reduction2 = reduction2 + atomicLoad(&wg_globalHist[i + 512u + hist_index]);
            reduction3 = reduction3 + atomicLoad(&wg_globalHist[i + 768u + hist_index]);
        }

        atomicAdd(&hist[i], reduction0);
        atomicAdd(&hist[i + 256u], reduction1);
        atomicAdd(&hist[i + 512u], reduction2);
        atomicAdd(&hist[i + 768u], reduction3);
    }
}

@compute @workgroup_size(BLOCK_DIM, 1, 1)
fn onesweep_scan(
    @builtin(local_invocation_id) threadid: vec3<u32>,
    @builtin(subgroup_invocation_id) laneid: u32,
    @builtin(subgroup_size) lane_count: u32)
{
    if (lane_count < MIN_SUBGROUP_SIZE || (BLOCK_DIM % lane_count) != 0u)
    {
        if (threadid.x == 0u)
        {
            status[STATUS_ERR_SCAN] = 0xDEAD0002u;
        }
        return;
    }

    let sid = threadid.x / lane_count;
    let pass_plane = info.shift >> 3u;
    let hist_index = threadid.x + pass_plane * RADIX;
    let scan_value = atomicLoad(&hist[hist_index]);
    let reduction = unsafeSubgroupAdd(scan_value);
    if (laneid == 0u)
    {
        wg_scan[sid] = reduction;
    }
    workgroupBarrier();

    var offset0 = 0u;
    var offset1 = 0u;
    let lane_log = u32(countTrailingZeros(lane_count));
    let spine_size = BLOCK_DIM >> lane_log;
    let aligned_size = 1u << ((u32(countTrailingZeros(spine_size)) + lane_log - 1u) / lane_log * lane_log);
    for (var j = lane_count; j <= aligned_size; j = j << lane_log)
    {
        let i0 = ((threadid.x + offset0) << offset1) - select(0u, 1u, j != lane_count);
        let pred0 = i0 < spine_size;
        let t0 = unsafeSubgroupInclusiveAdd(select(0u, wg_scan[i0], pred0));
        if (pred0)
        {
            wg_scan[i0] = t0;
        }
        workgroupBarrier();

        if (j != lane_count)
        {
            let rshift = j >> lane_log;
            let i1 = threadid.x + rshift;
            if ((i1 & (j - 1u)) >= rshift)
            {
                let pred1 = i1 < spine_size;
                let t1 = select(0u, wg_scan[((i1 >> offset1) << offset1) - 1u], pred1);
                if (pred1 && ((i1 + 1u) & (rshift - 1u)) != 0u)
                {
                    wg_scan[i1] = wg_scan[i1] + t1;
                }
            }
        }
        else
        {
            offset0 = offset0 + 1u;
        }
        offset1 = offset1 + lane_log;
    }
    workgroupBarrier();

    let plane_stride = info.thread_blocks * RADIX;
    let pass_index = threadid.x + pass_plane * plane_stride;
    let subgroup_prefix = unsafeSubgroupExclusiveAdd(scan_value);
    var spine_prefix = 0u;
    if (sid > 0u)
    {
        spine_prefix = wg_scan[sid - 1u];
    }
    atomicStore(&pass_hist[pass_index], ((subgroup_prefix + spine_prefix) << 2u) | FLAG_INCLUSIVE);
}

@compute @workgroup_size(BLOCK_DIM, 1, 1)
fn onesweep_pass(
    @builtin(local_invocation_id) threadid: vec3<u32>,
    @builtin(subgroup_invocation_id) laneid: u32,
    @builtin(subgroup_size) lane_count: u32)
{
    let shift = info.shift;
    let sid = threadid.x / lane_count;

    let subgroup_hist_size = (BLOCK_DIM / lane_count) * RADIX;
    if (lane_count < MIN_SUBGROUP_SIZE || subgroup_hist_size > WARP_HIST_CAPACITY)
    {
        if (threadid.x == 0u)
        {
            status[STATUS_ERR_PASS] = 0xDEAD0004u;
        }
        return;
    }

    for (var i = threadid.x; i < subgroup_hist_size; i = i + BLOCK_DIM)
    {
        atomicStore(&wg_subgroupHist[i], 0u);
    }
    workgroupBarrier();

    if (threadid.x == 0u)
    {
        wg_broadcast = atomicAdd(&bump[shift >> 3u], 1u);
    }
    let partid = workgroupUniformLoad(&wg_broadcast);

    var keys = array<u32, KEYS_PER_THREAD>();
    var values = array<u32, KEYS_PER_THREAD>();
    var key_valid = array<bool, KEYS_PER_THREAD>();
    let dev_offset = partid * PART_SIZE;
    let lane_stride = sid * lane_count * KEYS_PER_THREAD;
    var index = laneid + lane_stride + dev_offset;
    if (partid < info.thread_blocks - 1u)
    {
        for (var k = 0u; k < KEYS_PER_THREAD; k = k + 1u)
        {
            keys[k] = sort[index];
            values[k] = payload[index];
            key_valid[k] = true;
            index = index + lane_count;
        }
    }
    else
    {
        for (var k = 0u; k < KEYS_PER_THREAD; k = k + 1u)
        {
            if (index < info.size)
            {
                keys[k] = sort[index];
                values[k] = payload[index];
                key_valid[k] = true;
            }
            else
            {
                keys[k] = 0xffffffffu;
                values[k] = 0xffffffffu;
                key_valid[k] = false;
            }
            index = index + lane_count;
        }
    }

    var offsets = array<u32, KEYS_PER_THREAD>();
    let hist_offset = sid * RADIX;
    for (var k = 0u; k < KEYS_PER_THREAD; k = k + 1u)
    {
        offsets[k] = WLMS(keys[k], shift, laneid, lane_count, hist_offset, key_valid[k]);
    }
    workgroupBarrier();

    var local_reduction = 0u;
    if (threadid.x < RADIX)
    {
        local_reduction = atomicLoad(&wg_subgroupHist[threadid.x]);
        var subtotal = local_reduction;
        for (var i = threadid.x + RADIX; i < subgroup_hist_size; i = i + RADIX)
        {
            let current = atomicLoad(&wg_subgroupHist[i]);
            atomicStore(&wg_subgroupHist[i], subtotal);
            subtotal = subtotal + current;
        }
        local_reduction = subtotal;

        if (partid < info.thread_blocks - 1u)
        {
            let pass_plane = shift >> 3u;
            let pass_index = threadid.x + pass_plane * info.thread_blocks * RADIX + (partid + 1u) * RADIX;
            atomicStore(&pass_hist[pass_index], (local_reduction << 2u) | FLAG_REDUCTION);
        }

        let lane_mask = lane_count - 1u;
        let circular_lane_shift = (laneid + lane_mask) & lane_mask;
        let prefix = unsafeSubgroupInclusiveAdd(local_reduction);
        wg_localHist[threadid.x] = unsafeSubgroupShuffle(prefix, circular_lane_shift);
    }
    workgroupBarrier();

    if (threadid.x < lane_count)
    {
        let pred = threadid.x < RADIX / lane_count;
        let prefix = unsafeSubgroupExclusiveAdd(select(0u, wg_localHist[threadid.x * lane_count], pred));
        if (pred)
        {
            wg_localHist[threadid.x * lane_count] = prefix;
        }
    }
    workgroupBarrier();

    if (threadid.x < RADIX && laneid != 0u)
    {
        wg_localHist[threadid.x] = wg_localHist[threadid.x] + wg_localHist[(threadid.x / lane_count) * lane_count];
    }
    workgroupBarrier();

    for (var k = 0u; k < KEYS_PER_THREAD; k = k + 1u)
    {
        if (key_valid[k])
        {
            let digit = (keys[k] >> shift) & RADIX_MASK;
            let block_prefix = wg_localHist[digit];
            if (sid == 0u)
            {
                offsets[k] = offsets[k] + block_prefix;
            }
            else
            {
                let subgroup_prefix = atomicLoad(&wg_subgroupHist[digit + sid * RADIX]);
                offsets[k] = offsets[k] + block_prefix + subgroup_prefix;
            }
        }
    }
    workgroupBarrier();

    if (threadid.x < RADIX)
    {
        let pass_plane = shift >> 3u;
        let base_plane = pass_plane * info.thread_blocks * RADIX;
        let bin = threadid.x;
        let block_prefix = wg_localHist[bin];
        var prev_reduction = 0u;
        var lookbackid = partid;
        loop
        {
            let flag_payload = atomicLoad(&pass_hist[bin + base_plane + lookbackid * RADIX]);
            if ((flag_payload & FLAG_MASK) > FLAG_NOT_READY)
            {
                prev_reduction = prev_reduction + (flag_payload >> 2u);
                if ((flag_payload & FLAG_MASK) == FLAG_INCLUSIVE)
                {
                    if (partid < info.thread_blocks - 1u)
                    {
                        let next_idx = bin + base_plane + (partid + 1u) * RADIX;
                        atomicStore(&pass_hist[next_idx], ((prev_reduction + local_reduction) << 2u) | FLAG_INCLUSIVE);
                    }
                    wg_localHist[bin] = prev_reduction - block_prefix;
                    break;
                }
                lookbackid = lookbackid - 1u;
            }
        }
    }
    workgroupBarrier();

    for (var k = 0u; k < KEYS_PER_THREAD; k = k + 1u)
    {
        if (key_valid[k])
        {
            let digit = (keys[k] >> shift) & RADIX_MASK;
            let global_offset = wg_localHist[digit] + offsets[k];
            if (global_offset < info.size)
            {
                alt[global_offset] = keys[k];
                alt_payload[global_offset] = values[k];
            }
        }
    }
}
