#!/usr/bin/env python3
# This script compiles HLSL shaders to SPIR-V using dxc.
# It recursively finds all .hlsl files in a source directory and outputs
# corresponding .spv files to an output directory, preserving the hierarchy.
#
# Usage:
#   python3 compileHLSLToSpirV.py <source_directory> <output_directory>
#
# Or, if made executable:
#   ./compileHLSLToSpirV.py <source_directory> <output_directory>
#
# Example:
#   ./compileHLSLToSpirV.py ./shaders ./build/compiled_shaders
#
# Requires:
#   - Python 3.6+
#   - dxc (DirectX Shader Compiler) executable available in your system's PATH.
#     (On macOS, this typically comes with the Vulkan SDK installation.)

import argparse
import subprocess
import os
import sys

def find_dxc_executable():
    """
    Attempts to find the dxc executable in the system's PATH.
    Returns the path to dxc if found, otherwise None.
    """
    dxc_names = ["dxc", "dxc.exe"]
    for name in dxc_names:
        try:
            from shutil import which
            dxc_path = which(name)
            if dxc_path:
                subprocess.run([dxc_path, "-help"], check=True, capture_output=True, text=True)
                return dxc_path
        except (subprocess.CalledProcessError, TypeError, FileNotFoundError):
            continue
    return None

def find_hlsl_files(source_dir):
    """
    Recursively finds all .hlsl files in the given directory.
    Returns a list of absolute paths to HLSL files.
    """
    hlsl_files = []
    for root, _, files in os.walk(source_dir):
        for file in files:
            if file.endswith(".hlsl"):
                hlsl_files.append(os.path.join(root, file))
    return hlsl_files

def get_shader_profile(file_path):
    """
    Determines the HLSL shader profile based on a simple filename heuristic.
    You can extend this logic for more sophisticated detection.
    """
    file_name = os.path.basename(file_path).lower()
    if "vertex" in file_name or file_name.endswith(".vert.hlsl"):
        return "vs_6_4" # Or higher, e.g., vs_6_8
    elif "fragment" in file_name or "pixel" in file_name or file_name.endswith(".frag.hlsl"):
        return "ps_6_4" # Or higher, e.g., ps_6_8
    elif "compute" in file_name or file_name.endswith(".comp.hlsl"):
        return "cs_6_4" # Or higher, e.g., cs_6_8
    print(f"WARNING: Could not determine specific shader profile for '{file_path}'. Defaulting to 'ps_6_4'.", file=sys.stderr)
    return "ps_6_4"

def compile_shader(dxc_path, hlsl_file, spv_output_path, profile, entry_point="main"):
    """
    Compiles a single HLSL file to SPIR-V using DXC.
    Handles subprocess execution and error reporting.
    """
    command = [
        dxc_path,
        "-spirv",
        "-T", profile,
        "-E", entry_point,
        hlsl_file,
        "-Fo", spv_output_path
    ]
    
    print(f"  Compiling: {os.path.relpath(hlsl_file)} -> {os.path.relpath(spv_output_path)}")
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        print(f"\nERROR: DXC compilation failed for '{hlsl_file}' (Exit Code: {e.returncode})", file=sys.stderr)
        print(f"  DXC STDOUT:\n{e.stdout.strip()}", file=sys.stderr)
        print(f"  DXC STDERR:\n{e.stderr.strip()}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print(f"\nERROR: DXC executable not found at '{dxc_path}'. "
              "Please ensure it's in your system PATH.", file=sys.stderr)
        sys.exit(1)

def main():
    parser = argparse.ArgumentParser(
        description="Compile HLSL shaders to SPIR-V using dxc.",
        formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument("source_dir", help="Root directory containing HLSL shader files.")
    parser.add_argument("output_dir", help="Directory where compiled SPIR-V files will be placed.")
    
    args = parser.parse_args()

    source_dir = os.path.abspath(args.source_dir)
    output_dir = os.path.abspath(args.output_dir)

    if not os.path.isdir(source_dir):
        print(f"ERROR: Source directory '{source_dir}' does not exist or is not a directory.", file=sys.stderr)
        sys.exit(1)

    dxc_executable = find_dxc_executable()
    if not dxc_executable:
        print("ERROR: 'dxc' executable not found in your system's PATH. "
              "Please install the Vulkan SDK (which includes dxc) and ensure it's added to your PATH.", file=sys.stderr)
        sys.exit(1)
    print(f"Found dxc executable: {dxc_executable}")

    os.makedirs(output_dir, exist_ok=True)
    print(f"Output directory: {output_dir}")

    hlsl_files = find_hlsl_files(source_dir)
    if not hlsl_files:
        print(f"No HLSL files found in '{source_dir}'.")
        sys.exit(0)

    print(f"Found {len(hlsl_files)} HLSL files to compile.")

    compiled_count = 0
    for hlsl_file in hlsl_files:
        relative_path = os.path.relpath(hlsl_file, source_dir)
        spv_output_path = os.path.join(output_dir, os.path.splitext(relative_path)[0] + ".spv")
        os.makedirs(os.path.dirname(spv_output_path), exist_ok=True)
        profile = get_shader_profile(hlsl_file)
        compile_shader(dxc_executable, hlsl_file, spv_output_path, profile)
        compiled_count += 1
    
    print(f"\nSuccessfully compiled {compiled_count} HLSL shaders to SPIR-V.")

if __name__ == "__main__":
    main()