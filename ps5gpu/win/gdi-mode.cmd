@echo off
rem Sets ps5wddm's GDI switches (volatile: gone after a Windows restart) and restarts the display device in full mode.
rem   gdi-mode.cmd accel  kernel-mode GDI acceleration (RenderKm, GDI surfaces, aperture segment)
rem   gdi-mode.cmd accel-nocpu  the same with GDI TEXTURE surfaces not CPU-visible
rem   gdi-mode.cmd 1|3    CDD/DWM interop experiments (see NEXT.md 2026-11-21: not the way)
rem   gdi-mode.cmd 0      off (as before)
setlocal
set I=%1
set A=0
if "%I%"=="" set I=0
set T=1
if /i "%I%"=="accel" (set I=0& set A=1)
if /i "%I%"=="accel-nocpu" (set I=0& set A=1& set T=0)
powershell -NoProfile -Command "$k = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SYSTEM\CurrentControlSet\Services\ps5wddm\Session', [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [Microsoft.Win32.RegistryOptions]::Volatile); $k.SetValue('GdiInterop', [int]'%I%', 'DWord'); $k.SetValue('GdiAccel', [int]'%A%', 'DWord'); $k.SetValue('GdiTexCpu', [int]'%T%', 'DWord'); $k.Close()"
call C:\ps5\gpu\wddm-mode.cmd full
