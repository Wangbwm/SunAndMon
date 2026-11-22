#!/usr/bin/env python3
"""Build the SunAndMon Windows Inno Setup installer (.exe) from SunAndMon-Windows-x64.zip."""

import argparse
import os
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path


def find_iscc(repo_root: Path, explicit: str | None) -> Path:
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    env_iscc = os.environ.get("ISCC_EXE")
    if env_iscc:
        candidates.append(Path(env_iscc))
    candidates.extend([
        repo_root / ".build-win" / "innosetup" / "ISCC.exe",
        Path(r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"),
        Path(r"C:\Program Files\Inno Setup 6\ISCC.exe"),
    ])
    which = shutil.which("ISCC") or shutil.which("ISCC.exe")
    if which:
        candidates.append(Path(which))

    for c in candidates:
        if c.is_file():
            return c
    raise FileNotFoundError(
        "Inno Setup 6 compiler (ISCC.exe) not found. Install Inno Setup 6 or pass --iscc."
    )


def extract_zip(zip_path: Path, stage_root: Path) -> Path:
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(stage_root)
    payload_dir = stage_root / "SunAndMon"
    if not payload_dir.is_dir():
        raise ValueError(f"Expected SunAndMon/ root directory inside {zip_path}")
    required = [
        "SunAndMon.exe",
        "host/sunshine.exe",
        "host/tools/sunshinesvc.exe",
        "tunnel/sunandmon-tunnel.exe",
        "tunnel/easytier-core.exe",
    ]
    for rel in required:
        if not (payload_dir / rel).is_file():
            raise ValueError(f"Missing required payload file in {zip_path}: {rel}")
    return payload_dir


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zip", dest="zip_path", type=Path, default=Path("dist/SunAndMon-Windows-x64.zip"))
    parser.add_argument("--version", default="1.0.1")
    parser.add_argument("--output-dir", type=Path, default=Path("dist"))
    parser.add_argument("--iscc", default=None, help="Explicit path to ISCC.exe")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    zip_path = args.zip_path if args.zip_path.is_absolute() else (repo_root / args.zip_path)
    out_dir = args.output_dir if args.output_dir.is_absolute() else (repo_root / args.output_dir)
    iss_path = repo_root / "scripts" / "packaging" / "windows" / "SunAndMon.iss"
    icon_path = repo_root / "moonlight-qt" / "app" / "sunandmon.ico"

    if not zip_path.is_file():
        raise FileNotFoundError(f"Input ZIP not found: {zip_path}")
    if not iss_path.is_file():
        raise FileNotFoundError(f"Inno Setup script not found: {iss_path}")
    if not icon_path.is_file():
        raise FileNotFoundError(f"Icon file not found: {icon_path}")

    iscc = find_iscc(repo_root, args.iscc)
    out_dir.mkdir(parents=True, exist_ok=True)
    base_name = f"SunAndMon-Setup-{args.version}-x64"

    # Check if ChineseSimplified.isl is available alongside ISCC.exe
    zh_isl = iscc.parent / "Languages" / "ChineseSimplified.isl"
    has_zh = zh_isl.is_file()

    with tempfile.TemporaryDirectory(prefix="sunandmon-installer-stage-") as tmp:
        payload_dir = extract_zip(zip_path, Path(tmp))
        cmd = [
            str(iscc),
            f"/DAppVersion={args.version}",
            f"/DSourceDir={payload_dir}",
            f"/DIconFile={icon_path}",
            f"/DOutputDir={out_dir}",
            f"/DOutputBaseFilename={base_name}",
        ]
        if has_zh:
            cmd.append("/DHasChineseSimplified=1")
        cmd.append(str(iss_path))
        print("Running:", " ".join(cmd))
        proc = subprocess.run(cmd, check=False)
        if proc.returncode != 0:
            raise SystemExit(proc.returncode)

    out_exe = out_dir / f"{base_name}.exe"
    if not out_exe.is_file():
        raise FileNotFoundError(f"Expected output installer not created: {out_exe}")
    print(f"Built installer: {out_exe} ({out_exe.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
