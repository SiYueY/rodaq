#!/usr/bin/env python3
"""OpenAPI 3.1.0 wire conformance & minor compatibility gate (SV-API-008, SV-API-009, SV-CORE-001)."""

import json
import pathlib
import re
import sys
import yaml


REQUIRED_PATHS = {
    "/api/session": {"post", "get", "delete"},
    "/api/system": {"get"},
    "/api/capabilities": {"get"},
    "/api/tasks": {"get", "post"},
    "/api/tasks/{task_id}": {"get"},
    "/api/tasks/{task_id}/config": {"get", "put"},
    "/api/tasks/{task_id}/operations": {"get", "post"},
    "/api/tasks/{task_id}/operations/{operation_id}": {"get"},
    "/api/tasks/{task_id}/runtime": {"get"},
    "/api/artifacts": {"get"},
    "/api/artifacts/{artifact_id}": {"get"},
    "/api/artifacts/{artifact_id}/files/{file_id}": {"head", "get"},
}

CLOSED_ENUMS = {
    "Retryability": [
        "never",
        "after_condition_change",
        "after_backoff",
        "after_process_restart",
        "manual_recovery_required",
    ],
    "FailureDomain": [
        "request",
        "task_control",
        "pipeline",
        "collector",
        "storage",
        "volume",
        "persistence_state",
        "process",
    ],
    "DataImpact": [
        "none",
        "pre_admission_only",
        "accepted_known_failed",
        "persistence_unknown",
        "artifact_quarantined",
        "historical_artifact_lost_or_corrupt",
    ],
    "DesiredState": ["stopped", "running", "paused"],
    "ExecutionCondition": [
        "idle",
        "preparing",
        "active",
        "degraded",
        "draining",
        "protected",
        "pending_cleanup",
        "failed",
    ],
    "OperationKind": ["configure", "start", "pause", "resume", "stop", "reset", "retire"],
    "OperationState": [
        "accepted",
        "in_progress",
        "succeeded",
        "partially_succeeded",
        "failed",
        "superseded",
    ],
}


def check_minor_compatibility(baseline_schema: dict, candidate_schema: dict) -> list[str]:
    """SV-API-009: enforce additive-only minor compatibility rules."""
    violations: list[str] = []
    base_req = set(baseline_schema.get("required", []))
    cand_req = set(candidate_schema.get("required", []))
    # Existing published required response fields must remain required.
    for field in sorted(base_req - cand_req):
        violations.append(f"required field removed or weakened to optional: {field}")
    base_props = baseline_schema.get("properties", {})
    cand_props = candidate_schema.get("properties", {})
    for field, base_def in base_props.items():
        if field not in cand_props:
            violations.append(f"published field removed: {field}")
            continue
        cand_def = cand_props[field]
        if base_def.get("type") != cand_def.get("type"):
            violations.append(f"field type changed: {field}")
        if "enum" in base_def:
            if set(cand_def.get("enum", [])) != set(base_def["enum"]):
                violations.append(f"closed enum modified without major bump: {field}")
    return violations


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "usage: openapi_conformance_test.py <openapi.yaml> <task_config.schema.json> <server_src_dir>",
            file=sys.stderr,
        )
        return 2

    openapi_path = pathlib.Path(sys.argv[1])
    task_schema_path = pathlib.Path(sys.argv[2])
    server_src_dir = pathlib.Path(sys.argv[3])

    doc = yaml.safe_load(openapi_path.read_text(encoding="utf-8"))
    assert isinstance(doc, dict), "openapi.yaml root must be a mapping"
    assert doc.get("openapi") == "3.1.0", f"expected OpenAPI 3.1.0, got {doc.get('openapi')}"

    # 1. Route parity & no /api/v1/ prefix (SV-API-001, SV-API-008).
    paths = doc.get("paths", {})
    for p in paths:
        assert not p.startswith("/api/v1/"), f"forbidden /api/v1/ route: {p}"
    assert set(paths.keys()) == set(REQUIRED_PATHS.keys()), (
        f"path set mismatch: {set(paths.keys()) ^ set(REQUIRED_PATHS.keys())}"
    )
    for path, methods in REQUIRED_PATHS.items():
        present = {k.lower() for k in paths[path].keys() if k.lower() in {"get", "post", "put", "delete", "head", "patch"}}
        assert present == methods, f"method mismatch on {path}: {present} != {methods}"

    # 2. Closed enums & decimal uint64 & duration wire types.
    schemas = doc["components"]["schemas"]
    for name, expected_values in CLOSED_ENUMS.items():
        assert name in schemas, f"missing closed enum schema {name}"
        assert schemas[name].get("enum") == expected_values, (
            f"enum mismatch for {name}: {schemas[name].get('enum')} != {expected_values}"
        )
    assert schemas["DecimalString"]["type"] == "string"
    assert schemas["DurationValue"]["required"] == ["value", "unit"]

    # 3. PortableTaskConfig mirrors schemas/task_config.schema.json.
    task_schema = json.loads(task_schema_path.read_text(encoding="utf-8"))
    portable = schemas["PortableTaskConfig"]
    assert set(portable["required"]) == set(task_schema["required"]), (
        f"PortableTaskConfig required mismatch: {portable['required']} != {task_schema['required']}"
    )
    assert portable["properties"]["start_policy"]["type"] == "object"
    assert portable["properties"]["start_policy"]["required"] == ["mode"]
    assert portable["properties"]["segment_policy"]["required"] == [
        "max_duration",
        "max_physical_bytes",
    ]
    assert portable["properties"]["retention_policy"]["required"] == [
        "mode",
        "max_duration",
        "max_physical_bytes",
    ]

    # 4. SV-API-009 minor compatibility gate self-test.
    base_resp = {
        "required": ["task_id", "desired_state"],
        "properties": {
            "task_id": {"type": "string"},
            "desired_state": {"type": "string", "enum": ["stopped", "running", "paused"]},
        },
    }
    additive_minor = {
        "required": ["task_id", "desired_state"],
        "properties": {
            "task_id": {"type": "string"},
            "desired_state": {"type": "string", "enum": ["stopped", "running", "paused"]},
            "extra_optional": {"type": "string"},
        },
    }
    assert check_minor_compatibility(base_resp, additive_minor) == []

    weakened_required = {
        "required": ["task_id"],
        "properties": base_resp["properties"],
    }
    assert check_minor_compatibility(base_resp, weakened_required), "must reject required->optional"

    expanded_closed_enum = {
        "required": ["task_id", "desired_state"],
        "properties": {
            "task_id": {"type": "string"},
            "desired_state": {"type": "string", "enum": ["stopped", "running", "paused", "new_val"]},
        },
    }
    assert check_minor_compatibility(base_resp, expanded_closed_enum), "must reject closed enum expansion"

    # 5. SV-CORE-001 public-header-only boundary check on server/src.
    private_include_re = re.compile(r'#\s*include\s*[<"].*(?:^|/)src/(?:daq|task|pipeline|artifact|plugin|storage|system)/')
    for src_file in list(server_src_dir.rglob("*.hpp")) + list(server_src_dir.rglob("*.cpp")):
        text = src_file.read_text(encoding="utf-8")
        assert not private_include_re.search(text), f"private Core include in {src_file}"
        assert "../src/" not in text and "../../src/" not in text, (
            f"relative private Core include in {src_file}"
        )

    print("OpenAPI 3.1.0 conformance, SV-API-009 compatibility, and SV-CORE-001 boundary checks PASSED.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
