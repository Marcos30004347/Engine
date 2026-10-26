#!/usr/bin/env python3
"""Append an OpExecutionMode to a SPIR-V module's entry point.

WGSL has no way to declare conservative depth, and naga therefore never emits
DepthLess/DepthGreater. Without one of those a fragment shader that writes
frag_depth forces late-Z: the depth test cannot run until the shader has
produced its depth, so every fragment is shaded even when it is hidden.

A shader that only ever moves depth in one direction can say so, which lets the
hardware reject fragments before shading them. This patches that declaration in
after the fact, which is the only place it can be added in this toolchain.

Usage:
    spirv_add_execution_mode.py <module.spirv> --mode DepthLess [--model Fragment]
"""

import argparse
import struct
import sys

SPIRV_MAGIC = 0x07230203
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16

EXECUTION_MODELS = {"Vertex": 0, "Fragment": 4, "GLCompute": 5}
EXECUTION_MODES = {
    "DepthReplacing": 12,
    "DepthGreater": 14,
    "DepthLess": 15,
    "DepthUnchanged": 16,
}


def read_words(path):
    with open(path, "rb") as handle:
        blob = handle.read()
    if len(blob) < 20 or len(blob) % 4 != 0:
        raise SystemExit(f"{path}: not a SPIR-V module (bad length)")
    magic = struct.unpack("<I", blob[:4])[0]
    if magic == SPIRV_MAGIC:
        endian = "<"
    elif struct.unpack(">I", blob[:4])[0] == SPIRV_MAGIC:
        endian = ">"
    else:
        raise SystemExit(f"{path}: not a SPIR-V module (bad magic)")
    count = len(blob) // 4
    return list(struct.unpack(f"{endian}{count}I", blob)), endian


def write_words(path, words, endian):
    with open(path, "wb") as handle:
        handle.write(struct.pack(f"{endian}{len(words)}I", *words))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("module")
    parser.add_argument("--mode", required=True, choices=sorted(EXECUTION_MODES))
    parser.add_argument("--model", default="Fragment", choices=sorted(EXECUTION_MODELS))
    args = parser.parse_args()

    words, endian = read_words(args.module)
    model = EXECUTION_MODELS[args.model]
    mode = EXECUTION_MODES[args.mode]

    # Walk the instruction stream, collecting the matching entry points and the
    # position just past the existing execution modes. OpExecutionMode must sit
    # after every OpEntryPoint and before the debug and annotation sections, so
    # appending to that run is the only valid insertion point.
    entry_ids = []
    index = 5
    insert_at = None
    while index < len(words):
        header = words[index]
        word_count = header >> 16
        opcode = header & 0xFFFF
        if word_count == 0:
            raise SystemExit(f"{args.module}: malformed instruction at word {index}")
        if opcode == OP_ENTRY_POINT:
            if words[index + 1] == model:
                entry_ids.append(words[index + 2])
            insert_at = index + word_count
        elif opcode == OP_EXECUTION_MODE:
            # Already declared for this entry point: leave the module alone so
            # the step stays idempotent across incremental builds.
            if words[index + 1] in entry_ids and words[index + 2] == mode:
                return 0
            insert_at = index + word_count
        elif insert_at is not None:
            break
        index += word_count

    if not entry_ids:
        raise SystemExit(f"{args.module}: no {args.model} entry point found")
    if insert_at is None:
        raise SystemExit(f"{args.module}: no entry point section found")

    added = []
    for entry_id in entry_ids:
        added.extend([(3 << 16) | OP_EXECUTION_MODE, entry_id, mode])
    words[insert_at:insert_at] = added
    write_words(args.module, words, endian)
    print(
        f"SPIR-V: declared {args.mode} on {len(entry_ids)} {args.model} entry point(s) "
        f"in {args.module}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
