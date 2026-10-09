#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

cmd="${1:-debug}"

case "$cmd" in
  debug|release)
    cmake --preset "$cmd"
    cmake --build --preset "$cmd" --parallel
    ln -sf "build/$cmd/compile_commands.json" compile_commands.json   # for clangd
    ;;
  run)      shift; ./build/debug/cli/sn "$@" ;;
  install)  cmake --install build/release --prefix "${PREFIX:-$HOME/.local}" ;;
  clean)    rm -rf build compile_commands.json ;;
  analyze)  command -v clang-tidy >/dev/null || { echo "analyze: clang-tidy not installed" >&2; exit 1; }
            [[ -f compile_commands.json ]]    || { echo "analyze: no compile_commands.json, run ./b.sh debug first" >&2; exit 1; }
            JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
            EXCLUDE_PATTERN="^$PWD/(external|build)/"
            HEADER_FILTER="^$PWD/(src|include|cli)/"
            FILES=$(jq -r '.[].file' compile_commands.json | { grep -vE "$EXCLUDE_PATTERN" || true; } | sort -u)
            [[ -n "$FILES" ]] || { echo "analyze: no source files found" >&2; exit 1; }
            LINE_COUNT=$(echo "$FILES" | xargs cat | wc -l | tr -d ' ')
            OUTPUT=$(echo "$FILES" | tr '\n' '\0' \
                     | xargs -0 -n1 -P "$JOBS" clang-tidy -p . --quiet --header-filter="$HEADER_FILTER" 2>&1 || true)
            DIAGS=$(grep -E "warning:|error:|Error while processing" <<<"$OUTPUT" | sort -u || true)
            WARNINGS=$(grep -c "warning:" <<<"$DIAGS" || true)
            ERRORS=$(grep -cE "error:|Error while processing" <<<"$DIAGS" || true)

            [[ -z "$DIAGS" ]] || echo "$DIAGS"
            echo "${LINE_COUNT} lines, ${WARNINGS} warnings and ${ERRORS} errors"

            (( ERRORS == 0 )) || exit 1
            # (( WARNINGS == 0 )) || exit 1 # uncomment for a strict gate
            ;;
  test)
            shift
            preset="debug"
            if [[ "${1:-}" == "debug" || "${1:-}" == "release" ]]; then
              preset="$1"; shift
            fi
            cmake --preset "$preset"
            cmake --build --preset "$preset" --parallel
            ctest --test-dir "build/$preset" --output-on-failure \
                  --parallel "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" "$@"
            ;;
  *)        echo "usage: $0 [debug|release|run|install|clean|analyze|test]"; exit 1 ;;
esac