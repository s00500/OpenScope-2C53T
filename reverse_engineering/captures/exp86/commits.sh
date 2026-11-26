#!/bin/bash
# EXP-86: records committed per window (fpga.spi3_ok_count, low 16 bits of the
# word at OK_ADDR). In NORMAL a record is committed only when the FPGA fires,
# so this counts triggers. Usage: OK_ADDR=0x... commits.sh <label> [seconds]
OS=${OPENSCOPE:-openscope}
A=${OK_ADDR:?set OK_ADDR to &fpga.spi3_ok_count}
S=${2:-10}
rd() { $OS shell "mem read $A 1" | awk -v a="$A:" 'tolower($1)==tolower(a){print $2}'; }
a=$(rd); sleep "$S"; b=$(rd)
[ -z "$a" ] || [ -z "$b" ] && { echo "$1: READ FAILED"; exit 1; }
d=$(( (16#${b:4:4} - 16#${a:4:4}) & 0xFFFF ))
src=$($OS shell "fpga scope source" | tr -d '\r')
printf '%-34s %2ss  commits=%4d   %s\n' "$1" "$S" "$d" "$src"
