#!/usr/bin/env bash
# Download the pinned Slang toolchain into X3/libs/slang.
#
# WHY THIS IS A SCRIPT AND NOT A SUBMODULE OR A VENDORED BINARY.
#
# Slang releases roughly weekly, so "whatever is newest" is not a reproducible
# build. The extracted toolchain is ~200 MB, which does not belong in a git
# repository. Building Slang from source pulls in LLVM and adds many minutes to
# a clean build for a tool that only needs to run at build time.
#
# So: a pinned release, downloaded on demand, into a gitignored directory. Bump
# SLANG_VERSION deliberately, and re-verify that the rendered output is
# unchanged when you do -- a compiler upgrade is a shader change.
#
# CMake also accepts a slangc already on PATH. Note that a system package named
# "slang" is very likely S-Lang, an unrelated interpreted language, and its
# binary is not called slangc.

set -euo pipefail

SLANG_VERSION="2026.14"

cd "$(dirname "$0")/.."
DEST="X3/libs/slang"

if [ -x "$DEST/bin/slangc" ]; then
    HAVE=$("$DEST/bin/slangc" -v 2>&1 | head -1)
    if [ "$HAVE" = "$SLANG_VERSION" ]; then
        echo "slang $SLANG_VERSION already present in $DEST"
        exit 0
    fi
    echo "replacing slang $HAVE with $SLANG_VERSION"
    rm -rf "$DEST"
fi

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)   ASSET="slang-${SLANG_VERSION}-linux-x86_64.tar.gz" ;;
    Linux-aarch64)  ASSET="slang-${SLANG_VERSION}-linux-aarch64.tar.gz" ;;
    Darwin-arm64)   ASSET="slang-${SLANG_VERSION}-macos-aarch64.tar.gz" ;;
    Darwin-x86_64)  ASSET="slang-${SLANG_VERSION}-macos-x86_64.tar.gz" ;;
    *) echo "unsupported platform $(uname -s)-$(uname -m); download slangc manually into $DEST" >&2
       exit 1 ;;
esac

URL="https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/${ASSET}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "downloading $URL"
curl -fsSL "$URL" -o "$TMP/slang.tar.gz"

mkdir -p "$DEST"
tar -xzf "$TMP/slang.tar.gz" -C "$DEST"

"$DEST/bin/slangc" -v
echo "slang $SLANG_VERSION installed in $DEST"
