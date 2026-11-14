"""Enforce the production DSO's complete defined dynamic symbol whitelist."""

import argparse
import subprocess


def check(path):
    output = subprocess.check_output(["nm", "-D", "--defined-only", path], text=True)
    symbols = {line.split()[-1] for line in output.splitlines() if line.split()}
    expected = {"rodaq_plugin_abi_version", "rodaq_plugin_get_entry"}
    if symbols != expected:
        raise ValueError(f"Unexpected symbols: actual={sorted(symbols)}, expected={sorted(expected)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("plugin")
    check(parser.parse_args().plugin)
