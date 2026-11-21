#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${SUNANDMON_LINUX_BUILD_DIR:-$HOME/.cache/sunandmon-linux-build}"
FFMPEG_DIR="$REPO_ROOT/.build-linux/build-deps-repo/dist/Linux-x86_64"
SUNSHINE_BUILD="$BUILD_ROOT/sunshine-build"
MOONLIGHT_BUILD="$BUILD_ROOT/moonlight-build"
VERSION="${SUNANDMON_VERSION:-1.0.0}"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$BUILD_ROOT" "$SUNSHINE_BUILD" "$MOONLIGHT_BUILD" "$REPO_ROOT/dist"

echo "=== [1/4] Checking Linux FFmpeg build-deps ==="
if [ ! -f "$FFMPEG_DIR/lib/libavcodec.a" ]; then
  echo "Fetching Linux-x86_64 FFmpeg build-deps..."
  mkdir -p "$REPO_ROOT/.build-linux/build-deps-repo"
  if [ ! -d "$REPO_ROOT/.build-linux/build-deps-repo/.git" ]; then
    git -C "$REPO_ROOT/.build-linux/build-deps-repo" init
    git -C "$REPO_ROOT/.build-linux/build-deps-repo" remote add origin https://github.com/AlkaidLab/foundation-build-deps.git
    git -C "$REPO_ROOT/.build-linux/build-deps-repo" sparse-checkout init --cone
    git -C "$REPO_ROOT/.build-linux/build-deps-repo" sparse-checkout set dist/Linux-x86_64
  fi
  git -C "$REPO_ROOT/.build-linux/build-deps-repo" fetch --depth 1 --filter=blob:none origin 1b6a3546f48a1771b295d24de9f1fef8d174e16d
  git -C "$REPO_ROOT/.build-linux/build-deps-repo" checkout FETCH_HEAD
fi

echo "=== [2/4] Building Foundation Sunshine (Linux x86_64) ==="
cmake -S "$REPO_ROOT/foundation-sunshine" -B "$SUNSHINE_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFFMPEG_PREPARED_BINARIES="$FFMPEG_DIR" \
  -DBOOST_USE_STATIC=ON \
  -DBOOST_LOCALE_ENABLE_ICU=OFF \
  -DSUNSHINE_BOOST_SEPARATE_COMPILATION=ON \
  -DBUILD_WEB_UI=OFF \
  -DBUILD_DOCS=OFF \
  -DBUILD_TESTS=OFF \
  -DSUNSHINE_ENABLE_TRAY=OFF \
  -DSUNSHINE_ENABLE_CUDA=OFF \
  -DSUNSHINE_SYSTEM_WAYLAND_PROTOCOLS=ON \
  -DSUNSHINE_ASSETS_DIR=assets \
  -DSUNSHINE_ASSETS_DIR_DEF=assets
ninja -C "$SUNSHINE_BUILD" -j "$JOBS" sunshine

echo "=== [3/4] Building Moonlight Qt SunAndMon + clipboard-helper (Linux x86_64) ==="
QMAKE_BIN="$(command -v qmake6 || command -v qmake)"
cd "$MOONLIGHT_BUILD"
"$QMAKE_BIN" "$REPO_ROOT/moonlight-qt/moonlight-qt.pro" CONFIG+=release CONFIG+=sunandmon
make -j "$JOBS" release

echo "=== [4/4] Packaging Debian/Ubuntu .deb ==="
SUNSHINE_BIN="$SUNSHINE_BUILD/sunshine"
if [ -x "$MOONLIGHT_BUILD/app/release/SunAndMon" ]; then
  SUNANDMON_BIN="$MOONLIGHT_BUILD/app/release/SunAndMon"
else
  SUNANDMON_BIN="$MOONLIGHT_BUILD/app/SunAndMon"
fi
if [ -x "$MOONLIGHT_BUILD/clipboard-helper/release/moonlight-clipboard-helper" ]; then
  HELPER_BIN="$MOONLIGHT_BUILD/clipboard-helper/release/moonlight-clipboard-helper"
else
  HELPER_BIN="$MOONLIGHT_BUILD/clipboard-helper/moonlight-clipboard-helper"
fi

STRIPPED_DIR="$BUILD_ROOT/stripped"
mkdir -p "$STRIPPED_DIR"
cp "$SUNSHINE_BIN" "$STRIPPED_DIR/sunshine"
cp "$SUNANDMON_BIN" "$STRIPPED_DIR/SunAndMon"
cp "$HELPER_BIN" "$STRIPPED_DIR/moonlight-clipboard-helper"
strip --strip-unneeded "$STRIPPED_DIR/sunshine" "$STRIPPED_DIR/SunAndMon" "$STRIPPED_DIR/moonlight-clipboard-helper"

WEB_ZIP="$REPO_ROOT/.build-win/sunshine-portable-uac-x64.zip"
OUT_DEB="$REPO_ROOT/dist/sunandmon_${VERSION}_amd64.deb"

python3 "$REPO_ROOT/scripts/package-linux-deb.py" \
  --repo-root "$REPO_ROOT" \
  --controller "$STRIPPED_DIR/SunAndMon" \
  --clipboard-helper "$STRIPPED_DIR/moonlight-clipboard-helper" \
  --host "$STRIPPED_DIR/sunshine" \
  --web-assets-zip "$WEB_ZIP" \
  --version "$VERSION" \
  --arch amd64 \
  --output "$OUT_DEB"

dpkg-deb -I "$OUT_DEB"
echo "DONE: $OUT_DEB"
