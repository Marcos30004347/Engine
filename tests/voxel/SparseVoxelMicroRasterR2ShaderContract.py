#!/usr/bin/env python3
"""Verify the compiled R2 prepared-parent shader cannot reach legacy buffers."""

import argparse
import struct
import sys
from pathlib import Path


SPIRV_MAGIC = 0x07230203
OP_NAME = 5
OP_ENTRY_POINT = 15
EXECUTION_MODEL_VERTEX = 0
EXECUTION_MODEL_FRAGMENT = 4
ATOMIC_OPCODES = set(range(227, 243)) | {318, 319}

SHIPPING_FRAGMENT_INTERFACES = {
    "uniforms",
    "brickPalette",
    "microMaterials",
    "voxelGeometryPrograms",
    "voxelGeometryInstructions",
    "voxelGeometryParameters",
    "voxelMaterialFieldDefaults",
}
DIAGNOSTIC_FRAGMENT_INTERFACES = SHIPPING_FRAGMENT_INTERFACES | {
    "overdraw",
    "microCounters",
}
VERTEX_INTERFACES = {
    "uniforms",
    "nodes",
    "microBrickRecords",
}


def read_words(path: Path) -> tuple[list[int], str]:
    blob = path.read_bytes()
    if len(blob) < 20 or len(blob) % 4 != 0:
        raise ValueError("not a SPIR-V module (bad length)")
    if struct.unpack("<I", blob[:4])[0] == SPIRV_MAGIC:
        endian = "<"
    elif struct.unpack(">I", blob[:4])[0] == SPIRV_MAGIC:
        endian = ">"
    else:
        raise ValueError("not a SPIR-V module (bad magic)")
    return list(struct.unpack(f"{endian}{len(blob) // 4}I", blob)), endian


def decode_string(words: list[int], endian: str) -> tuple[str, int]:
    blob = struct.pack(f"{endian}{len(words)}I", *words)
    terminator = blob.find(b"\0")
    if terminator < 0:
        raise ValueError("unterminated SPIR-V string")
    return blob[:terminator].decode("utf-8"), terminator // 4 + 1


def entry_interfaces(
    words: list[int], endian: str
) -> tuple[dict[int, str], list[tuple[int, str, list[int]]]]:
    names: dict[int, str] = {}
    entries: list[tuple[int, str, list[int]]] = []
    index = 5
    while index < len(words):
        header = words[index]
        word_count = header >> 16
        opcode = header & 0xFFFF
        if word_count == 0 or index + word_count > len(words):
            raise ValueError(f"malformed instruction at word {index}")
        operands = words[index + 1 : index + word_count]
        if opcode == OP_NAME:
            if len(operands) < 2:
                raise ValueError(f"malformed OpName at word {index}")
            name, _ = decode_string(operands[1:], endian)
            names[operands[0]] = name
        elif opcode == OP_ENTRY_POINT:
            if len(operands) < 3:
                raise ValueError(f"malformed OpEntryPoint at word {index}")
            entry_name, name_words = decode_string(operands[2:], endian)
            entries.append(
                (operands[0], entry_name, operands[2 + name_words :])
            )
        index += word_count
    return names, entries


def named_interface(
    path: Path,
    names: dict[int, str],
    entries: list[tuple[int, str, list[int]]],
    model: int,
    entry_name: str,
) -> set[str]:
    matching = [
        interface
        for entry_model, name, interface in entries
        if entry_model == model and name == entry_name
    ]
    if len(matching) != 1:
        raise ValueError(
            f"expected one {entry_name} entry point, found {len(matching)}"
        )
    # Naga does not name synthesized stage inputs/outputs. Resource variables
    # retain their WGSL names, and the required-interface assertions below make
    # sure that metadata is present before absence is treated as proof.
    return {names[identifier] for identifier in matching[0] if identifier in names}


def count_atomics(words: list[int]) -> int:
    count = 0
    index = 5
    while index < len(words):
        header = words[index]
        word_count = header >> 16
        opcode = header & 0xFFFF
        if word_count == 0 or index + word_count > len(words):
            raise ValueError(f"malformed instruction at word {index}")
        if opcode in ATOMIC_OPCODES:
            count += 1
        index += word_count
    return count


def check_module(path: Path) -> None:
    if not path.is_file():
        raise ValueError("shader does not exist; build its prepared target first")
    words, endian = read_words(path)
    names, entries = entry_interfaces(words, endian)
    fragment = named_interface(
        path, names, entries, EXECUTION_MODEL_FRAGMENT, "drawBrickFragment"
    )
    vertex = named_interface(
        path, names, entries, EXECUTION_MODEL_VERTEX, "drawBrickVertex"
    )

    diagnostic = "diagnostics" in path.stem
    expected_fragment = (
        DIAGNOSTIC_FRAGMENT_INTERFACES
        if diagnostic
        else SHIPPING_FRAGMENT_INTERFACES
    )
    if fragment != expected_fragment:
        raise ValueError(
            "prepared Fragment interface differs from its direct-path ABI; "
            f"expected {sorted(expected_fragment)}, got {sorted(fragment)}"
        )
    if vertex != VERTEX_INTERFACES:
        raise ValueError(
            "prepared Vertex interface differs from its compact-record ABI; "
            f"expected {sorted(VERTEX_INTERFACES)}, got {sorted(vertex)}"
        )
    atomic_count = count_atomics(words)
    if diagnostic and atomic_count == 0:
        raise ValueError(
            "diagnostic prepared shader lost its instrumentation atomics"
        )
    if not diagnostic and atomic_count != 0:
        raise ValueError(
            f"shipping prepared shader contains {atomic_count} atomic operations"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("shader", nargs="+", type=Path)
    args = parser.parse_args()

    failures: list[str] = []
    for path in args.shader:
        try:
            check_module(path)
            print(f"R2 prepared shader contract passed: {path}")
        except (OSError, UnicodeDecodeError, ValueError) as error:
            failures.append(f"{path}: {error}")
    if failures:
        print("R2 prepared shader contract failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
