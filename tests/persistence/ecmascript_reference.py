"""Optional qualification against a real ECMAScript engine; no runtime dependency."""

import argparse
import json
import random
import subprocess


def main(args):
    randomizer = random.Random(0x8785)
    bits = [f"{randomizer.getrandbits(64):016x}" for _ in range(10000)]
    bits += [f"{value:016x}" for value in (0, 1, 0x8000000000000000, 0x7ff0000000000000,
                                          0x7fffffffffffffff, 0x7fefffffffffffff)]
    native = subprocess.run([args.probe], input="\n".join(bits) + "\n", text=True,
                            stdout=subprocess.PIPE, check=True, timeout=30).stdout.splitlines()
    javascript = """
const fs = require('fs');
const values = JSON.parse(fs.readFileSync(0, 'utf8'));
const view = new DataView(new ArrayBuffer(8));
for (const bits of values) {
  view.setBigUint64(0, BigInt('0x' + bits));
  const value = view.getFloat64(0);
  process.stdout.write((Number.isFinite(value) ? JSON.stringify(value) : 'INVALID') + '\\n');
}
"""
    reference = subprocess.run([args.node, "-e", javascript], input=json.dumps(bits), text=True,
                               stdout=subprocess.PIPE, check=True, timeout=30).stdout.splitlines()
    if len(native) != len(bits) or len(reference) != len(bits):
        raise RuntimeError("Missing number serialization results")
    for value, actual, expected in zip(bits, native, reference):
        if actual != expected:
            raise RuntimeError(f"JCS/ECMAScript mismatch: {value}: {actual!r} != {expected!r}")
    version = subprocess.check_output([args.node, "--version"], text=True).strip()
    print(f"JCS binary64: {len(bits)}/{len(bits)} bit patterns match ECMAScript JSON.stringify ({version})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--node", required=True)
    main(parser.parse_args())
