# shader-cache-dirs.ps1 (run elevated, once; again after adding users): the per-user shader cache folders the game
# layers need. DXVK caches in %LOCALAPPDATA%\dxvk but does not create it; vkd3d-proton caches in the working directory
# (often a Program Files folder a normal user cannot write) unless VKD3D_SHADER_CACHE_PATH is set. Mesa's own cache
# (our Vulkan and Direct3D drivers) goes to %LOCALAPPDATA%\mesa_shader_cache. Every profile + the default profile.
$ErrorActionPreference = 'Continue'
$subdirs = 'dxvk', 'vkd3d-proton', 'mesa_shader_cache'
$profiles = Get-ChildItem C:\Users -Directory -Force | Where-Object { Test-Path (Join-Path $_.FullName 'NTUSER.DAT') }
foreach ($p in $profiles) {
  foreach ($s in $subdirs) {
    $d = Join-Path $p.FullName "AppData\Local\$s"
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null; "created $d" } else { "exists  $d" }
  }
}
function Set-Env($root) {
  $k = "$root\Environment"
  if (-not (Test-Path $k)) { New-Item -Path $k -Force | Out-Null }
  New-ItemProperty -Path $k -Name 'VKD3D_SHADER_CACHE_PATH' -Value '%LOCALAPPDATA%\vkd3d-proton' -PropertyType ExpandString -Force | Out-Null
  "$k\VKD3D_SHADER_CACHE_PATH = %LOCALAPPDATA%\vkd3d-proton"
}
Set-Env 'HKCU:'
reg load HKU\P5Default C:\Users\Default\NTUSER.DAT | Out-Null
Set-Env 'Registry::HKEY_USERS\P5Default'
[gc]::Collect(); Start-Sleep -Milliseconds 500
reg unload HKU\P5Default | Out-Null
