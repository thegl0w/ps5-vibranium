@echo off
rem Brings a stuck GPU back without restarting Windows (called by the watchdog ps5guard.cmd, or by hand, elevated):
rem display driver to display-only, end every program that has the GPU open (the compositor is one of them in full
rem mode: Windows starts it again), restart the GPU (start.cmd: command-processor reset, microcode, ring,
rem self-test), then full mode again. At most three times per Windows start (recover.count; autostart.cmd clears it).
setlocal
cd /d C:\ps5\gpu
set N=0
if exist recover.count set /p N=<recover.count
set /a N+=1
>recover.count echo %N%
echo %date% %time% GPU recovery number %N%
call C:\ps5\gpu\wddm-mode.cmd dod
if %N% gtr 3 echo more than three recoveries since Windows started: staying display-only, GPU left as it is& exit /b 1
rem Who has the GPU open is in the library's own shared state (tasklist /m hung here on 2026-11-21).
for /f %%p in ('ps5gpu2.exe clients 2^>nul') do echo   ending pid %%p & taskkill /f /pid %%p
powershell -NoProfile -Command "Start-Sleep 3"
ps5gpu2.exe housekeeping
call C:\ps5\gpu\start.cmd nodemo
if errorlevel 1 echo %date% %time% the GPU restart FAILED (see start.log): staying display-only& exit /b 1
if exist umd-dwm-on if not exist autostart.nofull call C:\ps5\gpu\wddm-mode.cmd full
echo %date% %time% GPU recovery done
exit /b 0
