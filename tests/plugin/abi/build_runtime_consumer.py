"""Build a genuine installed-SDK client, then load precompiled fixture packages."""

import argparse
import os
import pathlib
import shutil

from run_counter_matrix import run


def remove_generated(path):
    if path.exists():
        path.chmod(0o700)
        for entry in path.rglob("*"):
            entry.chmod(0o700 if entry.is_dir() else 0o600)
        shutil.rmtree(path)


def main(args):
    root = args.build / "runtime-consumer"
    remove_generated(root)
    root.mkdir(parents=True)
    log = root / "runtime.log"
    prefix = root / "sdk"
    run(["cmake", "--install", args.build, "--prefix", prefix], log)
    source = root / "source"
    fixtures = args.source / "tests/plugin/abi"
    shutil.copytree(fixtures / "runtime_consumer", source)
    for header in ("counter.h", "counter.hpp"):
        shutil.copyfile(fixtures / header, source / header)
    flags = {"none": "", "address-undefined": "-fsanitize=address,undefined -fno-omit-frame-pointer",
             "thread": "-fsanitize=thread -fno-omit-frame-pointer"}[args.sanitizer]
    run(["cmake", "-S", source, "-B", root / "build", f"-DCMAKE_PREFIX_PATH={prefix}",
         f"-DCMAKE_CXX_COMPILER={args.compiler}", f"-DCMAKE_CXX_FLAGS={flags}"], log)
    run(["cmake", "--build", root / "build", "-j2"], log)
    binary = root / "build/runtime_consumer"
    packages = []
    for implementation in ("provider-0", "provider-1", "cpp_provider-0"):
        packages.append(("counter", args.build / f"counter-matrix/{implementation}-installed/lib/rodaq/plugins/com.example.counter/1.0.0"))
    for family in ("acquisition", "storage"):
        for minor in (0, 1):
            packages.append((family, args.build / f"domain-matrix/{family}-provider-{minor}-installed/lib/rodaq/plugins/com.example.{family}.compat/1.0.0"))
    release = root / "readonly-packages"
    release.mkdir()
    copies = []
    for index, (family, package) in enumerate(packages):
        destination = release / str(index)
        shutil.copytree(package, destination)
        copies.append((family, destination))
    for path in release.rglob("*"):
        path.chmod(0o555 if path.is_dir() else 0o444)
    release.chmod(0o555)
    for family, package in copies:
        run([binary, package, family], log,
            env={**os.environ, "LD_LIBRARY_PATH": str(prefix / "lib") + ":" + os.environ.get("LD_LIBRARY_PATH", "")})
    print("Installed C++17 SDK runtime consumer: 7/7 precompiled C/C++ providers loaded, invoked and closed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--sanitizer", choices=("none", "address-undefined", "thread"), default="none")
    main(parser.parse_args())
