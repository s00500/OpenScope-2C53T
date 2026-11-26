#!/bin/bash
# Cursor-press burst: full repaints, live frames and panel recoveries around it.
OS=${OPENSCOPE:-openscope}   # path to the openscope CLI (../openscope-cli)
A=${ADDR:?set ADDR}
cnt() { $OS shell "mem read $A 3" | awk -v a="$A:" 'tolower($1)==tolower(a){print $3, $4}'; }
rec() { $OS shell "lcd status" | grep -oE 'recoveries [0-9]+' | awk '{print $2}'; }
read l0 f0 <<<"$(cnt)"; r0=$(rec)
for k in "$@"; do $OS press $k >/dev/null 2>&1; done
sleep 1
read l1 f1 <<<"$(cnt)"; r1=$(rec)
echo "presses: $*  -> full=$((16#$f1-16#$f0)) live=$((16#$l1-16#$l0)) panel_recoveries=$((r1-r0))"
