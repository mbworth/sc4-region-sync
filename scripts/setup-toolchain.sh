#!/usr/bin/env bash
# One-time toolchain setup for building SC4RegionSync.dll on Linux.
#
# Installs nothing system-wide except via apt (if asked). Fetches the MSVC
# CRT and Windows SDK headers/libs for x86 into vendor/xwin using xwin,
# which is what the Makefile compiles against.
#
# Usage:
#   scripts/setup-toolchain.sh            # fetch xwin + MSVC/SDK splat
#   scripts/setup-toolchain.sh --apt      # also apt-get install clang lld llvm make
#
# Network: xwin downloads from aka.ms and download.visualstudio.microsoft.com
# (Microsoft's own VS package feed, under the VS license you accept with
# --accept-license). About 1 GB cached, ~400 MB splatted.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XWIN_VERSION="${XWIN_VERSION:-0.6.5}"
XWIN_DIR="$ROOT/vendor/xwin"
CACHE_DIR="${XWIN_CACHE_DIR:-$ROOT/.xwin-cache}"

if [[ "${1:-}" == "--apt" ]]; then
    sudo apt-get update
    sudo apt-get install -y clang lld llvm make curl
fi

for tool in clang-cl lld-link llvm-rc make curl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing $tool. On Debian/Ubuntu: sudo apt-get install clang lld llvm make curl" >&2
        echo "(or rerun with --apt)" >&2
        exit 1
    fi
done

if [[ -d "$XWIN_DIR/crt/include" ]]; then
    echo "xwin splat already present at $XWIN_DIR"
    exit 0
fi

mkdir -p "$CACHE_DIR"
cd "$CACHE_DIR"

TARBALL="xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl.tar.gz"
if [[ ! -x "xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl/xwin" ]]; then
    echo "Downloading xwin ${XWIN_VERSION}"
    curl -sSL -o "$TARBALL" "https://github.com/Jake-Shadle/xwin/releases/download/${XWIN_VERSION}/${TARBALL}"
    tar xzf "$TARBALL"
fi

echo "Fetching MSVC CRT + Windows SDK (x86) into $XWIN_DIR"
"./xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl/xwin" \
    --accept-license \
    --arch x86 \
    --cache-dir "$CACHE_DIR/dl" \
    splat --output "$XWIN_DIR"

echo
echo "Toolchain ready. Build with: make"
