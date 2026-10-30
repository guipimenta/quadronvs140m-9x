@echo off
rem nvapp.bat - per-program OpenGL settings for ANY game, without regedit (build 3112)
rem
rem   nvapp sof.exe gpu      run sof.exe on the GPU driver (removes its software override)
rem   nvapp sof.exe sw       run sof.exe on software OpenGL (llvmpipe) - the A/B
rem   nvapp sof.exe y2000    GL_EXTENSIONS year cap 2000 (games that crash at start with an
rem                          "invalid page fault" in an unknown module: Half-Life, SoF, other
rem                          Quake 2 / GoldSrc engines copy the list into a fixed buffer)
rem   nvapp sof.exe noyear   remove the year cap again
rem   nvapp sof.exe show     print the program's settings
rem
rem Takes effect at the program's next start, no reboot. The name is the exe as Windows
rem sees it (sof.exe, hl.exe, quake2.exe). Writes C:\nvapp.reg and imports it silently.
if "%1"=="" goto usage
if "%2"=="" goto usage
if "%2"=="show" goto show
>C:\nvapp.reg echo REGEDIT4
echo.>> C:\nvapp.reg
echo [HKEY_LOCAL_MACHINE\Software\vmdisp9x\apps\%1\mesa]>> C:\nvapp.reg
if "%2"=="gpu" echo "GALLIUM_DRIVER"=->> C:\nvapp.reg
if "%2"=="sw" echo "GALLIUM_DRIVER"="llvmpipe">> C:\nvapp.reg
if "%2"=="y2000" echo "MESA_EXTENSION_MAX_YEAR"="2000">> C:\nvapp.reg
if "%2"=="noyear" echo "MESA_EXTENSION_MAX_YEAR"=->> C:\nvapp.reg
if "%2"=="gpu" goto import
if "%2"=="sw" goto import
if "%2"=="y2000" goto import
if "%2"=="noyear" goto import
goto usage
:import
start /w regedit /s C:\nvapp.reg
echo %1: %2 applied (HKLM\Software\vmdisp9x\apps\%1\mesa)
goto end
:show
if exist C:\nvapp.txt del C:\nvapp.txt
start /w regedit /e C:\nvapp.txt HKEY_LOCAL_MACHINE\Software\vmdisp9x\apps\%1\mesa
if exist C:\nvapp.txt type C:\nvapp.txt
if not exist C:\nvapp.txt echo %1: no settings (follows the global default: GPU if nvgl-on.reg was imported)
echo.
echo global default:
if exist C:\nvapp.txt del C:\nvapp.txt
start /w regedit /e C:\nvapp.txt HKEY_LOCAL_MACHINE\Software\vmdisp9x\apps\global\mesa
if exist C:\nvapp.txt type C:\nvapp.txt
if not exist C:\nvapp.txt echo (none: software OpenGL for everything - import nvgl-on.reg)
goto end
:usage
echo usage: nvapp EXE gpu / sw / y2000 / noyear / show       e.g.  nvapp sof.exe gpu
:end
