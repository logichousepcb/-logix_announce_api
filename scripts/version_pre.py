Import("env")

from pathlib import Path


ROOT = Path(env["PROJECT_DIR"])
VERSION_FILE = ROOT / "version.txt"
HEADER_FILE = ROOT / "include" / "build_version.h"


def parse_version(value: str):
    value = value.strip()
    if "." not in value:
        return None

    major_str, minor_str = value.split(".", 1)
    if not major_str.isdigit() or not minor_str.isdigit():
        return None

    return int(major_str), int(minor_str), len(minor_str)


def ensure_current_version() -> str:
    if not VERSION_FILE.exists():
        VERSION_FILE.write_text("1.001\n", encoding="ascii")

    current = VERSION_FILE.read_text(encoding="ascii", errors="ignore").strip()
    if parse_version(current) is None:
        current = "1.001"
        VERSION_FILE.write_text(current + "\n", encoding="ascii")

    return current


def write_header(version_text: str):
    HEADER_FILE.write_text(
        "#pragma once\n" + f"#define BUILD_VERSION \"{version_text}\"\n",
        encoding="ascii",
    )


current_version = ensure_current_version()
write_header(current_version)
print(f"[version] Using build version {current_version}")
