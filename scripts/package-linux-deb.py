#!/usr/bin/env python3
"""Package the modified Linux Moonlight Qt controller and Foundation Sunshine host into one Debian/Ubuntu .deb."""

import argparse
import os
import shutil
import stat
import subprocess
import tempfile
import zipfile
from pathlib import Path


def check_binary_marker(path: Path, marker: bytes) -> None:
    data = path.read_bytes()
    if not data.startswith(b"\x7fELF") or marker not in data:
        raise ValueError(f"{path} is not a valid Linux ELF binary built from the modified source (missing {marker!r})")


def copy_file(src: Path, dst: Path, mode: int) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    os.chmod(dst, mode)


def extract_web_assets_from_zip(zip_path: Path, dst_web: Path) -> None:
    prefix = "host-portable/assets/web/"
    count = 0
    with zipfile.ZipFile(zip_path, "r") as zf:
        for member in zf.infolist():
            if member.is_dir():
                continue
            name = member.filename.replace("\\", "/")
            if not name.startswith(prefix):
                continue
            rel = name[len(prefix):]
            if not rel or ".." in Path(rel).parts:
                continue
            out = dst_web / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(member) as reader, out.open("wb") as writer:
                shutil.copyfileobj(reader, writer)
            os.chmod(out, 0o644)
            count += 1
    if count == 0:
        raise ValueError(f"No prebuilt Sunshine Web UI assets found under {prefix} in {zip_path}")


def compute_shlib_depends(binaries: list[Path]) -> list[str]:
    """Use dpkg-shlibdeps to compute exact shared-library Debian dependencies."""
    with tempfile.TemporaryDirectory(prefix="sunandmon-shlibdeps-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "debian").mkdir()
        (tmp_path / "debian" / "control").write_text(
            "Source: sunandmon\n\nPackage: sunandmon\nArchitecture: amd64\n",
            encoding="utf-8",
        )
        cmd = ["dpkg-shlibdeps", "-O", "--ignore-missing-info", *[str(b) for b in binaries]]
        proc = subprocess.run(cmd, cwd=tmp_path, capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            raise RuntimeError(f"dpkg-shlibdeps failed:\n{proc.stderr}")
        for line in proc.stdout.splitlines():
            if line.startswith("shlibs:Depends="):
                raw = line.split("=", 1)[1].strip()
                return [item.strip() for item in raw.split(",") if item.strip()]
    return []


def compute_installed_size_kb(root: Path) -> int:
    total = 0
    for dirpath, _, filenames in os.walk(root):
        if Path(dirpath).name == "DEBIAN":
            continue
        for fname in filenames:
            p = Path(dirpath) / fname
            if not p.is_symlink():
                total += p.stat().st_size
    return max(1, (total + 1023) // 1024)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True, help="SunAndMon workspace root")
    parser.add_argument("--controller", type=Path, required=True, help="Compiled SunAndMon ELF binary")
    parser.add_argument("--clipboard-helper", type=Path, required=True, help="Compiled moonlight-clipboard-helper ELF binary")
    parser.add_argument("--host", type=Path, required=True, help="Compiled sunshine ELF binary")
    parser.add_argument("--web-assets-dir", type=Path, help="Prebuilt Sunshine Web UI directory (assets/web)")
    parser.add_argument("--web-assets-zip", type=Path, help="Fallback ZIP containing host-portable/assets/web/")
    parser.add_argument("--version", default="1.0.0", help="Debian package version")
    parser.add_argument("--arch", default="amd64", help="Debian package architecture")
    parser.add_argument("--output", type=Path, required=True, help="Output .deb file path")
    args = parser.parse_args()

    repo = args.repo_root.resolve()
    check_binary_marker(args.controller, b"sunandmon-pair")
    check_binary_marker(args.host, b"sunandmon-status")
    if not args.clipboard_helper.read_bytes().startswith(b"\x7fELF"):
        raise ValueError(f"{args.clipboard_helper} is not a Linux ELF binary")

    with tempfile.TemporaryDirectory(prefix="sunandmon-deb-") as tmp:
        pkg_root = Path(tmp) / "pkg"
        opt_dir = pkg_root / "opt" / "sunandmon"
        host_dir = opt_dir / "host"
        assets_dir = host_dir / "assets"

        # 1. Core executables
        copy_file(args.controller, opt_dir / "SunAndMon", 0o755)
        copy_file(args.clipboard_helper, opt_dir / "moonlight-clipboard-helper", 0o755)
        copy_file(args.host, host_dir / "sunshine", 0o755)

        gcdb = repo / "moonlight-qt" / "app" / "SDL_GameControllerDB" / "gamecontrollerdb.txt"
        if gcdb.is_file():
            copy_file(gcdb, opt_dir / "gamecontrollerdb.txt", 0o644)

        # 2. Sunshine Linux & common assets + Web UI assets
        linux_assets = repo / "foundation-sunshine" / "src_assets" / "linux" / "assets"
        shutil.copytree(linux_assets, assets_dir, dirs_exist_ok=True)
        common_assets = repo / "foundation-sunshine" / "src_assets" / "common" / "assets"
        for item in common_assets.iterdir():
            if item.name == "web":
                continue
            target = assets_dir / item.name
            if item.is_dir():
                shutil.copytree(item, target, dirs_exist_ok=True)
            else:
                copy_file(item, target, 0o644)

        web_dst = assets_dir / "web"
        if args.web_assets_dir and args.web_assets_dir.is_dir():
            shutil.copytree(args.web_assets_dir, web_dst, dirs_exist_ok=True)
        elif args.web_assets_zip and args.web_assets_zip.is_file():
            extract_web_assets_from_zip(args.web_assets_zip, web_dst)
        else:
            raise ValueError("Provide either --web-assets-dir or --web-assets-zip for Sunshine Web UI assets")

        # Normalize asset permissions
        for root_dir, dirs, files in os.walk(assets_dir):
            for d in dirs:
                os.chmod(Path(root_dir) / d, 0o755)
            for f in files:
                os.chmod(Path(root_dir) / f, 0o644)

        # 3. /usr/bin symlink, .desktop entry, icon, udev rules
        usr_bin = pkg_root / "usr" / "bin"
        usr_bin.mkdir(parents=True, exist_ok=True)
        os.symlink("/opt/sunandmon/SunAndMon", usr_bin / "sunandmon")

        desktop_file = pkg_root / "usr" / "share" / "applications" / "sunandmon.desktop"
        desktop_file.parent.mkdir(parents=True, exist_ok=True)
        desktop_file.write_text(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Version=1.0\n"
            "Name=SunAndMon\n"
            "GenericName=Remote Desktop Client and Host\n"
            "Comment=SunAndMon unified Moonlight controller and Sunshine host\n"
            "Exec=/opt/sunandmon/SunAndMon\n"
            "Icon=sunandmon\n"
            "Terminal=false\n"
            "Categories=Network;RemoteAccess;Utility;\n"
            "StartupWMClass=SunAndMon\n",
            encoding="utf-8",
        )
        os.chmod(desktop_file, 0o644)

        icon_src = repo / "moonlight-qt" / "app" / "res" / "sunandmon.png"
        copy_file(icon_src, pkg_root / "usr" / "share" / "icons" / "hicolor" / "256x256" / "apps" / "sunandmon.png", 0o644)

        udev_src = repo / "foundation-sunshine" / "src_assets" / "linux" / "misc" / "60-sunshine.rules"
        copy_file(udev_src, pkg_root / "etc" / "udev" / "rules.d" / "60-sunandmon-uinput.rules", 0o644)

        # 4. Compute dependencies
        shlib_deps = compute_shlib_depends([
            opt_dir / "SunAndMon",
            opt_dir / "moonlight-clipboard-helper",
            host_dir / "sunshine",
        ])
        extra_deps = [
            "qml6-module-qtquick",
            "qml6-module-qtquick-controls",
            "qml6-module-qtquick-layouts",
            "qml6-module-qtquick-window",
            "qml6-module-qtquick-dialogs",
            "qml6-module-qtquick-templates",
            "qml6-module-qtqml-workerscript",
            "qml6-module-qtcore",
            "qml6-module-qt-labs-platform",
            "qt6-wayland",
            "libqt6svg6",
            "libcap2-bin",
            "udev",
        ]
        seen = set()
        all_deps = []
        for dep in shlib_deps + extra_deps:
            key = dep.split()[0]
            if key not in seen:
                seen.add(key)
                all_deps.append(dep)

        # 5. DEBIAN control & maintainer scripts
        debian_dir = pkg_root / "DEBIAN"
        debian_dir.mkdir(parents=True, exist_ok=True)
        installed_kb = compute_installed_size_kb(pkg_root)

        (debian_dir / "control").write_text(
            f"Package: sunandmon\n"
            f"Version: {args.version}\n"
            f"Section: net\n"
            f"Priority: optional\n"
            f"Architecture: {args.arch}\n"
            f"Installed-Size: {installed_kb}\n"
            f"Maintainer: SunAndMon <sunandmon@local>\n"
            f"Depends: {', '.join(all_deps)}\n"
            f"Description: SunAndMon unified Moonlight remote desktop client and Sunshine host\n"
            f" Combines the modified Moonlight Qt controller, bidirectional large-file\n"
            f" clipboard sync, center discovery/pairing, and embedded Foundation Sunshine host.\n",
            encoding="utf-8",
        )

        postinst = debian_dir / "postinst"
        postinst.write_text(
            "#!/bin/sh\n"
            "set -e\n"
            "if [ \"$1\" = \"configure\" ]; then\n"
            "  if command -v setcap >/dev/null 2>&1 && [ -x /opt/sunandmon/host/sunshine ]; then\n"
            "    setcap cap_sys_admin+p /opt/sunandmon/host/sunshine || true\n"
            "  fi\n"
            "  if command -v udevadm >/dev/null 2>&1; then\n"
            "    udevadm control --reload-rules >/dev/null 2>&1 || true\n"
            "    udevadm trigger --property-match=DEVNAME=/dev/uinput >/dev/null 2>&1 || true\n"
            "    udevadm trigger --property-match=DEVNAME=/dev/uhid >/dev/null 2>&1 || true\n"
            "  fi\n"
            "  if command -v update-desktop-database >/dev/null 2>&1; then\n"
            "    update-desktop-database -q /usr/share/applications || true\n"
            "  fi\n"
            "  if command -v gtk-update-icon-cache >/dev/null 2>&1; then\n"
            "    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true\n"
            "  fi\n"
            "fi\n"
            "exit 0\n",
            encoding="utf-8",
        )
        os.chmod(postinst, 0o755)

        postrm = debian_dir / "postrm"
        postrm.write_text(
            "#!/bin/sh\n"
            "set -e\n"
            "if command -v update-desktop-database >/dev/null 2>&1; then\n"
            "  update-desktop-database -q /usr/share/applications || true\n"
            "fi\n"
            "if command -v gtk-update-icon-cache >/dev/null 2>&1; then\n"
            "  gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true\n"
            "fi\n"
            "exit 0\n",
            encoding="utf-8",
        )
        os.chmod(postrm, 0o755)

        # 6. Build .deb
        args.output.parent.mkdir(parents=True, exist_ok=True)
        tmp_deb = args.output.with_suffix(".deb.tmp")
        if tmp_deb.exists():
            tmp_deb.unlink()
        subprocess.run(
            ["dpkg-deb", "--build", "--root-owner-group", str(pkg_root), str(tmp_deb)],
            check=True,
        )
        os.replace(tmp_deb, args.output)
        print(f"Created {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
