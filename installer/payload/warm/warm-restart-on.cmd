@echo off
rem Warm restart ON (the default): Start - Restart comes straight back into Windows, without leaving our firmware
rem and without the loader. No admin prompt needed: the PS5 watchdog task (ps5-guard) tells the firmware within a few seconds.
if exist C:\ps5\gpu\warm-restart.off del C:\ps5\gpu\warm-restart.off
if exist C:\ps5\gpu\warm-restart.off (echo Could not delete C:\ps5\gpu\warm-restart.off & pause & exit /b 1)
echo Warm restart is on (applied within 10 seconds). Shut down still switches the console off.
pause
