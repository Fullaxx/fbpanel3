#!/bin/bash
# resize_cycle.sh — Cycle the VNC/Xvfb display through several resolutions
# to reproduce resize-triggered crashes.
#
# Usage:
#   ./scripts/resize_cycle.sh [DISPLAY]
#
# Typical use: run alongside run_asan.sh in a separate terminal:
#   terminal 1: ./scripts/run_asan.sh
#   terminal 2: ./scripts/resize_cycle.sh

DISPLAY=${1:-${DISPLAY:-:1}}
DELAY=${RESIZE_DELAY:-0.7}

SIZES=(
  "1920x1080"
  "1280x800"
  "1920x1200"
  "1024x768"
  "1280x1024"
  "1920x1080"
  "2446x1307"   # typical TigerVNC default
)

echo "Cycling $DISPLAY through ${#SIZES[@]} resolutions (delay=${DELAY}s each)..."
for s in "${SIZES[@]}"; do
  echo "  -> $s"
  DISPLAY="$DISPLAY" xrandr --fb "$s" 2>/dev/null || {
    echo "  xrandr --fb $s failed (maybe no RandR extension?)"
  }
  sleep "$DELAY"
done
echo "Done."
