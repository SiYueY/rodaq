"""Check same-major compatibility; provenance is retained, not compared as layout."""

import argparse
import json
import pathlib


def compare(old, new):
    errors = []
    for key in ("format_version", "architecture", "symbols"):
        if old[key] != new[key]:
            errors.append(f"{key} changed")
    if old["abi"]["major"] != new["abi"]["major"]:
        errors.append("ABI major differs; no same-major compatibility claim")
    if new["abi"]["minor"] < old["abi"]["minor"]:
        errors.append("ABI minor regressed")
    for name, value in old["constants"].items():
        if new["constants"].get(name) != value:
            errors.append(f"constant {name} changed or disappeared")
    for name, previous in old["types"].items():
        current = new["types"].get(name)
        if current is None:
            errors.append(f"type {name} disappeared")
            continue
        if previous["class"] != current["class"]:
            errors.append(f"{name}: classification changed")
            continue
        if previous["class"] != "EXTENSIBLE_STRUCT":
            if previous != current:
                errors.append(f"{name}: frozen representation changed")
            continue
        if (current["sizeof"] < previous["sizeof"]
                or current["alignof"] != previous["alignof"]
                or current["required_prefix_size"] != previous["required_prefix_size"]):
            errors.append(f"{name}: size, alignment or required prefix changed")
        for field, facts in previous["fields"].items():
            if current["fields"].get(field) != facts:
                errors.append(f"{name}.{field}: field/slot changed or disappeared")
        for field, facts in current["fields"].items():
            if field not in previous["fields"]:
                previous_end = max(
                    value["offset"] + value["sizeof"]
                    for value in previous["fields"].values()
                )
                if facts["offset"] < previous_end:
                    errors.append(f"{name}.{field}: inserted before old prefix end")
                if facts.get("required", False):
                    errors.append(f"{name}.{field}: new required slot needs a new major")
    return errors


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("old", type=pathlib.Path)
    parser.add_argument("new", type=pathlib.Path)
    args = parser.parse_args()
    failures = compare(json.loads(args.old.read_text()), json.loads(args.new.read_text()))
    for failure in failures:
        print(failure)
    raise SystemExit(bool(failures))
