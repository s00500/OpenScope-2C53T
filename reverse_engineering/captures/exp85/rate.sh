#!/bin/bash
# Usage: ADDR=<addr of ui_scope_skipped> rate.sh <label> [seconds]
# full/live/skipped scope redraws over a window (skipped, live, full are
# consecutive words in .bss: ui_scope_skipped, ui_scope_live_draws, ui_scope_full_draws)
OS=${OPENSCOPE:-openscope}   # path to the openscope CLI (../openscope-cli)
A=${ADDR:?set ADDR}
S=${2:-10}
rd() { $OS shell "mem read $A 3" 2>&1 | awk -v a="$A:" 'tolower($1)==tolower(a){print $2, $3, $4}'; }
a=$(rd); sleep "$S"; b=$(rd)
[ -z "$a" ] || [ -z "$b" ] && { echo "$1: READ FAILED"; exit 1; }
read s0 l0 f0 <<<"$a"; read s1 l1 f1 <<<"$b"
printf '%-28s %2ss  full=%4d  live=%5d  skipped=%5d\n' "$1" "$S" $((16#$f1-16#$f0)) $((16#$l1-16#$l0)) $((16#$s1-16#$s0))
