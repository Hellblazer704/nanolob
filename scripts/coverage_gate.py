#!/usr/bin/env python3
"""Line-coverage gate over the engine headers, merged correctly.

gcovr (as of 8.x) counts each template *instantiation's* line records
separately when merging across translation units, which under-reports header
coverage badly (a TU that instantiates one engine method marks every other
method's lines uncovered). This script does what lcov does: run gcov in JSON
mode over every .gcda, then merge execution counts by (file, line) across all
TUs. A line is covered if ANY instantiation in ANY TU executed it.

Usage: python scripts/coverage_gate.py <build-dir> [--threshold 85]
Exits non-zero if total line coverage over include/nanolob/ (the engine
proper, excluding testing/) is below the threshold.
"""

import argparse
import collections
import glob
import gzip
import json
import os
import subprocess
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("build_dir")
    ap.add_argument("--threshold", type=float, default=85.0)
    ap.add_argument("--gcov", default="gcov")
    args = ap.parse_args()

    gcda = glob.glob(os.path.join(args.build_dir, "**", "*.gcda"), recursive=True)
    if not gcda:
        print(f"no .gcda files under {args.build_dir} — run the coverage build's tests first",
              file=sys.stderr)
        return 1

    lines: dict[tuple[str, int], int] = collections.defaultdict(int)
    for path in gcda:
        workdir = os.path.dirname(path)
        subprocess.run([args.gcov, "--json-format", os.path.basename(path)],
                       cwd=workdir, capture_output=True, check=False)
        for jz in glob.glob(os.path.join(workdir, "*.gcov.json.gz")):
            with gzip.open(jz) as f:
                data = json.load(f)
            os.remove(jz)
            for fr in data.get("files", []):
                name = fr["file"].replace("\\", "/")
                if "include/nanolob/" not in name or "/testing/" in name:
                    continue
                short = name.split("include/nanolob/")[-1]
                for ln in fr["lines"]:
                    lines[(short, ln["line_number"])] += ln["count"]

    per_file: dict[str, list[int]] = collections.defaultdict(lambda: [0, 0])
    for (fname, _), count in lines.items():
        per_file[fname][1] += 1
        per_file[fname][0] += count > 0

    total_cov = total_lines = 0
    print(f"{'file':42s} {'covered':>12s}")
    for fname in sorted(per_file):
        cov, n = per_file[fname]
        total_cov += cov
        total_lines += n
        print(f"{fname:42s} {cov:4d}/{n:<4d} {100 * cov / n:5.1f}%")
    pct = 100.0 * total_cov / total_lines if total_lines else 0.0
    print(f"{'TOTAL':42s} {total_cov:4d}/{total_lines:<4d} {pct:5.1f}%")

    if pct < args.threshold:
        print(f"FAIL: {pct:.1f}% < {args.threshold:.1f}% threshold", file=sys.stderr)
        return 1
    print(f"OK: {pct:.1f}% >= {args.threshold:.1f}% threshold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
