@echo off
rem PS5 Windows: one-time setup at the end of the first start (called by SetupComplete.cmd, SYSTEM, no logon yet).
rem Every step is a command that was proven by hand on the console (source in installer/DESIGN.md, section 2.4).
rem A failed step is logged and the rest still runs: Windows always comes up, at worst with the basic display.
rem Log: C:\ps5\install.log. Result: C:\ps5\install-result.txt (one line per step: OK/FAIL, read by Linux read-only).
rem Runs once: C:\ps5\setup\done.txt stops a second run.
rem Called from two places: SetupComplete.cmd (SYSTEM; Windows skips it on an install without a product key) and the
rem unattend's FirstLogonCommands (the ps5 administrator at the first logon). Needs administrator rights.
setlocal EnableExtensions
set LOG=C:\ps5\install.log
set RES=C:\ps5\install-result.txt
if exist C:\ps5\setup\done.txt exit /b 0
rem Not elevated: say so and leave done.txt absent, so the next elevated call still runs everything.
net session >nul 2>&1
if errorlevel 1 (
  >> C:\ps5\install-result.txt echo FAIL not-elevated %date% %time%
  exit /b 1
)
> %LOG% echo ==== ps5-setup %date% %time%
> %RES% echo started %date% %time%

rem 1. trust the project's test certificate (drivers load because BCD testsigning is on and this publisher is trusted).
rem    The installer already put it into Root + TrustedPublisher offline; these are the fallback, and the store is logged
rem    so a driver prompt can be explained (run 3 showed "unverified publisher" twice).
call :step cert certutil -addstore Root C:\ps5\gpu\wddm\ps5test.cer
call :step certpub certutil -f -addstore TrustedPublisher C:\ps5\gpu\wddm\ps5test.cer
>> %LOG% echo ---- trust stores before the driver installs
certutil -store TrustedPublisher >> %LOG% 2>&1
certutil -store Root "PS5GPU Test Signing" >> %LOG% 2>&1

rem 2. display driver ps5wddm on the GPU (PCI 1002:13FC); display-only until autostart.cmd switches it to full
reg add HKLM\SYSTEM\CurrentControlSet\Services\ps5wddm /v StartPending /t REG_DWORD /d 0 /f >> %LOG% 2>&1
call :step wddm pnputil /add-driver C:\ps5\gpu\wddm\ps5wddm.inf /install

rem 3. network: the Ethernet port (p5eth) is a root-enumerated device. No Wi-Fi driver and no network details here:
rem    Wi-Fi comes later from a scanning driver set up inside Windows.
call :step eth C:\ps5\eth\devadd.exe C:\ps5\eth\p5eth.inf root\p5eth

rem 4. HDMI audio (20:00.1, 1002:13EA) has no line interrupt: the inbox HD Audio driver must use MSI (code 12 otherwise)
set A=FAIL
for /f "delims=" %%k in ('reg query "HKLM\SYSTEM\CurrentControlSet\Enum\PCI" /f "VEN_1002&DEV_13EA" /k 2^>nul ^| findstr /b HKEY') do for /f "delims=" %%i in ('reg query "%%k" 2^>nul ^| findstr /b HKEY') do reg add "%%i\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties" /v MSISupported /t REG_DWORD /d 1 /f >> %LOG% 2>&1 && set A=OK
>> %RES% echo %A% audio-msi

rem 5. Vulkan for every program (Khronos loader in System32/SysWOW64, manifests in HKLM)
call :step vulkan cmd /c C:\ps5\gpu\vkreg\vk-install.cmd

rem 6. OpenGL through Zink for programs that use the gl folder's opengl32.dll
call :step gl setx /M GALLIUM_DRIVER zink

rem 7. game kit: the boot-time watcher task ps5-gamekit-watch and the Explorer right-click entries
call :step gamekit cmd /c C:\ps5\gpu\kit\kit-install.cmd

rem 7b. Game Bar / Game DVR off (LTSC has no Game Bar app: every game start asked for an app for ms-gamingoverlay:).
rem     Machine policy + the Default profile, which the ps5 profile is made from at the first logon.
call :step gamebar powershell -NoProfile -ExecutionPolicy Bypass -File C:\ps5\setup\gamebar-off.ps1

rem 7c. Shader cache folders (DXVK, vkd3d-proton, Mesa) and VKD3D_SHADER_CACHE_PATH in the Default profile
call :step shader-cache powershell -NoProfile -ExecutionPolicy Bypass -File C:\ps5\setup\shader-cache-dirs.ps1

rem 8. GPU autostart and the temperature/GPU watchdog, both SYSTEM at boot (XML: no 72-hour limit on the watchdog)
call :step task-autostart schtasks /create /f /tn ps5-gpu-autostart /xml C:\ps5\setup\tasks\ps5-gpu-autostart.xml
call :step task-guard schtasks /create /f /tn ps5-guard /xml C:\ps5\setup\tasks\ps5-guard.xml
rem    Both have boot triggers. Start the temperature watchdog now, so the first session is protected too. The GPU
rem    autostart is NOT started in the first session: switching the just-installed display driver to full mode before
rem    a restart was suspect on the r13 install; READ ME FIRST asks the user to restart once, and it runs from then on.
call :step run-guard schtasks /run /tn ps5-guard

rem 9. Windows Update: registry and services were set offline by the installer; switch its scheduled tasks off too
rem    as SYSTEM: the protected tasks refuse an administrator (run 3: Access is denied on 4 tasks)
call :step wu-tasks cmd /c C:\ps5\setup\wu-tasks-system.cmd

rem 10. power: no hibernation, no sleep, no screen or disk timeout (suspend hangs this console: SAFETY.md, Tier 2)
call :step power-hib powercfg /hibernate off
call :step power-standby powercfg /change standby-timeout-ac 0
call :step power-monitor powercfg /change monitor-timeout-ac 0
call :step power-disk powercfg /change disk-timeout-ac 0

rem 11. autologon for good: the unattend's LogonCount would switch it off after 9999 logons (if OOBE has written it yet;
rem     firstlogon.cmd does the same once more)
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoLogonCount /f >> %LOG% 2>&1
>> %RES% echo OK autologon-count

rem 12. a report of what this install exposes (accounts, listening ports, firewall, RDP, SSH, SMB, Defender, WU)
call :step security-report powershell -NoProfile -ExecutionPolicy Bypass -File C:\ps5\setup\security-report.ps1

>> %RES% echo finished %date% %time%
> C:\ps5\setup\done.txt echo done %date% %time%
>> %LOG% echo ==== finished %date% %time%
exit /b 0

:step
rem :step <name> <command...>   runs the command, logs its output and exit code, one OK/FAIL line in the result file
set NAME=%1
shift
set CMDLINE=
:collect
if "%~1"=="" goto run
set CMDLINE=%CMDLINE% %1
shift
goto collect
:run
>> %LOG% echo ---- %NAME%:%CMDLINE%
%CMDLINE% >> %LOG% 2>&1
if errorlevel 1 (
  >> %RES% echo FAIL %NAME% exit %errorlevel%
  >> %LOG% echo ---- %NAME% FAILED, exit %errorlevel%
) else (
  >> %RES% echo OK %NAME%
)
exit /b 0
