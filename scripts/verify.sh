#!/usr/bin/env bash
# Build and smoke-test X3. This is the verification gate for every commit
# during the Vulkan migration.
#
# Deliberately does NOT trust exit codes alone: a piped build reports the
# exit status of the last stage in the pipe, not the compiler, which has
# already produced one false "success" during this migration. Every check
# here confirms a produced artefact or greps the log.
#
#   ./scripts/verify.sh                 build + smoke-test every preset
#   ./scripts/verify.sh vulkan-debug    just one preset
#   X3_SKIP_RUN=1 ./scripts/verify.sh   build only, no smoke test
#
# The editor smoke test opens the committed fixture (TestProject/) so the run
# reaches the render path instead of sitting on the project launcher, and FAILS
# on any validation message. Expect it to be red until Phase 1 lands: the known
# bugs are real and now they are logged. docs/VALIDATION-BASELINE.md lists every
# VUID that is currently expected and who fixes it. A VUID that is NOT in that
# file is a new regression.
#
# Exit 0 only if everything passed.

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
LOGDIR="${TMPDIR:-/tmp}/x3-verify.$$"
mkdir -p "$LOGDIR"

# Committed render-path fixture. Absent (e.g. a shallow checkout that skipped
# it) the smoke test still runs, but it only proves Vulkan initialised.
FIXTURE="$ROOT/TestProject/TestProject.lrproj"
BASELINE_DOC="docs/VALIDATION-BASELINE.md"
SMOKE_SECONDS=20

PRESETS=("$@")
if [ ${#PRESETS[@]} -eq 0 ]; then
    mapfile -t PRESETS < <(cmake --list-presets 2>/dev/null \
        | sed -n 's/^  "\(.*\)".*/\1/p' | grep -v '^base$')
fi
[ ${#PRESETS[@]} -eq 0 ] && { echo "no presets found"; exit 1; }

FAILED=()
pass() { printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
fail() { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAILED+=("$1"); }
info() { printf '        %s\n' "$1"; }

# Run a GUI binary for a few seconds and decide whether it died on its own.
# timeout returns 124 when it had to kill the process, which is what we
# want: the app was still alive. Anything else means it exited early.
smoke() {
    local label=$1 bin=$2 log=$3
    [ -x "$bin" ] || { fail "$label: binary missing"; return; }
    if [ -n "${X3_SKIP_RUN:-}" ] || [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
        info "$label: smoke test skipped (no display or X3_SKIP_RUN)"
        return
    fi

    # Open the fixture so the run actually renders. Without a project the editor
    # sits on the launcher, RenderLayer never dispatches, and the run proves only
    # that Vulkan initialised - it cannot report a render-path problem because it
    # never reaches one. Only the editor honours X3_OPEN_PROJECT.
    local -a runner=()
    local exercised=0
    if [ "$label" = "X3Editor" ]; then
        if [ -f "$FIXTURE" ]; then
            runner+=(env "X3_OPEN_PROJECT=$FIXTURE")
            exercised=1
        else
            info "$label: fixture missing ($FIXTURE) - render path NOT exercised"
        fi
    fi

    # Validation output reaches stdout through vk-bootstrap's debug messenger.
    # Redirected to a file, stdout is fully buffered and abort() throws the
    # buffer away, which has already made a crashing run look perfectly clean.
    # Line-buffer it. (stdbuf is GNU coreutils; absent on macOS.)
    if command -v stdbuf >/dev/null 2>&1; then
        runner+=(stdbuf -oL -eL)
    else
        info "$label: stdbuf unavailable - validation output may be lost if the app crashes"
    fi

    timeout "$SMOKE_SECONDS" "${runner[@]}" "$bin" >"$log" 2>&1
    local rc=$?
    if [ $rc -eq 124 ]; then
        if [ $exercised -eq 1 ]; then
            pass "$label: ran ${SMOKE_SECONDS}s with the fixture open"
        else
            pass "$label: ran ${SMOKE_SECONDS}s without exiting"
        fi
    else
        fail "$label: exited early (rc=$rc)"
        info "last lines:"; tail -15 "$log" | sed 's/^/        | /'
    fi

    # A frame that renders nothing logs this every frame. With the fixture open
    # (camera, meshes, lights all present) it means the render path broke.
    if [ $exercised -eq 1 ] && grep -q "No frame produced" "$log"; then
        fail "$label: renderer produced no frames with the fixture open"
        grep -m3 "No frame produced" "$log" | sed 's/^/        | /'
    fi

    # Validation layers only report if the package is installed; absence of
    # errors here is not proof of correctness when they are missing.
    if grep -qiE "VUID-|validation layer|VK_ERROR" "$log"; then
        fail "$label: validation or Vulkan errors in output"
        info "distinct VUIDs (see $BASELINE_DOC; anything not listed there is new):"
        grep -oE "VUID-[A-Za-z0-9-]+" "$log" | sort -u | sed 's/^/        | /'
    fi
}

for P in "${PRESETS[@]}"; do
    echo "=== $P ==="
    CFG="$LOGDIR/$P.configure.log"
    BLD="$LOGDIR/$P.build.log"

    rm -rf "build/$P"
    if cmake --preset "$P" >"$CFG" 2>&1; then
        pass "configure"
    else
        fail "configure"; tail -15 "$CFG" | sed 's/^/        | /'; continue
    fi

    cmake --build "build/$P" -j "$(nproc)" >"$BLD" 2>&1
    BRC=$?
    # Check the log as well as the status: a nonzero grep count is decisive
    # even in the odd case where the status is misleading.
    ERRS=$(grep -cE "error:|fatal error|undefined reference" "$BLD")
    if [ $BRC -eq 0 ] && [ "$ERRS" -eq 0 ]; then
        pass "build"
    else
        fail "build (rc=$BRC, $ERRS error lines)"
        grep -E "error:|fatal error|undefined reference" "$BLD" | head -10 | sed 's/^/        | /'
        continue
    fi

    found=0
    while IFS= read -r bin; do
        found=1
        info "$(basename "$bin") $(du -h "$bin" | cut -f1)"
        smoke "$(basename "$bin")" "$bin" "$LOGDIR/$P.$(basename "$bin").run.log"
    done < <(find "build/$P" -type f -executable \
             \( -name 'X3Editor*' -o -name 'X3Runtime*' \) 2>/dev/null)
    [ $found -eq 0 ] && fail "no binaries produced"
done

echo
if [ ${#FAILED[@]} -eq 0 ]; then
    echo "ALL CHECKS PASSED"
    rm -rf "$LOGDIR"
    exit 0
fi
echo "FAILURES (${#FAILED[@]}):"
printf '  - %s\n' "${FAILED[@]}"
echo "logs: $LOGDIR"
echo
echo "Validation failures are expected until Phase 1 lands. Compare the VUIDs"
echo "above against $BASELINE_DOC - one that is not listed there is a new bug."
exit 1
