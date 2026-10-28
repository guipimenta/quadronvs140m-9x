#!/bin/sh
# Run wgltest's driver mode under Wine with the shim's fake GPU, then validate the kicks.
# SCENE=texture|stress selects wgltest's scene (3093+); NOKREF=1 reproduces the 3093 shim (no krefs) to show the KICK WARN check fire.
# 3096: PUSHCAP=<dwords> caps the pushbuffer (frequent pushbuf-full kicks cut packet sequences across kicks),
# SCANRESET=1 reproduces the 3095 kick walk (M2MF state forgotten per kick) to show the verify pass catch it, VSYNC=1 exercises the vblank wait.
# 3097: VERIFYVRAM=1 reads flushed ranges back after each kick, HEAPHI=1 = high heap region only; SCENE=texcheck (mismatches expected under the fake: nothing copies).
W="${1:-$PWD}"; R="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$W" || exit 1
rm -f ~/.wine/drive_c/nvshim.log ~/.wine/drive_c/nvkick.txt
GALLIUM_DRIVER=nouveau NVSHIM_FAKE=1 NVSHIM_TRACE=1 NVSHIM_KICKDUMP=${KICKS:-64} NVSHIM_NOKREF=${NOKREF:-0} NVSHIM_PUSHCAP=${PUSHCAP:-0} NVSHIM_SCANRESET=${SCANRESET:-0} NVSHIM_VSYNC=${VSYNC:-0} NVSHIM_VERIFYVRAM=${VERIFYVRAM:-0} NVSHIM_HEAPHI=${HEAPHI:-0} timeout ${T:-25} wine wgltest.exe driver mesa3d.dll window ${SCENE:-} > wine_fake.out 2>&1
echo "wine exit=$?"
pgrep -x wgltest.exe >/dev/null && pkill -x wgltest.exe
cp ~/.wine/drive_c/nvshim.log fake_nvshim.log 2>/dev/null; cp ~/.wine/drive_c/nvkick.txt fake_nvkick.txt 2>/dev/null
echo "log lines: $(wc -l < fake_nvshim.log 2>/dev/null)  kicks dumped: $(grep -c '^---- kick' fake_nvkick.txt 2>/dev/null)"
grep -E "try driver|result screen|nv50: create|bind handle|kick FAIL|FAKE WEDGE|GPU DEAD|present px|SetPF OK|CreateLayerContext ctx|graph_units|KICK WARN|stats traps|vsync" fake_nvshim.log | head -40
grep -E "stats m2mf splits|stats vsync|present frames|stats kicks/kickdw|vram checks|VRAM WARN|heap full|heap: high" fake_nvshim.log | tail -6
echo "--- validator ---"
python3 -I "$R/tools/nvkick_decode.py" fake_nvkick.txt --check --log fake_nvshim.log --quiet
