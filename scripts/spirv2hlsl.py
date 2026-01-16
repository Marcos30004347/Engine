#!/usr/bin/env python3
"""
Compile SPIR-V shaders to HLSL using SPIRV-Cross.

Usage:
  spirv2hlsl.py <input.spv> -o <output.hlsl>

Options allow control over shader model, binding style, and compatibility.

Example:
  spirv2hlsl.py shader.spv -o shader.hlsl --shader-model 66 --vulkan-semantics
"""

import argparse
import subprocess
import sys
from shutil import which
from pathlib import Path

def find_spirv_cross():
    exe = which("spirv-cross")
    if not exe:
        print(
            "ERROR: spirv-cross not found in PATH.\n"
            "Install from https://github.com/KhronosGroup/SPIRV-Cross",
            file=sys.stderr,
        )
        sys.exit(1)
    return exe

def main():
    parser = argparse.ArgumentParser(
        description="Convert SPIR-V to HLSL using SPIRV-Cross"
    )

    parser.add_argument("input", help="Input SPIR-V file (.spv)")
    parser.add_argument("-o", "--output", required=True, help="Output HLSL file (.hlsl)")

    parser.add_argument(
        "--shader-model",
        default="66",
        help="HLSL shader model (default: 66)",
    )

    parser.add_argument(
        "--vulkan-semantics",
        action="store_true",
        help="Preserve Vulkan-style semantics (descriptor sets → spaces)",
    )

    parser.add_argument(
        "--flatten-ubo",
        action="store_true",
        help="Flatten uniform buffers into plain HLSL structs",
    )

    parser.add_argument(
        "--preserve-bindings",
        action="store_true",
        help="Preserve original SPIR-V bindings",
    )

    parser.add_argument(
        "--entry",
        help="Force a specific entry point (default: all)",
    )

    args = parser.parse_args()

    spirv_cross = find_spirv_cross()

    input_path = Path(args.input)
    output_path = Path(args.output)

    if not input_path.exists():
        print(f"ERROR: Input file '{input_path}' does not exist.", file=sys.stderr)
        sys.exit(1)

    cmd = [
        spirv_cross,
        str(input_path),
        "--hlsl",
        f"--shader-model {args.shader_model}",
    ]

    if args.vulkan_semantics:
        cmd.append("--vulkan-semantics")

    if args.flatten_ubo:
        cmd.append("--flatten-ubo")

    if args.preserve_bindings:
        cmd.append("--preserve-bindings")

    if args.entry:
        cmd.append(f"--entry {args.entry}")

    cmd.append(f"--output {output_path}")

    print("Running:")
    print(" ", " ".join(cmd))

    try:
        subprocess.run(" ".join(cmd), shell=True, check=True)
    except subprocess.CalledProcessError as e:
        print("ERROR: SPIR-V to HLSL compilation failed.", file=sys.stderr)
        sys.exit(e.returncode)

if __name__ == "__main__":
    main()
