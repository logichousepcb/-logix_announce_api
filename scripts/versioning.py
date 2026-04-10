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


def format_version(major: int, minor: int, width: int) -> str:
    return f"{major}.{minor:0{width}d}"


def ensure_version_file() -> str:
    if not VERSION_FILE.exists():
        VERSION_FILE.write_text("1.001\n", encoding="ascii")

    content = VERSION_FILE.read_text(encoding="ascii", errors="ignore").strip()
    parsed = parse_version(content)
    if parsed is None:
        content = "1.001"
        VERSION_FILE.write_text(content + "\n", encoding="ascii")
    return content


def write_header(version_text: str):
    header = (
        "#pragma once\n"
        f"#define BUILD_VERSION \"{version_text}\"\n"
    )
    HEADER_FILE.write_text(header, encoding="ascii")


def on_pre_build(source, target, current_env):
    current_version = ensure_version_file()
    write_header(current_version)
    print(f"[version] Using build version {current_version}")


def on_post_build(source, target, current_env):
    current_version = ensure_version_file()
    parsed = parse_version(current_version)
    if parsed is None:
        return

    major, minor, width = parsed
    minor += 1
    next_version = format_version(major, minor, width)

    VERSION_FILE.write_text(next_version + "\n", encoding="ascii")
    write_header(next_version)
    print(f"[version] Next build version set to {next_version}")


elf_target = "$BUILD_DIR/${PROGNAME}.elf"

env.AddPreAction("buildprog", on_pre_build)
env.AddPostAction("buildprog", on_post_build)
env.AddPreAction(elf_target, on_pre_build)
env.AddPostAction(elf_target, on_post_build)
