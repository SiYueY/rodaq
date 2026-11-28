"""Validate the executable Task projection and compare it with the actual C++ codec."""

import argparse
import copy
import json
import pathlib
import subprocess

import jsonschema


def main(args):
    schema = json.loads(args.schema.read_text())
    jsonschema.Draft202012Validator.check_schema(schema)
    validator = jsonschema.Draft202012Validator(schema)
    continuous = json.loads((args.fixtures / "continuous.json").read_text())
    rolling = json.loads((args.fixtures / "rolling.json").read_text())
    cases = [("continuous", continuous, True), ("rolling", rolling, True)]

    for field in ("format_version", "segment_policy", "retention_policy", "pipelines"):
        missing = copy.deepcopy(continuous)
        del missing[field]
        cases.append(("missing " + field, missing, False))

    for obsolete in ("recording_mode", "start_policy", "recovery_policy"):
        legacy = copy.deepcopy(continuous)
        legacy[obsolete] = "legacy"
        cases.append(("obsolete field " + obsolete, legacy, False))

    legacy_ret = copy.deepcopy(rolling)
    legacy_ret["retention_policy"]["include_previous_continuous"] = True
    cases.append(("obsolete include_previous_continuous", legacy_ret, False))

    for decimal in ("0", "00", "+1", "-1", "1\n", "123456789012345678901"):
        invalid = copy.deepcopy(continuous)
        invalid["pipelines"][0]["streams"][0]["stream_id"] = decimal
        cases.append(("invalid ID " + decimal, invalid, False))

    for version in ("01.0.0", "1.2", "1.0.0-01", "1.0.0+", "1.0.0+a+b", "1.0.0\n"):
        invalid = copy.deepcopy(continuous)
        invalid["pipelines"][0]["acquisition"]["plugin_version"] = version
        cases.append(("invalid SemVer " + version, invalid, False))

    for offset in (0, 1):
        value = copy.deepcopy(continuous)
        value["segment_policy"]["max_duration_ms"] = 9007199254740991 + offset
        cases.append((f"safe int boundary +{offset}", value, offset == 0))

    invalid = copy.deepcopy(continuous)
    invalid["segment_policy"]["max_duration_ms"] = 0
    cases.append(("zero duration", invalid, False))

    invalid = copy.deepcopy(continuous)
    invalid["segment_policy"]["max_duration_ms"] = None
    invalid["segment_policy"]["max_physical_bytes"] = None
    cases.append(("segment policy requires at least one bound", invalid, False))

    invalid = copy.deepcopy(continuous)
    invalid["retention_policy"]["max_physical_bytes"] = 1
    cases.append(("keep_all cannot set rolling byte bound", invalid, False))

    invalid = copy.deepcopy(rolling)
    invalid["retention_policy"]["max_duration_ms"] = None
    invalid["retention_policy"]["max_physical_bytes"] = None
    cases.append(("rolling requires bound", invalid, False))

    invalid = copy.deepcopy(continuous)
    invalid["extra"] = True
    cases.append(("unknown business field", invalid, False))

    for field in ("resource_profile", "output_root"):
        invalid = copy.deepcopy(continuous)
        invalid["pipelines"][0][field] += "\n"
        cases.append(("invalid visible reference " + field, invalid, False))

    maximum = copy.deepcopy(continuous)
    maximum["pipelines"][0]["streams"][0]["stream_id"] = "18446744073709551615"
    cases.append(("maximum ID", maximum, True))

    for name, value, expected in cases:
        if validator.is_valid(value) != expected:
            raise RuntimeError(f"Schema result differs from contract for {name}: expected {expected}")
        result = subprocess.run([args.probe], input=json.dumps(value), text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
        if (expected and result.returncode != 0) or (
            not expected and (result.returncode != 1 or result.stdout.strip() != "1")
        ):
            raise RuntimeError(f"C++ codec differs for {name}: {result.returncode}, {result.stdout}")
        if expected:
            validator.validate(json.loads(result.stdout))

    # Owner-only constraints cannot be expressed by standard JSON Schema keywords.
    owner_cases = [
        ("UTF-8 byte limit", lambda v: v.__setitem__("name", "😀" * 33)),
        ("uint64 overflow ID", lambda v: v["pipelines"][0]["streams"][0].__setitem__("stream_id", "18446744073709551616")),
    ]
    for name, mutate in owner_cases:
        value = copy.deepcopy(continuous)
        mutate(value)
        if not validator.is_valid(value):
            raise RuntimeError("Owner-only rule unexpectedly hidden in Schema: " + name)
        result = subprocess.run([args.probe], input=json.dumps(value), text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
        if result.returncode != 1 or result.stdout.strip() != "1":
            raise RuntimeError("Missing Task-owned constraint: " + name)

    dup_stream = copy.deepcopy(continuous)
    second = copy.deepcopy(dup_stream["pipelines"][0])
    second["pipeline_id"] = "11"
    dup_stream["pipelines"].append(second)
    if not validator.is_valid(dup_stream):
        raise RuntimeError("Owner-only rule unexpectedly hidden in Schema: Task-scoped Stream identity")
    result = subprocess.run([args.probe], input=json.dumps(dup_stream), text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
    if result.returncode != 1 or result.stdout.strip() != "1":
        raise RuntimeError("Missing Task-owned constraint: Task-scoped Stream identity")

    print(f"Draft 2020-12 metaschema, {len(cases)} schema/codec vectors and 3 owner-only constraints passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema", type=pathlib.Path, required=True)
    parser.add_argument("--fixtures", type=pathlib.Path, required=True)
    parser.add_argument("--probe", required=True)
    main(parser.parse_args())
