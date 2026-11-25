@echo off
rem Warm restart OFF: Start - Restart restarts the console and you start the Linux loader again, as in 1.0. No admin prompt
rem needed: the PS5 watchdog task (ps5-guard) tells the firmware within a few seconds. Undo with warm-restart-on.cmd.
echo off> C:\ps5\gpu\warm-restart.off || (echo Could not write C:\ps5\gpu\warm-restart.off & pause & exit /b 1)
echo Warm restart is off (applied within 10 seconds). After Restart, start the loader again (as in 1.0).
pause
