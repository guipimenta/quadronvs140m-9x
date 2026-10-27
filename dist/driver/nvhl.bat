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
rem Afterwards send C:\nvshim.log, C:\nvtail.txt, C:\nvkick.txt.
set NVDRV=C:\NVMINI
set GALLIUM_DRIVER=nouveau
set MESA_EXTENSION_MAX_YEAR=2000
set NVSHIM_KICKDUMP=64
if exist C:\nvshim.log del C:\nvshim.log
if exist C:\nvkick.txt del C:\nvkick.txt
if exist C:\nvtail.txt del C:\nvtail.txt
if not exist hl.exe echo nvhl: run this from the Half-Life folder (hl.exe not found here)
if not exist hl.exe goto end
if not exist mesa3d.dll echo nvhl: WARNING mesa3d.dll is not in this folder - the ICD will come from C:\WINDOWS\SYSTEM
if exist %NVDRV%\nvprobe.exe start /min %NVDRV%\nvprobe tail C:\nvtail.txt
start /w hl.exe %1 %2 %3 %4 %5 %6
echo nvhl: Half-Life exited. Logs: C:\nvshim.log C:\nvtail.txt C:\nvkick.txt
:end
