#!/usr/bin/env bash
# Runs every test binary that the CONFIG+=tests build produced.
#
# The qmake tree has no top level 'check' target, and the fork's test binaries
# are plain Qt Test executables, so this walks build/tree/tests instead. Each
# test gets its own timeout: a driver quirk must not hang the whole run.
#
#   scripts/linux/run-tests.sh [name-filter]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/env.sh" >/dev/null

# Both ICDs: the round trip must decode on the dGPU and the panel driving iGPU,
# and a missing manifest silently halves the GPU list.
if [[ -d /usr/share/vulkan/icd.d && -z "${VK_DRIVER_FILES:-}" && -z "${VK_ICD_FILENAMES:-}" ]]; then
    VK_DRIVER_FILES=$(find /usr/share/vulkan/icd.d -name '*.json' | paste -sd: -)
    export VK_DRIVER_FILES
fi
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export LD_LIBRARY_PATH="$QT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="${QT_PLUGIN_PATH:-$QT_DIR/plugins}"

FILTER="${1:-}"
TESTROOT="$ML_BUILD/tree/tests"
[[ -d "$TESTROOT" ]] || { echo "run-tests: no $TESTROOT (build with CONFIG+=tests)"; exit 1; }

pass=0; fail=0; skip=0
for bin in $(find "$TESTROOT" -type f -executable -name 'tst_*' | sort); do
    name=$(basename "$bin")
    [[ -z "$FILTER" || "$name" == *"$FILTER"* ]] || continue
    # Mirror the arguments the Windows workflow passes: tst_overlay chdirs into
    # the directory holding res/ModeSeven.ttf, and the three tests that write
    # fixtures print their results instead of staying silent.
    args=()
    case "$name" in
        tst_overlay) args=("$ML_REPO/app") ;;
        tst_vrrratepolicy|tst_vrrreplayconfig|tst_vrrdiagnostics) args=(-o -,txt) ;;
    esac
    # Windows only tests are not built on Linux, but a binary may still need a
    # display or a device that this machine lacks.
    if out=$(timeout 300 "$bin" "${args[@]}" 2>&1); then
        if grep -q "Totals:" <<<"$out" && grep -q "skipped" <<<"$out"; then
            skipped=$(sed -n 's/.*Totals:[0-9]* passed,[0-9]* skipped.*/\0/p' <<<"$out" | tail -1)
        fi
        printf 'PASS  %-34s %s\n' "$name" "$(grep -o 'Totals:.*' <<<"$out" | tail -1)"
        pass=$((pass+1))
    else
        rc=$?
        if [[ $rc -eq 124 || $rc -eq 125 ]]; then
            printf 'TIMEOUT %-34s (300s)\n' "$name"
        else
            printf 'FAIL  %-34s rc=%d\n' "$name" "$rc"
            grep -E "^FAIL!|QFATAL|QWARN" <<<"$out" | head -3 | sed 's/^/        /'
        fi
        fail=$((fail+1))
    fi
done

echo
echo "run-tests: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
