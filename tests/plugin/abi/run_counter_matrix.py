"""Initial ABI fixture establishment in independent copied source/build trees."""

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess


def run(command, log, env=None):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    with log.open("a") as stream:
        stream.write(json.dumps([str(value) for value in command]) + "\n" + result.stdout)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log}\n{result.stdout}")


def main(args):
    root = args.build / "counter-matrix"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    log = root / "matrix.log"
    prefix = root / "sdk"
    run(["cmake", "--install", args.build, "--prefix", prefix], log)
    fixtures = args.source / "tests/plugin/abi"
    programs = {}
    sanitize = {"none": "", "address-undefined": "-fsanitize=address,undefined -fno-omit-frame-pointer",
                "thread": "-fsanitize=thread -fno-omit-frame-pointer"}[args.sanitizer]
    instrumentation = [f"-DCMAKE_C_FLAGS={sanitize}", f"-DCMAKE_CXX_FLAGS={sanitize}"]
    for kind in ("provider", "cpp_provider", "lifecycle_provider", "storage_provider", "acquisition_provider", "consumer"):
        for minor in ((0,) if kind in ("cpp_provider", "lifecycle_provider", "storage_provider", "acquisition_provider") else (0, 1)):
            identity = f"{kind}-{minor}"
            source = root / (identity + "-source")
            build = root / (identity + "-build")
            shutil.copytree(fixtures / kind, source)
            for header in ("counter.h", "counter_fixture.h", "counter.hpp", "probe.hpp"):
                shutil.copyfile(fixtures / header, source / header)
            compiler_flags = ([f"-DCMAKE_CXX_COMPILER={args.cxx_compiler}"]
                              if kind in ("cpp_provider", "lifecycle_provider", "storage_provider", "acquisition_provider") else
                              [f"-DCMAKE_C_COMPILER={args.compiler}", f"-DFIXTURE_MINOR={minor}"])
            run(["cmake", "-S", source, "-B", build,
                 f"-DCMAKE_PREFIX_PATH={prefix}", *compiler_flags, *instrumentation], log)
            run(["cmake", "--build", build, "-j2"], log)
            if kind in ("provider", "cpp_provider", "lifecycle_provider", "storage_provider", "acquisition_provider"):
                installed = root / (identity + "-installed")
                run(["cmake", "--install", build, "--prefix", installed], log)
                plugin_id = {"lifecycle_provider": "com.example.lifecycle",
                             "storage_provider": "com.example.storage.probe",
                             "acquisition_provider": "com.example.acquisition.probe"}.get(kind, "com.example.counter")
                manifest = installed / f"lib/rodaq/plugins/{plugin_id}/1.0.0/manifest.json"
                data = json.loads(manifest.read_text())
                artifact = manifest.parent / data["artifact"]["path"]
                if hashlib.sha256(artifact.read_bytes()).hexdigest() != data["artifact"]["sha256"]:
                    raise ValueError("Manifest artifact hash mismatch")
                run(["python3", args.source / "tools/check_plugin_symbols.py", artifact], log)
                programs[identity] = artifact
            else:
                programs[identity] = build / "consumer"

    # Each binary is built once; all six direction tests reuse those binaries.
    for consumer in (0, 1):
        for provider in ("provider-0", "provider-1", "cpp_provider-0"):
            run([programs[f"consumer-{consumer}"], programs[provider]], log,
                env={**os.environ, "LD_LIBRARY_PATH": str(prefix / "lib") + ":" + os.environ.get("LD_LIBRARY_PATH", "")})
    inventory = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in programs.values()}
    (root / "binary-sha256.json").write_text(json.dumps(inventory, indent=2) + "\n")
    print("Counter matrix: 6/6 binary directions and 6/6 production symbol policies passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sanitizer", choices=("none", "address-undefined", "thread"), default="none")
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cxx-compiler", required=True)
    main(parser.parse_args())
