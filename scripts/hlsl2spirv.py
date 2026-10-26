#!/usr/bin/env python3
import argparse
import subprocess
import os
import sys
from shutil import which

def find_dxc_executable():
    for name in ("dxc", "dxc.exe"):
        path = which(name)
        if path:
            try:
                subprocess.run(
                    [path, "-help"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=True
                )
                return path
            except subprocess.CalledProcessError:
                pass
    return None

def infer_profile(path):
    name = os.path.basename(path).lower()
    if "vertex" in name or name.endswith(".vert.hlsl"):
        return "vs_6_4"
    if "fragment" in name or "pixel" in name or name.endswith(".frag.hlsl"):
        return "ps_6_4"
    if "compute" in name or name.endswith(".comp.hlsl"):
        return "cs_6_4"
    print(f"WARNING: Could not infer shader profile for '{path}', defaulting to ps_6_4",
          file=sys.stderr)
    return "ps_6_4"

def main():
    parser = argparse.ArgumentParser(
        description="Compile a single HLSL file to SPIR-V using dxc"
    )
    parser.add_argument("input", help="Input .hlsl file")
    parser.add_argument("-o", "--output", required=True, help="Output .spv file")
    parser.add_argument("-E", "--entry", default="main", help="Entry point name")

    args = parser.parse_args()

    src = os.path.abspath(args.input)
    dst = os.path.abspath(args.output)

    if not os.path.isfile(src):
        print(f"ERROR: Input file '{src}' does not exist", file=sys.stderr)
        sys.exit(1)

    dxc = find_dxc_executable()
    if not dxc:
        print("ERROR: dxc not found in PATH", file=sys.stderr)
        sys.exit(1)

    os.makedirs(os.path.dirname(dst), exist_ok=True)

    profile = infer_profile(src)

    cmd = [
        dxc,
        "-spirv",
        "-T", profile,
        "-E", args.entry,
        src,
        "-Fo", dst
    ]

    print(f"Compiling HLSL → SPIR-V: {os.path.basename(src)}")
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"ERROR: DXC failed ({e.returncode})", file=sys.stderr)
        sys.exit(e.returncode)

if __name__ == "__main__":
    main()
