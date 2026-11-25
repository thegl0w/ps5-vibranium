@echo off
rem After a Windows start: load the kernel shim, start the GPU (microcode, ring), then show the window demo.
rem Needs administrator rights. Output in C:\ps5\gpu\start.log. Not run automatically: started on request
rem (scheduled task "ps5-gpu-start"), so a bad GPU start can never repeat by itself at every logon.
setlocal enabledelayedexpansion
cd /d C:\ps5\gpu
rem The log is appended to (a failed start stays readable after a later one); past 256 KiB it moves to start.prev.log.
if exist start.log for %%s in (start.log) do if %%~zs gtr 262144 move /y start.log start.prev.log >nul
echo ==== %date% %time% >> start.log
ps5gpu.exe load C:\ps5\gpu\ps5gpu.sys >> start.log 2>&1
echo load EXIT=!errorlevel!>> start.log
ps5gpu2.exe run C:\ps5\gpu\fw >> start.log 2>&1
set R=!errorlevel!
echo run EXIT=!R!>> start.log
if not "!R!"=="0" exit /b !R!
if /i "%1"=="nodemo" exit /b 0
call C:\ps5\gpu\vk\demo.cmd 0 1280 720
