"""Check the installed C++ ELF boundary and prohibit duplicate Plugin runtimes."""
import argparse
import pathlib
import os
import re
import subprocess


def command(*args):
    return subprocess.check_output(args, text=True, env={**os.environ, "LC_ALL": "C"})


def exported(path):
    return {line.split()[0] for line in command("nm", "-D", "--defined-only", "-P", path).splitlines()}


def check_exports(path, allowlist):
    expected = set(re.findall(r"^\s+(_Z\w+);", allowlist.read_text(), re.M))
    actual = exported(path)
    if not expected or actual != expected:
        raise RuntimeError(f"{path.name}: missing {sorted(expected - actual)}, unexpected {sorted(actual - expected)}")
    print(f"{path.name}: exact ELF whitelist {len(actual)} symbols")


def check_runtime(path, link_map, mode):
    needed = set(re.findall(r"\(NEEDED\).*?\[(.*?)\]", command("readelf", "-d", path)))
    plugin_deps = {n for n in needed if n.startswith("librodaq_plugin.so")}
    daq_deps = {n for n in needed if n.startswith("librodaq_daq.so")}
    mapping = link_map.read_text(errors="replace")
    static_plugin = "librodaq_plugin_whitebox.a" in mapping or "rodaq_plugin_obj.dir/" in mapping
    static_daq = "librodaq_daq_whitebox.a" in mapping or "rodaq_daq_obj.dir/" in mapping
    # A pure Plugin whitebox executable cannot also link the shared runtime.
    if mode == "plugin-whitebox":
        if plugin_deps or daq_deps or not static_plugin or static_daq:
            raise RuntimeError(f"{path.name}: invalid pure whitebox linkage")
    elif static_plugin or (static_daq and daq_deps):
        raise RuntimeError(f"{path.name}: duplicate runtime")
    elif mode in ("daq-whitebox", "daq-loading"):
        if not static_daq or daq_deps or "librodaq_plugin.so" not in mapping:
            raise RuntimeError(f"{path.name}: DAQ whitebox must use DAQ archive and shared Plugin target")
        if mode == "daq-loading" and len(plugin_deps) != 1:
            raise RuntimeError(f"{path.name}: plugin-loading DAQ whitebox lacks shared Plugin runtime")
    elif mode == "plugin-shared" and len(plugin_deps) != 1:
        raise RuntimeError(f"{path.name}: missing shared Plugin runtime")
    elif mode == "public-daq" and (static_daq or len(daq_deps) != 1):
        raise RuntimeError(f"{path.name}: public DAQ consumer must use only shared DAQ")
    print(f"{path.name}: {mode} link map and ELF dependency contract PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--plugin", type=pathlib.Path, required=True)
    parser.add_argument("--daq", type=pathlib.Path, required=True)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--runtime", action="append", nargs=3, default=[])
    args = parser.parse_args()
    check_exports(args.plugin, args.source / "cmake/plugin_runtime.exports")
    check_exports(args.daq, args.source / "cmake/daq.exports")
    needed = command("readelf", "-d", args.daq)
    if len(re.findall(r"\(NEEDED\).*?\[librodaq_plugin.so\]", needed)) != 1:
        raise RuntimeError("Shared DAQ must depend on shared Plugin runtime")
    for binary, link_map, mode in args.runtime:
        check_runtime(pathlib.Path(binary), pathlib.Path(link_map), mode)
