#!/bin/bash
# Cursor flicker check on my unit, each mode confirmed by a screenshot.
OS=${OPENSCOPE:-openscope}   # path to the openscope CLI (../openscope-cli)
export ADDR=${ADDR:-0x20000620}  # ui_scope_skipped
WHY=${WHY:-0x20000608}             # ui_scope_full_why[4]
cd "$(dirname "$0")"
why() { $OS shell "mem read $WHY 4" | cut -d' ' -f2-; }
for m in vertical horizontal both; do
  $OS press trigger >/dev/null 2>&1; sleep 1
  $OS screenshot "mode-$m.png" >/dev/null 2>&1; sleep 2
  ./rate.sh "cursors $m"
  w0=$(why)
  $OS press up 5 >/dev/null 2>&1; $OS press left 1 >/dev/null 2>&1; $OS press down 5 >/dev/null 2>&1; sleep 1
  echo "  11 cursor presses: full_why [redraw_all draw_scope scope_live periodic] $w0 -> $(why)"
done
$OS press trigger >/dev/null 2>&1; sleep 1; $OS screenshot "mode-off.png" >/dev/null 2>&1; sleep 2
./rate.sh "cursors off"
