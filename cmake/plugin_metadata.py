"""Build-time metadata generation shared by in-tree and installed plugin tooling."""

import argparse
import json
import pathlib
import re


SEMVER = re.compile(
    r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
)


def validate(plugin_id, name, version):
    if not re.fullmatch(r"[a-z0-9._-]{1,255}", plugin_id):
        raise ValueError("Plugin ID must be 1..255 canonical ASCII bytes")
    if not 1 <= len(name.encode("utf-8")) <= 255 or "\0" in name:
        raise ValueError("Plugin name must be 1..255 UTF-8 bytes without NUL")
    match = SEMVER.fullmatch(version)
    if not match or len(version) > 128:
        raise ValueError("Plugin version must be strict SemVer 2.0.0 (1..128 bytes)")
    if match[4] and any(part.isdigit() and len(part) > 1 and part[0] == "0"
                        for part in match[4].split(".")):
        raise ValueError("Numeric prerelease identifiers cannot have leading zeros")


def c_bytes(text):
    # Octal byte literals are independent of execution charset and cannot swallow
    # following hexadecimal digits as C hex escapes can.
    return '"' + "".join(f"\\{byte:03o}" for byte in text.encode("utf-8")) + '"'


def configure(args):
    validate(args.id, args.name, args.version)
    plugin = {"id": args.id, "name": args.name, "version": args.version,
              "abi": {"major": 1, "minor": 0, "revision": 1}}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "metadata.json").write_text(json.dumps({
        "format_version": 1, "plugin": plugin,
        "platform": {"os": "linux", "arch": args.arch},
    }) + "\n")
    fields = ",\n".join(
        f"    {{{c_bytes(plugin[key])}, {len(plugin[key].encode('utf-8'))}u}}"
        for key in ("id", "name", "version")
    )
    (args.output / "descriptor.c").write_text('''#include <rodaq/c/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private bridge provided by C author or RODAQ_DEFINE_PLUGIN, never exported. */
extern rodaq_status_code_t rodaq_plugin_create_instance(rodaq_plugin_instance_t*, rodaq_error_t*);

static const rodaq_plugin_descriptor_t descriptor = {
    {sizeof(rodaq_plugin_descriptor_t)},
''' + fields + ''',
    RODAQ_PLUGIN_ABI_REVISION
};

static const rodaq_plugin_descriptor_t* get_descriptor(void) {
    return &descriptor;
}

static const rodaq_plugin_entry_t entry = {
    {sizeof(rodaq_plugin_entry_t)}, get_descriptor, rodaq_plugin_create_instance
};

uint32_t rodaq_plugin_abi_version(void) {
    return RODAQ_PLUGIN_ABI_VERSION;
}

const rodaq_plugin_entry_t* rodaq_plugin_get_entry(void) {
    return &entry;
}

#ifdef __cplusplus
}
#endif
''')


def manifest(args):
    data = json.loads(args.metadata.read_text())
    data["library"] = "lib/" + args.artifact.name
    args.output.write_text(json.dumps(data, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    config = commands.add_parser("configure")
    config.add_argument("--id", required=True)
    config.add_argument("--name", required=True)
    config.add_argument("--version", required=True)
    config.add_argument("--arch", choices=["amd64", "arm64"], required=True)
    config.add_argument("--output", type=pathlib.Path, required=True)
    make_manifest = commands.add_parser("manifest")
    make_manifest.add_argument("--metadata", type=pathlib.Path, required=True)
    make_manifest.add_argument("--artifact", type=pathlib.Path, required=True)
    make_manifest.add_argument("--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    if arguments.command == "configure":
        configure(arguments)
    else:
        manifest(arguments)
