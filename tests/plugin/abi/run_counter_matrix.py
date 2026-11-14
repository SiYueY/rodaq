"""Initial ABI fixture establishment in independent copied source/build trees."""

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess


def run(command, log):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
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
    for kind in ("provider", "consumer"):
        for minor in (0, 1):
            identity = f"{kind}-{minor}"
            source = root / (identity + "-source")
            build = root / (identity + "-build")
            shutil.copytree(fixtures / kind, source)
            for header in ("counter.h", "counter_fixture.h"):
                shutil.copyfile(fixtures / header, source / header)
            run(["cmake", "-S", source, "-B", build,
                 f"-DCMAKE_PREFIX_PATH={prefix}", f"-DCMAKE_C_COMPILER={args.compiler}",
                 f"-DFIXTURE_MINOR={minor}"], log)
            run(["cmake", "--build", build, "-j2"], log)
            if kind == "provider":
                installed = root / (identity + "-installed")
                run(["cmake", "--install", build, "--prefix", installed], log)
                manifest = installed / "lib/rodaq/plugins/com.example.counter/1.0.0/manifest.json"
                data = json.loads(manifest.read_text())
                artifact = manifest.parent / data["artifact"]["path"]
                if hashlib.sha256(artifact.read_bytes()).hexdigest() != data["artifact"]["sha256"]:
                    raise ValueError("Manifest artifact hash mismatch")
                run(["python3", args.source / "tools/check_plugin_symbols.py", artifact], log)
                programs[identity] = artifact
            else:
                programs[identity] = build / "consumer"

    # Each binary is built once; all four direction tests reuse those binaries.
    for consumer in (0, 1):
        for provider in (0, 1):
            run([programs[f"consumer-{consumer}"], programs[f"provider-{provider}"]], log)
    inventory = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in programs.values()}
    (root / "binary-sha256.json").write_text(json.dumps(inventory, indent=2) + "\n")
    print("Counter matrix: 4/4 binary directions and 2/2 production symbol policies passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", required=True)
    main(parser.parse_args())
