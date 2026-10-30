#!/bin/sh
# Repopulate dist/driver from the build tree (binaries, INF, VERSION.txt, nvsetup.reg).
# Run from anywhere: tools/make_dist.sh   (does NOT rebuild; run wmake/mingw first)
#
# Every text file in the package (*.bat *.txt *.reg *.inf) is normalized to DOS text
# format at the end - CRLF line ends, ASCII only - and the script FAILS if that is not
# possible. COMMAND.COM on Windows 98 rejects LF-only batch files with "invalid
# command" (that silently broke the 3096/3097 launchers), and Notepad/EDIT there are
# not UTF-8. Editing these files with Python in text mode or with a Linux editor
# strips the CR; this step puts it back, whatever the source looked like.
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
S="$R/driver/vmdisp9x-nv"
D="$R/dist/driver"
mkdir -p "$D"
cp "$S/nvmini.vxd" "$S/nvmini.drv" "$S/tools/nvprobe.exe" "$S/tools/pftest.exe" "$S/tools/icdload.exe" "$S/tools/vmmap.exe" "$S/tools/pfprobe.exe" "$S/tools/ostest.exe" "$S/vmdisp9x.inf" "$S/VERSION.txt" "$S/nvsetup.reg" "$S/d3dsetup.reg" "$S/nouveau.reg" "$D/"
cp "$R/driver/vmhal9x-bin/vmhal9x.dll" "$R/driver/vmhal9x-bin/vmdisp9x.dll" "$R/driver/vmhal9x-bin/vmhal3d.dll" "$D/"
[ -f "$R/driver/mesa9x-bin/mesa3d.dll" ] && cp "$R/driver/mesa9x-bin/mesa3d.dll" "$D/" || echo "WARNING: driver/mesa9x-bin/mesa3d.dll missing (Direct3D will not work)"
cp "$R/driver/mesa9x-bin/wgltest.exe" "$R/driver/mesa9x-bin/icdtest.exe" "$D/"
for f in INSTALL.txt nvdump.bat nvdump2.bat nvdump3.bat nvgl.bat nvhl.bat; do
  [ -f "$D/$f" ] || git -C "$R" checkout -- "dist/driver/$f" 2>/dev/null || true
done

# --- DOS text format for everything the Win98 machine opens or runs ---------------
cr=$(printf '\r'); tab=$(printf '\t'); ntext=0
for f in "$D"/*.bat "$D"/*.txt "$D"/*.reg "$D"/*.inf; do
  [ -f "$f" ] || continue
  sed -i -e '$a\' "$f"                      # newline at EOF (so the last line gets its CR too)
  sed -i -e "s/$cr\$//" -e "s/\$/$cr/" "$f" # strip any CR, then CRLF on every line
  lines=$(wc -l < "$f"); crs=$(grep -c "$cr\$" "$f" || true)
  if [ "$lines" -ne "$crs" ]; then
    echo "make_dist: $f is not CRLF after normalization ($lines lines, $crs CR)" >&2; exit 1
  fi
  if LC_ALL=C grep -q "[^ -~$tab$cr]" "$f"; then
    echo "make_dist: NON-ASCII (or control) bytes in $f - Win98 cannot display them; fix the source and rerun:" >&2
    LC_ALL=C grep -n "[^ -~$tab$cr]" "$f" | head -5 | cut -c1-120 >&2
    exit 1
  fi
  ntext=$((ntext + 1))
done
echo "DOS text format (CRLF, ASCII) verified on $ntext package text files"

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
