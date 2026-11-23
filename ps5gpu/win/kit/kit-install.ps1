# Installs the automatic game kit (run elevated): the watcher task (SYSTEM, from boot, see below) and the
# right-click entries on .exe files. Files expected in C:\ps5\gpu\kit.
$ErrorActionPreference = 'Stop'
# The watcher (ps5-gamekit.ps1 -Watch): SYSTEM, at boot, no time limit (schtasks /sc would stop it after 72 h),
# restarted if it ever exits. It replaces the earlier logon + 15-minute scan tasks.
foreach ($old in 'ps5-gamekit', 'ps5-gamekit-repeat') {
  if (Get-ScheduledTask -TaskName $old -ErrorAction SilentlyContinue) { Unregister-ScheduledTask -TaskName $old -Confirm:$false }
}
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\ps5\gpu\kit\ps5-gamekit.ps1 -Watch'
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName 'ps5-gamekit-watch' -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName 'ps5-gamekit-watch'
$menu = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\ps5\gpu\kit\kit-menu.ps1 -Exe "%1"'
foreach ($v in @(@('ps5gpu', 'Prepare for PS5 GPU', ''), @('ps5gpuremove', 'Remove PS5 GPU kit', ' -Remove'))) {
  $k = "HKLM:\SOFTWARE\Classes\exefile\shell\$($v[0])"
  New-Item -Path "$k\command" -Force | Out-Null
  Set-ItemProperty -Path $k -Name '(default)' -Value $v[1]
  Set-ItemProperty -Path $k -Name 'HasLUAShield' -Value ''
  Set-ItemProperty -Path "$k\command" -Name '(default)' -Value ($menu + $v[2])
}
'game kit installed'
