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
    for template, mode in ((continuous, "continuous"), (rolling, "rolling")):
        for flag in (False, True, None, 1, "true"):
            value = copy.deepcopy(template)
            value["retention_policy"]["include_previous_continuous"] = flag
            expected = type(flag) is bool and (mode == "rolling" or not flag)
            cases.append((f"previous continuous {mode} {flag!r}", value, expected))
    for field in ("format_version", "recovery_policy", "start_policy", "segment_policy", "retention_policy"):
        missing = copy.deepcopy(continuous)
        del missing[field]
        cases.append(("missing " + field, missing, False))
    for decimal in ("0", "00", "+1", "-1", "18446744073709551616", "1\n"):
        invalid = copy.deepcopy(continuous)
        invalid["pipelines"][0]["streams"][0]["stream_id"] = decimal
        cases.append(("invalid ID " + decimal, invalid, False))
    for version in ("01.0.0", "1.2", "1.0.0-01", "1.0.0+", "1.0.0+a+b", "1.0.0\n"):
        invalid = copy.deepcopy(continuous)
        invalid["pipelines"][0]["acquisition"]["plugin_version"] = version
        cases.append(("invalid SemVer " + version, invalid, False))
    for unit, maximum in (("ns", 9223372036854775807), ("us", 9223372036854775), ("ms", 9223372036854), ("s", 9223372036)):
        for offset in (0, 1):
            value = copy.deepcopy(continuous)
            value["segment_policy"]["max_duration"] = {"value": str(maximum + offset), "unit": unit}
            cases.append(("duration " + unit + str(offset), value, offset == 0))
    invalid = copy.deepcopy(continuous)
    invalid["retention_policy"]["max_physical_bytes"] = "1"
    cases.append(("continuous cannot retain by rolling bound", invalid, False))
    invalid = copy.deepcopy(rolling)
    invalid["retention_policy"]["max_duration"] = None
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
            raise RuntimeError("Schema result differs from contract: " + name)
        result = subprocess.run([args.probe], input=json.dumps(value), text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
        if (expected and result.returncode != 0) or (
            not expected and (result.returncode != 1 or result.stdout.strip() != "1")
        ):
            raise RuntimeError(f"C++ codec differs for {name}: {result.returncode}, {result.stdout}")
        if expected:
            validator.validate(json.loads(result.stdout))
    # Owner-only constraints cannot be expressed by standard JSON Schema keywords.
    for name, value in (("UTF-8 byte limit", copy.deepcopy(continuous)), ("Task-scoped Stream identity", copy.deepcopy(continuous))):
        if name == "UTF-8 byte limit":
            value["name"] = "😀" * 33
        else:
            second = copy.deepcopy(value["pipelines"][0])
            second["pipeline_id"] = "11"
            value["pipelines"].append(second)
        if not validator.is_valid(value):
            raise RuntimeError("Owner-only rule unexpectedly hidden in Schema: " + name)
        result = subprocess.run([args.probe], input=json.dumps(value), text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
        if result.returncode != 1 or result.stdout.strip() != "1":
            raise RuntimeError("Missing Task-owned constraint: " + name)
    print(f"Draft 2020-12 metaschema, {len(cases)} schema/codec vectors and 2 owner-only constraints passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema", type=pathlib.Path, required=True)
    parser.add_argument("--fixtures", type=pathlib.Path, required=True)
    parser.add_argument("--probe", required=True)
    main(parser.parse_args())
