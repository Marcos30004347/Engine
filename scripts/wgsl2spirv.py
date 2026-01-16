#!/usr/bin/env python3

import argparse
import subprocess
import sys
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(
        description="Compile WGSL to SPIR-V using wgslc-naga"
    )
    parser.add_argument("input", help="Input WGSL file")
    parser.add_argument("-o", "--output", required=True, help="Output SPIR-V file")
    parser.add_argument("--int64", action="store_true", help="Enable 64-bit integers")
    parser.add_argument("--atomic-u64-min-max", action="store_true", help="Enable 64-bit min max atomics")
    parser.add_argument("--atomic-u64", action="store_true", help="Enable 64-bit atomics")
    parser.add_argument("--float64", action="store_true", help="Enable float64")
    parser.add_argument("--atomic-u64-texture", action="store_true", help="Enable 64-bit atomics on textures")
    
    args = parser.parse_args()

    cmd = [
        "wgslc-naga",
        args.input,
        "-o", args.output,
    ]

    if args.int64:
        cmd.append("--int64")
    if args.atomic_u64_min_max:
        cmd.append("--atomic-u64-min-max")
    if args.atomic_u64:
        cmd.append("--atomic-u64")
    if args.float64:
        cmd.append("--float64")
    if args.atomic_u64_texture:
        cmd.append("--texture-int64-atomic")
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        print("WGSL compilation failed", file=sys.stderr)
        sys.exit(e.returncode)

if __name__ == "__main__":
    main()
