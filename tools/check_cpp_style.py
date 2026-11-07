#!/usr/bin/env python3
"""Check project formatting and clang-tidy without touching source files.

Configure with tests, benchmarks and CMAKE_EXPORT_COMPILE_COMMANDS enabled, build,
then run rodaq.abi.install_smoke before invoking this script. Generated and
third-party sources are excluded; public headers and install consumers are checked
as separate C++17 translation units. The Stable C ABI fixtures remain included.
"""

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("include/rodaq", "src", "tests", "benchmarks", "apps")
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp"}


def run(command):
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
    return result.returncode, result.stdout + result.stderr


def compiler_include_args():
    """Supply GCC's C++ paths when Clang cannot discover a local GCC install."""
    compiler = os.environ.get("CXX", "c++")
    version = subprocess.check_output([compiler, "-dumpversion"], text=True).strip().split(".")[0]
    multiarch = subprocess.check_output([compiler, "-print-multiarch"], text=True).strip()
    paths = (Path("/usr/include/c++") / version, Path("/usr/include") / multiarch / "c++" / version)
    return [item for path in paths if path.is_dir() for item in ("-isystem", str(path))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--clang-tidy", default="clang-tidy-14")
    parser.add_argument("--clang-format", default="clang-format-14")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    build = args.build_dir.resolve()
    database = build / "compile_commands.json"
    if not database.is_file():
        parser.error("configure the build with CMAKE_EXPORT_COMPILE_COMMANDS=ON")
    commands = json.loads(database.read_text())
    files = sorted({
        Path(entry["file"]).resolve() for entry in commands
        if Path(entry["file"]).resolve().is_relative_to(ROOT)
        and not Path(entry["file"]).resolve().is_relative_to(ROOT / "third_party")
        and not Path(entry["file"]).resolve().is_relative_to(build)
    })
    sources = sorted(
        p for root in SOURCE_ROOTS for p in (ROOT / root).rglob("*")
        if p.is_file() and p.suffix in SOURCE_SUFFIXES
    )
    headers = sorted((ROOT / "include/rodaq").rglob("*.hpp"))
    c_headers = sorted((ROOT / "include/rodaq").rglob("*.h"))
    consumers = sorted(p for p in (ROOT / "tests/abi/install").rglob("*")
                       if p.is_file() and p.suffix in {".c", ".cpp"})
    generated = sorted(build.rglob("consumer_sdk_plugin_metadata.hpp"))
    if not generated:
        parser.error("run rodaq.abi.install_smoke before the style check to generate SDK metadata")
    include_args = compiler_include_args()
    common = [args.clang_tidy, "--quiet", "--config-file=" + str(ROOT / ".clang-tidy"), "--header-filter=^" + re.escape(str(ROOT)) + "/(include/rodaq|src|tests|benchmarks)/"]
    standalone = ["-x", "c++", "-std=c++17", "-I" + str(ROOT / "include"),
                  "-I" + str(ROOT / "third_party/tl_expected/include"),
                  "-I" + str(generated[0].parent), *include_args]
    work = [("format " + str(p.relative_to(ROOT)), [args.clang_format, "--dry-run", "--Werror", str(p)])
            for p in sources]
    for p in files:
        extra = [] if p.suffix == ".c" else include_args
        flags = ["--extra-arg=" + flag for flag in [*extra, "-Wno-error", "-Wno-unknown-warning-option"]]
        work.append(("tidy " + str(p.relative_to(ROOT)), [*common, "-p", str(build), str(p), *flags]))
    c_flags = ["-x", "c", "-std=c11", "-I" + str(ROOT / "include")]
    for p in [*headers, *c_headers, *consumers]:
        flags = c_flags if p.suffix in {".c", ".h"} else standalone
        work.append(("standalone " + str(p.relative_to(ROOT)), [*common, str(p), "--", *flags]))
    for p in c_headers:
        work.append(("C++ header " + str(p.relative_to(ROOT)), [*common, str(p), "--", *standalone]))
    print(f"Checking {len(sources)} files, {len(files)} translation units, "
          f"{len(headers)} C++ headers, {len(c_headers)} C ABI headers in both languages "
          f"and {len(consumers)} install consumers", flush=True)
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for (label, _), (status, output) in zip(work, pool.map(lambda item: run(item[1]), work)):
            if status != 0:
                failures.append((label, output))
    for label, output in failures:
        print(f"FAIL: {label}\n{output}", file=sys.stderr)
    if failures:
        print(f"{len(failures)} checks failed", file=sys.stderr)
        return 1
    print("All project formatting and clang-tidy checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
