"""Combine the two modified Windows portable builds into one SunAndMon ZIP."""

import argparse
import shutil
import zipfile
from pathlib import Path, PurePosixPath


def add_portable(output, written, archive_path, target, executable, marker):
    with zipfile.ZipFile(archive_path) as archive:
        files = {}
        for member in archive.infolist():
            if member.is_dir():
                continue
            source = PurePosixPath(member.filename.replace("\\", "/"))
            if source.is_absolute() or any(part in ("..", "") or ":" in part for part in source.parts):
                raise ValueError(f"Unsafe ZIP path: {member.filename}")
            if source in files:
                raise ValueError(f"Duplicate ZIP path: {source}")
            files[source] = member

        launchers = [path for path in files if path.name.lower() == executable.lower()]
        if len(launchers) != 1:
            raise ValueError(f"{archive_path} must contain one {executable}")
        launcher = launchers[0]
        binary = archive.read(files[launcher])
        if not binary.startswith(b"MZ") or (marker not in binary and marker.decode().encode("utf-16le") not in binary):
            raise ValueError(f"{executable} was not built from the modified source")
        prefix = launcher.parent
        for source, member in files.items():
            if not source.is_relative_to(prefix):
                continue
            rel = source.relative_to(prefix)
            if target == PurePosixPath("SunAndMon") and rel == PurePosixPath("portable.dat"):
                # Default to storing Windows user config under %APPDATA%\SunAndMon
                # so replacing or deleting the extracted app folder preserves settings.
                rel = PurePosixPath("portable.dat.inactive")
            path = (target / rel).as_posix()
            if path.casefold() in written:
                raise ValueError(f"Duplicate ZIP path: {path}")
            written.add(path.casefold())
            with archive.open(member) as reader, output.open(path, "w", force_zip64=True) as writer:
                shutil.copyfileobj(reader, writer)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("controller", type=Path, help="modified Moonlight Qt x64 portable ZIP")
    parser.add_argument("host", type=Path, help="modified Foundation Sunshine x64 portable ZIP")
    parser.add_argument("output", type=Path, help="output ZIP path")
    args = parser.parse_args()

    root = PurePosixPath("SunAndMon")
    try:
        with args.output.open("xb") as destination:
            try:
                with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as output:
                    written = set()
                    add_portable(output, written, args.controller, root, "SunAndMon.exe", b"sunandmon-pair")
                    add_portable(output, written, args.host, root / "host", "sunshine.exe", b"sunandmon-status")
            except (OSError, ValueError, zipfile.BadZipFile):
                destination.close()
                args.output.unlink()
                raise
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
