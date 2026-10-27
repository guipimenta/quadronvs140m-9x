#!/bin/sh
# Repopulate dist/driver from the build tree (binaries, INF, VERSION.txt, nvsetup.reg).
# Run from anywhere: tools/make_dist.sh   (does NOT rebuild; run wmake/mingw first)
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
S="$R/driver/vmdisp9x-nv"
D="$R/dist/driver"
mkdir -p "$D"
cp "$S/nvmini.vxd" "$S/nvmini.drv" "$S/tools/nvprobe.exe" "$S/tools/pftest.exe" "$S/tools/icdload.exe" "$S/tools/vmmap.exe" "$S/vmdisp9x.inf" "$S/VERSION.txt" "$S/nvsetup.reg" "$S/d3dsetup.reg" "$S/nouveau.reg" "$D/"
cp "$R/driver/vmhal9x-bin/vmhal9x.dll" "$R/driver/vmhal9x-bin/vmdisp9x.dll" "$R/driver/vmhal9x-bin/vmhal3d.dll" "$D/"
[ -f "$R/driver/mesa9x-bin/mesa3d.dll" ] && cp "$R/driver/mesa9x-bin/mesa3d.dll" "$D/" || echo "WARNING: driver/mesa9x-bin/mesa3d.dll missing (Direct3D will not work)"
cp "$R/driver/mesa9x-bin/wgltest.exe" "$R/driver/mesa9x-bin/icdtest.exe" "$D/"
for f in INSTALL.txt nvdump.bat nvdump2.bat nvdump3.bat nvgl.bat; do
  [ -f "$D/$f" ] || git -C "$R" checkout -- "dist/driver/$f" 2>/dev/null || true
done
python3 - "$D" <<'EOF'
import struct,sys
D=sys.argv[1]
import re,os
want=int(re.search(r"NV_BUILD\s+(\d+)UL", open(os.path.join(D,"..","..","driver","vmdisp9x-nv","vxd_nv.h")).read()).group(1))
for f in ("nvmini.vxd","nvprobe.exe"):
    d=open(f"{D}/{f}","rb").read()
    print(f"{f}: {len(d)} bytes, carries NV_BUILD {want}: {struct.pack('<I',want) in d}")
d=open(f"{D}/nvmini.drv","rb").read()
print(f"nvmini.drv: {len(d)} bytes, VDD id-probe loop at {hex(d.find(bytes.fromhex('66B883000000')))}")
EOF
ls -la "$D"
