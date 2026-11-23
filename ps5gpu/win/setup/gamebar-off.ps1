# gamebar-off.ps1 (run elevated, once): Game Bar / Game DVR off for good. LTSC has no Game Bar app, so every game start
# made Windows open an "ms-gamingoverlay:" link and show "You'll need a new app to open this ms-gamingoverlay link".
# Machine policy + the current user's settings + the default profile (for users created later). Persistent registry.
$ErrorActionPreference = 'Continue'
function Set-Dword($path, $name, $value) {
  if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
  New-ItemProperty -Path $path -Name $name -Value $value -PropertyType DWord -Force | Out-Null
  "{0}\{1} = {2}" -f $path, $name, $value
}
# Machine: Group Policy "Windows Game Recording and Broadcasting" disabled.
Set-Dword 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\GameDVR' 'AllowGameDVR' 0
Set-Dword 'HKLM:\SOFTWARE\Microsoft\PolicyManager\default\ApplicationManagement\AllowGameDVR' 'value' 0
# Per user: capture off, Game Bar off, no "game mode" overlay tips.
$userKeys = @(
  @('System\GameConfigStore', 'GameDVR_Enabled', 0),
  @('SOFTWARE\Microsoft\Windows\CurrentVersion\GameDVR', 'AppCaptureEnabled', 0),
  @('SOFTWARE\Microsoft\GameBar', 'UseNexusForGameBarEnabled', 0),
  @('SOFTWARE\Microsoft\GameBar', 'ShowStartupPanel', 0),
  @('SOFTWARE\Microsoft\GameBar', 'AutoGameModeEnabled', 0)
)
foreach ($k in $userKeys) { Set-Dword ("HKCU:\" + $k[0]) $k[1] $k[2] }
# The default profile, so accounts created later start with the same settings.
$hive = 'C:\Users\Default\NTUSER.DAT'
if (Test-Path $hive) {
  reg load HKU\P5Default $hive | Out-Null
  foreach ($k in $userKeys) { Set-Dword ("Registry::HKEY_USERS\P5Default\" + $k[0]) $k[1] $k[2] }
  [gc]::Collect(); Start-Sleep -Milliseconds 500
  reg unload HKU\P5Default | Out-Null
}
# A do-nothing handler for the link itself, in case some program still opens it: no "pick an app" prompt.
$cls = 'HKLM:\SOFTWARE\Classes\ms-gamingoverlay'
if (-not (Test-Path "$cls\shell\open\command")) { New-Item -Path "$cls\shell\open\command" -Force | Out-Null }
Set-ItemProperty -Path $cls -Name '(default)' -Value 'URL:ms-gamingoverlay (disabled)'
New-ItemProperty -Path $cls -Name 'URL Protocol' -Value '' -PropertyType String -Force | Out-Null
New-ItemProperty -Path $cls -Name 'NoOpenWith' -Value '' -PropertyType String -Force | Out-Null
Set-ItemProperty -Path "$cls\shell\open\command" -Name '(default)' -Value 'C:\Windows\System32\cmd.exe /d /c exit 0'
"ms-gamingoverlay: handled by a no-op"
