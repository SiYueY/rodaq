"""Establish independent C11 domain 1.0/synthetic-1.1 binary fixtures."""

import argparse
import hashlib
import json
import os
import pathlib
import shutil

from run_counter_matrix import run


def main(args):
    root = args.build / "domain-matrix"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    log = root / "matrix.log"
    prefix = root / "sdk"
    run(["cmake", "--install", args.build, "--prefix", prefix], log)
    fixtures = args.source / "tests/plugin/abi"
    sanitize = {"none": "", "address-undefined": "-fsanitize=address,undefined -fno-omit-frame-pointer",
                "thread": "-fsanitize=thread -fno-omit-frame-pointer"}[args.sanitizer]
    programs = {}
    for domain in ("acquisition", "storage"):
        for kind in ("provider", "consumer"):
            for minor in (0, 1):
                identity = f"{domain}-{kind}-{minor}"
                source = root / (identity + "-source")
                build = root / (identity + "-build")
                shutil.copytree(fixtures / ("domain_" + kind), source)
                shutil.copyfile(fixtures / "domain_fixture.h", source / "domain_fixture.h")
                run(["cmake", "-S", source, "-B", build,
                     f"-DCMAKE_PREFIX_PATH={prefix}", f"-DCMAKE_C_COMPILER={args.compiler}",
                     f"-DDOMAIN_STORAGE={int(domain == 'storage')}", f"-DFIXTURE_MINOR={minor}",
                     f"-DCMAKE_C_FLAGS={sanitize}"], log)
                run(["cmake", "--build", build, "-j2"], log)
                if kind == "provider":
                    installed = root / (identity + "-installed")
                    run(["cmake", "--install", build, "--prefix", installed], log)
                    manifest = installed / f"lib/rodaq/plugins/com.example.{domain}.compat/1.0.0/manifest.json"
                    data = json.loads(manifest.read_text())
                    artifact = manifest.parent / data["library"]
                    if not artifact.is_file():
                        raise ValueError("Installed domain library missing")
                    run(["python3", args.source / "tools/check_plugin_symbols.py", artifact], log)
                    programs[identity] = artifact
                else:
                    programs[identity] = build / "domain_consumer"
        for consumer in (0, 1):
            for provider in (0, 1):
                run([programs[f"{domain}-consumer-{consumer}"],
                     programs[f"{domain}-provider-{provider}"], str(provider)], log,
                    env={**os.environ, "LD_LIBRARY_PATH": str(prefix / "lib") + ":" + os.environ.get("LD_LIBRARY_PATH", "")})
    inventory = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in programs.values()}
    (root / "binary-sha256.json").write_text(json.dumps(inventory, indent=2) + "\n")
    print("Domain ABI matrix: 8/8 binary directions, 4/4 symbol policies, caller-capacity canaries passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--sanitizer", choices=("none", "address-undefined", "thread"), default="none")
    main(parser.parse_args())
