#!/usr/bin/env bash
# Boot nbashowt in MAME from a clean state and dump RAM at the GAME.EXE entry point.
# Usage: tools/mame/run_dump.sh   (run from the project root)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W="$ROOT/work"
S="$W/mame-state"
rm -rf "$S"; mkdir -p "$S"/{nvram,diff,cfg,sta} "$W/dumps"
DUMP_DIR="$W/dumps" mame nbashowt -rompath "$W/roms" \
  -nvram_directory "$S/nvram" -diff_directory "$S/diff" -cfg_directory "$S/cfg" -state_directory "$S/sta" \
  -debug -debugger none -video none -sound none -nothrottle -skip_gameinfo \
  -autoboot_script "$ROOT/tools/mame/dump_game.lua" "$@"
