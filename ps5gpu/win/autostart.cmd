@echo off
rem Starts the GPU (kernel shim, microcode, ring) by itself after Windows has started: task "ps5-gpu-autostart",
rem SYSTEM, at boot (the 30 s wait is in here: with a delay on the trigger the task never fired, 2026-11-20). Safety latch: a marker file is written before the start and removed after it; if the marker
rem is still there at the next boot, the last automatic start never finished (crash or hang), and automatic starting
rem switches itself off (autostart.off) until someone deletes that file. A start that merely fails is tried again.
rem Since 2026-11-22 the display driver can start the GPU itself at boot (gpu-bootstart.cmd; armed by this script after
rem a good start the old way). When it did, this script only checks the result (kernel shim loaded at boot, full mode ran
rem its first 20 s, GPU healthy) and restarts nothing; if a check fails it runs gpu-recover.cmd (the watchdog's GPU
rem restart, one blink). When it did not, everything below runs as before.
rem Log: C:\ps5\gpu\autostart.log. By hand at any time: C:\ps5\gpu\start.cmd nodemo
setlocal
cd /d C:\ps5\gpu
if exist autostart.off exit /b 0
powershell -NoProfile -Command "Start-Sleep 30"
if exist autostart.trying (
  echo %date% %time% the previous automatic start did not finish: automatic start is now off ^(delete autostart.off to re-arm^) >> autostart.log
  ren autostart.trying autostart.off
  exit /b 1
)
del recover.count 2>C:\ps5\gpu\ping.tmp
set K=HKLM\SYSTEM\CurrentControlSet\Services\ps5wddm
set BOOT=
for /f "tokens=3" %%a in ('reg query %K%\Session /v BootStart 2^>nul ^| find "BootStart"') do set BOOT=%%a
if "%BOOT%"=="0x1" goto booted
for /f "tokens=2,*" %%a in ('reg query %K% /v GpuBootReason 2^>nul ^| find "GpuBootReason"') do echo %date% %time% the display driver did not start the GPU at boot: %%b >> autostart.log
echo %date% %time% > autostart.trying
call C:\ps5\gpu\start.cmd nodemo
set R=%errorlevel%
echo %date% %time% automatic start finished, exit %R% >> autostart.log
rem GPU up: the display driver may now run as a full graphics driver (desktop composed on the GPU). autostart.nofull
rem keeps it display-only. Then arm the driver's own start for the next boot (does nothing if it cannot or is off).
if "%R%"=="0" if not exist autostart.nofull (
  echo on > C:\ps5\gpu\umd-dwm-on
  call C:\ps5\gpu\wddm-mode.cmd full >> autostart.log 2>&1
  echo %date% %time% display driver switched to full mode >> autostart.log
  if exist C:\ps5\gpu\gpu-bootstart.cmd call C:\ps5\gpu\gpu-bootstart.cmd arm >> autostart.log 2>&1
)
del autostart.trying
exit /b %R%

:booted
rem The display driver started the GPU in DriverEntry and came up as the full driver. Was the kernel shim there at boot
rem (the compositor's Direct3D driver needs it), did full mode run its first 20 s (GpuBootTrusted) without being stopped
rem since (Stage 90/91 = stopped), and is the GPU healthy?
set SHIM=0
sc query ps5gpu | find "RUNNING" >C:\ps5\gpu\ping.tmp && set SHIM=1
set W=0
:waittrust
set TRUST=
set STAGE=
for /f "tokens=3" %%a in ('reg query %K% /v GpuBootTrusted 2^>nul ^| find "GpuBootTrusted"') do set TRUST=%%a
for /f "tokens=3" %%a in ('reg query %K% /v Stage 2^>nul ^| find "Stage"') do set STAGE=%%a
if "%TRUST%"=="0x1" goto trusted
if %W% geq 6 goto trusted
set /a W+=1
ping -n 6 127.0.0.1 >C:\ps5\gpu\ping.tmp
goto waittrust
:trusted
ps5gpu.exe load C:\ps5\gpu\ps5gpu.sys >> autostart.log 2>&1
ps5gpu2.exe health >> autostart.log 2>&1
set H=%errorlevel%
echo %date% %time% the display driver started the GPU at boot: kernel shim at boot %SHIM%, full mode trusted %TRUST%, stage %STAGE%, health %H% >> autostart.log
if not "%SHIM%"=="1" goto bootfix
if not "%TRUST%"=="0x1" goto bootfix
if "%STAGE%"=="0x5a" goto bootfix
if "%STAGE%"=="0x5b" goto bootfix
if not "%H%"=="0" if not "%H%"=="3" goto bootfix
echo %date% %time% all well: full mode since boot, no device restart >> autostart.log
exit /b 0
:bootfix
rem Without the shim at boot the boot start is no use: switch it off (gpu-bootstart.cmd on re-arms) and say so.
if not "%SHIM%"=="1" if exist C:\ps5\gpu\gpu-bootstart.cmd call C:\ps5\gpu\gpu-bootstart.cmd off the kernel shim was not loaded at boot >> autostart.log 2>&1
echo %date% %time% after the boot start something is off: restarting the GPU and the full driver ^(gpu-recover.cmd^) >> autostart.log
echo %date% %time% > autostart.trying
call C:\ps5\gpu\gpu-recover.cmd >> autostart.log 2>&1
set R=%errorlevel%
echo %date% %time% recovery finished, exit %R% >> autostart.log
del autostart.trying
exit /b %R%
