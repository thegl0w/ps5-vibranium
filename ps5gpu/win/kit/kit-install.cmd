@echo off
rem Run elevated. Installs the game-kit tasks and right-click entries (see kit-install.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0kit-install.ps1"
