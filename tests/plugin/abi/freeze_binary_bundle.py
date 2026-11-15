"""Freeze already-built ABI fixtures. This tool never configures or compiles code."""

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def freeze(args):
    # Refuse overwrite: a baseline identity must never silently acquire new bytes.
    args.output.mkdir(parents=True, exist_ok=False)
    metadata = {"format": 1, "identity": args.identity, "profiles": {}}
    architecture = None
    for argument in args.profile:
        name, value = argument.split("=", 1)
        if name not in ("gcc11", "clang14") or name in metadata["profiles"]:
            raise ValueError("Expected one gcc11 and one clang14 profile")
        build = pathlib.Path(value).resolve()
        destination = args.output / name
        destination.mkdir()
        layout = json.loads(subprocess.check_output([build / "tests/rodaq_abi_probe"], text=True))
        if architecture is not None and architecture != layout["architecture"]:
            raise ValueError("A bundle must contain a single architecture")
        architecture = layout["architecture"]
        (destination / "layout.json").write_text(json.dumps(layout, indent=2) + "\n")
        for family in ("counter", "domain"):
            matrix = build / (family + "-matrix")
            # Preserve the actual copied fixture sources and their build flags,
            # which can predate the Runtime's layout-probe source commit.
            for source in matrix.glob("*-source"):
                shutil.copytree(source, destination / "qualification-source" / family / source.name)
            for fixture in matrix.glob("*-build"):
                provenance = destination / "qualification-build" / family / fixture.name
                provenance.mkdir(parents=True)
                shutil.copyfile(fixture / "CMakeCache.txt", provenance / "CMakeCache.txt")
                for flag in fixture.glob("CMakeFiles/*.dir/*.txt"):
                    target = provenance / flag.parent.name / flag.name
                    target.parent.mkdir(exist_ok=True)
                    shutil.copyfile(flag, target)
                for flag in fixture.glob("CMakeFiles/*.dir/flags.make"):
                    target = provenance / flag.parent.name / flag.name
                    target.parent.mkdir(exist_ok=True)
                    shutil.copyfile(flag, target)
            inventory = json.loads((matrix / "binary-sha256.json").read_text())
            for relative, expected in inventory.items():
                source = matrix / relative
                if digest(source) != expected:
                    raise ValueError(f"Fixture changed after validation: {source}")
                target = destination / family / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
                target.chmod(0o555)
                if "-installed/" in relative:
                    package = source.parent.parent
                    shutil.copyfile(package / "manifest.json", target.parent.parent / "manifest.json")
        runtime = build / "runtime-consumer"
        shutil.copyfile(runtime / "build/runtime_consumer", destination / "runtime_consumer")
        (destination / "runtime_consumer").chmod(0o555)
        (destination / "lib").mkdir()
        for library in (runtime / "sdk/lib").glob("*.so*"):
            shutil.copyfile(library, destination / "lib" / library.name)
        shutil.copytree(runtime / "sdk/include", destination / "headers")
        fixture_headers = destination / "headers/fixture"
        fixture_headers.mkdir()
        for header in (build / "counter-matrix/provider-0-source").glob("*.h*"):
            shutil.copyfile(header, fixture_headers / header.name)
        shutil.copyfile(build / "domain-matrix/acquisition-provider-0-source/domain_fixture.h",
                        fixture_headers / "domain_fixture.h")
        # Preserve compiler invocation and concrete platform/runtime provenance.
        cache = (build / "CMakeCache.txt").read_text()
        (destination / "CMakeCache.txt").write_text(cache)
        runtime_info = subprocess.check_output(["ldd", "--version"], stderr=subprocess.STDOUT, text=True)
        (destination / "platform.txt").write_text(
            subprocess.check_output(["uname", "-a"], text=True) + runtime_info)
        metadata["profiles"][name] = {"compiler": layout["compiler"],
                                      "source_commit": layout["source_commit"],
                                      "source_dirty": layout["source_dirty"]}
    if set(metadata["profiles"]) != {"gcc11", "clang14"}:
        raise ValueError("Both compiler profiles are required")
    metadata["architecture"] = architecture
    metadata["sha256"] = {str(path.relative_to(args.output)): digest(path)
                          for path in sorted(args.output.rglob("*")) if path.is_file()}
    (args.output / "bundle.json").write_text(json.dumps(metadata, indent=2) + "\n")
    for path in args.output.rglob("*"):
        path.chmod(0o555 if path.is_dir() or path.stat().st_mode & 0o111 else 0o444)
    args.output.chmod(0o555)
    print(f"Frozen {args.identity}: {architecture}, {len(metadata['sha256'])} immutable files")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--profile", action="append", required=True)
    freeze(parser.parse_args())
