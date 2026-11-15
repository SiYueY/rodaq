"""A negative qualification passes only when the intended sanitizer report occurs."""

import argparse
import pathlib
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True)
parser.add_argument("--report", type=pathlib.Path, required=True)
args = parser.parse_args()
result = subprocess.run([args.binary], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
args.report.write_text(result.stdout)
expected = "ERROR: AddressSanitizer: stack-use-after-"
if result.returncode == 0 or expected not in result.stdout:
    raise SystemExit(f"Expected Provider lifetime violation was not diagnosed ({result.returncode}):\n{result.stdout}")
print("Expected configure-local Provider lifetime violation detected by ASan; full report:", args.report)
