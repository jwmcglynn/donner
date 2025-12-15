#!/usr/bin/env python3
"""Run tiny_skia_cpp benchmarks and fail on large regressions."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Iterable, List, Mapping


BASELINES_NS_PER_SAMPLE: Mapping[str, float] = {
    "linear_gradient_sample": 139.734,
    "blend_span": 142.539,
    "rasterize_fill": 15.3994,
}


@dataclass
class BenchmarkResult:
    name: str
    samples: int
    elapsed_ms: float
    nanos_per_sample: float
    checksum: int


def run_benchmark(bazel_args: List[str], binary_args: List[str]) -> str:
    cmd = [
        "bazelisk",
        "run",
        "//donner/backends/tiny_skia_cpp:tiny_skia_cpp_benchmarks",
    ] + bazel_args + ["--", "--json"] + binary_args

    process = subprocess.run(cmd, check=False, capture_output=True, text=True)
    if process.returncode != 0:
        sys.stderr.write(process.stdout)
        sys.stderr.write(process.stderr)
        sys.stderr.write("Benchmark command failed\n")
        sys.exit(process.returncode)

    return process.stdout


def parse_json_output(output: str) -> List[BenchmarkResult]:
    results: List[BenchmarkResult] = []
    brace_pattern = re.compile(r"\{[^}]*}\n")
    for match in brace_pattern.finditer(output):
        data = json.loads(match.group(0))
        results.append(
            BenchmarkResult(
                name=data["name"],
                samples=int(data["samples"]),
                elapsed_ms=float(data["elapsed_ms"]),
                nanos_per_sample=float(data["ns_per_sample"]),
                checksum=int(data["checksum"]),
            )
        )

    if not results:
        sys.stderr.write("No benchmark results found in output\n")
        sys.exit(1)

    return results


def check_regressions(results: Iterable[BenchmarkResult], tolerance: float) -> int:
    failures: List[str] = []
    for result in results:
        baseline = BASELINES_NS_PER_SAMPLE.get(result.name)
        if baseline is None:
            continue
        allowed = baseline * tolerance
        if result.nanos_per_sample > allowed:
            failures.append(
                f"{result.name}: {result.nanos_per_sample:.3f} ns/sample "
                f"> {allowed:.3f} ns/sample (baseline {baseline:.3f})"
            )

    if failures:
        for failure in failures:
            print(failure)
        return 1

    for result in results:
        if result.name in BASELINES_NS_PER_SAMPLE:
            print(
                f"{result.name}: {result.nanos_per_sample:.3f} ns/sample "
                f"(baseline {BASELINES_NS_PER_SAMPLE[result.name]:.3f})"
            )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--tolerance",
        type=float,
        default=1.35,
        help="Allowed slowdown factor versus baseline (default: 1.35)",
    )
    parser.add_argument(
        "--bazel-arg",
        action="append",
        default=[],
        dest="bazel_args",
        help="Additional arguments to pass to bazel run (repeatable)",
    )
    parser.add_argument(
        "--iterations",
        type=int,
        help="Override iteration count passed to the benchmark binary",
    )

    args = parser.parse_args()

    bazel_args: List[str] = list(args.bazel_args)
    binary_args: List[str] = []
    if args.iterations is not None:
        binary_args.append(f"--iterations={args.iterations}")

    output = run_benchmark(bazel_args, binary_args)
    results = parse_json_output(output)
    return check_regressions(results, args.tolerance)


if __name__ == "__main__":
    sys.exit(main())
