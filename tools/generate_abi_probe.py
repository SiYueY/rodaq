"""Generate a C11 layout probe; all layout numbers come from the C compiler."""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess


EXTENSIBLE = {
    "rodaq_config_t": "json",
    "rodaq_application_callbacks_t": "log",
    "rodaq_plugin_descriptor_t": "plugin_version",
    "rodaq_plugin_instance_vtable_t": "destroy",
    "rodaq_plugin_entry_t": "create",
    "rodaq_acquisition_prepare_config_t": "max_callback_concurrency",
    "rodaq_acquisition_interface_vtable_t": "quiesce",
    "rodaq_acquisition_callbacks_t": "report_event",
    "rodaq_acquisition_event_t": "reserved",
    "rodaq_acquisition_descriptor_t": "ros2_subscription",
    "rodaq_ros2_subscription_facts_t": "executor_threads",
    "rodaq_acquisition_record_attempt_t": "reserved",
    "rodaq_storage_prepare_config_t": "max_relative_path_bytes",
    "rodaq_storage_interface_vtable_t": "quiesce",
    "rodaq_storage_batch_result_t": "tail_state",
    "rodaq_storage_descriptor_t": "reserved1",
    "rodaq_segment_spec_t": "staging_directory",
    "rodaq_segment_usage_t": "reserved0",
    "rodaq_segment_seal_result_t": "total_physical_bytes",
}

FROZEN_STRUCTS = {
    "rodaq_acquisition_stream_binding_t", "rodaq_ros2_duration_t",
    "rodaq_ros2_qos_profile_t", "rodaq_source_id_t",
    "rodaq_interface_version_t", "rodaq_interface_query_t",
    "rodaq_interface_reference_t",
    "rodaq_plugin_instance_t", "rodaq_string_t", "rodaq_bytes_t", "rodaq_buffer_t",
    "rodaq_deadline_t", "rodaq_struct_header_t", "rodaq_error_t", "rodaq_storage_record_t",
    "rodaq_path_buffer_t", "rodaq_sealed_file_t",
}


def c_string(value):
    return json.dumps(value, ensure_ascii=True)


def generate(root, output, flags):
    headers = sorted((root / "include/rodaq").glob("*/c/*.h"))
    texts = [re.sub(r"/\*.*?\*/", "", path.read_text(), flags=re.S) for path in headers]
    source = "\n".join(texts)
    records = list(re.finditer(r"typedef struct \w+\s*\{(.*?)\}\s*(rodaq_\w+_t);", source, re.S))
    names = {record[2] for record in records}
    classified = set(EXTENSIBLE) | FROZEN_STRUCTS
    if names != classified:
        raise ValueError(f"Unclassified/missing structs: {names ^ classified}")
    for record in records:
        for extensible in EXTENSIBLE:
            if re.search(rf"\b{extensible}\s+\w+", record[1]):
                raise ValueError(f"Extensible type embedded by value: {extensible} in {record[2]}")

    lines = ["#include <stdio.h>", "#include <stddef.h>", "#include <inttypes.h>"]
    lines += [f"#include <{path.relative_to(root / 'include')}>" for path in headers]
    lines += ["int main(void) {"]

    def literal(text):
        lines.append(f"    fputs({c_string(text)}, stdout);")

    def number(expression):
        lines.append(f'    printf("%zu", (size_t)({expression}));')

    commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    metadata = {
        "format_version": 1,
        "abi": {"major": 1, "minor": 0},
        "source_commit": commit,
        "source_dirty": bool(subprocess.check_output(
            ["git", "-C", str(root), "status", "--porcelain"], text=True)),
        "compiler_flags": flags,
        "headers": {
            str(path.relative_to(root / "include")): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in headers
        },
        "symbols": ["rodaq_plugin_abi_version", "rodaq_plugin_get_entry"],
    }
    literal(json.dumps(metadata)[:-1] + ', "compiler": "')
    lines.append('    fputs(__VERSION__, stdout);')
    literal('", "architecture": "')
    lines += [
        "#if defined(__x86_64__)", '    fputs("amd64", stdout);',
        "#elif defined(__aarch64__)", '    fputs("arm64", stdout);',
        "#else", '#error "ABI architecture not qualified"', "#endif",
    ]
    literal('", "types": {')
    separator = ""
    for record in records:
        body, name = record[1], record[2]
        literal(separator + json.dumps(name) + ': {"class": ' + json.dumps(
            "EXTENSIBLE_STRUCT" if name in EXTENSIBLE else "FROZEN_VALUE") + ', "sizeof": ')
        number(f"sizeof({name})")
        literal(', "alignof": ')
        number(f"_Alignof({name})")
        if name in EXTENSIBLE:
            field = EXTENSIBLE[name]
            literal(', "required_prefix_size": ')
            number(f"offsetof({name}, {field}) + sizeof((({name}*)0)->{field})")
        literal(', "fields": {')
        field_separator = ""
        for declaration in body.split(";"):
            declaration = declaration.strip()
            if not declaration:
                continue
            slot = re.search(r"\(\*(\w+)\)", declaration)
            ordinary = re.search(r"(\w+)\s*(?:\[[^\]]+\])?\s*$", declaration)
            if not slot and not ordinary:
                raise ValueError(f"Unrecognized field: {declaration}")
            field = (slot or ordinary)[1]
            literal(field_separator + json.dumps(field) + ': {"offset": ')
            number(f"offsetof({name}, {field})")
            literal(', "sizeof": ')
            number(f"sizeof((({name}*)0)->{field})")
            if slot:
                # Registry optional slots predate this initial 1.0 candidate.
                literal(', "introduced_minor": 0, "required": ' + (
                    "false" if name == "rodaq_service_registry_vtable_t" and field != "query"
                    else "true"))
                literal(', "signature": ' + json.dumps(" ".join(declaration.split())))
            literal("}")
            field_separator = ","
        literal("}}")
        separator = ","

    for scalar in re.finditer(r"typedef\s+(u?int\d+_t)\s+(rodaq_\w+_t);", source):
        name = scalar[2]
        literal(separator + json.dumps(name) + ': {"class": "FROZEN_VALUE", "sizeof": ')
        number(f"sizeof({name})")
        literal(', "alignof": ')
        number(f"_Alignof({name})")
        literal("}")
        separator = ","
    for name in sorted(set(re.findall(r"typedef struct \w+\s+(rodaq_\w+_t);", source))):
        literal(separator + json.dumps(name) + ': {"class": "OPAQUE_HANDLE"}')
        separator = ","
    literal('}, "constants": {')
    separator = ""
    constants = re.findall(r"\b(RODAQ_\w+)\s*=", source)
    constants += re.findall(r"^#define\s+(RODAQ_\w+)\s+\d+u?\s*$", source, re.M)
    constants += ["RODAQ_PLUGIN_ABI_VERSION", "RODAQ_DEADLINE_INFINITE"]
    for name in sorted(set(constants)):
        literal(separator + json.dumps(name) + ": ")
        number(name)
        separator = ","
    for name, value in re.findall(r'^#define\s+(RODAQ_\w+)\s+"([^"\n]*)"', source, re.M):
        literal(separator + json.dumps(name) + ": " + json.dumps(value))
        separator = ","
    literal('}}\n')
    lines += ["    return ferror(stdout) ? 1 : 0;", "}"]
    output.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--flags", default="")
    args = parser.parse_args()
    generate(args.root, args.output, args.flags)
