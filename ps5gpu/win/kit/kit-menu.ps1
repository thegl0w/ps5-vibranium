# Right-click helper: asks for elevation (game folders under Program Files need it) and runs the game kit on one exe.
param([string]$Exe, [switch]$Remove)
$a = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"C:\ps5\gpu\kit\ps5-gamekit.ps1`" -Notify -Path `"$Exe`""
if ($Remove) { $a += ' -Remove' }
try { Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList $a } catch {}
