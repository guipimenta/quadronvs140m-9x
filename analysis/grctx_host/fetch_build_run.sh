#!/bin/sh
# Regenerate driver/vmdisp9x-nv/nv50_grctx_g86.h from CURRENT nouveau (3100+).
# Needs gh (GitHub CLI) and gcc. Usage: analysis/grctx_host/fetch_build_run.sh [chipset-hex] [units-hex]
#   default: 86 F3010001 (the T61's NVS 140M; read 0x1540 with `nvprobe rd 1540` for another board)
set -e
H="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$H/../.." && pwd)"
CHIP="${1:-86}"; UNITS="${2:-F3010001}"
W="$H/work"; mkdir -p "$W"
for f in ctxnv50.c ctxnv40.h; do
  [ -s "$W/$f" ] || gh api "repos/torvalds/linux/contents/drivers/gpu/drm/nouveau/nvkm/engine/gr/$f" -H "Accept: application/vnd.github.raw" > "$W/$f"
done
cp "$H/main.c" "$W/"; cp -r "$H/stubs/." "$W/stubs/"
cc -I "$W/stubs" -I "$W" -w -o "$W/grctx" "$W/main.c" "$W/ctxnv50.c"
"$W/grctx" "$CHIP" "$UNITS" > "$W/g${CHIP}_${UNITS}.txt"
head -1 "$W/g${CHIP}_${UNITS}.txt"
python3 -I "$H/gen_header.py" "$W/g${CHIP}_${UNITS}.txt" "$R/driver/vmdisp9x-nv/nv50_grctx_g86.h"
