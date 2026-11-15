"""Replay immutable ABI fixtures without invoking a compiler or build tool."""

import argparse
import hashlib
import json
import os
import pathlib
import subprocess


def verify(root, metadata):
    actual = {str(path.relative_to(root)) for path in root.rglob("*") if path.is_file()}
    if actual != set(metadata["sha256"]) | {"bundle.json"}:
        raise ValueError("Bundle file set changed")
    for relative, expected in metadata["sha256"].items():
        path = root / relative
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            raise ValueError("Bundle contains an escaping path")
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f"Frozen artifact changed: {relative}")


def packages(profile):
    for provider in ("provider-0", "provider-1", "cpp_provider-0"):
        yield "counter", profile / f"counter/{provider}-installed/lib/rodaq/plugins/com.example.counter/1.0.0"
    for family in ("acquisition", "storage"):
        for minor in (0, 1):
            yield family, profile / f"domain/{family}-provider-{minor}-installed/lib/rodaq/plugins/com.example.{family}.compat/1.0.0"


def replay(args):
    root = args.bundle.resolve()
    metadata = json.loads((root / "bundle.json").read_text())
    verify(root, metadata)
    count = 0
    with args.log.open("w") as log:
        def run(command, environment):
            nonlocal count
            result = subprocess.run(command, env=environment, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            log.write(json.dumps([str(part) for part in command]) + "\n" + result.stdout)
            log.flush()
            if result.returncode:
                raise RuntimeError(f"Frozen replay failed ({result.returncode}); see {args.log}")
            count += 1

        for consumer_name in metadata["profiles"]:
            consumer = root / consumer_name
            environment = {**os.environ, "LD_LIBRARY_PATH": str(consumer / "lib")}
            runtime = args.runtime_consumer or consumer / "runtime_consumer"
            if args.runtime_library_dir:
                environment["LD_LIBRARY_PATH"] = str(args.runtime_library_dir.resolve())
            for provider_name in metadata["profiles"]:
                provider = root / provider_name
                for minor in (0, 1):
                    for implementation in ("provider-0", "provider-1", "cpp_provider-0"):
                        artifact = provider / f"counter/{implementation}-installed/lib/rodaq/plugins/com.example.counter/1.0.0/lib"
                        binary = next(artifact.glob("*.so"))
                        run([consumer / f"counter/consumer-{minor}-build/consumer", binary], environment)
                for family in ("acquisition", "storage"):
                    for old in (0, 1):
                        for new in (0, 1):
                            run([consumer / f"domain/{family}-consumer-{old}-build/domain_consumer",
                                 provider / f"domain/{family}-provider-{new}-installed/lib/rodaq/plugins/com.example.{family}.compat/1.0.0/lib/libdomain_provider.so",
                                 str(new)], environment)
                for family, package in packages(provider):
                    run([runtime, package, family], environment)
    verify(root, metadata)
    print(f"{metadata['identity']}: {count}/84 frozen binary directions passed; no rebuild, hashes unchanged")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=pathlib.Path, required=True)
    parser.add_argument("--log", type=pathlib.Path, required=True)
    parser.add_argument("--runtime-consumer", type=pathlib.Path)
    parser.add_argument("--runtime-library-dir", type=pathlib.Path)
    arguments = parser.parse_args()
    if bool(arguments.runtime_consumer) != bool(arguments.runtime_library_dir):
        parser.error("Current runtime consumer and library directory must be supplied together")
    replay(arguments)
