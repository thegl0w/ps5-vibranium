@echo off
rem Native Direct3D 12 ON (the default): WDDM 2.0 from the next start. No admin prompt needed: the PS5 startup task
rem applies it.
del C:\ps5\gpu\dx12-native.off 2>nul
if exist C:\ps5\gpu\dx12-native.off (echo Could not remove C:\ps5\gpu\dx12-native.off & pause & exit /b 1)
echo Native Direct3D 12 (WDDM 2.0) is ON from the next start. Restart Windows now.
pause
