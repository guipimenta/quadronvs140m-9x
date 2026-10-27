@echo off
rem nvgl.bat - run the GPU (nouveau) OpenGL test and open the shim trace (build 3085+)
rem
rem   nvgl          wgltest window test on the nouveau driver, then Notepad with C:\nvshim.log
rem   nvgl sw       same test on the software renderer (llvmpipe), for comparison
rem   nvgl type     like nvgl, but prints the log in this window instead of Notepad
rem
rem Run it from the folder that holds wgltest.exe and mesa3d.dll. GALLIUM_DRIVER stays
rem set in this DOS box afterwards, so other GL programs started from here use the GPU too.
if exist C:\nvshim.log del C:\nvshim.log
set GALLIUM_DRIVER=nouveau
if "%1"=="sw" set GALLIUM_DRIVER=
if "%1"=="sw" echo [software renderer]
if not "%1"=="sw" echo [nouveau GPU driver]
wgltest driver mesa3d.dll window
if not exist C:\nvshim.log goto nolog
if "%1"=="type" goto typeit
start notepad C:\nvshim.log
goto end
:typeit
type C:\nvshim.log
goto end
:nolog
echo No C:\nvshim.log was written - the nouveau driver was not selected or mesa3d.dll is older than 3082.
:end
