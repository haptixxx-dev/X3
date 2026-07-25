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
# Exit 0 only if everything passed.

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
LOGDIR="${TMPDIR:-/tmp}/x3-verify.$$"
mkdir -p "$LOGDIR"

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
    timeout 15 "$bin" >"$log" 2>&1
    local rc=$?
    if [ $rc -eq 124 ]; then
        pass "$label: ran 15s without exiting"
    else
        fail "$label: exited early (rc=$rc)"
        info "last lines:"; tail -15 "$log" | sed 's/^/        | /'
    fi
    # Validation layers only report if the package is installed; absence of
    # errors here is not proof of correctness when they are missing.
    if grep -qiE "VUID-|validation layer|VK_ERROR" "$log"; then
        fail "$label: validation or Vulkan errors in output"
        grep -iE "VUID-|validation layer|VK_ERROR" "$log" | head -10 | sed 's/^/        | /'
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
exit 1
