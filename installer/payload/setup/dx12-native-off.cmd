@echo off
rem Native Direct3D 12 OFF: WDDM 1.3 (as in 1.0) from the next start, e.g. if a program misbehaves in 2.0. No admin
rem prompt needed: the PS5 startup task applies it. Undo with dx12-native-on.cmd.
echo off> C:\ps5\gpu\dx12-native.off || (echo Could not write C:\ps5\gpu\dx12-native.off & pause & exit /b 1)
echo WDDM 1.3 (as in 1.0) from the next start. Restart Windows now. Direct3D 12 games keep working through the game kit.
pause
