@echo off
rem E3-pre validation dump (read-only). Needs nvmini.vxd build 2006+.
rem Confirms the VBIOS display instance-memory / RAMHT / ctxdma layout and VRAM size.
echo NV EVO instance-memory / ctxdma dump > nvdump3.txt
echo [BUILD CHECK] >> nvdump3.txt
nvprobe >> nvdump3.txt
echo [RAMHT slots - VRAM 0x0] >> nvdump3.txt
nvprobe vramrd 0x0 32 >> nvdump3.txt
echo [ctxdma objects - VRAM 0x1000] >> nvdump3.txt
nvprobe vramrd 0x1000 16 >> nvdump3.txt
echo [RAM_AMOUNT 0x610384 = vram_size-1] >> nvdump3.txt
nvprobe rd 0x610384 1 >> nvdump3.txt
echo Done. Bring back nvdump3.txt
type nvdump3.txt
