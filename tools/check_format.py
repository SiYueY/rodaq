"""Check repository-owned C/C++ using the pinned formatter (no vendor sources)."""

import pathlib
import subprocess


def main():
    version = subprocess.check_output(["clang-format-14", "--version"], text=True)
    if "version 14." not in version:
        raise SystemExit("clang-format 14.x is required")

    files = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"]
    ).decode().split("\0")
    sources = sorted(
        name for name in files
        if name and not name.startswith("third_party/")
        and not any(part.startswith(".") for part in pathlib.Path(name).parts)
        and pathlib.Path(name).suffix in {".h", ".hpp", ".c", ".cpp"}
    )
    subprocess.run(["clang-format-14", "--dry-run", "--Werror", *sources], check=True)


if __name__ == "__main__":
    main()
