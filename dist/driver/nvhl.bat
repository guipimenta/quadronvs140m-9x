@echo off
rem nvhl.bat - run Half-Life on the GPU (Mesa nv50 through the OpenGL ICD route).
rem
rem Setup (once):
rem   1. copy this file and mesa3d.dll from the driver package into the Half-Life
rem      folder (next to hl.exe); opengl32 loads the ICD named in the registry
rem      (mesa3d.dll) from the game folder first. Copy mesa3d.dll again after
rem      every driver update - nvshim.log line 'vxd build / shim build' must show
rem      the same number twice.
rem   2. set NVDRV below to the driver package folder (nvprobe.exe, grctx*.reg).
rem Run from the Half-Life folder:  nvhl [switches] [hl.exe options]
rem   e.g.  nvhl -width 800 -height 600
rem         nvhl grctx 4 present3d -width 1024 -height 768
rem Switches (any order, before the hl.exe options; all off by default):
rem   grctx N     apply %NVDRV%\grctxN.reg first (N = 0..4: the PGRAPH context variant;
rem               since 3104 it is read at every GL program start, no reboot)
rem   barrier     cache barrier (GRAPH SERIALIZE + TEX_CACHE_CTL) before the present blit
rem   present3d   present with the 3D engine (textured quad) instead of the 2D-engine blit
rem   cpupresent  CPU readback + copy instead of the GPU present (slow; the reference picture)
rem   vsync       the present waits for the panel's vertical blank (no tearing, max 60 fps)
rem   fullflush   whole-buffer dirty diffs instead of range-directed ones (slower)
rem   verify      read every flushed range back from VRAM after each submit ('VRAM WARN'; slow)
rem   kickdump N  dump the first N submits to C:\nvkick.txt (default 64)
rem (3097's 'heaphi' is gone: it starved the HUD sprites of VRAM and proved nothing.)
rem Logs: C:\nvshim.log (Mesa/shim), C:\nvtail.txt (VxD ring, written through while the
rem game runs, survives a crash), C:\nvkick.txt (submit dump), C:\nvclk.txt (clocks +
rem VxD probe). Send all four.
set NVDRV=C:\NVMINI
set GALLIUM_DRIVER=nouveau
set MESA_EXTENSION_MAX_YEAR=2000
set NVSHIM_KICKDUMP=64
set NVSHIM_VSYNC=
set NVSHIM_FULLFLUSH=
set NVSHIM_VERIFYVRAM=
set NVSHIM_HEAPHI=
set NVSHIM_PRESENT_BARRIER=
set NVSHIM_PRESENT_3D=
set NVSHIM_CPU_PRESENT=
set NVHL_GRCTX=
:flags
if "%1"=="grctx" goto f_grctx
if "%1"=="barrier" goto f_barrier
if "%1"=="present3d" goto f_present3d
if "%1"=="cpupresent" goto f_cpupresent
if "%1"=="vsync" goto f_vsync
if "%1"=="fullflush" goto f_full
if "%1"=="verify" goto f_verify
if "%1"=="kickdump" goto f_kickdump
if "%1"=="heaphi" goto f_heaphi
goto run
:f_grctx
set NVHL_GRCTX=%2
echo nvhl: PGRAPH context variant %2
shift
shift
goto flags
:f_barrier
set NVSHIM_PRESENT_BARRIER=1
echo nvhl: present barrier on
shift
goto flags
:f_present3d
set NVSHIM_PRESENT_3D=1
echo nvhl: 3D-engine present on
shift
goto flags
:f_cpupresent
set NVSHIM_CPU_PRESENT=1
echo nvhl: CPU present on
shift
goto flags
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
:f_kickdump
set NVSHIM_KICKDUMP=%2
echo nvhl: kick dump %2 submits
shift
shift
goto flags
:f_heaphi
echo nvhl: 'heaphi' is no longer a switch (it starved the HUD sprites) - ignored
shift
goto flags
:run
if not "%GALLIUM_DRIVER%"=="nouveau" echo nvhl: WARNING out of environment space - the switches did not take (give this DOS box a bigger initial environment)
if not exist hl.exe echo nvhl: run this from the Half-Life folder (hl.exe not found here)
if not exist hl.exe goto end
if not exist mesa3d.dll echo nvhl: WARNING mesa3d.dll is not in this folder - the ICD will come from C:\WINDOWS\SYSTEM
if not exist %NVDRV%\nvprobe.exe echo nvhl: WARNING %NVDRV%\nvprobe.exe not found - no VxD log tail, no clock dump (set NVDRV in nvhl.bat)
if "%NVHL_GRCTX%"=="" goto nogrctx
if not exist %NVDRV%\grctx%NVHL_GRCTX%.reg echo nvhl: WARNING %NVDRV%\grctx%NVHL_GRCTX%.reg not found - context variant NOT applied
if exist %NVDRV%\grctx%NVHL_GRCTX%.reg start /w regedit /s %NVDRV%\grctx%NVHL_GRCTX%.reg
:nogrctx
if exist C:\nvtail.run del C:\nvtail.run
if exist C:\nvshim.log del C:\nvshim.log
if exist C:\nvkick.txt del C:\nvkick.txt
if exist C:\nvtail.txt del C:\nvtail.txt
if exist %NVDRV%\nvprobe.exe %NVDRV%\nvprobe rd 4000 16 > C:\nvclk.txt
if exist %NVDRV%\nvprobe.exe %NVDRV%\nvprobe rd c040 1 >> C:\nvclk.txt
if exist %NVDRV%\nvprobe.exe %NVDRV%\nvprobe rd 22000 1 >> C:\nvclk.txt
if exist %NVDRV%\nvprobe.exe %NVDRV%\nvprobe rd e81c 1 >> C:\nvclk.txt
if exist %NVDRV%\nvprobe.exe %NVDRV%\nvprobe rd 101000 1 >> C:\nvclk.txt
if exist %NVDRV%\nvprobe.exe %NVDRV%\nvprobe >> C:\nvclk.txt
echo nvhl > C:\nvtail.run
if exist %NVDRV%\nvprobe.exe start /min %NVDRV%\nvprobe tail C:\nvtail.txt C:\nvtail.run
start /w hl.exe %1 %2 %3 %4 %5 %6 %7 %8 %9
if exist C:\nvtail.run del C:\nvtail.run
echo nvhl: Half-Life exited. Logs: C:\nvshim.log C:\nvtail.txt C:\nvkick.txt C:\nvclk.txt
:end
