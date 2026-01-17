struct Params {
 n: u32,
};

const SUBARRAY_SIZE: u32 = 1024u;
const ITERATIONS : u32 = SUBARRAY_SIZE / 256u;

@group(0) @binding(0) var<storage, read_write> data: array<u32>;
@group(0) @binding(1) var<storage, read_write> partial_sums: array<u32>;
@group(0) @binding(2) var<uniform> params : Params;

var<workgroup> shared_data: array<u32, 256>;

@compute @workgroup_size(256)
fn prefix_sum_local(@builtin(global_invocation_id) global_id: vec3<u32>, @builtin(local_invocation_id) local_id: vec3<u32>, @builtin(workgroup_id) workgroup_id: vec3<u32>) {
    var local_idx = local_id.x;
    
    let global_base_idx = workgroup_id.x * SUBARRAY_SIZE;
	let iterations = SUBARRAY_SIZE / 256u;
	var partials = array<u32, ITERATIONS>();
	var iter = 0u;	
    
    // Loop through all chunks of size 256 in the SUBARRAY_SIZE
    for (var offset = 0u; offset < SUBARRAY_SIZE; offset += 256u) {
        // Load data into shared memory
        let global_idx = global_base_idx + offset + local_idx;
        if (global_idx < params.n) {
            shared_data[local_idx] = data[global_idx];
        } else {
            shared_data[local_idx] = 0u;
        }
        
        workgroupBarrier();

        // Up-sweep phase (reduce step) within the chunk
        var stride: u32 = 1u;
        while (stride < 256u) {
            let index = (local_idx + 1u) * stride * 2u - 1u;
            if index < 256u {
                shared_data[index] += shared_data[index - stride];
            }
            stride *= 2u;
            workgroupBarrier();
        }

				workgroupBarrier();

				// NOTE: For some reason it doesn't work to write directly to partials in here
				if local_idx == 256u - 1u {
					partial_sums[workgroup_id.x] = shared_data[256u - 1u];
					shared_data[256u - 1u] = 0u;
				}

				workgroupBarrier();

				partials[iter] = partial_sums[workgroup_id.x];
				iter += 1u;

        // Down-sweep phase (inclusive -> exclusive) within the chunk
        stride = 128u;
        while (stride > 0u) {
            let index = (local_idx + 1u) * stride * 2u - 1u;
            if index < 256u {
                let temp = shared_data[index - stride];
                shared_data[index - stride] = shared_data[index];
                shared_data[index] += temp;
            }
            stride /= 2u;
            workgroupBarrier();
        }

        workgroupBarrier(); // Ensure all threads finish before continuing to next chunk

				// Write the result back to the output buffer
        if (global_idx < params.n) {
					data[global_idx] = shared_data[local_idx];
        }
        
    }

		workgroupBarrier();

		var acc = 0u;
		var itr = 0u;

    for (var offset = 0u; offset < SUBARRAY_SIZE; offset += 256u) {
      // Load data into shared memory
      let global_idx = global_base_idx + offset + local_idx;
		 	data[global_idx] += acc;
			acc += partials[itr];
			itr += 1u;
		}

		workgroupBarrier();
		
		if(local_id.x == 0u) {
			partial_sums[workgroup_id.x] = acc;
		}
}

@compute @workgroup_size(256)
fn prefix_sum_accumulate(@builtin(global_invocation_id) global_id: vec3<u32>, @builtin(local_invocation_id) local_id: vec3<u32>, @builtin(workgroup_id) workgroup_id: vec3<u32>) {
	 var local_idx = local_id.x;
   let global_base_idx = workgroup_id.x * SUBARRAY_SIZE;
	 let partialSum = partial_sums[workgroup_id.x];
		 
	 for (var offset = 0u; offset < SUBARRAY_SIZE; offset += 256u) {
     let global_idx = global_base_idx + offset + local_idx;
		 if (global_idx < arrayLength(&data)) {
			 data[global_idx] += partialSum;
		 }
	 }
}
