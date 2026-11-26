#!/usr/bin/env bash
# Boot nbashowt in MAME from a clean state and run a Lua trace script.
# Usage: tools/mame/run_trace.sh <script.lua> [seconds]   (run from the project root)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W="$ROOT/work"
S="$W/mame-state-trace"
rm -rf "$S"; mkdir -p "$S"/{nvram,diff,cfg,sta} "$W/dumps"
TRACE_SECONDS="${2:-30}" DUMP_DIR="$W/dumps" mame nbashowt -rompath "$W/roms" \
  -nvram_directory "$S/nvram" -diff_directory "$S/diff" -cfg_directory "$S/cfg" -state_directory "$S/sta" \
  -debug -debugger none -video none -sound none -nothrottle -skip_gameinfo \
  -autoboot_script "$1"
