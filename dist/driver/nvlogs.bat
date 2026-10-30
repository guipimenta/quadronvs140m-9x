@echo off
rem nvlogs.bat - save the driver's logs under a name before the next OpenGL program
rem overwrites them (build 3112). Run it right after a hang/crash, from any folder:
rem
rem   nvlogs sof1        copies the logs to C:\NVLOGS\sof1\
rem   nvlogs             copies them to C:\NVLOGS\last\
rem
rem Collected: C:\nvshim.log, C:\nvlast.txt, C:\nvkick.txt, C:\nvtail.txt, C:\nvclk.txt and wgltest_load.txt / wgltest_report.txt from the current folder.
rem Then copy the C:\NVLOGS folder off the machine and send it.
set NVLOGDIR=C:\NVLOGS\last
if not "%1"=="" set NVLOGDIR=C:\NVLOGS\%1
if not exist C:\NVLOGS\nul mkdir C:\NVLOGS
if not exist %NVLOGDIR%\nul mkdir %NVLOGDIR%
if exist C:\nvshim.log copy C:\nvshim.log %NVLOGDIR% > nul
if exist C:\nvlast.txt copy C:\nvlast.txt %NVLOGDIR% > nul
if exist C:\nvkick.txt copy C:\nvkick.txt %NVLOGDIR% > nul
if exist C:\nvtail.txt copy C:\nvtail.txt %NVLOGDIR% > nul
if exist C:\nvclk.txt copy C:\nvclk.txt %NVLOGDIR% > nul
if exist wgltest_load.txt copy wgltest_load.txt %NVLOGDIR% > nul
if exist wgltest_report.txt copy wgltest_report.txt %NVLOGDIR% > nul
echo saved to %NVLOGDIR%:
dir /b %NVLOGDIR%
set NVLOGDIR=
