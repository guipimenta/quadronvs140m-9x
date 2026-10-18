@echo off
rem NV50/G86 display clock + output dump (read-only). Needs nvmini.vxd build 2004+.
echo NV display clock/output dump > nvdump2.txt
echo [BUILD CHECK] >> nvdump2.txt
nvprobe >> nvdump2.txt
echo [CLOCK/VPLL head0 0x614000] >> nvdump2.txt
nvprobe rd 0x614000 256 >> nvdump2.txt
echo [CLOCK/VPLL head1 0x614800] >> nvdump2.txt
nvprobe rd 0x614800 256 >> nvdump2.txt
echo [DAC0 0x61a000] >> nvdump2.txt
nvprobe rd 0x61a000 64 >> nvdump2.txt
echo [SOR0 0x61c000] >> nvdump2.txt
nvprobe rd 0x61c000 128 >> nvdump2.txt
echo [SOR1 0x61c800] >> nvdump2.txt
nvprobe rd 0x61c800 128 >> nvdump2.txt
echo Done. Bring back nvdump2.txt
type nvdump2.txt
