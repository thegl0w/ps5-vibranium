@echo off
rem Registers the PS5 Vulkan driver for every program on this Windows, 64- and 32-bit, the way GPU vendors do:
rem the Khronos loader in System32 / SysWOW64 and the driver manifests in HKLM\SOFTWARE\Khronos\Vulkan\Drivers
rem (and WOW6432Node). Then no game needs an app-local vulkan-1.dll or the VK_DRIVER_FILES variable, elevated or not.
rem Run elevated from the folder holding vulkan-1.x64.dll, vulkan-1.x86.dll and ps5_icd.json.
setlocal
cd /d "%~dp0"
copy /y vulkan-1.x64.dll %SystemRoot%\System32\vulkan-1.dll >nul || exit /b 1
copy /y vulkan-1.x86.dll %SystemRoot%\SysWOW64\vulkan-1.dll >nul || exit /b 1
copy /y ps5_icd.json C:\ps5\gpu\vk\ps5_icd.json >nul || exit /b 1
copy /y ps5_icd.json C:\ps5\gpu\vk32\ps5_icd.json >nul || exit /b 1
reg add HKLM\SOFTWARE\Khronos\Vulkan\Drivers /v C:\ps5\gpu\vk\ps5_icd.json /t REG_DWORD /d 0 /f >nul || exit /b 1
reg add HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers /v C:\ps5\gpu\vk32\ps5_icd.json /t REG_DWORD /d 0 /f >nul || exit /b 1
echo Vulkan driver registered (64- and 32-bit).
