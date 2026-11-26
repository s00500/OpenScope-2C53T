#!/bin/bash
# Sample the last live-frame cycle count N times; print min/median/max in ms.
OS=${OPENSCOPE:-openscope}   # path to the openscope CLI (../openscope-cli)
A=${CYC:?set CYC to &ui_scope_live_cyc}
vals=()
for i in $(seq 1 ${2:-15}); do
  v=$($OS shell "mem read $A 1" | awk '{print $2}'); vals+=($((16#$v))); sleep 0.3
done
printf '%s\n' "${vals[@]}" | sort -n | awk -v l="$1" '{a[NR]=$1} END{printf "%-26s live frame: min %.1f ms  median %.1f ms  max %.1f ms  (n=%d)\n", l, a[1]/240000, a[int((NR+1)/2)]/240000, a[NR]/240000, NR}'
