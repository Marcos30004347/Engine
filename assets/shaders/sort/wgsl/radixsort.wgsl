// Adapted from https://github.com/KeKsBoTer/wgpu_sort
//
// BSD 2-Clause License
//
// Copyright (c) 2024, Simon Niedermayr, Josef Stumpfegger 
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// This value is replaced in the RadixSortEncoder when loading the shader
// The shader can fail if subgroup size changes over time.

// enable subgroups;

const histogram_wg_size = 256u;
const rs_radix_log2 = 8u;
const rs_radix_size = 1u << rs_radix_log2;
const rs_keyval_size = 32u / rs_radix_log2;
const rs_histogram_block_rows = 14u;
const prefix_wg_size = 1u << 7u;
const rs_scatter_block_rows = rs_histogram_block_rows;
const scatter_wg_size = 1u << 8u;
const rs_partition_mask_status : u32 = 0xC0000000u;
const rs_partition_mask_count : u32 = 0x3FFFFFFFu;
const rs_mem_dwords = rs_radix_size + rs_scatter_block_rows * scatter_wg_size;

var<workgroup> smem : array<atomic<u32>, rs_radix_size>;
var<workgroup> scatter_smem : array<u32, rs_mem_dwords>;

var<private> kr : array<u32, rs_scatter_block_rows>; // Is used to store digit location
var<private> pv : array<u32, rs_scatter_block_rows>; // Is used to store the payload value
var<private> kv : array<u32, rs_histogram_block_rows>; // Is used to store the key value

struct Info {
 num_keys: u32,
 padded_size: u32,
 even_pass: u32,
 odd_pass: u32,
};

@group(0) @binding(0) var<storage, read_write> infos: Info;
@group(0) @binding(1) var<storage, read_write> histograms : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> keys_a: array<u32>;
@group(0) @binding(3) var<storage, read_write> keys_b: array<u32>;
@group(0) @binding(4) var<storage, read_write> payload_a : array<u32>;
@group(0) @binding(5) var<storage, read_write> payload_b : array<u32>;


fn zero_smem(lid: u32) {
    if lid < rs_radix_size {
        atomicStore(&smem[lid], 0u);
    }
}

// Histogram //

// here the histograms are set to zero and the partitions are set to 0xfffffffff to avoid sorting problems
@compute @workgroup_size(histogram_wg_size)
fn zero_histograms(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>) {
    if gid.x == 0u {
        infos.even_pass = 0u;
			// has to be one, as on the first call to even pass + 1 % 2 is calculated
        infos.odd_pass = 1u;
    }

    let scatter_block_kvs = histogram_wg_size * rs_scatter_block_rows;
	
	// Ceil(num_keyus / scatter_block_kvs)
    let scatter_blocks_ru = (infos.num_keys + scatter_block_kvs - 1u) / scatter_block_kvs;

    let histo_size = rs_radix_size;

	// buffer size
    var n = (rs_keyval_size + scatter_blocks_ru - 1u) * histo_size;
    let b = n;

    if infos.num_keys < infos.padded_size {
        n += infos.padded_size - infos.num_keys;
    }

    let line_size = nwg.x * histogram_wg_size;

    for (var cur_index = gid.x; cur_index < n; cur_index += line_size) {
		//if cur_index >= n {
		//	return;
		//}
    // cur_index is within range of histogram or cur_index is within padding
        if cur_index < (rs_keyval_size * histo_size) || cur_index < b {
            atomicStore(&histograms[cur_index], 0u);
        } else {
            keys_a[infos.num_keys + cur_index - b] = 0xFFFFFFFFu;
        }
    }
}


fn fill_kv(wid: u32, lid: u32) {
    let rs_block_keyvals: u32 = rs_histogram_block_rows * histogram_wg_size;
    let kv_in_offset = wid * rs_block_keyvals + lid;
    for (var i = 0u; i < rs_histogram_block_rows; i++) {
        let pos = kv_in_offset + i * histogram_wg_size;
        kv[i] = keys_a[pos];
    }
}
fn fill_kv_keys_b(wid: u32, lid: u32) {
    let rs_block_keyvals: u32 = rs_histogram_block_rows * histogram_wg_size;
    let kv_in_offset = wid * rs_block_keyvals + lid;
    for (var i = 0u; i < rs_histogram_block_rows; i++) {
        let pos = kv_in_offset + i * histogram_wg_size;
        kv[i] = keys_b[pos];
    }
}

fn histogram_pass(pass_: u32, lid: u32) {
    zero_smem(lid);
    workgroupBarrier();

    for (var j = 0u; j < rs_histogram_block_rows; j++) {
        let u_val = bitcast<u32>(kv[j]);
        let digit = extractBits(u_val, pass_ * rs_radix_log2, rs_radix_log2);

        atomicAdd(&smem[digit], 1u);
    }

    workgroupBarrier();

    let histogram_offset = rs_radix_size * pass_ + lid;

    if lid < rs_radix_size && atomicLoad(&smem[lid]) >= 0u {
        atomicAdd(&histograms[histogram_offset], atomicLoad(&smem[lid]));
    }
}


@compute @workgroup_size(histogram_wg_size)
fn calculate_histogram(@builtin(workgroup_id) wid: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>) {
    // efficient loading of multiple values
    fill_kv(wid.x, lid.x);
    
    // Accumulate and store histograms for passes
    histogram_pass(3u, lid.x);
    histogram_pass(2u, lid.x);
    histogram_pass(1u, lid.x);
    histogram_pass(0u, lid.x);
}

// Prefix Sum //
fn prefix_reduce_smem(lid: u32) {
    var offset = 1u;
    for (var d = rs_radix_size >> 1u; d > 0u; d = d >> 1u) { // sum in place tree
        workgroupBarrier();
        if lid < d {
            let ai = offset * (2u * lid + 1u) - 1u;
            let bi = offset * (2u * lid + 2u) - 1u;
            atomicAdd(&smem[bi], atomicLoad(&smem[ai]));
        }
        offset = offset << 1u;
	}

    if lid == 0u {
        atomicStore(&smem[rs_radix_size - 1u], 0u);
    } // clear the last element

    for (var d = 1u; d < rs_radix_size; d = d << 1u) {
        offset = offset >> 1u;
        workgroupBarrier();
        if lid < d {
            let ai = offset * (2u * lid + 1u) - 1u;
            let bi = offset * (2u * lid + 2u) - 1u;

            let t = atomicLoad(&smem[ai]);
            atomicStore(&smem[ai], atomicLoad(&smem[bi]));
            atomicAdd(&smem[bi], t);
        }
    }
}

@compute @workgroup_size(prefix_wg_size)
fn prefix_histogram(@builtin(workgroup_id) wid: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>) {
    // the work group id is the pass, and is inverted in the next line, such that pass 3 is at the first position in the histogram buffer
    let histogram_base = (rs_keyval_size - 1u - wid.x) * rs_radix_size;
    let histogram_offset = histogram_base + lid.x;
    
    // the following coode now corresponds to the prefix calc code in fuchsia/../shaders/prefix.h
    // however the implementation is taken from https://www.eecs.umich.edu/courses/eecs570/hw/parprefix.pdf listing 2 (better overview, nw subgroup arithmetic)
    // this also means that only half the amount of workgroups is spawned (one workgroup calculates for 2 positioons)
    // the smemory is used from the previous section
    atomicStore(&smem[lid.x], atomicLoad(&histograms[histogram_offset]));
    atomicStore(&smem[lid.x + prefix_wg_size], atomicLoad(&histograms[histogram_offset + prefix_wg_size]));

    prefix_reduce_smem(lid.x);
    workgroupBarrier();

    atomicStore(&histograms[histogram_offset], atomicLoad(&smem[lid.x]));
    atomicStore(&histograms[histogram_offset + prefix_wg_size], atomicLoad(&smem[lid.x + prefix_wg_size]));
}


// Scattering //
// The data is scattered to achieve coallesed memory fetches

fn fill_kv_even(wgid: u32, lid: u32, subgroup_size: u32) {
	// A subgroup will contain a few threads, so first find the block offset, then the subgroup_offset for the
	// subgroup of threads, the the offset for each individual thread.

    let subgroup_id = lid / subgroup_size;
    let subgroup_offset = subgroup_id * subgroup_size;
    let subgroup_tid = lid - subgroup_offset;
    let subgroup_keyvals = rs_scatter_block_rows * subgroup_size; // NOTE: sg != wg
    let rs_block_keyvals = rs_histogram_block_rows * histogram_wg_size;

	// kv_in_offset  = (     block_offset    ) + (       subgroup_offset      ) + (subgroup_tid_offset);
    let kv_in_offset = wgid * rs_block_keyvals + subgroup_id * subgroup_keyvals + subgroup_tid;

	// Load elements coallesed
    for (var i = 0u; i < rs_histogram_block_rows; i++) {
        let pos = kv_in_offset + i * subgroup_size;
        kv[i] = keys_a[pos];
    }
		
	// Load elements coallesed
    for (var i = 0u; i < rs_histogram_block_rows; i++) {
        let pos = kv_in_offset + i * subgroup_size;
        pv[i] = payload_a[pos];
    }
}

fn fill_kv_odd(wgid: u32, lid: u32, subgroup_size: u32) {
    let subgroup_id = lid / subgroup_size;
    let subgroup_offset = subgroup_id * subgroup_size;
    let subgroup_tid = lid - subgroup_offset;
    let subgroup_keyvals = rs_scatter_block_rows * subgroup_size;
    let rs_block_keyvals = rs_histogram_block_rows * histogram_wg_size;

    let kv_in_offset = wgid * rs_block_keyvals + subgroup_id * subgroup_keyvals + subgroup_tid;

    for (var i = 0u; i < rs_histogram_block_rows; i++) {
        let pos = kv_in_offset + i * subgroup_size;
        kv[i] = keys_b[pos];
    }

    for (var i = 0u; i < rs_histogram_block_rows; i++) {
        let pos = kv_in_offset + i * subgroup_size;
        pv[i] = payload_b[pos];
    }
}


fn histogram_load(digit: u32) -> u32 {
    return atomicLoad(&smem[digit]);
}

fn histogram_store(digit: u32, count: u32) {
    atomicStore(&smem[digit], count);
}


fn partitions_base_offset() -> u32 { return rs_keyval_size * rs_radix_size;}
fn smem_prefix_offset() -> u32 { return rs_radix_size + rs_radix_size;}
//fn rs_prefix_sweep_0(idx: u32) -> u32 { return scatter_smem[smem_prefix_offset() + rs_mem_sweep_0_offset + idx];}
//fn rs_prefix_sweep_1(idx: u32) -> u32 { return scatter_smem[smem_prefix_offset() + rs_mem_sweep_1_offset + idx];}
//fn rs_prefix_sweep_2(idx: u32) -> u32 { return scatter_smem[smem_prefix_offset() + rs_mem_sweep_2_offset + idx];}
fn rs_prefix_load(lid: u32, idx: u32) -> u32 { return scatter_smem[rs_radix_size + lid + idx];}
fn rs_prefix_store(lid: u32, idx: u32, val: u32) { scatter_smem[rs_radix_size + lid + idx] = val;}
fn is_first_local_invocation(lid: u32) -> bool { return lid == 0u;}


fn least_significant_bits_mask(i: u32) -> u32 {
    return (1u << i) - 1u;
}

//var<private> digits: array<u32, 32>;

fn scatter(pass_: u32, lid: vec3<u32>, gid: vec3<u32>, wid: vec3<u32>, nwg: vec3<u32>, partition_status_invalid: u32, partition_status_reduction: u32, partition_status_prefix: u32, subgroup_size: u32) {
    let partition_mask_invalid = partition_status_invalid << 30u;
    let partition_mask_reduction = partition_status_reduction << 30u;
    let partition_mask_prefix = partition_status_prefix << 30u;
	// kv_filling is done in the scatter_even and scatter_odd functions to account for front and backbuffer switch
	// in the reference there is a nulling of the smmem here, was moved to line 251 as smem is used in the code until then

	// The following implements conceptually the same as the
	// Emulate a "match" operation with broadcasts for small subgroup sizes (line 665 ff in scatter.glsl)
	// The difference however is, that instead of using subrgoupBroadcast each thread stores
	// its current number in the smem at lid.x, and then looks up their neighbouring values of the subgroup

    let subgroup_id = lid.x / subgroup_size;
    let subgroup_offset = subgroup_id * subgroup_size;
    let subgroup_tid = lid.x - subgroup_offset;
    let subgroup_count = scatter_wg_size / subgroup_size;

    for (var i = 0u; i < rs_scatter_block_rows; i++) {
        let u_val = bitcast<u32>(kv[i]);
        let digit = extractBits(u_val, pass_ * rs_radix_log2, rs_radix_log2);

        atomicStore(&smem[lid.x], digit);

        var count = 0u;
		//answers how many threads before me in subgroup have same digit
        var rank = 0u;

        for (var j = 0u; j < subgroup_size; j++) {
			//Compare digits with other digits in subgroup
            if atomicLoad(&smem[subgroup_offset + j]) == digit {
                count += 1u;
                if j <= subgroup_tid {
                    rank += 1u;
                }
            }
        }

		//Store results
        kr[i] = (count << 16u) | rank;
    }

    zero_smem(lid.x);   // now zeroing the smmem as we are now accumulating the histogram there
    workgroupBarrier();

	// Accumulate subgroup values in smem
	// The final histogram is stored in the smem buffer
    for (var i = 0u; i < subgroup_count; i++) {
        if subgroup_id == i {
            for (var j = 0u; j < rs_scatter_block_rows; j++) {
                let v = bitcast<u32>(kv[j]);
                let digit = extractBits(v, pass_ * rs_radix_log2, rs_radix_log2);
                let prev = histogram_load(digit);

                let rank = kr[j] & 0xFFFFu;
                let count = kr[j] >> 16u;

                kr[j] = prev + rank;

                if rank == count {
                    histogram_store(digit, (prev + count));
                }

					// TODO: check if the barrier here is needed
            }
        }
        workgroupBarrier();
    }

	// kr filling is now done and contains the total offset for each value to be able to 
	// move the values into order without having any collisions
    
	// we do not check for single work groups (is currently not assumed to occur very often)
    let partition_offset = lid.x + partitions_base_offset();    // is correct, the partitions pointer does not change
    let partition_base = wid.x * rs_radix_size;

	// save smem to histogram
    if wid.x == 0u {
			// special treating for the first workgroup as the data might be read back by later workgroups
			// corresponds to rs_first_prefix_store
        let hist_offset = pass_ * rs_radix_size + lid.x;
        if lid.x < rs_radix_size {
            let exc = atomicLoad(&histograms[hist_offset]);
					// load from smem
            let red = histogram_load(lid.x);

            scatter_smem[lid.x] = exc;

            let inc = exc + red;

					// store to smem
            atomicStore(&histograms[partition_offset], inc | partition_mask_prefix);
        }
    } else {
		// standard case for the "inbetween" workgroups
        
		// rs_reduction_store, only for inbetween workgroups
        if lid.x < rs_radix_size && wid.x < nwg.x - 1u {
				// save smem to histogram
            let red = histogram_load(lid.x);
            atomicStore(&histograms[partition_offset + partition_base], red | partition_mask_reduction);
        }
        
		// rs_loopback_store
        if lid.x < rs_radix_size {
            var partition_base_prev = partition_base - rs_radix_size;
            var exc = 0u;

				// Note: Each workgroup invocation can proceed independently.
				// Subgroups and workgroups do NOT have to coordinate.
            while true {
                let prev = atomicLoad(&histograms[partition_base_prev + partition_offset]);
                if (prev & rs_partition_mask_status) == partition_mask_invalid {
								continue;
                }
                exc += prev & rs_partition_mask_count;
                if (prev & rs_partition_mask_status) != partition_mask_prefix {
								// continue accumulating reduction
                    partition_base_prev -= rs_radix_size;
								continue;
                }

						// otherwise save the exclusive scan and atomically transform the
						// reduction into an inclusive prefix status math: reduction + 1 = prefix
                scatter_smem[lid.x] = exc;

                if wid.x < nwg.x - 1u { // only store when inbetween, skip for last workgrup
                    atomicAdd(&histograms[partition_offset + partition_base], exc | (1u << 30u));
							}
						break;
            }
        }
    }
	// NOTE: special case for last workgroup is also done in the "inbetween" case
    
	// compute exclusive prefix scan of histogram
	// corresponds to rs_prefix
	// TODO: make sure that the data is put into smem.
	// NOTE: i think it is already???
    prefix_reduce_smem(lid.x);

    workgroupBarrier();

	// convert keyval rank to local index, corresponds to rs_rank_to_local
    for (var i = 0u; i < rs_scatter_block_rows; i++) {
        let v = bitcast<u32>(kv[i]);
        let digit = extractBits(v, pass_ * rs_radix_log2, rs_radix_log2);
        let exc = histogram_load(digit);
        let idx = exc + kr[i];

		// Save index position
        kr[i] |= (idx << 16u);
    }

    workgroupBarrier();
    
	// reorder kv[] and kr[], corresponds to rs_reorder
    let smem_reorder_offset = rs_radix_size;
    let smem_base = smem_reorder_offset + lid.x;  // as we are in smem, the radix_size offset is not needed

	// Reorder Keys and Payload 
	// store keyval to sorted location
    for (var j = 0u; j < rs_scatter_block_rows; j++) {
        let smem_idx = smem_reorder_offset + (kr[j] >> 16u) - 1u;
		// TODO: try just  scatter_smem[smem_idx] = kv[j];
        scatter_smem[smem_idx] = bitcast<u32>(kv[j]);
    }

    workgroupBarrier();

    for (var j = 0u; j < rs_scatter_block_rows; j++) {
        kv[j] = scatter_smem[smem_base + j * scatter_wg_size];
    }

    workgroupBarrier();

    for (var j = 0u; j < rs_scatter_block_rows; j++) {
        let smem_idx = smem_reorder_offset + (kr[j] >> 16u) - 1u;
        scatter_smem[smem_idx] = pv[j];
    }

    workgroupBarrier();

    for (var j = 0u; j < rs_scatter_block_rows; j++) {
        pv[j] = scatter_smem[smem_base + j * scatter_wg_size];
    }

    workgroupBarrier();
    
	// store the digit-index to sorted location
    for (var i = 0u; i < rs_scatter_block_rows; i++) {
        let smem_idx = smem_reorder_offset + (kr[i] >> 16u) - 1u;
        scatter_smem[smem_idx] = kr[i];
    }

    workgroupBarrier();

	// Load kr[] from sorted location -- we only need the rank
    for (var i = 0u; i < rs_scatter_block_rows; i++) {
        kr[i] = scatter_smem[smem_base + i * scatter_wg_size] & 0xFFFFu;
    }
    
	// convert local index to a global index, corresponds to rs_local_to_global
    for (var i = 0u; i < rs_scatter_block_rows; i++) {
        let v = bitcast<u32>(kv[i]);
        let digit = extractBits(v, pass_ * rs_radix_log2, rs_radix_log2);
        let exc = scatter_smem[digit];

        kr[i] += exc - 1u;
    }
    
	// the storing is done in the scatter_even and scatter_odd functions as the front and back buffer changes
}


@compute @workgroup_size(scatter_wg_size)
fn scatter_even(@builtin(workgroup_id) wid: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>, @builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>, @builtin(subgroup_size) lane_count: u32, @builtin(subgroup_invocation_id) subgroup_tid: u32) {
    if gid.x == 0u {
        infos.odd_pass = (infos.odd_pass + 1u) % 2u; // for this to work correctly the odd_pass has to start 1
    }
    let cur_pass = infos.even_pass * 2u;
    
	// load from keys, store to keys_b
    fill_kv_even(wid.x, lid.x, lane_count);

    let partition_status_invalid = 0u;
    let partition_status_reduction = 1u;
    let partition_status_prefix = 2u;

    scatter(cur_pass, lid, gid, wid, nwg, partition_status_invalid, partition_status_reduction, partition_status_prefix, lane_count);

	// store keyvals to their new locations, corresponds to rs_store
    for (var i = 0u; i < rs_scatter_block_rows; i++) {
        let index = kr[i];
        keys_b[index] = kv[i];
    }

    for (var i = 0u; i < rs_scatter_block_rows; i++) {
        let index = kr[i];
        payload_b[index] = pv[i];
    }
}

@compute @workgroup_size(scatter_wg_size)
fn scatter_odd(@builtin(workgroup_id) wid: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>, @builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>, @builtin(subgroup_size) lane_count: u32) {
    if gid.x == 0u {
        infos.even_pass = (infos.even_pass + 1u) % 2u; // for this to work correctly the even_pass has to start at 0
    }
    let cur_pass = infos.odd_pass * 2u + 1u;

	// load from keys_b, store to keys
    fill_kv_odd(wid.x, lid.x, lane_count);

    let partition_status_invalid = 2u;
    let partition_status_reduction = 3u;
    let partition_status_prefix = 0u;

    scatter(cur_pass, lid, gid, wid, nwg, partition_status_invalid, partition_status_reduction, partition_status_prefix, lane_count);

	// store keyvals to their new locations, corresponds to rs_store
    for (var i = 0u; i < rs_scatter_block_rows; i++) {
        let index = kr[i];
        keys_a[index] = kv[i];
    }

    for (var i = 0u; i < rs_scatter_block_rows; i++) {
        let index = kr[i];
        payload_a[index] = pv[i];
    }

	// the indirect buffer is reset after scattering via write buffer, see record_scatter_indirect for details
}
