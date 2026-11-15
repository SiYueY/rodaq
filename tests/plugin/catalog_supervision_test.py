"""Block the actual discovery fdopendir call, without changing production code."""

import argparse
import os
import pathlib
import re
import subprocess
import tempfile


def main(args):
    with tempfile.TemporaryDirectory(prefix="rodaq-catalog-supervision-") as temporary:
        root = pathlib.Path(temporary)
        package = root / "packages"
        package.mkdir(mode=0o555)
        control = root / "control"
        control.mkdir()
        os.mkfifo(control / "gate", 0o600)
        preload = str(args.interposer)
        # GCC ASan's runtime must remain first; do not disable its order check.
        dependencies = subprocess.check_output(["ldd", args.consumer], text=True)
        asan = re.search(r"libasan\S* => (\S+)", dependencies)
        if asan:
            preload = asan[1] + ":" + preload
        subprocess.run([args.consumer, package, control], check=True, timeout=40,
                       env={**os.environ, "LD_PRELOAD": preload,
                            "RODAQ_CATALOG_BLOCK_ROOT": str(package),
                            "RODAQ_CATALOG_GATE": str(control / "gate"),
                            "RODAQ_CATALOG_ENTERED": str(control / "entered"),
                            "RODAQ_CATALOG_RETURNED": str(control / "returned")})
        package.chmod(0o700)
    print("Actual discovery fdopendir stall: 30s supervision, observer return and sticky authority passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--interposer", type=pathlib.Path, required=True)
    parser.add_argument("--consumer", type=pathlib.Path, required=True)
    main(parser.parse_args())
