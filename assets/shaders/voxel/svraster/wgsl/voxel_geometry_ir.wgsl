// Fixed Q16.16 GeometryIR evaluator. Records and arithmetic mirror
// voxel/material/VoxelGeometryIR.hpp; no floating-point operation participates
// in occupancy decisions.
struct VoxelGeometryProgram {
    instructionOffset : u32,
    instructionCount : u32,
    executionFlags : u32,
    builtInProfile : u32,
}

struct VoxelGeometryInstruction {
    opcodeOperands : u32,
    immediate : i32,
    reserved0 : u32,
    reserved1 : u32,
}

struct VoxelGeometryParameters {
    values : vec4<i32>,
}

struct VoxelMaterialFieldDefaults {
    values : vec4<u32>,
}

struct VoxelGeometryResult {
    retainedHeight : i32,
    carveDepth : i32,
    coverage : i32,
}

const VOXEL_GEOMETRY_FIXED_ONE : i32 = 65536;
const VOXEL_GEOMETRY_PROGRAM_ENABLED : u32 = 1u;
const VOXEL_GEOMETRY_PROGRAM_MATERIAL_FIELD_MASK : u32 = 0xf00u;

fn voxelGeometryDefaultMaterialFields(materialHandle : u32) -> vec4<i32> {
    if (materialHandle >= arrayLength(&voxelGeometryPrograms) ||
        materialHandle >= arrayLength(&voxelMaterialFieldDefaults) ||
        (voxelGeometryPrograms[materialHandle].executionFlags &
            VOXEL_GEOMETRY_PROGRAM_MATERIAL_FIELD_MASK) == 0u) {
        return vec4<i32>(0);
    }
    return vec4<i32>(min(
        voxelMaterialFieldDefaults[materialHandle].values,
        vec4<u32>(u32(VOXEL_GEOMETRY_FIXED_ONE))));
}

fn voxelGeometryMix(valueIn : u32) -> u32 {
    var value = valueIn;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

fn voxelGeometryMultiply(a : i32, b : i32) -> i32 {
    let negative = (a < 0) != (b < 0);
    let ua = u32(abs(a));
    let ub = u32(abs(b));
    let product = (ua >> 16u) * ub +
        ((ua & 0xffffu) * (ub >> 16u)) +
        (((ua & 0xffffu) * (ub & 0xffffu)) >> 16u);
    return select(i32(product), -i32(product), negative);
}

fn voxelGeometryWorldNoise(
    parentKey : vec3<i32>, faceCell : vec2<u32>, volumeCell : vec3<u32>,
    face : u32,
    subdivisionIn : u32, seed : u32, salt : u32) -> i32 {
    let subdivision = max(subdivisionIn, 1u);
    if (face >= 6u) {
        let coordinate = bitcast<vec3<u32>>(parentKey) * subdivision +
            volumeCell;
        var volumeValue = seed ^ salt;
        volumeValue = voxelGeometryMix(
            volumeValue ^ coordinate.x * 0x8da6b343u);
        volumeValue = voxelGeometryMix(
            volumeValue ^ coordinate.y * 0xd8163841u);
        volumeValue = voxelGeometryMix(
            volumeValue ^ coordinate.z * 0xcb1ab31fu);
        return i32(voxelGeometryMix(volumeValue ^ 0x517cc1b7u) >> 16u);
    }
    let axis = (face >> 1u) % 3u;
    let uAxis = select(0u, 2u, axis == 0u);
    let vAxis = select(1u, 2u, axis == 1u);
    var coordinate = bitcast<vec3<u32>>(parentKey) * subdivision;
    coordinate[uAxis] += faceCell.x;
    coordinate[vAxis] += faceCell.y;
    if ((face & 1u) != 0u) {
        coordinate[axis] += subdivision;
    }
    var value = seed ^ salt;
    value = voxelGeometryMix(value ^ coordinate.x * 0x8da6b343u);
    value = voxelGeometryMix(value ^ coordinate.y * 0xd8163841u);
    value = voxelGeometryMix(value ^ coordinate.z * 0xcb1ab31fu);
    value = voxelGeometryMix(value ^ face * 0x85ebca6bu);
    return i32(voxelGeometryMix(value) >> 16u);
}

fn voxelGeometryEvaluate(
    materialHandle : u32, parentKey : vec3<i32>, faceCell : vec2<u32>,
    volumeCell : vec3<u32>,
    face : u32, subdivision : u32, seed : u32, context : u32,
    baseSurface : i32, auxContext : u32,
    materialFields : vec4<i32>) -> VoxelGeometryResult {
    var result = VoxelGeometryResult(
        VOXEL_GEOMETRY_FIXED_ONE, 0, VOXEL_GEOMETRY_FIXED_ONE);
    if (materialHandle >= arrayLength(&voxelGeometryPrograms) ||
        materialHandle >= arrayLength(&voxelGeometryParameters)) {
        return result;
    }
    let program = voxelGeometryPrograms[materialHandle];
    if ((program.executionFlags & VOXEL_GEOMETRY_PROGRAM_ENABLED) == 0u) {
        return result;
    }
    var registers = array<i32, 8>();
    for (var index = 0u; index < 24u; index += 1u) {
        if (index >= program.instructionCount) {
            break;
        }
        let instruction = voxelGeometryInstructions[
            program.instructionOffset + index];
        let opcode = instruction.opcodeOperands & 0xffu;
        let destination = (instruction.opcodeOperands >> 8u) & 0xffu;
        let source0 = (instruction.opcodeOperands >> 16u) & 0xffu;
        let source1 = instruction.opcodeOperands >> 24u;
        let a = registers[min(source0, 7u)];
        let b = registers[min(source1, 7u)];
        if (opcode == 0u) {
            registers[destination] = clamp(
                baseSurface, 0, VOXEL_GEOMETRY_FIXED_ONE);
        } else if (opcode == 1u) {
            registers[destination] = voxelGeometryWorldNoise(
                parentKey, faceCell, volumeCell, face, subdivision, seed,
                bitcast<u32>(instruction.immediate));
        } else if (opcode == 2u) {
            registers[destination] = instruction.immediate;
        } else if (opcode == 3u) {
            registers[destination] =
                clamp(voxelGeometryParameters[materialHandle].values[source0],
                    -VOXEL_GEOMETRY_FIXED_ONE, VOXEL_GEOMETRY_FIXED_ONE);
        } else if (opcode == 4u) {
            registers[destination] = select(
                0, VOXEL_GEOMETRY_FIXED_ONE,
                (context & bitcast<u32>(instruction.immediate)) != 0u);
        } else if (opcode == 5u) {
            registers[destination] = clamp(
                a + b, -VOXEL_GEOMETRY_FIXED_ONE,
                VOXEL_GEOMETRY_FIXED_ONE);
        } else if (opcode == 6u) {
            registers[destination] = clamp(
                a - b, -VOXEL_GEOMETRY_FIXED_ONE,
                VOXEL_GEOMETRY_FIXED_ONE);
        } else if (opcode == 7u) {
            registers[destination] = clamp(
                voxelGeometryMultiply(a, b), -VOXEL_GEOMETRY_FIXED_ONE,
                VOXEL_GEOMETRY_FIXED_ONE);
        } else if (opcode == 8u) {
            registers[destination] = min(a, b);
        } else if (opcode == 9u) {
            registers[destination] = max(a, b);
        } else if (opcode == 10u) {
            registers[destination] = clamp(
                VOXEL_GEOMETRY_FIXED_ONE - a,
                -VOXEL_GEOMETRY_FIXED_ONE, VOXEL_GEOMETRY_FIXED_ONE);
        } else if (opcode == 11u) {
            registers[destination] = clamp(a, 0, VOXEL_GEOMETRY_FIXED_ONE);
        } else if (opcode == 12u) {
            registers[destination] =
                ((a + instruction.immediate / 2) / instruction.immediate) *
                instruction.immediate;
        } else if (opcode == 13u) {
            registers[destination] = select(instruction.immediate, b, a != 0);
        } else if (opcode == 14u) {
            result.retainedHeight = clamp(a, 0, VOXEL_GEOMETRY_FIXED_ONE);
            result.carveDepth = max(
                0, VOXEL_GEOMETRY_FIXED_ONE - result.retainedHeight);
        } else if (opcode == 15u) {
            result.carveDepth = max(a, 0);
            result.retainedHeight = clamp(
                VOXEL_GEOMETRY_FIXED_ONE - a,
                0, VOXEL_GEOMETRY_FIXED_ONE);
        } else if (opcode == 16u) {
            result.coverage = clamp(a, 0, VOXEL_GEOMETRY_FIXED_ONE);
        } else if (opcode == 17u) {
            let encodedSlope = context & 0x7fu;
            let slope = select(0u, encodedSlope - 1u, encodedSlope != 0u);
            registers[destination] = i32(
                slope * u32(VOXEL_GEOMETRY_FIXED_ONE) / 126u);
        } else if (opcode == 18u) {
            var slope = auxContext & 0xffu;
            if (slope == 0u && (context & 0x80u) != 0u) {
                let encodedSlope = context & 0x7fu;
                slope = select(
                    0u, (encodedSlope - 1u) * 255u / 126u,
                    encodedSlope != 0u);
            }
            registers[destination] = i32(
                slope * u32(VOXEL_GEOMETRY_FIXED_ONE) / 255u);
        } else if (opcode == 19u) {
            var depth = (auxContext >> 8u) & 0xffu;
            if (depth == 0u && face >= 6u && subdivision > 1u) {
                depth = (subdivision - 1u - min(
                    volumeCell.y, subdivision - 1u)) * 255u /
                    (subdivision - 1u);
            }
            registers[destination] = i32(
                depth * u32(VOXEL_GEOMETRY_FIXED_ONE) / 255u);
        } else if (opcode == 20u) {
            registers[destination] = i32(
                ((auxContext >> 16u) & 0xffu) *
                u32(VOXEL_GEOMETRY_FIXED_ONE) / 255u);
        } else if (opcode == 21u) {
            registers[destination] = i32(
                (auxContext >> 24u) *
                u32(VOXEL_GEOMETRY_FIXED_ONE) / 255u);
        } else if (opcode == 22u) {
            registers[destination] = abs(a);
        } else if (opcode == 23u) {
            registers[destination] = max(a, b);
        } else if (opcode == 24u) {
            registers[destination] = min(a, b);
        } else if (opcode == 25u) {
            registers[destination] = min(
                a, VOXEL_GEOMETRY_FIXED_ONE -
                    clamp(b, 0, VOXEL_GEOMETRY_FIXED_ONE));
        } else if (opcode == 26u) {
            registers[destination] = clamp(
                materialFields[source0], 0, VOXEL_GEOMETRY_FIXED_ONE);
        }
    }
    return result;
}
