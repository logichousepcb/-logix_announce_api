Import("env")

from pathlib import Path
import shutil


ROOT = Path(env["PROJECT_DIR"])
VERSION_FILE = ROOT / "version.txt"
HEADER_FILE = ROOT / "include" / "build_version.h"
RELEASES_DIR = ROOT / "releases"


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


def export_release_binary(version_text: str, target):
    release_source = Path(str(target[0]))
    RELEASES_DIR.mkdir(parents=True, exist_ok=True)
    release_name = f"logix_announce_api-{version_text}.bin"
    release_target = RELEASES_DIR / release_name
    shutil.copyfile(release_source, release_target)
    print(f"[release] Exported {release_target.relative_to(ROOT)}")


def on_build_success(target, source, env):
    current = ensure_current_version()
    parsed = parse_version(current)
    if parsed is None:
        return

    export_release_binary(current, target)

    major, minor, width = parsed
    next_version = format_version(major, minor + 1, width)
    VERSION_FILE.write_text(next_version + "\n", encoding="ascii")
    write_header(next_version)
    print(f"[version] Next build version set to {next_version}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", on_build_success)
