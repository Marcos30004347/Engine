#!/usr/bin/env python3

import argparse
import subprocess
import sys
import time
from pathlib import Path

SPIRV_MAGIC = b"\x03\x02\x23\x07"
WGSLC_TIMEOUT_SECONDS = 60

def is_valid_spirv(path: Path) -> bool:
    try:
        with path.open("rb") as spirv_file:
            return spirv_file.read(4) == SPIRV_MAGIC
    except OSError:
        return False

def replace_file(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    src.replace(dst)

def cleanup_paths(*paths: Path) -> None:
    for path in paths:
        try:
            path.unlink()
        except FileNotFoundError:
            pass

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

    compile_start = time.monotonic()
    final_output = Path(args.output)
    compile_output = Path(f"{args.output}.tmp")

    cleanup_paths(compile_output)

    cmd = [
        "wgslc-naga",
        args.input,
        "-o", str(compile_output),
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
        subprocess.run(
            cmd,
            check=True,
            timeout=WGSLC_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        print(
            f"WGSL compilation timed out after {WGSLC_TIMEOUT_SECONDS}s",
            file=sys.stderr,
        )
        sys.exit(1)
    except subprocess.CalledProcessError as exc:
        print("WGSL compilation failed", file=sys.stderr)
        sys.exit(exc.returncode)

    if not is_valid_spirv(compile_output):
        print(f"WGSL compilation did not produce a valid SPIR-V module at {compile_output}", file=sys.stderr)
        sys.exit(1)

    replace_file(compile_output, final_output)
    elapsed_ms = (time.monotonic() - compile_start) * 1000.0
    print(
        f"WGSL compile: wrote SPIR-V module for {args.input} in {elapsed_ms:.1f} ms",
        file=sys.stderr,
    )

if __name__ == "__main__":
    main()
