# SPDX-License-Identifier: MIT
# Long-lived install test: one CSV line per boot with free space and the sizes of everything that can grow.
# Runs at startup as SYSTEM (scheduled task ps5-longrun, see LONGRUN-TESTS.md). Read-only apart from its own CSV.
$ErrorActionPreference = 'SilentlyContinue'
$dir = 'C:\ps5\longrun'; $csv = Join-Path $dir 'boots.csv'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
Start-Sleep 60   # let startup settle (autostart, GPU start) before measuring

function SizeMB($p) {
  if (-not (Test-Path $p)) { return 0 }
  $i = Get-Item $p -Force
  if ($i.PSIsContainer) { $s = (Get-ChildItem $p -Recurse -Force -File | Measure-Object Length -Sum).Sum } else { $s = $i.Length }
  [math]::Round(($s / 1MB), 1)
}
function CacheMB {
  $t = 0
  foreach ($u in Get-ChildItem 'C:\Users' -Directory -Force) {
    foreach ($c in 'AppData\Local\dxvk', 'AppData\Local\vkd3d-proton', 'AppData\Local\mesa_shader_cache', 'AppData\Local\Temp') {
      $t += SizeMB (Join-Path $u.FullName $c)
    }
  }
  $t
}

$n = 1; if (Test-Path $csv) { $n = (Get-Content $csv | Measure-Object).Count }   # header line counts as 1
$c = Get-Volume -DriveLetter C
$boot = Get-Volume | Where-Object { $_.FileSystemLabel -eq 'PS5BOOT' } | Select-Object -First 1
$up = [math]::Round(((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds)
$row = [ordered]@{
  boot = $n; time = (Get-Date -Format s); uptime_s = $up
  c_free_mb = [math]::Round($c.SizeRemaining / 1MB); ps5boot_free_mb = if ($boot) { [math]::Round($boot.SizeRemaining / 1MB) } else { -1 }
  pagefile_mb = SizeMB 'C:\pagefile.sys'; hiberfil_mb = SizeMB 'C:\hiberfil.sys'
  minidump_mb = SizeMB 'C:\Windows\Minidump'; memdmp_mb = SizeMB 'C:\Windows\MEMORY.DMP'
  winlogs_mb = SizeMB 'C:\Windows\Logs'; cbs_mb = SizeMB 'C:\Windows\Logs\CBS'; wintemp_mb = SizeMB 'C:\Windows\Temp'
  usercache_mb = CacheMB; ps5_mb = SizeMB 'C:\ps5'
}
if (-not (Test-Path $csv)) { ($row.Keys -join ',') | Set-Content $csv }
($row.Values -join ',') | Add-Content $csv
