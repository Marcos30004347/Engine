// Optional material-aware leaf acceptance shared by Tree64 primary, shadow,
// GI and probe rays. The ordinary Tree64 variants do not include this file,
// preserving their Phase-0 instruction and binding paths exactly.
const TREE64_MATERIAL_CELL_VALID : u32 = 1u << 31u;
const TREE64_MATERIAL_HANDLE_MASK : u32 = 0xffffu;
const TREE64_MATERIAL_MAX_PROBES : u32 = 8u;

struct SparseVoxelTree64MaterialHit {
    distance : f32,
    parentExit : f32,
    accepted : bool,
}

fn tree64MaterialHash(cell : vec3<i32>) -> u32 {
    var value = bitcast<u32>(cell.x) * 0x8da6b343u;
    value = voxelGeometryMix(value ^ bitcast<u32>(cell.y) * 0xd8163841u);
    value = voxelGeometryMix(value ^ bitcast<u32>(cell.z) * 0xcb1ab31fu);
    return value;
}

fn tree64MaterialCell(cell : vec3<i32>) -> SparseVoxelTree64MaterialCell {
    let capacity = arrayLength(&tree64MaterialCells);
    if (capacity == 0u) {
        return SparseVoxelTree64MaterialCell(
            vec3<i32>(0), 0u, 0u, 0u, 0u, 0u);
    }
    let start = tree64MaterialHash(cell) % capacity;
    for (var probe = 0u; probe < TREE64_MATERIAL_MAX_PROBES; probe += 1u) {
        atomicAdd(&tree64MaterialCounters.sidecarProbes, 1u);
        let record = tree64MaterialCells[(start + probe) % capacity];
        if ((record.materialAndFlags & TREE64_MATERIAL_CELL_VALID) == 0u) {
            break;
        }
        if (all(record.globalCell == cell)) {
            return record;
        }
    }
    return SparseVoxelTree64MaterialCell(
        vec3<i32>(0), 0u, 0u, 0u, 0u, 0u);
}

// Exact material resolution in an arbitrary unit-cell grid. Ray distance stays
// in world metres because only origin/direction coordinates are normalized.
// `recordCell` is the stable sidecar key; `parentCell` is the cell intersected
// in this grid. Static trees use the same value for both, while dynamic page
// models namespace their local key by model id.
fn tree64ResolveMaterialRecordInGrid(
    ray : SparseVoxelTree64Ray, recordCell : vec3<i32>,
    parentCell : vec3<i32>,
    record : SparseVoxelTree64MaterialCell,
    candidateDistance : f32, maximumDistance : f32)
    -> SparseVoxelTree64MaterialHit {
    atomicAdd(&tree64MaterialCounters.candidateParents, 1u);
    let parentMinimum = vec3<f32>(parentCell);
    let parentMaximum = parentMinimum + vec3<f32>(1.0);
    let parentInterval = tree64IntersectBox(
        ray, parentMinimum, parentMaximum,
        candidateDistance, maximumDistance);
    let parentExit = select(
        candidateDistance, parentInterval.exit, parentInterval.valid != 0u);
    if ((record.materialAndFlags & TREE64_MATERIAL_CELL_VALID) == 0u) {
        atomicAdd(&tree64MaterialCounters.acceptedParents, 1u);
        return SparseVoxelTree64MaterialHit(
            candidateDistance, parentExit, true);
    }
    atomicAdd(&tree64MaterialCounters.contextLoads, 1u);
    let materialHandle = record.materialAndFlags & TREE64_MATERIAL_HANDLE_MASK;
    if (materialHandle == 0u ||
        materialHandle >= arrayLength(&tree64MicroMaterials)) {
        atomicAdd(&tree64MaterialCounters.acceptedParents, 1u);
        return SparseVoxelTree64MaterialHit(
            candidateDistance, parentExit, true);
    }

    let material = tree64MicroMaterials[materialHandle];
    let subdivision = material.geometry & 0x0fu;
    if (subdivision < 2u) {
        atomicAdd(&tree64MaterialCounters.acceptedParents, 1u);
        return SparseVoxelTree64MaterialHit(
            candidateDistance, parentExit, true);
    }
    // Transform the world ray into the parent's unit cube without changing
    // its distance parameter. The exact micro-DDA below is the same routine
    // used by brick raster visibility.
    let localOrigin = ray.originMaximumDistance.xyz - parentMinimum;
    let localDirection = ray.direction.xyz;
    let context = record.surfaceData >> 24u;
    let surfaceData = record.surfaceData & 0x00ffffffu;
    let profile = (material.geometry >> 18u) & 0x07u;
    let geometryExecutionFlags =
        sparseVoxelExactMicroGeometryExecutionFlags(materialHandle);
    let geometryIREnabled =
        (geometryExecutionFlags & VOXEL_GEOMETRY_PROGRAM_ENABLED) != 0u;
    let geometryIRVolume = geometryIREnabled &&
        (geometryExecutionFlags & 2u) != 0u;
    let geometryIRFieldCached = geometryIREnabled &&
        (geometryExecutionFlags &
         VOXEL_GEOMETRY_PROGRAM_MATERIAL_FIELD_MASK) != 0u;
    let legacyHeightfield =
        profile != SPARSE_VOXEL_EXACT_MICRO_VOLUME_DENSITY;
    let heightfield = select(
        legacyHeightfield,
        !geometryIRVolume && !geometryIRFieldCached,
        geometryIREnabled);
    var micro = sparseVoxelExactMicroMiss(
        parentExit, materialHandle, 0u);
    if (heightfield) {
        micro = traceSparseVoxelExactMicroOneFace(
            material, materialHandle, geometryExecutionFlags, recordCell,
            localOrigin, localDirection, candidateDistance, parentExit, 0u,
            context, surfaceData, record.auxContext, vec4<u32>(0u));
    } else {
        micro = traceSparseVoxelExactMicroVolume(
            material, materialHandle, geometryExecutionFlags, recordCell,
            localOrigin, localDirection, candidateDistance, parentExit, 0u,
            context, surfaceData, record.auxContext);
    }
    atomicAdd(&tree64MaterialCounters.microCellSteps, micro.steps);
    if (geometryIREnabled) {
        atomicAdd(
            &tree64MaterialCounters.interpreterInstructions,
            voxelGeometryPrograms[materialHandle].instructionCount *
                micro.steps);
    }
    if (micro.valid) {
        atomicAdd(&tree64MaterialCounters.acceptedParents, 1u);
        return SparseVoxelTree64MaterialHit(
            micro.distance, parentExit, true);
    }
    atomicAdd(&tree64MaterialCounters.carvedParents, 1u);
    return SparseVoxelTree64MaterialHit(parentExit, parentExit, false);
}

fn tree64ResolveMaterialLeafInGrid(
    ray : SparseVoxelTree64Ray, recordCell : vec3<i32>,
    parentCell : vec3<i32>,
    candidateDistance : f32, maximumDistance : f32)
    -> SparseVoxelTree64MaterialHit {
    return tree64ResolveMaterialRecordInGrid(
        ray, recordCell, parentCell, tree64MaterialCell(recordCell),
        candidateDistance, maximumDistance);
}

fn tree64ResolveMaterialLeaf(
    ray : SparseVoxelTree64Ray, globalCell : vec3<i32>,
    candidateDistance : f32, maximumDistance : f32)
    -> SparseVoxelTree64MaterialHit {
    let gridRay = SparseVoxelTree64Ray(
        vec4<f32>(
            ray.originMaximumDistance.xyz / treeState.cellSize,
            ray.originMaximumDistance.w),
        vec4<f32>(ray.direction.xyz / treeState.cellSize, 0.0));
    return tree64ResolveMaterialLeafInGrid(
        gridRay, globalCell, globalCell,
        candidateDistance, maximumDistance);
}

#ifdef TREE64_DYNAMIC_LAYER
// Dynamic terrain model ids occupy the high part of X and local cells occupy
// the low 16 bits. Tree64 model sides are capped at 65536, making this key
// collision-free for the fixed physical clipmap slots used by the renderer.
fn tree64DynamicMaterialCell(
    modelId : u32, localCell : vec3<i32>) -> vec3<i32> {
    return vec3<i32>(
        i32(modelId * 65536u) + localCell.x,
        localCell.y,
        localCell.z);
}

fn tree64TerrainPageMaterialCell(
    index : u32) -> SparseVoxelTree64MaterialCell {
    let word = index * 8u;
    return SparseVoxelTree64MaterialCell(
        vec3<i32>(
            bitcast<i32>(terrainTree64PageMaterialCells[word]),
            bitcast<i32>(terrainTree64PageMaterialCells[word + 1u]),
            bitcast<i32>(terrainTree64PageMaterialCells[word + 2u])),
        terrainTree64PageMaterialCells[word + 3u],
        terrainTree64PageMaterialCells[word + 4u],
        terrainTree64PageMaterialCells[word + 5u],
        terrainTree64PageMaterialCells[word + 6u],
        terrainTree64PageMaterialCells[word + 7u]);
}

fn tree64ResolveDynamicMaterialLeaf(
    ray : SparseVoxelTree64Ray, modelId : u32, localCell : vec3<i32>,
    terrainPage : bool, materialPageSlot : u32,
    materialCellsPerPage : u32, materialCellOffset : u32,
    materialCellCount : u32,
    candidateDistance : f32, maximumDistance : f32)
    -> SparseVoxelTree64MaterialHit {
    let recordCell = tree64DynamicMaterialCell(modelId, localCell);
    if (!terrainPage) {
        return tree64ResolveMaterialLeafInGrid(
            ray, recordCell, localCell,
            candidateDistance, maximumDistance);
    }
    var lower = 0u;
    var upper = materialCellCount;
    loop {
        if (lower >= upper) {
            break;
        }
        let middle = lower + (upper - lower) / 2u;
        let index = materialPageSlot * materialCellsPerPage +
            materialCellOffset + middle;
        let candidate = tree64TerrainPageMaterialCell(index);
        let before = candidate.globalCell.x < recordCell.x ||
            (candidate.globalCell.x == recordCell.x &&
             (candidate.globalCell.y < recordCell.y ||
              (candidate.globalCell.y == recordCell.y &&
               candidate.globalCell.z < recordCell.z)));
        if (before) {
            lower = middle + 1u;
        } else {
            upper = middle;
        }
    }
    var record = SparseVoxelTree64MaterialCell(
        vec3<i32>(0), 0u, 0u, 0u, 0u, 0u);
    if (lower < materialCellCount) {
        let index = materialPageSlot * materialCellsPerPage +
            materialCellOffset + lower;
        let candidate = tree64TerrainPageMaterialCell(index);
        if (all(candidate.globalCell == recordCell)) {
            record = candidate;
        }
    }
    return tree64ResolveMaterialRecordInGrid(
        ray, recordCell, localCell, record,
        candidateDistance, maximumDistance);
}
#endif
