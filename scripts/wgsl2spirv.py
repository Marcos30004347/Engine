#!/usr/bin/env python3

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path

SPIRV_MAGIC = b"\x03\x02\x23\x07"
WGSLC_TIMEOUT_SECONDS = 60
WGSLC_LOCK_TIMEOUT_SECONDS = 300

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

def sanitize_id_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", value.lstrip("%"))

def patch_clip_distance_array_stores(path: Path) -> None:
    spirv_dis = shutil.which("spirv-dis")
    spirv_as = shutil.which("spirv-as")
    spirv_val = shutil.which("spirv-val")
    if not spirv_dis or not spirv_as:
        return

    asm_path = Path(f"{path}.clipdistance.spvasm")
    patched_asm_path = Path(f"{path}.clipdistance.patched.spvasm")
    patched_spv_path = Path(f"{path}.clipdistance.patched.spv")

    try:
        subprocess.run([spirv_dis, str(path), "-o", str(asm_path)], check=True)
        lines = asm_path.read_text(encoding="utf-8").splitlines()

        clip_variables: set[str] = set()
        for line in lines:
            match = re.match(r"\s*OpDecorate\s+(%\S+)\s+BuiltIn\s+ClipDistance\b", line)
            if match:
                clip_variables.add(match.group(1))
        if not clip_variables:
            return

        type_lines: dict[str, tuple[int, str]] = {}
        constants: dict[str, tuple[int, str, int]] = {}
        uint_constants_by_value: dict[int, str] = {}
        uint_type_id = ""
        for index, line in enumerate(lines):
            type_match = re.match(r"\s*(%\S+)\s*=\s*(OpType\S+)\b(.*)", line)
            if type_match:
                type_lines[type_match.group(1)] = (index, line)
                if type_match.group(2) == "OpTypeInt" and type_match.group(3).strip() == "32 0":
                    uint_type_id = type_match.group(1)
                continue
            const_match = re.match(r"\s*(%\S+)\s*=\s*OpConstant\s+(%\S+)\s+(\d+)\s*$", line)
            if const_match:
                value = int(const_match.group(3))
                constants[const_match.group(1)] = (index, const_match.group(2), value)
                if const_match.group(2) == uint_type_id:
                    uint_constants_by_value.setdefault(value, const_match.group(1))

        clip_infos: dict[str, tuple[str, int, int]] = {}
        required_index_constants: set[int] = set()
        required_pointer_types: dict[str, tuple[str, int]] = {}
        for index, line in enumerate(lines):
            variable_match = re.match(r"\s*(%\S+)\s*=\s*OpVariable\s+(%\S+)\s+Output\b", line)
            if not variable_match or variable_match.group(1) not in clip_variables:
                continue

            variable_id = variable_match.group(1)
            pointer_type_id = variable_match.group(2)
            pointer_type_line = type_lines.get(pointer_type_id)
            if not pointer_type_line:
                continue
            pointer_match = re.match(r"\s*%\S+\s*=\s*OpTypePointer\s+Output\s+(%\S+)\s*$", pointer_type_line[1])
            if not pointer_match:
                continue

            array_type_id = pointer_match.group(1)
            array_type_line = type_lines.get(array_type_id)
            if not array_type_line:
                continue
            array_match = re.match(r"\s*%\S+\s*=\s*OpTypeArray\s+(%\S+)\s+(%\S+)\s*$", array_type_line[1])
            if not array_match:
                continue

            element_type_id = array_match.group(1)
            length_const_id = array_match.group(2)
            length_info = constants.get(length_const_id)
            if not length_info:
                continue
            length = length_info[2]
            if length == 0:
                continue

            pointer_element_type_id = f"%_ptr_Output_{sanitize_id_name(element_type_id)}_clip_distance"
            clip_infos[variable_id] = (element_type_id, length, array_type_line[0])
            required_pointer_types[pointer_element_type_id] = (element_type_id, pointer_type_line[0])
            required_index_constants.update(range(length))

        if not clip_infos:
            return

        existing_ids = {match.group(1) for line in lines if (match := re.match(r"\s*(%\S+)\s*=", line))}
        insertions: dict[int, list[str]] = {}

        for value in sorted(required_index_constants):
            if value not in uint_constants_by_value:
                if not uint_type_id:
                    return
                const_id = f"%clip_distance_index_{value}"
                uint_constants_by_value[value] = const_id
                if const_id not in existing_ids:
                    uint_type_line = type_lines[uint_type_id][0]
                    insertions.setdefault(uint_type_line + 1, []).append(f"{const_id} = OpConstant {uint_type_id} {value}")
                    existing_ids.add(const_id)

        for pointer_type_id, (element_type_id, after_line) in required_pointer_types.items():
            if pointer_type_id not in existing_ids:
                insertions.setdefault(after_line + 1, []).append(f"{pointer_type_id} = OpTypePointer Output {element_type_id}")
                existing_ids.add(pointer_type_id)

        patched_lines: list[str] = []
        replacement_count = 0
        for index, line in enumerate(lines):
            if index in insertions:
                patched_lines.extend(insertions[index])

            store_match = re.match(r"(\s*)OpStore\s+(%\S+)\s+(%\S+)\s*$", line)
            if store_match and store_match.group(2) in clip_infos:
                indent = store_match.group(1)
                clip_var = store_match.group(2)
                source_id = store_match.group(3)
                element_type_id, length, _ = clip_infos[clip_var]
                pointer_element_type_id = f"%_ptr_Output_{sanitize_id_name(element_type_id)}_clip_distance"
                source_name = sanitize_id_name(source_id)
                for element_index in range(length):
                    index_id = uint_constants_by_value[element_index]
                    ptr_id = f"%clip_distance_ptr_{source_name}_{replacement_count}_{element_index}"
                    value_id = f"%clip_distance_value_{source_name}_{replacement_count}_{element_index}"
                    patched_lines.append(f"{ptr_id} = OpAccessChain {pointer_element_type_id} {clip_var} {index_id}")
                    patched_lines.append(f"{value_id} = OpCompositeExtract {element_type_id} {source_id} {element_index}")
                    patched_lines.append(f"{indent}OpStore {ptr_id} {value_id}")
                replacement_count += 1
            else:
                patched_lines.append(line)

        if replacement_count == 0:
            return

        patched_asm_path.write_text("\n".join(patched_lines) + "\n", encoding="utf-8")
        subprocess.run([spirv_as, str(patched_asm_path), "-o", str(patched_spv_path)], check=True)
        if spirv_val:
            subprocess.run([spirv_val, str(patched_spv_path)], check=True)
        replace_file(patched_spv_path, path)
    finally:
        cleanup_paths(asm_path, patched_asm_path, patched_spv_path)

@contextmanager
def acquire_lock(lock_name: str, timeout_seconds: float):
    lock_path = Path(tempfile.gettempdir()) / lock_name
    start = time.monotonic()
    fd = None

    while True:
        try:
            fd = os.open(str(lock_path), os.O_CREAT | os.O_EXCL | os.O_RDWR)
            os.write(fd, f"{os.getpid()}\n".encode("utf-8"))
            break
        except FileExistsError:
            if time.monotonic() - start >= timeout_seconds:
                raise TimeoutError(f"Timed out waiting for lock {lock_path}")
            time.sleep(0.05)

    try:
        yield
    finally:
        if fd is not None:
            os.close(fd)
        try:
            lock_path.unlink()
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

    user_local_wgslc = Path.home() / ".cargo" / "bin" / "wgslc-naga"
    needs_user_local_wgslc = "meshlet_per_page_rsm" in Path(args.input).name
    wgslc_naga = str(user_local_wgslc) if needs_user_local_wgslc and user_local_wgslc.exists() else "wgslc-naga"

    cmd = [
        wgslc_naga,
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
        with acquire_lock("wgslc-naga.lock", WGSLC_LOCK_TIMEOUT_SECONDS):
            subprocess.run(
                cmd,
                check=True,
                timeout=WGSLC_TIMEOUT_SECONDS,
            )
    except TimeoutError as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
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

    try:
        patch_clip_distance_array_stores(compile_output)
    except subprocess.CalledProcessError as exc:
        print("Clip distance SPIR-V patch failed", file=sys.stderr)
        sys.exit(exc.returncode)

    replace_file(compile_output, final_output)
    elapsed_ms = (time.monotonic() - compile_start) * 1000.0
    print(
        f"WGSL compile: wrote SPIR-V module for {args.input} in {elapsed_ms:.1f} ms",
        file=sys.stderr,
    )

if __name__ == "__main__":
    main()
