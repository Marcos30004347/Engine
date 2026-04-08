#!/usr/bin/env python3
import io
import re
from pathlib import Path

from pcpp import Preprocessor


INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def resolve_include(include_name: str, source_file: Path, include_paths: list[Path]) -> Path:
    include_path = Path(include_name)
    candidates = []

    if include_path.is_absolute():
        candidates.append(include_path)
    else:
        candidates.append(source_file.parent / include_path)
        candidates.extend(base / include_path for base in include_paths)

    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()

    raise FileNotFoundError(f'Unable to resolve include "{include_name}" from "{source_file}"')


def collect_dependencies(source_file: Path, include_paths: list[Path], visited: set[Path], ordered: list[Path]) -> None:
    resolved_source = source_file.resolve()
    if resolved_source in visited:
        return

    visited.add(resolved_source)
    ordered.append(resolved_source)

    source_text = resolved_source.read_text(encoding="utf-8")
    for include_name in INCLUDE_RE.findall(source_text):
        include_file = resolve_include(include_name, resolved_source, include_paths)
        collect_dependencies(include_file, include_paths, visited, ordered)


def write_depfile(depfile: Path, output_file: Path, dependencies: list[Path]) -> None:
    depfile.parent.mkdir(parents=True, exist_ok=True)
    escaped_output = output_file.as_posix().replace(" ", "\\ ")
    escaped_deps = [dep.as_posix().replace(" ", "\\ ") for dep in dependencies]
    depfile.write_text(f"{escaped_output}: {' '.join(escaped_deps)}\n", encoding="utf-8")


def main():
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("src")
    parser.add_argument("-o", "--out", required=True)
    parser.add_argument("--depfile")
    parser.add_argument("-I", action="append", default=[])
    parser.add_argument("-D", action="append", default=[])
    args = parser.parse_args()

    pp = Preprocessor()
    pp.line_directive = None
    include_paths = [Path(inc).resolve() for inc in args.I]

    for inc in include_paths:
        pp.add_path(str(inc))

    for d in args.D:
        if "=" in d:
            k, v = d.split("=", 1)
            pp.define(f"{k} {v}")
        else:
            pp.define(d)

    src = Path(args.src).resolve()
    with src.open("r", encoding="utf-8") as f:
        pp.parse(f.read(), str(src))

    buf = io.StringIO()
    pp.write(buf)

    processed = "\n".join(
        line for line in buf.getvalue().splitlines()
        if line.strip()
    )

    output_path = Path(args.out).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(processed + "\n", encoding="utf-8")

    if args.depfile:
        dependencies = []
        collect_dependencies(src, include_paths, set(), dependencies)
        write_depfile(Path(args.depfile).resolve(), output_path, dependencies)


if __name__ == "__main__":
    main()
