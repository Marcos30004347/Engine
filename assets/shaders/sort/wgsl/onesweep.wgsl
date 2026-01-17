enable subgroups;
// Can't compile because of the strict uniform analysis :c

const PART_SIZE : u32 = 3840u;       //size of a partition tile
const US_DIM : u32 = 128u;           //The number of threads in a Upsweep threadblock
const SCAN_DIM : u32 = 128u;         //The number of threads in a Scan threadblock
const DS_DIM : u32 = 256u;           //The number of threads in a Downsweep threadblock

const RADIX : u32 = 256u;            //Number of digit bins
const RADIX_MASK : u32 = 255u;       //Mask of digit bins
const RADIX_LOG : u32 = 8u;          //log2(RADIX)

const HALF_RADIX : u32 = 128u;       //For smaller waves where bit packing is necessary
const HALF_MASK : u32 = 127u;        //'' 

//For the downsweep kernels
const DS_KEYS_PER_THREAD : u32 = 15u;    //The number of keys per thread in a Downsweep Threadblock
const MAX_DS_SMEM : u32 = 4096u;        //shared memory for downsweep kernel

struct ParallelSort {
    e_numKeys: u32,
    e_radixShift: u32,
    e_threadBlocks: u32,
    padding0: u32,
};

@group(0) @binding(0) var<uniform> cbParallelSort : ParallelSort;
@group(0) @binding(1) var<storage, read_write> b_sort: array<u32>;
@group(0) @binding(2) var<storage, read_write> b_alt: array<u32>;
@group(0) @binding(3) var<storage, read_write> b_sortPayload: array<u32>;
@group(0) @binding(4) var<storage, read_write> b_altPayload: array<u32>;
@group(0) @binding(5) var<storage, read_write> b_globalHist: array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> b_passHist: array<u32>;

var<workgroup> g_us: array<atomic<u32>, 512u>;
var<workgroup> g_scan: array<u32, 128u>;
var<workgroup> g_ds: array<atomic<u32>, 4096u>;

// fn getwaveGetLaneCount(u32 waveGetLaneCount) -> u32 {
//     return u32(waveGetLaneCount); // Returns the size of the current wave
// }

fn getWaveIndex(gtid: u32, waveGetLaneCount: u32) -> u32 {
    return gtid / waveGetLaneCount;
}

fn ExtractDigit(key: u32) -> u32 {
    return (key >> cbParallelSort.e_radixShift) & RADIX_MASK; // RADIX_MASK
}

fn ExtractPackedIndex(key: u32) -> u32 {
    return (key >> (cbParallelSort.e_radixShift + 1u)) & HALF_MASK; // HALF_MASK
}

fn ExtractPackedShift(key: u32) -> u32 {
    if ((key >> cbParallelSort.e_radixShift) & 1u) == 1u { 
        return 16u;
    } else { 
        return 0u;
    }
}

fn ExtractPackedValue(packed: u32, key: u32) -> u32 {
    return (packed >> ExtractPackedShift(key)) & 0xffffu;
}

fn SubPartSizeWGE16(waveGetLaneCount: u32) -> u32 {
    return DS_KEYS_PER_THREAD * waveGetLaneCount; // DS_KEYS_PER_THREAD * waveGetLaneCount
}

fn SharedOffsetWGE16(gtid: u32, waveGetLaneCount: u32, waveGetLaneIndex: u32) -> u32 {
	return waveGetLaneIndex + getWaveIndex(gtid, waveGetLaneCount) * SubPartSizeWGE16(waveGetLaneCount);
}

fn DeviceOffsetWGE16(gtid: u32, gid: u32, waveGetLaneCount: u32, waveGetLaneIndex: u32) -> u32 {
    return SharedOffsetWGE16(gtid, waveGetLaneCount, waveGetLaneIndex) + gid * 3840u; // PART_SIZE
}

fn SubPartSizeWLT16(serialIterations: u32, waveGetLaneCount : u32) -> u32 {
    return  DS_KEYS_PER_THREAD * waveGetLaneCount * serialIterations; // DS_KEYS_PER_THREAD * waveGetLaneCount * serialIterations
}

fn SharedOffsetWLT16(gtid: u32, serialIterations: u32, waveGetLaneIndex : u32, waveGetLaneCount: u32) -> u32 {
    return waveGetLaneIndex  +
			((getWaveIndex(gtid, waveGetLaneCount) / serialIterations) * SubPartSizeWLT16(serialIterations, waveGetLaneCount)) +
			((getWaveIndex(gtid, waveGetLaneCount) % serialIterations) * waveGetLaneCount);
}

fn DeviceOffsetWLT16(gtid: u32, gid: u32, serialIterations: u32, waveGetLaneIndex : u32, waveGetLaneCount: u32) -> u32 {
    return SharedOffsetWLT16(gtid, serialIterations, waveGetLaneIndex, waveGetLaneCount) + gid * 3840u; // PART_SIZE
}

fn GlobalHistOffset() -> u32 {
    return cbParallelSort.e_radixShift << 5u;
}

fn WaveHistsSizeWGE16(waveGetLaneCount: u32) -> u32 {
    return DS_DIM / waveGetLaneCount * RADIX; // DS_DIM / waveGetLaneCount * RADIX
}

fn WaveHistsSizeWLT16() -> u32 {
    return MAX_DS_SMEM; 
}

@compute @workgroup_size(1024, 1, 1)
fn InitDeviceRadixSort(@builtin(global_invocation_id) id: vec3<u32>) {
    if id.x < arrayLength(&b_globalHist) {
        atomicStore(&b_globalHist[id.x], 0u);
    }
}

@compute @workgroup_size(US_DIM)
fn upsweep(@builtin(workgroup_id) gid: vec3<u32>, @builtin(local_invocation_id) gtid: vec3<u32>, @builtin(subgroup_size) waveGetLaneCount: u32, @builtin(subgroup_invocation_id) waveGetLaneIndex : u32) {
	let histEnd = RADIX * 2u;
	for(var i = gtid.x; i < histEnd; i += US_DIM) {
		atomicStore(&g_us[i], 0u);
	}

	workgroupBarrier();

    let histOffset = gtid.x / 64u * RADIX;
    let partitionEnd = select((gid.x + 1) * PART_SIZE, cbParallelSort.e_numKeys,  gid.x == cbParallelSort.e_threadBlocks - 1u);

    for (var i = gtid.x + gid.x * PART_SIZE; i < partitionEnd; i += US_DIM) {
        atomicAdd(&g_us[ExtractDigit(b_sort[i]) + histOffset], 1u);
    }

    workgroupBarrier();
	
    for (var i = gtid.x; i < RADIX; i += US_DIM)
    {
        atomicAdd(&g_us[i], atomicLoad(&g_us[i + RADIX]));
        b_passHist[i * cbParallelSort.e_threadBlocks + gid.x] = atomicLoad(&g_us[i]);
    }
    
    if waveGetLaneCount >= 16u {
        for (var i = gtid.x; i < RADIX; i += US_DIM) {
            atomicAdd(&g_us[i], subgroupExclusiveAdd(atomicLoad(&g_us[i])));
        }

        workgroupBarrier();
        
        if (gtid.x < (RADIX / waveGetLaneCount))
        {
            atomicAdd(&g_us[(gtid.x + 1) * waveGetLaneCount - 1], subgroupExclusiveAdd(atomicLoad(&g_us[(gtid.x + 1u) * waveGetLaneCount - 1])));
        }

        workgroupBarrier();
        
        //atomically add to global histogram
        let globalHistOffset = GlobalHistOffset();
        let laneMask = waveGetLaneCount - 1u;
        let circularLaneShift = (waveGetLaneIndex + 1u) & laneMask;

        for (var i = gtid.x; i < RADIX; i += US_DIM)
        {
            let index = circularLaneShift + (i & ~laneMask);
            atomicAdd(&b_globalHist[index + globalHistOffset], select(0u, atomicLoad(&g_us[i]) , waveGetLaneIndex != laneMask) + select(0u, subgroupBroadcast(atomicLoad(&g_us[i - 1]), 0u), i >= waveGetLaneCount));
        }
    }

    if waveGetLaneCount < 16u
    {
        let globalHistOffset = GlobalHistOffset();

        for (var i = gtid.x; i < RADIX; i += US_DIM) {
            atomicAdd(&g_us[i], subgroupExclusiveAdd(atomicLoad(&g_us[i])));
        }
        
        if (gtid.x < waveGetLaneCount)
        {
            let circularLaneShift = (waveGetLaneIndex + 1u) & (waveGetLaneCount - 1u);
            atomicAdd(&b_globalHist[circularLaneShift + globalHistOffset], select(0u, atomicLoad(&g_us[gtid.x]), circularLaneShift != 0));
        }

        workgroupBarrier();
        
        let laneLog = countOneBits(waveGetLaneCount - 1u);
        var offset = laneLog;
        var j = waveGetLaneCount;
        for (; j < (RADIX >> 1); j <<= laneLog)
        {
            for (var i = gtid.x; i < (RADIX >> offset); i += US_DIM)
            {
                atomicAdd(&g_us[((i + 1u) << offset) - 1u], subgroupExclusiveAdd(atomicLoad(&g_us[((i + 1u) << offset) - 1u])));
            }

            workgroupBarrier();
            
            for (var i = gtid.x + j; i < RADIX; i += US_DIM)
            {
                if ((i & ((j << laneLog) - 1)) >= j)
                {
                    if (i < (j << laneLog))
                    {
                        atomicAdd(&b_globalHist[i + globalHistOffset], subgroupBroadcast(atomicLoad(&g_us[((i >> offset) << offset) - 1u]), 0u) + select(0u, atomicLoad(&g_us[i - 1u]), (i & (j - 1u)) != 0));
                    }
                    else
                    {
                        if ((i + 1u) & (j - 1u)) != 0
                        {
                            atomicAdd(&g_us[i], subgroupBroadcast(atomicLoad(&g_us[((i >> offset) << offset) - 1]), 0));
                        }
                    }
                }
            }
            offset += laneLog;
        }

        workgroupBarrier();
        
        for (var i = gtid.x + j; i < RADIX; i += US_DIM)
        {
            atomicAdd(&b_globalHist[i + globalHistOffset], subgroupBroadcast(atomicLoad(&g_us[((i >> offset) << offset) - 1u]), 0) + select(0, atomicLoad(&g_us[i - 1u]), (i & (j - 1)) != 0));
        }
    }
}

@compute @workgroup_size(SCAN_DIM)
fn scan(@builtin(workgroup_id) gid: vec3<u32>, @builtin(local_invocation_id) gtid: vec3<u32>, @builtin(subgroup_size) waveGetLaneCount: u32, @builtin(subgroup_invocation_id) waveGetLaneIndex : u32) {
    if waveGetLaneCount >= 16u {
        var aggregate = 0u;
        let laneMask = waveGetLaneCount - 1u;
        let circularLaneShift = (waveGetLaneIndex + 1u) & laneMask;
        let partionsEnd = cbParallelSort.e_threadBlocks / SCAN_DIM * SCAN_DIM;
        let offset = gid.x * cbParallelSort.e_threadBlocks;
        var i = gtid.x;

        for (; i < partionsEnd; i += SCAN_DIM)
        {
            g_scan[gtid.x] = b_passHist[i + offset];
            g_scan[gtid.x] += subgroupExclusiveAdd(g_scan[gtid.x]);
            workgroupBarrier();
            
            if (gtid.x < SCAN_DIM / waveGetLaneCount)
            {
                g_scan[(gtid.x + 1u) * waveGetLaneCount - 1u] += subgroupExclusiveAdd(g_scan[(gtid.x + 1u) * waveGetLaneCount - 1u]);
            }
            workgroupBarrier();
            
            b_passHist[circularLaneShift + (i & ~laneMask) + offset] =
                select(0u, g_scan[gtid.x], waveGetLaneIndex != laneMask) +
                select(0u, subgroupBroadcast(g_scan[gtid.x - 1u], 0u), gtid.x >= waveGetLaneCount) +
                aggregate;

            aggregate += g_scan[SCAN_DIM - 1u];
            workgroupBarrier();
        }
        
        if (i < cbParallelSort.e_threadBlocks) {
            g_scan[gtid.x] = b_passHist[offset + i];
        }

        g_scan[gtid.x] += subgroupExclusiveAdd(g_scan[gtid.x]);
        
        workgroupBarrier();
            
        if (gtid.x < SCAN_DIM / waveGetLaneCount)
        {
            g_scan[(gtid.x + 1u) * waveGetLaneCount - 1u] += subgroupExclusiveAdd(g_scan[(gtid.x + 1) * waveGetLaneCount - 1u]);
        }

        workgroupBarrier();
        
        let index = circularLaneShift + (i & ~laneMask);
        if (index < cbParallelSort.e_threadBlocks)
        {
            b_passHist[index + offset] = select(0, g_scan[gtid.x], waveGetLaneIndex != laneMask) + select(0, g_scan[(gtid.x & ~laneMask) - 1u], gtid.x >= waveGetLaneCount) + aggregate;
        }
    }

    if (waveGetLaneCount < 16u)
    {
        var aggregate = 0u;
        let partitions = cbParallelSort.e_threadBlocks / SCAN_DIM;
        let deviceOffset = gid.x * cbParallelSort.e_threadBlocks;
        let laneLog = countOneBits(waveGetLaneCount - 1u);
        let circularLaneShift = (waveGetLaneIndex + 1u) & (waveGetLaneCount - 1u);
        
        var k = 0u;
        for (; k < partitions; k++)
        {
            g_scan[gtid.x] = b_passHist[gtid.x + k * SCAN_DIM + deviceOffset];
            g_scan[gtid.x] += subgroupExclusiveAdd(g_scan[gtid.x]);
            
            if (gtid.x < waveGetLaneCount)
            {
                b_passHist[circularLaneShift + k * SCAN_DIM + deviceOffset] = select(0u, g_scan[gtid.x], circularLaneShift != 0) + aggregate;
            }

            workgroupBarrier();
            
            var offset = laneLog;
            var j = waveGetLaneCount;

            for (; j < (SCAN_DIM >> 1); j <<= laneLog)
            {
                for (var i = gtid.x; i < (SCAN_DIM >> offset); i += SCAN_DIM)
                {
                    g_scan[((i + 1) << offset) - 1] += subgroupExclusiveAdd(g_scan[((i + 1) << offset) - 1]);
                }
                workgroupBarrier();
            
                if ((gtid.x & ((j << laneLog) - 1)) >= j)
                {
                    if (gtid.x < (j << laneLog))
                    {
                        b_passHist[gtid.x + k * SCAN_DIM + deviceOffset] = subgroupBroadcast(g_scan[((gtid.x >> offset) << offset) - 1u], 0u) + select(0u, g_scan[gtid.x - 1u], (gtid.x & (j - 1)) != 0) + aggregate;
                    }
                    else
                    {
                        if ((gtid.x + 1) & (j - 1)) != 0
                        {
                            g_scan[gtid.x] += subgroupBroadcast(g_scan[((gtid.x >> offset) << offset) - 1u], 0u);
                        }
                    }
                }
                offset += laneLog;
            }
            workgroupBarrier();
        
            for (var i = gtid.x + j; i < SCAN_DIM; i += SCAN_DIM)
            {
                b_passHist[i + k * SCAN_DIM + deviceOffset] = subgroupBroadcast(g_scan[((i >> offset) << offset) - 1u], 0) + select(0, g_scan[i - 1], (i & (j - 1)) != 0) + aggregate;
            }
            
            aggregate += subgroupBroadcast(g_scan[SCAN_DIM - 1], 0) + subgroupBroadcast(g_scan[(((SCAN_DIM - 1u) >> offset) << offset) - 1u], 0u);

            workgroupBarrier();
        }
        
        //partial
        let finalPartSize = cbParallelSort.e_threadBlocks - k * SCAN_DIM;

        if (gtid.x < finalPartSize)
        {
            g_scan[gtid.x] = b_passHist[gtid.x + k * SCAN_DIM + deviceOffset];
            g_scan[gtid.x] += subgroupExclusiveAdd(g_scan[gtid.x]);
        }
        
        if (gtid.x < waveGetLaneCount && circularLaneShift < finalPartSize)
        {
            b_passHist[circularLaneShift + k * SCAN_DIM + deviceOffset] = select(0u, g_scan[gtid.x], circularLaneShift != 0) + aggregate;
        }

        workgroupBarrier();
        
        var offset = laneLog;

        for (var j = waveGetLaneCount; j < finalPartSize; j <<= laneLog)
        {
            for (var i = gtid.x; i < (finalPartSize >> offset); i += SCAN_DIM)
            {
                g_scan[((i + 1) << offset) - 1] += subgroupExclusiveAdd(g_scan[((i + 1) << offset) - 1]);
            }

            workgroupBarrier();
            
            if ((gtid.x & ((j << laneLog) - 1)) >= j && gtid.x < finalPartSize)
            {
                if (gtid.x < (j << laneLog))
                {
                    b_passHist[gtid.x + k * SCAN_DIM + deviceOffset] = subgroupBroadcast(g_scan[((gtid.x >> offset) << offset) - 1], 0) + select(0, g_scan[gtid.x - 1], (gtid.x & (j - 1)) != 0) + aggregate;
                }
                else
                {
                    if ((gtid.x + 1) & (j - 1)) != 0
                    {
                        g_scan[gtid.x] += subgroupBroadcast(g_scan[((gtid.x >> offset) << offset) - 1u], 0u);
                    }
                }
            }

            offset += laneLog;
        }
    }
}



@compute @workgroup_size(DS_DIM)
fn Downsweep(@builtin(workgroup_id) gid: vec3<u32>, @builtin(local_invocation_id) gtid: vec3<u32>, @builtin(subgroup_size) waveGetLaneCount: u32, @builtin(subgroup_invocation_id) waveGetLaneIndex : u32) {
 if (gid.x <cbParallelSort.e_threadBlocks - 1)
    {
        var keys = array<u32,DS_KEYS_PER_THREAD>();
        var offsets = array<u32,DS_KEYS_PER_THREAD>();
        
        if (waveGetLaneCount >= 16u)
        {
            var t = DeviceOffsetWGE16(gtid.x, gid.x, waveGetLaneCount, waveGetLaneIndex);
            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
            {
                keys[i] = b_sort[t];
                t += waveGetLaneCount;
            }
            
            //Clear histogram memory
            for (var i = gtid.x; i < WaveHistsSizeWGE16(waveGetLaneCount); i += DS_DIM) {
                atomicStore(&g_ds[i], 0u);
            }

            workgroupBarrier();

            let waveParts = (waveGetLaneCount + 31) / 32;
            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
            {
                var waveFlags = vec4<u32>();
                // select(0xffffffff, (1u << waveGetLaneCount) - 1u, (waveGetLaneCount & 31u));
                waveFlags[0] = select(0xffffffff, (1u << waveGetLaneCount) - 1u, (waveGetLaneCount & 31u) != 0);
                waveFlags[1] = select(0xffffffff, (1u << waveGetLaneCount) - 1u, (waveGetLaneCount & 31u) != 0);
                waveFlags[2] = select(0xffffffff, (1u << waveGetLaneCount) - 1u, (waveGetLaneCount & 31u) != 0);
                waveFlags[3] = select(0xffffffff, (1u << waveGetLaneCount) - 1u, (waveGetLaneCount & 31u) != 0);

                for (var k = 0u; k < RADIX_LOG; k++)
                {
                    let t = keys[i] >> ((k +cbParallelSort.e_radixShift) & 1u);
                    let ballot = subgroupBallot(t > 0);
                    
                    for (var wavePart = 0u; wavePart < waveParts; wavePart++) {
                        waveFlags[wavePart] &= select(0xffffffff, 0u, t > 0) ^ ballot[wavePart];
                    }
                }
                    
                var bits : u32 = 0u;
                for (var wavePart = 0u; wavePart < waveParts; wavePart++)
                {
                    if (waveGetLaneIndex >= wavePart * 32u)
                    {
                        var ltMask = select((1u << (waveGetLaneIndex & 31u)) - 1u, 0xffffffffu, waveGetLaneIndex >= (wavePart + 1u) * 32u);
                        bits += countOneBits(waveFlags[wavePart] & ltMask);
                    }
                }
                    
                let index = ExtractDigit(keys[i]) + (getWaveIndex(gtid.x, waveGetLaneCount) * RADIX);
                offsets[i] = atomicLoad(&g_ds[index]) + bits;
                    
                workgroupBarrier();

                if (bits == 0u)
                {
                    for (var wavePart = 0u; wavePart < waveParts; wavePart++) {
                        atomicAdd(&g_ds[index], countOneBits(waveFlags[wavePart]));
                    }
                }
                
                workgroupBarrier();
            }
            
            //inclusive/exclusive prefix sum up the histograms
            //followed by exclusive prefix sum across the reductions
            var reduction = atomicLoad(&g_ds[gtid.x]);
            for (var i = gtid.x + RADIX; i < WaveHistsSizeWGE16(waveGetLaneCount); i += RADIX)
            {
                reduction += atomicLoad(&g_ds[i]);
                atomicStore(&g_ds[i], reduction - atomicLoad(&g_ds[i]));
            }
            
            reduction += subgroupExclusiveAdd(reduction);
            workgroupBarrier();

            let laneMask = waveGetLaneCount - 1u;

            atomicStore(&g_ds[((waveGetLaneIndex + 1) & laneMask) + (gtid.x & ~laneMask)], reduction);
            
            workgroupBarrier();
                
            if (gtid.x < RADIX / waveGetLaneCount)
            {
                atomicStore(&g_ds[gtid.x * waveGetLaneCount], subgroupExclusiveAdd(atomicLoad(&g_ds[gtid.x * waveGetLaneCount])));
            }
            workgroupBarrier();
                
            if (waveGetLaneIndex != 0) {
                atomicAdd(&g_ds[gtid.x], subgroupBroadcast(atomicLoad(&g_ds[gtid.x - 1u]), 1u));
            }

            workgroupBarrier();
        
            //Update offsets
            if (gtid.x >= waveGetLaneCount)
            {
                let t = getWaveIndex(gtid.x, waveGetLaneCount) * RADIX;

                for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
                {
                    let t2 = ExtractDigit(keys[i]);
                    offsets[i] += atomicLoad(&g_ds[t2 + t]) + atomicLoad(&g_ds[t2]);
                }
            }
            else
            {
                for (var i = 0u; i < DS_KEYS_PER_THREAD; i++) {
                    offsets[i] += atomicLoad(&g_ds[ExtractDigit(keys[i])]);
                }
            }
            
            let exclusiveWaveReduction = atomicLoad(&g_ds[gtid.x]);
            workgroupBarrier();
            
            //scatter keys into shared memory
            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++) {
                let offset = offsets[i];
                atomicStore(&g_ds[offset], keys[i]);
            }
        
            atomicStore(&g_ds[gtid.x + PART_SIZE], atomicLoad(&b_globalHist[gtid.x + GlobalHistOffset()]) + b_passHist[gtid.x *cbParallelSort.e_threadBlocks + gid.x] - exclusiveWaveReduction);
            
            workgroupBarrier();

            t = SharedOffsetWGE16(gtid.x, waveGetLaneCount, waveGetLaneIndex);

            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
            {
                keys[i] = atomicLoad(&g_ds[ExtractDigit(atomicLoad(&g_ds[t])) + PART_SIZE]) + t;

                let offset = keys[i];
                b_alt[offset] = atomicLoad(&g_ds[t]);
                
                t += waveGetLaneCount;
            }
            
            workgroupBarrier();
                
            t = DeviceOffsetWGE16(gtid.x, gid.x, waveGetLaneCount, waveGetLaneIndex);
            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
            {
                let offset = offsets[i];
                atomicStore(&g_ds[offset], b_sortPayload[t]);
                t += waveGetLaneCount;
            }

            workgroupBarrier();
            
            t = SharedOffsetWGE16(gtid.x, waveGetLaneCount, waveGetLaneIndex);

            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
            {
                let offset = keys[i];
                b_altPayload[offset] = atomicLoad(&g_ds[t]);
                t += waveGetLaneCount;
            }
        }
        
        if (waveGetLaneCount < 16u)
        {
            let serialIterations = (DS_DIM / waveGetLaneCount + 31u) / 32u;
            var t = DeviceOffsetWLT16(gtid.x, gid.x, serialIterations, waveGetLaneIndex, waveGetLaneCount);
            
            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
            {
                keys[i] = b_sort[t];
                t += waveGetLaneCount * serialIterations;
            }
                
            for (var i = gtid.x; i < WaveHistsSizeWLT16(); i += DS_DIM) {
                atomicStore(&g_ds[i], 0u);
            }

            workgroupBarrier();
            
            let ltMask = (1u << waveGetLaneIndex) - 1u;
            
            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
            {
                var waveFlag = (1u << waveGetLaneCount) - 1; //for full agnostic add ternary and var4
                
                for (var k = 0u; k < RADIX_LOG; k++)
                {
                    let t = (((keys[i] >> (k +cbParallelSort.e_radixShift)) & 1) > 0);
                    waveFlag &= select(0xffffffff, 0u, t) ^ (u32(subgroupBallot(t).x));
                }
                
                var bits = countOneBits(waveFlag & ltMask);
                let index = ExtractPackedIndex(keys[i]) + (getWaveIndex(gtid.x, waveGetLaneCount) / serialIterations * HALF_RADIX);
                    
                for (var k = 0u; k < serialIterations; k++)
                {
                    if (getWaveIndex(gtid.x, waveGetLaneCount) % serialIterations == k) {
                        offsets[i] = ExtractPackedValue(atomicLoad(&g_ds[index]), keys[i]) + bits;
                    }

                    workgroupBarrier();
                    
                    if (getWaveIndex(gtid.x, waveGetLaneCount) % serialIterations == k && bits == 0u)
                    {
                        atomicAdd(&g_ds[index], countOneBits(waveFlag) << ExtractPackedShift(keys[i]));
                    }
                    workgroupBarrier();
                }
            }
            
            //inclusive/exclusive prefix sum up the histograms,
            //use a blelloch scan for in place exclusive
            var reduction : u32;
            if (gtid.x < HALF_RADIX)
            {
                reduction = atomicLoad(&g_ds[gtid.x]);
                
                for (var i = gtid.x + HALF_RADIX; i < WaveHistsSizeWLT16(); i += HALF_RADIX)
                {
                    reduction += atomicLoad(&g_ds[i]);
                    atomicStore(&g_ds[i], reduction - atomicLoad(&g_ds[i]));
                }

                atomicStore(&g_ds[gtid.x], reduction + (reduction << 16));
            }
                
            var shift : u32 = 1u;

            for (var j = RADIX >> 2; j > 0u; j >>= 1)
            {
                workgroupBarrier();
                
                for (var i = gtid.x; i < j; i += DS_DIM)
                {
                    atomicAdd(&g_ds[(((((i << 1u) + 2u) << shift) - 1u) >> 1u)], atomicLoad(&g_ds[((((i << 1u) + 1u) << shift) - 1u) >> 1u]) & 0xffff0000);
                }

                shift++;
            }
            workgroupBarrier();
                
            if (gtid.x == 0u) {
                atomicAnd(&g_ds[HALF_RADIX - 1], 0xffff);
            }

            for (var j = 1u; j < (RADIX >> 1u); j <<= 1u)
            {
                shift--;

                workgroupBarrier();
                
                for (var i = gtid.x; i < j; i += DS_DIM)
                {
                    let t = (((((i << 1u) + 1u) << shift) - 1u) >> 1u);
                    let t2 = (((((i << 1u) + 2u) << shift) - 1u) >> 1u);
                    let t3 = atomicLoad(&g_ds[t]);

                    atomicStore(&g_ds[t], ((atomicLoad(&g_ds[t]) & 0xffff) | (atomicLoad(&g_ds[t2]) & 0xffff0000)));
                    atomicAdd(&g_ds[t2], t3 & 0xffff0000);
                }
            }

            workgroupBarrier();

            if (gtid.x < HALF_RADIX)
            {
                let t = atomicLoad(&g_ds[gtid.x]);
                atomicStore(&g_ds[gtid.x], (t >> 16u) + (t << 16u) + (t & 0xffff0000));
            }
            
            workgroupBarrier();
            
            //Update offsets
            if (gtid.x >= waveGetLaneCount * serialIterations)
            {
                let t = getWaveIndex(gtid.x, waveGetLaneCount) / serialIterations * HALF_RADIX;

                for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
                {
                    let t2 = ExtractPackedIndex(keys[i]);
                    offsets[i] += ExtractPackedValue(atomicLoad(&g_ds[t2 + t]) + atomicLoad(&g_ds[t2]), keys[i]);
                }
            }
            else
            {
                for (var i = 0u; i < DS_KEYS_PER_THREAD; i++) {
                    offsets[i] += ExtractPackedValue(atomicLoad(&g_ds[ExtractPackedIndex(keys[i])]), keys[i]);
                }
            }
            
            let exclusiveWaveReduction = ((atomicLoad(&g_ds[gtid.x >> 1]) >> select(0u, 16u, (gtid.x & 1) != 0)) & 0xffff);
            workgroupBarrier();
            
            //scatter keys into shared memory
            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++) {
                let offset = offsets[i];
                atomicStore(&g_ds[offset], keys[i]);
            }
        
            atomicStore(&g_ds[gtid.x + PART_SIZE], atomicLoad(&b_globalHist[gtid.x + GlobalHistOffset()]) + b_passHist[gtid.x *cbParallelSort.e_threadBlocks + gid.x] - exclusiveWaveReduction);
            workgroupBarrier();
        
            //scatter runs of keys into device memory, 
            //store the scatter location in the key register to reuse for the payload
            t = SharedOffsetWLT16(gtid.x, serialIterations, waveGetLaneIndex, waveGetLaneCount);
            
            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
            {
                keys[i] = atomicLoad(&g_ds[ExtractDigit(atomicLoad(&g_ds[t])) + PART_SIZE]) + t;
                var offset = keys[i];
                b_alt[offset] = atomicLoad(&g_ds[t]);
                t += waveGetLaneCount * serialIterations;
            }
            
            workgroupBarrier();
            
            t = DeviceOffsetWLT16(gtid.x, gid.x, serialIterations, waveGetLaneIndex, waveGetLaneCount);
            
            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
            {
                var offset = offsets[i];
                atomicStore(&g_ds[offset], b_sortPayload[t]);
                t += waveGetLaneCount * serialIterations;
            }

            workgroupBarrier();
            
            t = SharedOffsetWLT16(gtid.x, serialIterations, waveGetLaneIndex, waveGetLaneCount);

            for (var i = 0u; i < DS_KEYS_PER_THREAD; i++)
            {
                var offset = keys[i];
                b_altPayload[offset] = atomicLoad(&g_ds[t]);
                t += waveGetLaneCount * serialIterations;
            }
        }
    }
    
    //perform the sort on the final partition slightly differently 
    //to handle input sizes not perfect multiples of the partition
    if (gid.x ==cbParallelSort.e_threadBlocks - 1u)
    {
        //load the global and pass histogram values into shared memory
        if (gtid.x < RADIX)
        {
            atomicStore(&g_ds[gtid.x], atomicLoad(&b_globalHist[gtid.x + GlobalHistOffset()]) + b_passHist[gtid.x *cbParallelSort.e_threadBlocks + gid.x]);
        }

        workgroupBarrier();
        
        let waveParts = (waveGetLaneCount + 31) / 32;
        let partEnd = (cbParallelSort.e_numKeys + DS_DIM - 1) / DS_DIM * DS_DIM;
        for (var i = gtid.x + gid.x * PART_SIZE; i < partEnd; i += DS_DIM)
        {
            var key : u32 = 0u;

            if (i <cbParallelSort.e_numKeys)
            {
                key = b_sort[i];
            }
            
            var waveFlags = array<u32, 4>();

            waveFlags[0] = select(0xffffffff, (1u << waveGetLaneCount) - 1u, (waveGetLaneCount & 31u) != 0);
            waveFlags[1] = select(0xffffffff, (1u << waveGetLaneCount) - 1u, (waveGetLaneCount & 31u) != 0);
            waveFlags[2] = select(0xffffffff, (1u << waveGetLaneCount) - 1u, (waveGetLaneCount & 31u) != 0);
            waveFlags[3] = select(0xffffffff, (1u << waveGetLaneCount) - 1u, (waveGetLaneCount & 31u) != 0);

            var offset : u32 = 0u;
            var bits = 0u;

            if (i < cbParallelSort.e_numKeys)
            {
                for (var k = 0u; k < RADIX_LOG; k++)
                {
                    let t = ((key >> (k +cbParallelSort.e_radixShift)) & 1) > 0;

                    var ballot = subgroupBallot(t);

                    for (var wavePart = 0u; wavePart < waveParts; wavePart++) {
                        waveFlags[wavePart] &= select(0xffffffff, 0u, t) ^ ballot[wavePart];
                    }
                }
            
                for (var wavePart = 0u; wavePart < waveParts; wavePart++)
                {
                    if (waveGetLaneIndex >= wavePart * 32u)
                    {
                        let ltMask = select((1u << (waveGetLaneIndex & 31u)) - 1u, 0xffffffff, waveGetLaneIndex >= (wavePart + 1) * 32);
                        bits += countOneBits(waveFlags[wavePart] & ltMask);
                    }
                }
            }
            
            for (var k = 0u; k < DS_DIM / waveGetLaneCount; k++)
            {
                if (getWaveIndex(gtid.x, waveGetLaneCount) == k && i <cbParallelSort.e_numKeys) {
                    offset = atomicLoad(&g_ds[ExtractDigit(key)]) + bits;
                }

                workgroupBarrier();
                
                if (getWaveIndex(gtid.x, waveGetLaneCount) == k && i <cbParallelSort.e_numKeys && bits == 0)
                {
                    for (var wavePart = 0u; wavePart < waveParts; wavePart++) {
                        atomicAdd(&g_ds[ExtractDigit(key)], countOneBits(waveFlags[wavePart]));
                    }
                }
                workgroupBarrier();
            }

            if (i <cbParallelSort.e_numKeys)
            {
                b_alt[offset] = key;
                b_altPayload[offset] = b_sortPayload[i];
            }
        }
    }
}