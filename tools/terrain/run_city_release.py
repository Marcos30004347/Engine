#!/usr/bin/env python3
"""Run the full native world pipeline against immutable history milestones.

Generation failures retain their native reports. An old output directory is never
reused, and a render is not considered visual approval. No history is fabricated.
"""
import argparse
import hashlib
import json
import functools
import http.server
import sys
import threading
from pathlib import Path
import subprocess
import time

YEARS = (20, 100, 1000, 2000)


def evaluate(report, exit_code):
    failures = []
    if exit_code:
        failures.append(f"generator-exit-{exit_code}")
    jobs = report.get("jobs", [])
    if not jobs or not all(j.get("built") is True for j in jobs):
        failures.append("incomplete-runtime-terrain")
    release = report.get("release", {})
    failures.extend(release.get("failures", []))
    if release.get("geometryAccepted") is not True:
        failures.append("geometry-not-accepted")
    # This tool does not automatically approve images that it has not inspected.
    if release.get("viewsReviewed") is not True:
        failures.append("visual-review-pending")
    return sorted(set(failures))


def run_case(executable, snapshot, output, seed, timeout):
    output.mkdir(parents=True, exist_ok=False)
    command = [str(executable), "--history-snapshot", str(snapshot),
               "--seed", str(seed), "--city-region", "1", "--release-check", "1",
               "--output", str(output)]
    started = time.monotonic()
    with (output / "generator.log").open("w") as log:
        try:
            code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=timeout, check=False).returncode
        except subprocess.TimeoutExpired:
            code = 124
    report_path = output / "settlement.json"
    report = json.loads(report_path.read_text()) if report_path.exists() else {}
    return {"seed": seed, "snapshot": str(snapshot), "command": command,
            "snapshotSha256": hashlib.sha256(snapshot.read_bytes()).hexdigest(),
            "seconds": time.monotonic() - started, "exitCode": code,
            "report": str(report_path), "failures": evaluate(report, code),
            "nativeErrors": {key: report[key] for key in ("siteError", "territoryError", "fabricError", "runtimeError") if report.get(key)},
            "failedTiles": [{"name": job.get("name"), "error": job.get("error")} for job in report.get("jobs", []) if not job.get("built")],
            "runtimeBuilt": sum(j.get("built") is True for j in report.get("jobs", [])),
            "runtimeRequested": len(report.get("jobs", [])), "release": report.get("release")}


def render_case(output, node):
    """Capture the actual native export, even when its release gate failed."""
    source = output / "city-model-c10.json"
    if not source.exists():
        return {"captured": False, "reason": "native mesh was not generated"}
    tools = Path(__file__).resolve().parent
    viewer, views = output / "viewer", output / "views"
    with (output / "render.log").open("w") as log:
        packed = subprocess.run([sys.executable, str(tools / "export_city_viewer.py"),
                                 "--input", str(source.resolve()), "--output", str(viewer.resolve())],
                                stdout=log, stderr=subprocess.STDOUT, timeout=600, check=False)
        if packed.returncode:
            return {"captured": False, "reason": "native viewer export failed"}
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(viewer.resolve()))
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}/"
            captured = subprocess.run([node, str(tools / "city_viewer/render_views.cjs"), url, str(views.resolve())],
                                      stdout=log, stderr=subprocess.STDOUT, timeout=300, check=False)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
    return {"captured": captured.returncode == 0, "reviewed": False, "manifest": str(views / "views.json")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executable", type=Path, default=Path("build/tests/WorldSettlementPreview"))
    parser.add_argument("--seeds", type=int, nargs="+", default=[41, 42, 43])
    parser.add_argument("--years", type=int, nargs="+", choices=YEARS, default=list(YEARS))
    parser.add_argument("--settlement", type=int, default=100)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--render", action="store_true", help="requires NumPy/Pillow and Node with Playwright")
    parser.add_argument("--node", default="node")
    args = parser.parse_args()
    if len(set(args.seeds)) != len(args.seeds) or len(set(args.years)) != len(args.years):
        parser.error("duplicate seeds or years would reuse artifacts")
    executable = args.executable.resolve(strict=True)
    snapshots = {year: (args.history / f"year-{year}" / f"settlement-{args.settlement}" / "history.json").resolve(strict=True)
                 for year in args.years}
    # Validate all inputs before starting an expensive matrix.
    for year, path in snapshots.items():
        history = json.loads(path.read_text())
        if history.get("year") != year or history.get("settlement") != args.settlement:
            parser.error(f"snapshot identity mismatch: {path}")
        if not isinstance(history.get("defenceCircuits"), list):
            parser.error(f"snapshot lacks dated defence ledger: regenerate {path}")
    args.output.mkdir(parents=True, exist_ok=False)
    result = {"schemaVersion": 1, "accepted": False, "cases": [],
              "remainingGates": ["hill/ridge/valley site coverage", "manual native render review"]}
    for seed in args.seeds:
        for year, snapshot in snapshots.items():
            case = run_case(executable, snapshot, args.output / f"seed-{seed}-year-{year}", seed, args.timeout)
            if args.render:
                try:
                    case["renders"] = render_case(args.output / f"seed-{seed}-year-{year}", args.node)
                except (OSError, subprocess.TimeoutExpired) as error:
                    case["renders"] = {"captured": False, "reason": str(error)}
            result["cases"].append(case)
            (args.output / "release-matrix.json").write_text(json.dumps(result, indent=2) + "\n")
            print(f"seed={seed} year={year}: {', '.join(case['failures']) or 'geometry passed'}", flush=True)
    return 1 if any(c["failures"] for c in result["cases"]) or result["remainingGates"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
