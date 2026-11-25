@echo off
rem Starts the GPU (kernel shim, microcode, ring) by itself after Windows has started: task "ps5-gpu-autostart",
rem SYSTEM, at boot (the 30 s wait is in here: with a delay on the trigger the task never fired, 2026-11-20). Safety latch: a marker file is written before the start and removed after it; if the marker
rem is still there at the next boot, the last automatic start never finished (crash or hang), and automatic starting
rem switches itself off (autostart.off) until someone deletes that file. A start that merely fails is tried again.
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
echo %date% %time% > autostart.trying
call C:\ps5\gpu\start.cmd nodemo
set R=%errorlevel%
echo %date% %time% automatic start finished, exit %R% >> autostart.log
rem GPU up: the display driver may now run as a full graphics driver (desktop composed on the GPU). autostart.nofull
rem keeps it display-only.
rem v1.0.1: WDDM mode by the user's choice, applied before every switch to full: 2.0 (native Direct3D 12) by default,
rem 1.3 (as in 1.0) when C:\ps5\gpu\dx12-native.off exists (dx12-native-off.cmd / -on.cmd, no admin needed).
set V=20
if exist C:\ps5\gpu\dx12-native.off set V=13
reg add HKLM\SYSTEM\CurrentControlSet\Services\ps5wddm /v DefaultVersion /t REG_DWORD /d %V% /f >nul 2>&1
echo %date% %time% WDDM mode for this start: %V% >> autostart.log
if "%R%"=="0" if not exist autostart.nofull (
  echo on > C:\ps5\gpu\umd-dwm-on
  call C:\ps5\gpu\wddm-mode.cmd full >> autostart.log 2>&1
  echo %date% %time% display driver switched to full mode >> autostart.log
)
del autostart.trying
exit /b %R%
