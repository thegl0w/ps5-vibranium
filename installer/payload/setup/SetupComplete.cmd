@echo off
rem Windows Setup runs this once, as SYSTEM, at the end of the first start (before the first logon). The installer
rem puts it at C:\Windows\Setup\Scripts\SetupComplete.cmd. All the work is in C:\ps5\setup\ps5-setup.cmd.
if exist C:\ps5\setup\ps5-setup.cmd call C:\ps5\setup\ps5-setup.cmd
exit /b 0
