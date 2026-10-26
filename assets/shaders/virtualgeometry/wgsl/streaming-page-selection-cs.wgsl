#include "virtualgeometrydata.wgsl"

const SENTINEL_VALUE : u32 = 0xFFFFFFFFu;
const WORKGROUP_SIZE : u32 = 128u;
const ITEMS_PER_GROUP : u32 = WORKGROUP_SIZE * 2u;
const TOP_N : u32 = 64u;

struct StreamingSelectionUniforms {
    inputCount           : u32,
    useRawInput          : u32,
    selectInstalled      : u32,
    selectLowestPriority : u32,
}

struct StreamingPageCandidate {
    globalPageIndex : u32,
    priority        : u32,
}

@group(0) @binding(0) var<uniform> uniforms : StreamingSelectionUniforms;
@group(0) @binding(1) var<storage, read> pageTable : array<PageTableEntry>;
@group(0) @binding(2) var<storage, read> pagePriorities : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> inputCandidates : array<StreamingPageCandidate>;
@group(0) @binding(4) var<storage, read_write> outputCandidates : array<StreamingPageCandidate>;

var<workgroup> wgCandidateIndices : array<u32, ITEMS_PER_GROUP>;
var<workgroup> wgCandidatePriorities : array<u32, ITEMS_PER_GROUP>;

/// @brief Constructs a sentinel StreamingPageCandidate representing an invalid or absent entry.
/// @returns A StreamingPageCandidate with globalPageIndex set to SENTINEL_VALUE.
fn invalidCandidate() -> StreamingPageCandidate {
    return StreamingPageCandidate(SENTINEL_VALUE, 0u);
}

/// @brief Returns true if the candidate holds a valid page index (i.e. is not the sentinel).
/// @param candidate The StreamingPageCandidate to test.
/// @returns True when the candidate's globalPageIndex is not SENTINEL_VALUE.
fn candidateValid(candidate: StreamingPageCandidate) -> bool {
    return candidate.globalPageIndex != SENTINEL_VALUE;
}

/// @brief Comparator for streaming page candidates: invalid entries sort before valid ones, and among valid entries ordering depends on priority direction.
/// @param a The first candidate to compare.
/// @param b The second candidate to compare.
/// @param selectLowestPriority When true, lower priority values are considered greater (candidate with higher priority wins); when false, higher priority values win.
/// @returns True when candidate a should be ordered before candidate b (a is strictly less than b in the sort order).
fn candidateLess(a: StreamingPageCandidate, b: StreamingPageCandidate, selectLowestPriority: bool) -> bool {
    let aValid = candidateValid(a);
    let bValid = candidateValid(b);

    if (aValid != bValid) {
        return !aValid;
    }
    if (!aValid) {
        return a.globalPageIndex > b.globalPageIndex;
    }

    if (a.priority != b.priority) {
        return select(a.priority < b.priority, a.priority > b.priority, selectLowestPriority);
    }
    return a.globalPageIndex > b.globalPageIndex;
}

/// @brief Loads a StreamingPageCandidate from the workgroup-shared candidate arrays by index.
/// @param index The slot index within the workgroup-shared candidate arrays.
/// @returns The StreamingPageCandidate stored at the given slot.
fn loadSharedCandidate(index: u32) -> StreamingPageCandidate {
    return StreamingPageCandidate(wgCandidateIndices[index], wgCandidatePriorities[index]);
}

/// @brief Stores a StreamingPageCandidate into the workgroup-shared candidate arrays at the given index.
/// @param index The slot index within the workgroup-shared candidate arrays.
/// @param candidate The StreamingPageCandidate to store.
fn storeSharedCandidate(index: u32, candidate: StreamingPageCandidate) {
    wgCandidateIndices[index] = candidate.globalPageIndex;
    wgCandidatePriorities[index] = candidate.priority;
}

/// @brief Conditionally swaps two candidates in workgroup-shared memory if they are out of sort order for the current Bitonic sort stage.
/// @param left The shared-memory index of the left candidate.
/// @param right The shared-memory index of the right candidate.
/// @param ascending When true, places the smaller candidate at the left index; when false, places the larger candidate there.
/// @param selectLowestPriority Passed through to candidateLess to determine priority ordering.
fn compareSwap(left: u32, right: u32, ascending: bool, selectLowestPriority: bool) {
    let leftCandidate = loadSharedCandidate(left);
    let rightCandidate = loadSharedCandidate(right);
    let shouldSwap = select(
        candidateLess(rightCandidate, leftCandidate, selectLowestPriority),
        candidateLess(leftCandidate, rightCandidate, selectLowestPriority),
        ascending);
    if (shouldSwap) {
        storeSharedCandidate(left, rightCandidate);
        storeSharedCandidate(right, leftCandidate);
    }
}

/// @brief Loads a streaming page candidate directly from the page table and priority buffer, returning an invalid candidate for unqualified entries.
/// @param rawIndex The global page table index to evaluate.
/// @returns A valid StreamingPageCandidate if the page qualifies (installed or not, per uniforms), otherwise invalidCandidate().
fn loadRawCandidate(rawIndex: u32) -> StreamingPageCandidate {
    if (rawIndex >= uniforms.inputCount || rawIndex >= arrayLength(&pageTable)) {
        return invalidCandidate();
    }

    let entry = pageTable[rawIndex];
    if (entry.prioritySlot >= arrayLength(&pagePriorities)) {
        return invalidCandidate();
    }

    let priority = atomicLoad(&pagePriorities[entry.prioritySlot]);
    let installed = entry.isInstalled != 0u;
    let shouldKeep = select(!installed && priority > 0u, installed, uniforms.selectInstalled != 0u);
    if (!shouldKeep) {
        return invalidCandidate();
    }

    return StreamingPageCandidate(rawIndex, priority);
}

/// @brief Loads a streaming page candidate from the pre-built input candidates array, returning an invalid candidate for out-of-bounds indices.
/// @param inputIndex The index into the inputCandidates storage buffer.
/// @returns The StreamingPageCandidate at the given index, or invalidCandidate() if out of bounds.
fn loadInputCandidate(inputIndex: u32) -> StreamingPageCandidate {
    if (inputIndex >= uniforms.inputCount || inputIndex >= arrayLength(&inputCandidates)) {
        return invalidCandidate();
    }
    return inputCandidates[inputIndex];
}

/// @brief Compute shader that performs a Bitonic sort on ITEMS_PER_GROUP streaming page candidates per workgroup and writes the top-N results to the output buffer.
@compute @workgroup_size(WORKGROUP_SIZE)
fn reduceStreamingCandidates(
    @builtin(local_invocation_index) lid : u32,
    @builtin(workgroup_id) wgid : vec3<u32>
) {
    let baseIndex = wgid.x * ITEMS_PER_GROUP;
    let selectLowestPriority = uniforms.selectLowestPriority != 0u;

    let inputIndex0 = baseIndex + lid;
    let inputIndex1 = baseIndex + lid + WORKGROUP_SIZE;
    if (uniforms.useRawInput != 0u) {
        storeSharedCandidate(lid, loadRawCandidate(inputIndex0));
        storeSharedCandidate(lid + WORKGROUP_SIZE, loadRawCandidate(inputIndex1));
    } else {
        storeSharedCandidate(lid, loadInputCandidate(inputIndex0));
        storeSharedCandidate(lid + WORKGROUP_SIZE, loadInputCandidate(inputIndex1));
    }
    workgroupBarrier();

    var k = 2u;
    loop {
        if (k > ITEMS_PER_GROUP) {
            break;
        }

        var j = k >> 1u;
        loop {
            for (var index = lid; index < ITEMS_PER_GROUP; index += WORKGROUP_SIZE) {
                let partner = index ^ j;
                if (partner > index) {
                    let stageAscending = ((index & k) == 0u);
                    compareSwap(index, partner, stageAscending, selectLowestPriority);
                }
            }
            workgroupBarrier();

            if (j == 1u) {
                break;
            }
            j >>= 1u;
        }

        k <<= 1u;
    }

    if (lid < TOP_N) {
        let outputIndex = wgid.x * TOP_N + lid;
        let candidateIndex = ITEMS_PER_GROUP - 1u - lid;
        outputCandidates[outputIndex] = loadSharedCandidate(candidateIndex);
    }
}
