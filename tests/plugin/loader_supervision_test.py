"""Exercise a genuinely blocked ELF initializer and supervised late rollback."""

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile


def main(args):
    with tempfile.TemporaryDirectory(prefix="rodaq-load-supervision-") as temporary:
        root = pathlib.Path(temporary)
        packages = root / "packages"
        packages.mkdir()
        control = root / "control"
        control.mkdir()
        os.mkfifo(control / "gate", 0o600)
        shutil.copytree(args.healthy_package, packages / "healthy")
        fault = packages / "fault"
        fault.mkdir()
        shutil.copyfile(args.provider, fault / "fault.so")
        manifest = json.loads(args.manifest.read_text())
        manifest["artifact"] = {"path": "fault.so", "sha256": hashlib.sha256(args.provider.read_bytes()).hexdigest()}
        (fault / "manifest.json").write_text(json.dumps(manifest))
        for path in packages.rglob("*"):
            path.chmod(0o555 if path.is_dir() else 0o444)
        packages.chmod(0o555)
        try:
            subprocess.run([args.consumer, packages, control], check=True, timeout=40,
                           env={**os.environ, "RODAQ_LOADER_FAULT": "hang_load",
                                "RODAQ_LOADER_GATE": str(control / "gate"),
                                "RODAQ_LOADER_ENTERED": str(control / "entered"),
                                "RODAQ_LOADER_UNLOADED": str(control / "unloaded")})
        finally:
            packages.chmod(0o700)
            for path in packages.rglob("*"):
                path.chmod(0o700 if path.is_dir() else 0o600)
    print("Blocked dlopen: 30s supervision, independent Instance close and late unpublished rollback passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", type=pathlib.Path, required=True)
    parser.add_argument("--consumer", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--healthy-package", type=pathlib.Path, required=True)
    main(parser.parse_args())
