@echo off
rem NV50/G86 MMIO register dump (read-only). Needs the instrumented nvmini.vxd.
echo NV register dump > nvdump.txt
echo [BUILD CHECK - plain probe] >> nvdump.txt
nvprobe >> nvdump.txt
echo [PMC 0x000000] >> nvdump.txt
nvprobe rd 0x0 16 >> nvdump.txt
echo [PBUS 0x001000] >> nvdump.txt
nvprobe rd 0x1000 16 >> nvdump.txt
echo [PTIMER 0x009000] >> nvdump.txt
nvprobe rd 0x9000 16 >> nvdump.txt
echo [PNVIO 0x00e000 I2C/GPIO] >> nvdump.txt
nvprobe rd 0xe000 64 >> nvdump.txt
echo [PFB 0x100000 mem/VRAM] >> nvdump.txt
nvprobe rd 0x100000 64 >> nvdump.txt
echo [PSTRAPS 0x101000] >> nvdump.txt
nvprobe rd 0x101000 8 >> nvdump.txt
echo [PTHERM 0x020000] >> nvdump.txt
nvprobe rd 0x20000 8 >> nvdump.txt
echo [PRMIO 0x601300 VGA] >> nvdump.txt
nvprobe rd 0x601300 32 >> nvdump.txt
echo [PDISPLAY 0x610000 root] >> nvdump.txt
nvprobe rd 0x610000 256 >> nvdump.txt
echo [PDISPLAY 0x610400] >> nvdump.txt
nvprobe rd 0x610400 256 >> nvdump.txt
echo [PDISPLAY 0x610800 heads] >> nvdump.txt
nvprobe rd 0x610800 256 >> nvdump.txt
echo [PDISPLAY 0x610c00] >> nvdump.txt
nvprobe rd 0x610c00 256 >> nvdump.txt
echo Done. Bring back nvdump.txt
type nvdump.txt
