@echo off
rem nvsky.bat - the 'sky' GPU test at a fullscreen mode (build 3100): a GoldSrc-like
rem skybox + floor + near wall, camera turning, cleared to MAGENTA every frame. Any
rem magenta block on screen = pixels the GPU did not cover (the 1024x768 corner
rem artifacts); anything that is not a checkerboard = sampled from the wrong place.
rem Alt+F4 or Esc ends it.
rem
rem   nvsky               1024x768, 16 bpp (Half-Life's mode)
rem   nvsky 800 600       800x600, 16 bpp
rem   nvsky 1024 768 32   32 bpp
rem
rem Run it from the folder that holds wgltest.exe, mesa3d.dll and nvprobe.exe.
rem Logs: C:\nvshim.log, C:\nvtail.txt, C:\nvkick.txt (first 64 kicks).
set W=%1
set H=%2
set B=%3
if "%W%"=="" set W=1024
if "%H%"=="" set H=768
if "%B%"=="" set B=16
if exist C:\nvshim.log del C:\nvshim.log
if exist C:\nvkick.txt del C:\nvkick.txt
if exist C:\nvtail.txt del C:\nvtail.txt
set GALLIUM_DRIVER=nouveau
set NVSHIM_KICKDUMP=64
echo [nouveau GPU driver] sky scene at %W%x%H% %B% bpp - watch the four screen corners
start /min nvprobe tail C:\nvtail.txt
start /w wgltest driver mesa3d.dll fullscreen width %W% height %H% bpp %B% sky
echo nvsky: done. Logs: C:\nvshim.log C:\nvtail.txt C:\nvkick.txt
