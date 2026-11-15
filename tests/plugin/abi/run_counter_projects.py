"""Build the four independent Counter SDK/Backend/Application projects."""

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess

from build_runtime_consumer import remove_generated
from run_counter_matrix import run


def main(args):
    root = args.build / "counter-projects"
    remove_generated(root)
    root.mkdir()
    log = root / "projects.log"
    prefix = root / "sdk"
    run(["cmake", "--install", args.build, "--prefix", prefix], log)
    fixture = args.source / "tests/plugin/abi"
    flags = {"none": "", "address-undefined": "-fsanitize=address,undefined -fno-omit-frame-pointer",
             "thread": "-fsanitize=thread -fno-omit-frame-pointer"}[args.sanitizer]
    for project in ("interface_sdk", "atomic_counter_provider", "shared_memory_counter_provider", "counter_application"):
        source = root / (project + "-source")
        build = root / (project + "-build")
        shutil.copytree(fixture / project, source)
        if project == "interface_sdk":
            for header in ("counter.h", "counter.hpp"):
                shutil.copyfile(fixture / header, source / header)
        run(["cmake", "-S", source, "-B", build, f"-DCMAKE_PREFIX_PATH={prefix}",
             f"-DCMAKE_CXX_COMPILER={args.compiler}", f"-DCMAKE_CXX_FLAGS={flags}"], log)
        run(["cmake", "--build", build, "-j2"], log)
        if project != "counter_application":
            run(["cmake", "--install", build, "--prefix", prefix], log)
    packages = prefix / "lib/rodaq/plugins"
    for manifest in packages.rglob("manifest.json"):
        data = json.loads(manifest.read_text())
        artifact = manifest.parent / data["artifact"]["path"]
        if hashlib.sha256(artifact.read_bytes()).hexdigest() != data["artifact"]["sha256"]:
            raise ValueError("Counter package digest mismatch")
        run(["python3", args.source / "tools/check_plugin_symbols.py", artifact], log)
    for path in packages.rglob("*"):
        path.chmod(0o555 if path.is_dir() else 0o444)
    packages.chmod(0o555)
    environment = {**os.environ, "LD_LIBRARY_PATH": str(prefix / "lib") + ":" + os.environ.get("LD_LIBRARY_PATH", "")}
    application = root / "counter_application-build/counter_application"
    for identity in ("com.example.counter.atomic", "com.example.counter.shared_memory"):
        run([application, packages, identity], log, env=environment)
    failed = subprocess.run([application, packages, "com.example.missing"], env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10)
    if failed.returncode != 1 or "Counter application failed:" not in failed.stdout or "abandoned" in failed.stdout:
        raise RuntimeError("Application failed to preserve load error and explicitly clean up Manager")
    with log.open("a") as stream:
        stream.write("Expected missing-Plugin failure with explicit cleanup:\n" + failed.stdout)
    print("Four independent projects: shared installed Interface SDK, atomic/shared-memory Backends, public Application passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--sanitizer", choices=("none", "address-undefined", "thread"), default="none")
    main(parser.parse_args())
