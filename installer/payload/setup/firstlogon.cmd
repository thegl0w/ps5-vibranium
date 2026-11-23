@echo off
rem First logon (from unattend.xml FirstLogonCommands): a device report next to the setup log, for checking an
rem install from Linux (read-only). Power and drivers are done earlier by C:\ps5\setup\ps5-setup.cmd.
(echo first logon %date% %time%
 ver
 pnputil /enum-devices /problem
 pnputil /enum-devices /class Display
 pnputil /enum-devices /class Net
 schtasks /query /tn ps5-guard
 schtasks /query /tn ps5-gpu-autostart) > C:\ps5\firstlogon.txt 2>&1
rem READ ME FIRST on the desktop: updates off on purpose, set a password, anti-cheat warning, the official source.
copy /y C:\ps5\setup\readme-first.txt "%USERPROFILE%\Desktop\READ ME FIRST.txt" >> C:\ps5\firstlogon.txt 2>&1
rem Autologon for good (no AutoLogonCount), and the answer file goes: it may hold the password in Windows' base64 form.
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoLogonCount /f >> C:\ps5\firstlogon.txt 2>&1
for %%f in (C:\Windows\Panther\unattend.xml C:\Windows\Panther\Unattend\unattend.xml C:\Windows\System32\Sysprep\unattend.xml) do if exist %%f del /f /q %%f >> C:\ps5\firstlogon.txt 2>&1
exit /b 0
