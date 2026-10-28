@echo off
rem nvhl.bat - run Half-Life on the GPU (Mesa nv50 through the OpenGL ICD route).
rem
rem Setup (once):
rem   1. copy this file and mesa3d.dll from the driver package into the Half-Life
rem      folder (next to hl.exe); opengl32 loads the ICD named in the registry
rem      (mesa3d.dll) from the game folder first.
rem   2. set NVDRV below to the driver package folder (for nvprobe.exe).
rem Run from the Half-Life folder:  nvhl [hl.exe options]
rem   e.g.  nvhl -width 640 -height 480
rem 3096 A/B switches (before the hl.exe options):
rem   nvhl vsync      present waits for the panel's vertical blank: no tearing, at most 60 fps
rem   nvhl fullflush  whole-buffer flushes instead of range-directed ones (slower)
rem   nvhl vsync fullflush [options]   both
rem 3097: nvhl verify   read every flushed range back from VRAM after each submit (VRAM WARN lines; slower)
rem       nvhl heaphi   use only the 64 MB heap region above the GPU channel (the 3094 layout)
rem       the launcher also dumps the clock registers to C:\nvclk.txt
rem Afterwards send C:\nvshim.log, C:\nvtail.txt, C:\nvkick.txt, C:\nvclk.txt.
set NVDRV=C:\NVMINI
set GALLIUM_DRIVER=nouveau
set MESA_EXTENSION_MAX_YEAR=2000
set NVSHIM_KICKDUMP=64
set NVSHIM_VSYNC=
set NVSHIM_FULLFLUSH=
set NVSHIM_VERIFYVRAM=
set NVSHIM_HEAPHI=
:flags
if "%1"=="vsync" goto f_vsync
if "%1"=="fullflush" goto f_full
if "%1"=="verify" goto f_verify
if "%1"=="heaphi" goto f_heaphi
goto run
:f_vsync
set NVSHIM_VSYNC=1
echo nvhl: vsync on
shift
goto flags
:f_full
set NVSHIM_FULLFLUSH=1
echo nvhl: fullflush on
shift
goto flags
:f_verify
set NVSHIM_VERIFYVRAM=1
echo nvhl: verify on
shift
goto flags
:f_heaphi
set NVSHIM_HEAPHI=1
echo nvhl: heaphi on
shift
goto flags
:run
if exist C:\nvshim.log del C:\nvshim.log
if exist C:\nvkick.txt del C:\nvkick.txt
if exist C:\nvtail.txt del C:\nvtail.txt
if not exist hl.exe echo nvhl: run this from the Half-Life folder (hl.exe not found here)
if not exist hl.exe goto end
if not exist mesa3d.dll echo nvhl: WARNING mesa3d.dll is not in this folder - the ICD will come from C:\WINDOWS\SYSTEM
if exist %NVDRV%\nvprobe.exe %NVDRV%\nvprobe rd 4000 16 > C:\nvclk.txt
if exist %NVDRV%\nvprobe.exe %NVDRV%\nvprobe rd c040 1 >> C:\nvclk.txt
if exist %NVDRV%\nvprobe.exe %NVDRV%\nvprobe rd 22000 1 >> C:\nvclk.txt
if exist %NVDRV%\nvprobe.exe start /min %NVDRV%\nvprobe tail C:\nvtail.txt
start /w hl.exe %1 %2 %3 %4 %5 %6
echo nvhl: Half-Life exited. Logs: C:\nvshim.log C:\nvtail.txt C:\nvkick.txt C:\nvclk.txt
:end
