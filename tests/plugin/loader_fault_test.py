"""Run bootstrap rejection in isolated processes against malformed plugin DSOs."""

import argparse
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
        manifest.pop("artifact", None)
        manifest["library"] = "fault.so"
        (package / "manifest.json").write_text(json.dumps(manifest))
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
    print("10/10 Loader version/prefix/alignment/descriptor/exception rejections passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", type=pathlib.Path, required=True)
    parser.add_argument("--consumer", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--proxy", type=pathlib.Path, action="append", default=[])
    main(parser.parse_args())
