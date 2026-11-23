@echo off
rem Switches ps5wddm between its two modes and restarts the display device (no Windows restart).
rem   wddm-mode.cmd full   -> full graphics driver (WDDM 1.3, stage W3; FullVersion=20 in the service key would make it 2.0)
rem   wddm-mode.cmd dod    -> display-only driver, as before (the way back)
setlocal
set V=0
if /i "%1"=="full" set V=1
rem The switch lives in a volatile key: after a Windows restart the driver is display-only again (full mode needs the
rem GPU started first; autostart.cmd switches to full once that is done).
powershell -NoProfile -Command "$k = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SYSTEM\CurrentControlSet\Services\ps5wddm\Session', [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [Microsoft.Win32.RegistryOptions]::Volatile); $k.SetValue('FullDriver', %V%, 'DWord'); $k.Close()"
reg add HKLM\SYSTEM\CurrentControlSet\Services\ps5wddm /v StartPending /t REG_DWORD /d 0 /f
reg delete HKLM\SYSTEM\CurrentControlSet\Services\ps5wddm /v Log /f 2>C:\ps5\gpu\ping.tmp
rem The trace of the run before (a crashed one, perhaps) is kept: the next full-mode start overwrites the file.
if exist C:\ps5\gpu\ps5wddm-trace.log copy /y C:\ps5\gpu\ps5wddm-trace.log C:\ps5\gpu\ps5wddm-trace.prev.log >C:\ps5\gpu\ping.tmp
pnputil /restart-device "PCI\VEN_1002&DEV_13FC&SUBSYS_00001022&REV_00\4&19caf403&0&0041"
