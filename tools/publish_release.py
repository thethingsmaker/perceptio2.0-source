#!/usr/bin/env python3
"""Publish the current firmware version to GitHub. This is the only procedure.

  python tools/publish_release.py "why this version"

Reads FW_VERSION_STR from include/config.h (already bumped). Then, in order:

  1. Drop sdkconfig.perceptio2 when sdkconfig.defaults is newer, and build.
  2. Confirm firmware.bin contains that version string.
  3. Commit the source tree, if it has changes, and push perceptio2.0-source.
  4. Commit firmware.bin, bootloader.bin, partitions.bin, ota_data_initial.bin
     and manifest.json to perceptio2.0-firmware, push, tag v<version>.
  5. Create the GitHub Release and upload firmware.bin (the only .bin asset;
     OTA uses the first asset whose name ends in .bin) and firmware-<version>.elf
     (needed to decode a core dump of that build: esp-coredump checks the app
     SHA256, a rebuilt ELF will not do). The ELF is also kept locally in
     res/elf/ (git-ignored).

Git credentials do the push and the release. res/pat.txt is Contents:read only
and cannot create a release. Do not pass target_commitish: a short SHA is a 422.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_URL = "https://github.com/thethingsmaker/perceptio2.0-firmware.git"
FIRMWARE_SLUG = "thethingsmaker/perceptio2.0-firmware"
BIN_NAMES = ("firmware.bin", "bootloader.bin", "partitions.bin", "ota_data_initial.bin")
SECRET_NAMES = ("pat.txt", "p2_token.txt", "cloud_secrets.h", "wifi_defaults.h", ".env")

VERSION_RE = re.compile(
    r"#define\s+FW_VERSION_MAJOR\s+(\d+)\s+"
    r"#define\s+FW_VERSION_MINOR\s+(\d+)\s+"
    r"#define\s+FW_VERSION_PATCH\s+(\d+)\s+"
    r'#define\s+FW_VERSION_STR\s+"([^"]+)"'
)


def run(cmd: list[str], cwd: Path | None = None, check: bool = True, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=cwd or ROOT,
        input=input_text,
        text=True,
        capture_output=True,
        check=check,
    )


def echo(proc: subprocess.CompletedProcess[str]) -> None:
    if proc.stdout:
        print(proc.stdout, end="" if proc.stdout.endswith("\n") else "\n")
    if proc.stderr:
        print(proc.stderr, end="" if proc.stderr.endswith("\n") else "\n", file=sys.stderr)


def git(args: list[str], cwd: Path | None = None) -> str:
    proc = run(["git", *args], cwd=cwd)
    echo(proc)
    return proc.stdout.strip()


def read_version() -> str:
    text = (ROOT / "include" / "config.h").read_text(encoding="utf-8")
    match = VERSION_RE.search(text)
    if not match:
        sys.exit("include/config.h has no FW_VERSION_* block")
    major, minor, patch, string = match.groups()
    composed = f"{major}.{minor}.{patch}"
    if composed != string:
        sys.exit(f"FW_VERSION_STR {string!r} does not match {composed}")
    return string


def sentence(summary: str) -> str:
    text = " ".join(summary.split())
    if not text:
        sys.exit("summary is empty")
    if text[-1] not in ".!?":
        text += "."
    return text


def pio_exe() -> Path:
    candidate = Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe"
    if candidate.is_file():
        return candidate
    found = shutil.which("pio")
    if found:
        return Path(found)
    sys.exit(f"PlatformIO not found at {candidate}")


def prepare_sdkconfig() -> None:
    sdk = ROOT / "sdkconfig.perceptio2"
    defaults = ROOT / "sdkconfig.defaults"
    if sdk.exists() and sdk.stat().st_mtime >= defaults.stat().st_mtime:
        return
    if sdk.exists():
        sdk.unlink()
        print("removed sdkconfig.perceptio2 (older than sdkconfig.defaults)")


def build() -> Path:
    prepare_sdkconfig()
    print("build")
    proc = subprocess.run([str(pio_exe()), "run"], cwd=ROOT)
    if proc.returncode != 0:
        sys.exit(proc.returncode)
    image = ROOT / ".pio" / "build" / "perceptio2" / "firmware.bin"
    if not image.is_file():
        sys.exit(f"missing {image}")
    return image


def archive_elf(version: str) -> Path:
    """Keep this build's ELF next to the sources: res/elf/firmware-<version>.elf (git-ignored)."""
    src = ROOT / ".pio" / "build" / "perceptio2" / "firmware.elf"
    if not src.is_file():
        sys.exit(f"missing {src}")
    dest = ROOT / "res" / "elf" / f"firmware-{version}.elf"
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)
    print(f"elf archived: {dest.relative_to(ROOT)}")
    return dest


def assert_image_version(image: Path, version: str) -> None:
    if version.encode("ascii") not in image.read_bytes():
        sys.exit(f"{image} does not contain {version}")


def porcelain() -> list[str]:
    out = run(["git", "status", "--porcelain"]).stdout
    return [line for line in out.splitlines() if line.strip()]


def assert_no_secrets(paths: list[str]) -> None:
    bad = [p for p in paths if any(name in Path(p).name for name in SECRET_NAMES)]
    if bad:
        sys.exit("refusing to commit secrets: " + ", ".join(bad))


def commit_source(version: str, summary: str) -> None:
    lines = porcelain()
    if not lines:
        print("source: clean, nothing to commit")
        return
    assert_no_secrets([line[3:] for line in lines])
    git(["add", "-A"])
    staged = run(["git", "diff", "--cached", "--name-only"]).stdout.splitlines()
    assert_no_secrets(staged)
    if not staged:
        print("source: clean, nothing to commit")
        return
    message = f"Bump to {version}: {summary}"
    print(f"source commit: {message}")
    git(["commit", "-m", message])


def push_source() -> str:
    print("source push")
    git(["push", "origin", "HEAD"])
    return git(["rev-parse", "--short=7", "HEAD"])


def tag_sha(version: str) -> str | None:
    proc = run(["git", "ls-remote", "--tags", FIRMWARE_URL, f"refs/tags/v{version}"], check=False)
    echo(proc)
    if proc.returncode != 0:
        sys.exit(proc.returncode)
    line = proc.stdout.strip()
    return line.split()[0] if line else None


def github_token() -> str:
    proc = run(
        ["git", "credential", "fill"],
        input_text="protocol=https\nhost=github.com\n\n",
        check=False,
    )
    if proc.returncode != 0:
        sys.exit("git credential fill failed for github.com")
    for line in proc.stdout.splitlines():
        if line.startswith("password="):
            token = line.split("=", 1)[1].strip()
            if token:
                return token
    sys.exit("git credential fill returned no GitHub password")


def api(method: str, url: str, token: str, payload: bytes | None = None, content_type: str | None = "application/json") -> tuple[int, object]:
    req = urllib.request.Request(url, data=payload, method=method)
    req.add_header("Authorization", "Bearer " + token)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "perceptio-publish")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    if content_type and payload is not None:
        req.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(req) as resp:
            raw = resp.read()
            return resp.status, json.loads(raw.decode()) if raw else {}
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        try:
            parsed: object = json.loads(detail)
        except json.JSONDecodeError:
            parsed = detail
        return exc.code, parsed


def release_url(version: str) -> str:
    return f"https://api.github.com/repos/{FIRMWARE_SLUG}/releases/tags/v{version}"


def release_record(token: str, version: str) -> dict | None:
    status, body = api("GET", release_url(version), token, payload=None, content_type=None)
    if status == 404:
        return None
    if status != 200 or not isinstance(body, dict):
        sys.exit(f"GitHub release lookup failed ({status}): {body}")
    return body


def asset_ok(record: dict, size: int, name: str = "firmware.bin") -> bool:
    for asset in record.get("assets") or []:
        if asset.get("name") == name and asset.get("size") == size and asset.get("state") == "uploaded":
            return True
    return False


def create_release(token: str, version: str, summary: str, source: str) -> dict:
    body = {"tag_name": f"v{version}", "name": f"v{version}", "body": f"{summary}\n\nSource {source}.\n", "draft": False, "prerelease": False}
    status, record = api(
        "POST",
        f"https://api.github.com/repos/{FIRMWARE_SLUG}/releases",
        token,
        json.dumps(body).encode("utf-8"),
    )
    if status == 422:
        # Tag already has a release, or the POST raced a previous attempt.
        existing = release_record(token, version)
        if existing:
            return existing
    if status not in (200, 201) or not isinstance(record, dict):
        sys.exit(f"creating the release failed ({status}): {record}")
    return record


def upload_asset(token: str, release_id: int, path: Path, name: str) -> None:
    url = f"https://uploads.github.com/repos/{FIRMWARE_SLUG}/releases/{release_id}/assets?name={name}"
    status, record = api("POST", url, token, path.read_bytes(), "application/octet-stream")
    if status not in (200, 201) or not isinstance(record, dict):
        sys.exit(f"uploading {name} failed ({status}): {record}")
    print(f"asset {name} {record.get('size')} bytes")


def publish_release(version: str, summary: str, source: str, image: Path, elf: Path, token: str) -> None:
    elf_name = f"firmware-{version}.elf"
    record = release_record(token, version)
    if record and asset_ok(record, image.stat().st_size) and asset_ok(record, elf.stat().st_size, elf_name):
        print(record.get("html_url") or f"v{version} already published")
        return
    if record is None:
        print(f"release v{version}")
        record = create_release(token, version, summary, source)
    if not asset_ok(record, image.stat().st_size):
        upload_asset(token, int(record["id"]), image, "firmware.bin")
    if not asset_ok(record, elf.stat().st_size, elf_name):
        upload_asset(token, int(record["id"]), elf, elf_name)
    print(record.get("html_url") or release_url(version))


def firmware_checkout(cache: Path) -> None:
    if (cache / ".git").exists() and porcelain_at(cache):
        shutil.rmtree(cache)
    if (cache / ".git").exists():
        proc = run(["git", "pull", "--ff-only", "origin", "main"], cwd=cache, check=False)
        echo(proc)
        if proc.returncode != 0:
            shutil.rmtree(cache)
    if not (cache / ".git").exists():
        cache.parent.mkdir(parents=True, exist_ok=True)
        git(["clone", "--depth", "1", FIRMWARE_URL, str(cache)], cwd=ROOT)


def porcelain_at(cwd: Path) -> list[str]:
    out = run(["git", "status", "--porcelain"], cwd=cwd).stdout
    return [line for line in out.splitlines() if line.strip()]


def file_info(path: Path) -> tuple[str, int]:
    data = path.read_bytes()
    return hashlib.sha256(data).hexdigest(), len(data)


def write_manifest(dest: Path, version: str, source: str, built: Path) -> None:
    built_at = datetime.fromtimestamp(built.stat().st_mtime, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    entries = []
    for name in BIN_NAMES:
        digest, size = file_info(dest / name)
        entries.append(
            "                  {\n"
            f'                      "sha256":  "{digest}",\n'
            f'                      "name":  "{name}",\n'
            f'                      "size":  {size}\n'
            "                  }"
        )
    text = (
        "{\n"
        f'    "version":  "{version}",\n'
        f'    "built_at":  "{built_at}",\n'
        '    "files":  [\n'
        + ",\n".join(entries)
        + "\n              ],\n"
        '    "hw_rev":  20,\n'
        '    "source_repo":  "https://github.com/thethingsmaker/perceptio2.0-source",\n'
        f'    "source_commit":  "{source}",\n'
        '    "target":  "esp32-s3"\n'
        "}\n"
    )
    dest.joinpath("manifest.json").write_text(text, encoding="utf-8", newline="\n")


def publish_firmware_repo(version: str, source: str, token: str, summary: str, elf: Path) -> None:
    cache = ROOT / ".cache" / "perceptio2.0-firmware"
    print("firmware repo")
    firmware_checkout(cache)
    build_dir = ROOT / ".pio" / "build" / "perceptio2"
    for name in BIN_NAMES:
        shutil.copyfile(build_dir / name, cache / name)
    write_manifest(cache, version, source, build_dir / "firmware.bin")
    if porcelain_at(cache):
        git(["add", *BIN_NAMES, "manifest.json"], cwd=cache)
        git(["commit", "-m", f"Release firmware {version}."], cwd=cache)
        git(["push", "origin", "HEAD"], cwd=cache)
    else:
        print("firmware repo: binaries already match")
    if tag_sha(version) is None:
        git(["tag", f"v{version}"], cwd=cache)
        git(["push", "origin", f"v{version}"], cwd=cache)
    publish_release(version, summary, source, build_dir / "firmware.bin", elf, token)


def already_published(version: str, token: str) -> bool:
    if tag_sha(version) is None:
        return False
    if porcelain():
        sys.exit(f"v{version} is already tagged. Bump FW_VERSION_STR before publishing these changes.")
    image = ROOT / ".pio" / "build" / "perceptio2" / "firmware.bin"
    record = release_record(token, version)
    if record and image.is_file() and asset_ok(record, image.stat().st_size):
        print(record.get("html_url") or f"v{version} already published")
        return True
    if record and any(a.get("name") == "firmware.bin" for a in record.get("assets") or []):
        print(record.get("html_url") or f"v{version} already published")
        return True
    return False


def main() -> None:
    parser = argparse.ArgumentParser(description="Publish include/config.h FW_VERSION_STR to GitHub (source and firmware).")
    parser.add_argument("summary", help="One line: why this version. Source commit subject and release text.")
    args = parser.parse_args()
    summary = sentence(args.summary)
    version = read_version()
    print(f"version {version}")
    token = github_token()
    if already_published(version, token):
        return
    image = build()
    assert_image_version(image, version)
    elf = archive_elf(version)
    commit_source(version, summary)
    source = push_source()
    publish_firmware_repo(version, source, token, summary, elf)


if __name__ == "__main__":
    main()
