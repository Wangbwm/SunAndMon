#!/usr/bin/env python3
"""Sync the local SunAndMon workspace into a clean Git publishing checkout."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

COMPONENTS = ("foundation-sunshine", "moonlight-qt", "moonlight-vplus")

TOP_LEVEL_FILES = (
    ".gitignore",
    "LICENSE",
    "NOTICE",
    "README.md",
    "SECURITY.md",
)

TOP_LEVEL_DIRS = (
    ".github",
    "center",
    "scripts",
    "tunnel",
)

EXECUTABLE_PATHS = {
    "scripts/build-linux.sh",
    "scripts/linux/virtual-display/sunandmon-virtual-display",
}

IGNORED_SUBTREE_PARTS = {
    ".deployment",
    "__pycache__",
    "build",
    "state",
}

BLOCKED_PATH_RE = re.compile(
    r"(^|/)(\.deployment|state/devices\.json|\.env(?!\.example)|\.build-[^/]+|dist|id_ed25519|id_rsa|local\.properties)(\.|/|$)",
    re.I,
)

SECRET_RE = re.compile(
    rb"-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----"
    rb"|\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,}|AKIA[0-9A-Z]{16})\b"
    rb"|SUNANDMON_CENTER_(?:ADMIN_)?KEY\s*=\s*['\"]?[a-fA-F0-9]{64}\b"
)

ALLOWED_TEST_FIXTURES = {
    "foundation-sunshine/tests/data/log_scrub_vectors.json",
    "foundation-sunshine/tests/unit/test_log_scrub.cpp",
    "foundation-sunshine/tests/unit/test_telemetry.cpp",
    "moonlight-qt/tests/clipboard_payload_routing/fixtures/server-key.pem",
}

PRESERVE_IN_STAGE = {".gitattributes", ".gitmodules"}


def run_git(path: Path, *args: str, data: bytes | None = None) -> bytes:
    cmd = ["git", "-C", str(path), "-c", "core.longpaths=true", "-c", "core.autocrlf=false", *args]
    return subprocess.run(cmd, input=data, check=True, stdout=subprocess.PIPE).stdout


def iter_component_files(comp_root: Path) -> tuple[list[tuple[str, str]], list[tuple[str, str, str]]]:
    """Return (regular_files[(rel, mode)], gitlinks[(mode, oid, rel)])."""
    regular: list[tuple[str, str]] = []
    gitlinks: list[tuple[str, str, str]] = []
    seen: set[str] = set()

    for record in run_git(comp_root, "ls-files", "--stage", "-z").split(b"\0"):
        if not record:
            continue
        meta, raw_name = record.split(b"\t", 1)
        mode, oid, merge_stage = meta.decode().split()
        if merge_stage != "0":
            raise ValueError(f"Unmerged file in {comp_root.name}: {raw_name!r}")
        rel = raw_name.decode()
        if mode == "160000":
            gitlinks.append((mode, oid, rel))
            continue
        if (comp_root / rel).is_file():
            regular.append((rel, mode))
            seen.add(rel)

    untracked = run_git(comp_root, "ls-files", "--others", "--exclude-standard", "-z").decode()
    for rel in untracked.split("\0"):
        if not rel or rel in seen:
            continue
        if BLOCKED_PATH_RE.search(rel):
            continue
        if (comp_root / rel).is_file():
            regular.append((rel, "100644"))
            seen.add(rel)

    return regular, gitlinks


def copy_if_changed(src: Path, dst: Path) -> bool:
    if src.stat().st_size >= 100 * 1024 * 1024:
        raise ValueError(f"File exceeds GitHub 100 MB limit: {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_file() and src.read_bytes() == dst.read_bytes():
        return False
    shutil.copyfile(src, dst)
    return True


def collect_top_level(root: Path) -> list[str]:
    result: list[str] = []
    for rel in TOP_LEVEL_FILES:
        if (root / rel).is_file():
            result.append(rel)
    for folder in TOP_LEVEL_DIRS:
        base = root / folder
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file():
                continue
            rel = p.relative_to(root).as_posix()
            if any(part in IGNORED_SUBTREE_PARTS for part in p.relative_to(root).parts):
                continue
            if BLOCKED_PATH_RE.search(rel):
                continue
            result.append(rel)
    return result


def audit_stage(stage: Path) -> int:
    findings: list[tuple[str, str]] = []
    count = 0
    for record in run_git(stage, "ls-files", "--stage", "-z").split(b"\0"):
        if not record:
            continue
        meta, raw_name = record.split(b"\t", 1)
        mode = meta.decode().split()[0]
        if mode == "160000":
            continue
        rel = raw_name.decode()
        count += 1
        if BLOCKED_PATH_RE.search(rel):
            findings.append(("BLOCKED_PATH", rel))
            continue
        if rel in ALLOWED_TEST_FIXTURES:
            continue
        data = (stage / rel).read_bytes()
        if SECRET_RE.search(data):
            findings.append(("SECRET_MATCH", rel))
    if findings:
        details = "\n".join(f"  - [{kind}] {path}" for kind, path in findings)
        raise ValueError(f"Security audit failed:\n{details}")
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--workspace",
        type=Path,
        default=Path.cwd(),
        help="SunAndMon workspace root (default: current directory)",
    )
    parser.add_argument(
        "--stage",
        type=Path,
        default=Path(".build-linux/github-source"),
        help="Target Git publishing checkout directory",
    )
    parser.add_argument(
        "--stage-changes",
        action="store_true",
        help="Run git add -A and audit the staged index inside --stage",
    )
    args = parser.parse_args()

    root = args.workspace.resolve()
    stage = (args.stage if args.stage.is_absolute() else (root / args.stage)).resolve()
    if not (stage / ".git").exists():
        raise SystemExit(f"Target stage directory is not a Git repository: {stage}")

    expected: set[str] = set(PRESERVE_IN_STAGE)
    executables: set[str] = set(EXECUTABLE_PATHS)
    copied = 0

    for rel in collect_top_level(root):
        expected.add(rel)
        if copy_if_changed(root / rel, stage / rel):
            copied += 1

    upstream_revs: list[tuple[str, str]] = []
    for comp in COMPONENTS:
        comp_root = root / comp
        rev = run_git(comp_root, "rev-parse", "HEAD").decode().strip()
        upstream_revs.append((comp, rev))
        regular, gitlinks = iter_component_files(comp_root)
        for rel, mode in regular:
            target_rel = f"{comp}/{rel}"
            expected.add(target_rel)
            if mode == "100755":
                executables.add(target_rel)
            if copy_if_changed(comp_root / rel, stage / target_rel):
                copied += 1
        for _, _, rel in gitlinks:
            expected.add(f"{comp}/{rel}")

    # Remove tracked files in stage that no longer exist in workspace
    removed = 0
    for record in run_git(stage, "ls-files", "--stage", "-z").split(b"\0"):
        if not record:
            continue
        meta, raw_name = record.split(b"\t", 1)
        mode = meta.decode().split()[0]
        if mode == "160000":
            continue
        rel = raw_name.decode()
        if rel not in expected:
            target = stage / rel
            if target.exists():
                target.unlink()
                removed += 1

    if args.stage_changes:
        run_git(stage, "add", "-A")
        for rel in sorted(executables):
            if (stage / rel).is_file():
                run_git(stage, "update-index", "--chmod=+x", "--", rel)
        audited = audit_stage(stage)
        print(f"Synced {copied} changed files, removed {removed}, audited {audited} tracked files.")
    else:
        print(f"Synced {copied} changed files, removed {removed} (not yet staged).")

    status = run_git(stage, "status", "--short").decode("utf-8", "replace")
    if status.strip():
        print(status.rstrip())
    else:
        print("Working tree clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
