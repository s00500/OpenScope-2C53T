#!/bin/bash
# Rigol zero check: GND coupling vs DC (generator off) at several settings.
for cfg in "500mV -1.75V" "2V -1.4V" "500mV 0V" "200mV 0V" "100mV 0V"; do
  read sc off <<< "$cfg"
  rigol channel 1 --scale "$sc" --offset="$off" --coupling gnd | tail -1
  sleep 2
  g=$(rigol measure vavg -c 1 --json | tail -1 | grep -o '"CH1 vavg":[^,]*')
  rigol channel 1 --coupling dc > /dev/null
  sleep 2
  d=$(rigol measure vavg -c 1 --json | tail -1 | grep -o '"CH1 vavg":[^,]*')
  echo "  -> GND $g   DC(gen off) $d"
done
