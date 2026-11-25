@echo off
rem SPDX-License-Identifier: MIT
rem DirectPlay (Legacy Components) for old games, offline from the Windows image itself (no download). A 2001 game's
rem DirectX check loads dpnhpast.dll: without the feature Windows offers to download it, and "Skip" ends in
rem "requires DirectX 8.1" (fresh r26 install, 2026-11-25). 3010 = enabled, restart wanted: counts as success.
dism /online /enable-feature /featurename:DirectPlay /all /limitaccess /norestart
if %errorlevel%==3010 exit /b 0
exit /b %errorlevel%
