#!/bin/sh
# Run wgltest's driver mode under Wine with the shim's fake GPU, then validate the kicks.
# SCENE=texture selects wgltest's scene (3093+); NOKREF=1 reproduces the 3093 shim (no krefs) to show the KICK WARN check fire.
W="${1:-$PWD}"; R="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$W" || exit 1
rm -f ~/.wine/drive_c/nvshim.log ~/.wine/drive_c/nvkick.txt
GALLIUM_DRIVER=nouveau NVSHIM_FAKE=1 NVSHIM_TRACE=1 NVSHIM_KICKDUMP=${KICKS:-64} NVSHIM_NOKREF=${NOKREF:-0} timeout ${T:-25} wine wgltest.exe driver mesa3d.dll window ${SCENE:-} > wine_fake.out 2>&1
echo "wine exit=$?"
pgrep -x wgltest.exe >/dev/null && pkill -x wgltest.exe
cp ~/.wine/drive_c/nvshim.log fake_nvshim.log 2>/dev/null; cp ~/.wine/drive_c/nvkick.txt fake_nvkick.txt 2>/dev/null
echo "log lines: $(wc -l < fake_nvshim.log 2>/dev/null)  kicks dumped: $(grep -c '^---- kick' fake_nvkick.txt 2>/dev/null)"
grep -E "try driver|result screen|nv50: create|bind handle|kick FAIL|FAKE WEDGE|GPU DEAD|present px|SetPF OK|CreateLayerContext ctx|graph_units|KICK WARN|stats traps" fake_nvshim.log | head -40
echo "--- validator ---"
python3 -I "$R/tools/nvkick_decode.py" fake_nvkick.txt --check --log fake_nvshim.log --quiet
