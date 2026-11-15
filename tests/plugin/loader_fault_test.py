"""Run bootstrap rejection in isolated processes against readonly, correctly hashed packages."""

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile


def main(args):
    manifest = json.loads(args.manifest.read_text())
    with tempfile.TemporaryDirectory(prefix="rodaq-loader-fault-") as temporary:
        root = pathlib.Path(temporary)
        package = root / "package"
        package.mkdir()
        artifact = package / "fault.so"
        shutil.copyfile(args.provider, artifact)
        manifest["artifact"] = {"path": "fault.so", "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest()}
        (package / "manifest.json").write_text(json.dumps(manifest))
        for entry in package.iterdir():
            entry.chmod(0o444)
        package.chmod(0o555)
        try:
            for mode, status in (("major", 7), ("minor", 7), ("short_entry", 1),
                                 ("null_entry", 1), ("misaligned_entry", 1), ("missing_slot", 1),
                                 ("short_descriptor", 1), ("descriptor_mismatch", 1),
                                 ("throw_version", 10), ("throw_entry", 10)):
                result = subprocess.run([args.consumer, package, str(status)],
                                        env={**os.environ, "RODAQ_LOADER_FAULT": mode},
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10)
                print(mode + ":\n" + result.stdout, flush=True)
                if result.returncode:
                    raise RuntimeError(f"Bootstrap rejection failed: {mode} ({result.returncode})")
        finally:
            package.chmod(0o700)
        for index, proxy in enumerate(args.proxy):
            dependent = root / ("dependency-" + str(index))
            dependent.mkdir()
            shutil.copyfile(args.provider, dependent / args.provider.name)
            shutil.copyfile(proxy, dependent / "proxy.so")
            manifest["artifact"] = {"path": "proxy.so", "sha256": hashlib.sha256(proxy.read_bytes()).hexdigest()}
            (dependent / "manifest.json").write_text(json.dumps(manifest))
            for entry in dependent.iterdir():
                entry.chmod(0o444)
            dependent.chmod(0o555)
            try:
                subprocess.run([args.consumer, dependent, "1"], check=True, timeout=10,
                               env={**os.environ, "RODAQ_LOADER_FAULT": "dependency"})
            finally:
                dependent.chmod(0o700)
    print("12/12 Loader version/prefix/alignment/descriptor/exception/provenance rejections passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", type=pathlib.Path, required=True)
    parser.add_argument("--consumer", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--proxy", type=pathlib.Path, action="append", required=True)
    main(parser.parse_args())
