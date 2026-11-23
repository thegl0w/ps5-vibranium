@echo off
rem Runs wu-tasks-off.ps1 as SYSTEM through a one-shot scheduled task, then removes the task (as ps5gpu/win/wu-off.cmd
rem did on the dev install). Needed because ps5-setup.cmd runs as the ps5 administrator at the first logon, and the
rem protected Windows Update / UpdateOrchestrator tasks refuse changes from an administrator ("Access is denied" on 4
rem tasks, Toshiba run 3). Exit code = the number of problems wu-tasks-off.ps1 reported (99 = it never finished).
setlocal
del C:\ps5\wu-tasks-off.done 2>nul
schtasks /create /f /tn ps5-wu-tasks-off /ru SYSTEM /sc once /st 00:00 /rl HIGHEST /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\ps5\setup\wu-tasks-off.ps1"
if errorlevel 1 exit /b 98
schtasks /run /tn ps5-wu-tasks-off
powershell -NoProfile -Command "for ($i = 0; $i -lt 120 -and -not (Test-Path 'C:\ps5\wu-tasks-off.done'); $i++) { Start-Sleep 1 }"
schtasks /delete /f /tn ps5-wu-tasks-off
if not exist C:\ps5\wu-tasks-off.done exit /b 99
set /p N=<C:\ps5\wu-tasks-off.done
type C:\ps5\wu-tasks-off.log
exit /b %N%
