#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ndk=${ANDROID_NDK_HOME:-${ANDROID_HOME:?ANDROID_HOME is required}/ndk/28.2.13676358}
toolchain="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
output=${1:-"$root/moonlight-vplus/app/src/main/jniLibs"}

mkdir -p "$output/arm64-v8a" "$output/armeabi-v7a"
output=$(cd -- "$output" && pwd)
cd "$root/tunnel"

export CGO_ENABLED=1 GOOS=android
GOARCH=arm64 CC="$toolchain/aarch64-linux-android22-clang" \
    go build -trimpath -ldflags='-s -w -extldflags=-Wl,-z,max-page-size=16384' \
    -o "$output/arm64-v8a/libsunandmon_tunnel.so" .
GOARCH=arm GOARM=7 CC="$toolchain/armv7a-linux-androideabi22-clang" \
    go build -trimpath -ldflags='-s -w -extldflags=-Wl,-z,max-page-size=16384' \
    -o "$output/armeabi-v7a/libsunandmon_tunnel.so" .
