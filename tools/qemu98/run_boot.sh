#!/bin/bash
# run_boot.sh <workdir> <tag> [max_seconds]  - boot the VM headless and watch it: from t=70 s press Esc every
# 6 s (dismisses New Hardware wizards and error boxes), screenshot every 30 s into <tag>_NNN.png, stop when
# QEMU exits (Windows restart/shutdown) or at the timeout. Prints a summary of the driver's debug console.
W=$(realpath "$1"); TAG=$2; MAX=${3:-480}; Q=$(dirname "$(realpath "$0")")/qmp.py
: > $W/e9.txt; rm -f $W/${TAG}_*.ppm $W/${TAG}_*.png
nohup $W/qemu.sh >/dev/null 2>&1 &
QPID=$!; t=0; n=0
while kill -0 $QPID 2>/dev/null && [ $t -lt $MAX ]; do
  sleep 6; t=$((t+6))
  [ $t -ge 70 ] && python3 $Q keys esc >/dev/null 2>&1
  if [ $((t % 30)) -eq 0 ]; then n=$((n+1)); f=$(printf "%s_%03d" $TAG $n); python3 $Q shot $W/$f.ppm >/dev/null 2>&1; python3 -c "from PIL import Image; Image.open('$W/$f.ppm').save('$W/$f.png')" 2>/dev/null; fi
done
if kill -0 $QPID 2>/dev/null; then echo "timeout at ${t}s, QEMU still running (pid $QPID)"; else echo "QEMU exited at ${t}s"; fi
echo "e9 lines: $(wc -l < $W/e9.txt)"; grep -vE '^V> HB ' $W/e9.txt | grep -E 'DriverInit|LoadFail|Control \(16bit\): 4353|mode set' | sort | uniq -c | sort -rn | head -6
