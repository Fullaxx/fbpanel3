#!/bin/bash
# run_asan.sh — Build fbpanel with AddressSanitizer and run it on :1
#
# Usage:
#   ./scripts/run_asan.sh           # build + run
#   ./scripts/run_asan.sh --no-build  # run existing build
#
# Output is logged to /tmp/fbpanel_asan.log

set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
DISPLAY=${DISPLAY:-:1}
INSTALL_PREFIX=/tmp/fbtest
PLUGIN_DEST=/usr/local/lib/fbpanel
LOG=/tmp/fbpanel_asan.log

# ---- Parse args ----
NO_BUILD=0
for arg in "$@"; do
  case "$arg" in
    --no-build) NO_BUILD=1 ;;
    *) echo "Unknown arg: $arg"; exit 1 ;;
  esac
done

# ---- Build ----
if [ $NO_BUILD -eq 0 ]; then
  echo "==> Configuring with AddressSanitizer..."
  cmake -B "$REPO/build" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
        "$REPO" >/dev/null
  echo "==> Building..."
  cmake --build "$REPO/build" -j"$(nproc)" 2>&1 | tail -3
  echo "==> Installing to $INSTALL_PREFIX..."
  cmake --install "$REPO/build" --prefix "$INSTALL_PREFIX" 2>&1 | tail -3
  echo "==> Copying plugins to $PLUGIN_DEST..."
  mkdir -p "$PLUGIN_DEST"
  cp "$INSTALL_PREFIX/lib/fbpanel/"*.so "$PLUGIN_DEST/"
fi

# ---- Set root background ----
xsetroot -display "$DISPLAY" -solid '#2e3436' 2>/dev/null || true

# ---- Run ----
echo "==> Running fbpanel on $DISPLAY (log: $LOG)"
echo "    Press Ctrl+C or kill the panel to stop."
DISPLAY="$DISPLAY" \
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1 \
  "$INSTALL_PREFIX/bin/fbpanel" 2>&1 | tee "$LOG"
