#!/usr/bin/env bash
# Run the renderer's golden-image tests.
#
#   ./scripts/render-test.sh                    compare against tests/golden
#   ./scripts/render-test.sh --update-goldens   re-record them
#   ./scripts/render-test.sh --filter phong     just the matching scenarios
#
# Needs a display. The window is created unmapped so nothing appears on screen,
# but the frame lifecycle still goes through swapchain acquire, so this will not
# run over a bare SSH session.
#
# Builds first, deliberately: a golden-image test run against a stale binary is
# worse than no test run, because it reports green for code that is not what is
# on disk.

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PRESET="${X3_PRESET:-debug}"
BIN="build/${PRESET}/Debug/X3RenderTest"
[ "$PRESET" = "release" ] && BIN="build/${PRESET}/Release/X3RenderTest"

if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    echo "no display -- X3RenderTest needs one (the window is unmapped, not headless)" >&2
    exit 2
fi

cmake --build "build/${PRESET}" --target X3RenderTest -j"$(nproc)" > /tmp/x3-render-test-build.log 2>&1
if [ $? -ne 0 ] || ! [ -x "$BIN" ]; then
    echo "build failed; see /tmp/x3-render-test-build.log" >&2
    grep -E 'error:' /tmp/x3-render-test-build.log | head -20 >&2
    exit 1
fi

exec "$BIN" "$@"
