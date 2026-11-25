<#
ps5-gamekit.ps1 - makes Direct3D games use the PS5 GPU without manual steps.
For every game program that uses Direct3D 8/9/10/11/12 or DXGI it places DXVK (D3D8-11 -> Vulkan) and vkd3d-proton
(D3D12 -> Vulkan) next to the program, matching its bitness; DirectDraw/Direct3D 7 games also get dgVoodoo2 (-> D3D11)
when it is present in C:\ps5\gpu\dgvoodoo. Vulkan itself is registered system-wide (vk-install.cmd).
It never overwrites a DLL it did not put there (an identical copy is adopted), records what it placed
(ps5gpu-kit.txt) so -Remove takes exactly that away again, and refreshes its files when the kit is updated.
  ps5-gamekit.ps1 -Scan                  all library folders (Steam, Epic, GOG, C:\Games, roots.txt); used by the task
  ps5-gamekit.ps1 -Path <exe|dir>        one game (right-click "Prepare for PS5 GPU"); an exe is prepared even when
                                         detection finds no Direct3D in it
  ps5-gamekit.ps1 -Path <exe|dir> -Remove   takes the kit away; the scan then leaves that folder alone (skip.txt)
  -Notify                                show the result in a message box (used by the right-click entries)
  ps5-gamekit.ps1 -Watch                 runs forever (SYSTEM task at boot): full scan, then prepares a game folder as
                                         soon as an .exe or a folder appears in a library and the folder has been quiet
                                         for 8 s; new libraries every 10 min, full fallback scan every 6 h
#>
param([string]$Path, [switch]$Scan, [switch]$Remove, [switch]$Notify, [switch]$Watch)
$ErrorActionPreference = 'Continue'
$Home5 = 'C:\ps5\gpu\kit'
$Log = Join-Path $Home5 'gamekit.log'
$StateFile = Join-Path $Home5 'state.txt'
$SkipFile = Join-Path $Home5 'skip.txt'
$KitDir = @{ 0x8664 = 'C:\ps5\gpu\d3d'; 0x14C = 'C:\ps5\gpu\d3d32' }
$VooDir = @{ 0x8664 = 'C:\ps5\gpu\dgvoodoo\x64'; 0x14C = 'C:\ps5\gpu\dgvoodoo\x86' }
$VooConf = 'C:\ps5\gpu\dgvoodoo\dgVoodoo.conf'
$KitFiles = @('d3d8.dll','d3d9.dll','d3d10core.dll','d3d11.dll','dxgi.dll','d3d12.dll','d3d12core.dll')
$VooFiles = @('DDraw.dll','D3DImm.dll')
$Marker = 'ps5gpu-kit.txt'
$D3dNames = @('d3d8.dll','d3d9.dll','d3d10.dll','d3d10_1.dll','d3d10core.dll','d3d11.dll','dxgi.dll','d3d12.dll')
$SkipDirs = '\\(_?CommonRedist|redist|DirectX|vcredist|EasyAntiCheat|EasyAntiCheat_EOS|BattlEye|__Installer|Installers?|Support|Prerequisites|Engine\\Extras)(\\|$)'
$SkipExe = '^(unins|setup|install|UnityCrashHandler|CrashReport|CrashSender|UE4PrereqSetup|UEPrereqSetup|dxsetup|vc_redist|DXSETUP|EpicGamesLauncher|EpicWebHelper|UnrealCEFSubProcess|QtWebEngineProcess|steam|steamwebhelper|GalaxyClient|dgVoodooCpl)'

function Log($m) {
  try {
    if ((Test-Path $Log) -and (Get-Item $Log).Length -gt 2MB) { Move-Item $Log "$Log.old" -Force }
    Add-Content -Path $Log -Value ("{0} {1}" -f (Get-Date -Format s), $m)
  } catch {}
}

# PE: machine and the DLL names in the import and delay-import tables. Reads only the headers and the sections that
# hold those tables (big engine executables keep them past 64 MB). -Strings also returns the Direct3D DLL names found
# as text in the data sections (programs that LoadLibrary Direct3D instead of importing it, e.g. Unreal Engine).
function Get-PeInfo([string]$file, [switch]$Strings) {
  $fs = $null
  try {
    $fs = [IO.File]::Open($file, 'Open', 'Read', 'ReadWrite')
    function ReadAt([long]$off, [long]$cnt) {
      if ($off -lt 0 -or $off -ge $fs.Length -or $cnt -le 0) { return ,([byte[]]@()) }
      $cnt = [Math]::Min($cnt, $fs.Length - $off); $buf = New-Object byte[] ([int]$cnt)
      [void]$fs.Seek($off, 'Begin'); $got = 0
      while ($got -lt $cnt) { $r = $fs.Read($buf, $got, [int]$cnt - $got); if ($r -le 0) { break }; $got += $r }
      return ,$buf
    }
    $b = ReadAt 0 65536
    if ($b.Length -lt 0x200 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return $null }
    $e = [BitConverter]::ToInt32($b, 0x3C); if ($e -le 0 -or $e + 0x200 -gt $b.Length) { return $null }
    if ([BitConverter]::ToUInt32($b, $e) -ne 0x4550) { return $null }
    $machine = [BitConverter]::ToUInt16($b, $e + 4); $nsec = [BitConverter]::ToUInt16($b, $e + 6)
    $optSize = [BitConverter]::ToUInt16($b, $e + 20); $opt = $e + 24
    $magic = [BitConverter]::ToUInt16($b, $opt)
    $dd = if ($magic -eq 0x20B) { $opt + 112 } else { $opt + 96 }
    $st = $opt + $optSize; if ($st + 40 * $nsec -gt $b.Length) { return $null }
    $secs = @()
    for ($i = 0; $i -lt $nsec; $i++) { $o = $st + 40 * $i
      $secs += ,@([BitConverter]::ToUInt32($b, $o + 12), [Math]::Max([BitConverter]::ToUInt32($b, $o + 8), [BitConverter]::ToUInt32($b, $o + 16)),
                  [BitConverter]::ToUInt32($b, $o + 20), [BitConverter]::ToUInt32($b, $o + 16), [BitConverter]::ToUInt32($b, $o + 36)) }
    $cache = @{}
    function At([uint32]$rva, [int]$need) {      # -> @(buffer, offset) for an RVA, loading its section once
      for ($i = 0; $i -lt $secs.Count; $i++) { $s = $secs[$i]
        if ($rva -ge $s[0] -and $rva -lt $s[0] + $s[1]) {
          if (-not $cache.ContainsKey($i)) { $cache[$i] = ReadAt $s[2] ([Math]::Min([long]$s[3], 256MB)) }
          $o = [int]($rva - $s[0]); if ($o + $need -gt $cache[$i].Length) { return $null }
          return ,@($cache[$i], $o) } }
      return $null
    }
    function U32([uint32]$rva) { $a = At $rva 4; if (-not $a) { return [uint32]0 }; return [BitConverter]::ToUInt32($a[0], $a[1]) }
    function Str([uint32]$rva) { $a = At $rva 1; if (-not $a) { return '' }; $x = $a[0]; $k = $a[1]
      while ($k -lt $x.Length -and $x[$k] -ne 0 -and $k - $a[1] -lt 260) { $k++ }; return [Text.Encoding]::ASCII.GetString($x, $a[1], $k - $a[1]) }
    $names = New-Object System.Collections.Generic.List[string]
    $imp = [BitConverter]::ToUInt32($b, $dd + 8)
    if ($imp) { for ($r = $imp; ; $r += 20) { if (-not (At $r 20)) { break }; $n = U32 ($r + 12); if (-not $n) { break }; $names.Add((Str $n).ToLower()) } }
    $dl = [BitConverter]::ToUInt32($b, $dd + 13 * 8)
    if ($dl) { for ($r = $dl; ; $r += 32) { if (-not (At $r 32)) { break }; $n = U32 ($r + 4); if (-not $n) { break }
      if (-not ((U32 $r) -band 1)) { $n = [uint32]($n - [BitConverter]::ToUInt32($b, $opt + 28)) }   # old VA-based entry (32-bit)
      $names.Add((Str $n).ToLower()) } }
    $found = New-Object System.Collections.Generic.List[string]
    if ($Strings) {
      $latin = [Text.Encoding]::GetEncoding(28591)
      for ($i = 0; $i -lt $secs.Count; $i++) { $s = $secs[$i]
        if (($s[4] -band 0x20000020) -or -not ($s[4] -band 0x40) -or $s[3] -eq 0) { continue }   # skip code, keep initialized data
        if (-not $cache.ContainsKey($i)) { $cache[$i] = ReadAt $s[2] ([Math]::Min([long]$s[3], 256MB)) }
        $t = $latin.GetString($cache[$i])
        foreach ($d in $D3dNames + 'ddraw.dll') {
          if ($found -contains $d) { continue }
          $w = ($d.ToCharArray() | ForEach-Object { "$_`0" }) -join ''
          if ($t.IndexOf($d, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or $t.IndexOf($w, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $found.Add($d) } } }
    }
    return @{ Machine = $machine; Imports = $names; Strings = $found }
  } catch { return $null } finally { if ($fs) { $fs.Close() } }
}

function Read-State { $h = @{}; if (Test-Path $StateFile) { foreach ($l in Get-Content $StateFile) { $p = $l.Split("`t"); if ($p.Count -eq 2) { $h[$p[0]] = $p[1] } } }; return $h }
function Save-State($h) { try { $h.GetEnumerator() | ForEach-Object { "$($_.Key)`t$($_.Value)" } | Set-Content $StateFile } catch {} }
function Read-Skip { if (Test-Path $SkipFile) { return @(Get-Content $SkipFile | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim().ToLower() }) }; return @() }
function Write-Skip($l) { try { Set-Content -Path $SkipFile -Value @($l | Where-Object { $_ } | Sort-Object -Unique) } catch {} }

# Changes whenever a kit file is replaced, so prepared games get the new files on the next scan.
function Kit-Version {
  $parts = @(foreach ($d in @($KitDir.Values) + @($VooDir.Values)) { foreach ($f in $KitFiles + $VooFiles) { $x = Join-Path $d $f; if (Test-Path $x) { $i = Get-Item $x; "$x|$($i.Length)|$($i.LastWriteTimeUtc.Ticks)" } } })
  if (Test-Path $VooConf) { $i = Get-Item $VooConf; $parts += "conf|$($i.Length)|$($i.LastWriteTimeUtc.Ticks)" }
  $md5 = [Security.Cryptography.MD5]::Create()
  return ([BitConverter]::ToString($md5.ComputeHash([Text.Encoding]::UTF8.GetBytes(($parts -join ';')))) -replace '-','').Substring(0, 12)
}

# What a program needs: 'd3d' (DXVK/vkd3d), 'ddraw' (dgVoodoo2 + DXVK) or $null. Looks at its imports, then at
# game DLLs next to it that it imports (UnityPlayer.dll and the like), then at DLL names stored as text.
function Get-Need([string]$exe, $pe) {
  $dir = Split-Path $exe -Parent
  $all = @($pe.Imports)
  foreach ($m in @($pe.Imports | Where-Object { $_ -like '*.dll' -and $KitFiles -notcontains $_ -and $VooFiles -notcontains $_ } | Select-Object -First 8)) {
    $f = Join-Path $dir $m
    if (Test-Path $f -PathType Leaf) { $sub = Get-PeInfo $f -Strings; if ($sub) { $all += $sub.Imports; $all += $sub.Strings } }
  }
  if ($all | Where-Object { $D3dNames -contains $_ }) { $need = 'd3d' } elseif ($all -contains 'ddraw.dll') { $need = 'ddraw' } else { $need = $null }
  if (-not $need) {
    $st = Get-PeInfo $exe -Strings
    if ($st -and ($st.Strings | Where-Object { $D3dNames -contains $_ })) { $need = 'd3d' } elseif ($st -and $st.Strings -contains 'ddraw.dll') { $need = 'ddraw' }
  }
  # ddraw + Direct3D: a DirectDraw game only when the program itself imports no Direct3D 8+ DLL (DX8/9 games
  # often import ddraw.dll for queries only; they need DXVK alone).
  if ($need -eq 'd3d' -and $all -contains 'ddraw.dll' -and -not (@($pe.Imports) | Where-Object { $D3dNames -contains $_ })) { $need = 'ddraw' }
  return $need
}

function Same([string]$a, [string]$b) {
  try { if ((Get-Item $a).Length -ne (Get-Item $b).Length) { return $false }; return (Get-FileHash $a).Hash -eq (Get-FileHash $b).Hash } catch { return $false }
}

# -> @(result, message); result: kit, partial, denied, other-arch
function Install-Kit([string]$exe, $pe, [string]$need) {
  $dir = Split-Path $exe -Parent
  if (-not $KitDir.ContainsKey([int]$pe.Machine)) { return @('other-arch', "not x86/x64: $exe") }
  $arch = if ($pe.Machine -eq 0x8664) { 'x64' } else { 'x86' }
  $want = @(); foreach ($f in $KitFiles) { $want += ,@($f, (Join-Path $KitDir[[int]$pe.Machine] $f)) }
  $voo = $false
  if ($need -eq 'ddraw') {
    if ((Test-Path (Join-Path $VooDir[[int]$pe.Machine] 'DDraw.dll')) -and (Test-Path $VooConf)) {
      $voo = $true; foreach ($f in $VooFiles) { $want += ,@($f, (Join-Path $VooDir[[int]$pe.Machine] $f)) }; $want += ,@('dgVoodoo.conf', $VooConf)
    }
  }
  $markerPath = Join-Path $dir $Marker
  $ours = @{}; if (Test-Path $markerPath) { foreach ($l in Get-Content $markerPath) { if ($l -match '^file (.+)$') { $ours[$matches[1].ToLower()] = 1 } } }
  $placed = @(); $kept = @(); $now = @()
  foreach ($w in $want) {
    $f = $w[0]; $from = $w[1]; if (-not (Test-Path $from)) { continue }
    $to = Join-Path $dir $f
    if (Test-Path $to) {
      if ($ours.ContainsKey($f.ToLower())) { if (Same $from $to) { $now += $f; continue } }
      elseif (Same $from $to) { $now += $f; $placed += "$f (adopted)"; continue }        # an identical copy put there by hand
      else { $kept += $f; continue }                                                     # the game's own (or a mod's): leave it
    }
    try { Copy-Item $from $to -Force -ErrorAction Stop; $now += $f; $placed += $f } catch { Log "copy failed $to : $($_.Exception.Message)"; return @('denied', "could not write to $dir (in use or no access)") }
  }
  $txt = @("PS5 GPU kit placed $(Get-Date -Format s) for $(Split-Path $exe -Leaf), arch $arch, $(if ($voo) {'DXVK + vkd3d-proton + dgVoodoo2'} else {'DXVK + vkd3d-proton'})") +
         (@($now) + @($ours.Keys | Where-Object { Test-Path (Join-Path $dir $_) }) | ForEach-Object { $_.ToLower() } | Sort-Object -Unique | ForEach-Object { "file $_" })
  Set-Content -Path $markerPath -Value $txt
  if ($kept) { Log "kept the game's own: $($kept -join ', ') in $dir" }
  if ($placed) { Log "kit $arch -> $dir : $($placed -join ', ') (for $(Split-Path $exe -Leaf))" }
  $msg = "$(Split-Path $exe -Leaf) ($arch): $(if ($voo) {'DXVK + vkd3d-proton + dgVoodoo2'} else {'DXVK + vkd3d-proton'})"
  if ($kept) { $msg += "; kept the game's own $($kept -join ', ')" }
  if ($need -eq 'ddraw' -and -not $voo) { Log "needs dgVoodoo2 (DirectDraw/D3D7): $exe"; return @('partial', "$msg; this is a DirectDraw/Direct3D 7 game and needs dgVoodoo2 in C:\ps5\gpu\dgvoodoo") }
  return @('kit', $msg)
}

# -NoSkip: do not add the folders to skip.txt (the anti-cheat clean-up; the scan skips those games anyway).
# A file that cannot be deleted (the game is running) stays listed in the marker, so a later -Remove finishes the job.
function Remove-Kit([string]$p, [switch]$NoSkip) {
  $dirs = if (Test-Path $p -PathType Leaf) { @(Split-Path $p -Parent) } else { @(Get-ChildItem -Path $p -Recurse -Filter $Marker -ErrorAction SilentlyContinue | ForEach-Object { $_.DirectoryName }) }
  $out = @(); $skip = @(Read-Skip)        # @(): one entry comes back as a plain string, and += would glue the next one to it
  foreach ($d in $dirs) {
    $m = Join-Path $d $Marker
    if (Test-Path $m) {
      $left = @()
      foreach ($l in Get-Content $m) {
        if ($l -match '^file (.+)$') {
          $f = Join-Path $d $matches[1]
          Remove-Item $f -Force -ErrorAction SilentlyContinue
          if (Test-Path $f) { $left += $matches[1] }
        }
      }
      if ($left) {
        try { Set-Content -Path $m -Value (@("PS5 GPU kit: removal not finished $(Get-Date -Format s), still in use:") + @($left | ForEach-Object { "file $_" })) } catch {}
        Log "kit removal in $d incomplete, in use: $($left -join ', ')"
        $out += "NOT finished in $d : $($left -join ', ') in use (close the game, then remove again)"
      } else {
        Remove-Item $m -Force; Log "kit removed from $d"; $out += "removed from $d"
      }
    }
    if (-not $NoSkip) { $skip += $d.ToLower() }
  }
  if (-not $NoSkip) { Write-Skip $skip }
  if (-not $out) { $out = @("no kit found in $p") }
  return $out
}

# -> list of messages for the programs it prepared
# Anti-cheat (2026-11-22): games protected by EasyAntiCheat, BattlEye or Riot Vanguard are left alone. Placing changed
# Direct3D DLLs next to them can get an online account banned. Looks in the exe's folder and up to 3 levels above.
$script:VanguardPresent = $null
function Get-AntiCheat([string]$dir) {
  if ($null -eq $script:VanguardPresent) { $script:VanguardPresent = [bool](Get-Service -Name vgk -ErrorAction SilentlyContinue) }
  $d = $dir
  for ($i = 0; $i -lt 4 -and $d; $i++) {
    if ((Test-Path (Join-Path $d 'EasyAntiCheat')) -or (Test-Path (Join-Path $d 'EasyAntiCheat_EOS')) -or
        (Test-Path (Join-Path $d 'start_protected_game.exe'))) { return 'EasyAntiCheat' }
    if ((Test-Path (Join-Path $d 'BattlEye')) -or (Get-ChildItem -Path $d -Filter 'BEService*.exe' -File -ErrorAction SilentlyContinue)) { return 'BattlEye' }
    $d = Split-Path $d -Parent
  }
  if ($script:VanguardPresent -and $dir -match '\\Riot Games\\') { return 'Riot Vanguard' }
  return $null
}

function Process-Tree([string]$root, $state, [string]$kitver, [switch]$Explicit) {
  $done = @()
  $leaf = Test-Path $root -PathType Leaf
  $exes = if ($leaf) { @(Get-Item $root) } else { Get-ChildItem -Path $root -Recurse -Filter *.exe -File -ErrorAction SilentlyContinue }
  $skip = Read-Skip
  foreach ($x in $exes) {
    if (-not $leaf -and ($x.FullName -match $SkipDirs -or $x.Name -match $SkipExe)) { continue }
    if (-not $Explicit -and $skip -contains $x.DirectoryName.ToLower()) { continue }
    $key = $x.FullName.ToLower(); $stamp = "$($x.Length)-$($x.LastWriteTimeUtc.Ticks)-$kitver"
    if (-not $Explicit -and $state[$key] -eq $stamp) { continue }
    $ac = Get-AntiCheat $x.DirectoryName
    if ($ac) {
      Log "skipped, anti-cheat ($ac): $($x.FullName)"; $state[$key] = $stamp
      $undo = ''
      if (Test-Path (Join-Path $x.DirectoryName $Marker)) { [void](Remove-Kit $x.FullName -NoSkip); $undo = ' The kit files an earlier version placed here were removed.' }
      $done += "$($x.Name): skipped. This game uses $ac anti-cheat; changed Direct3D DLLs can get your online account banned, so the kit leaves it alone.$undo"
      continue
    }
    $pe = Get-PeInfo $x.FullName
    if (-not $pe) { $state[$key] = $stamp; continue }
    $need = Get-Need $x.FullName $pe
    if (-not $need -and $leaf -and $Explicit) { $need = 'd3d' }        # asked for this program by name: prepare it anyway
    if (-not $need) { $state[$key] = $stamp; continue }
    $r = Install-Kit $x.FullName $pe $need
    if ($r[0] -eq 'kit' -or $r[0] -eq 'partial' -or $r[0] -eq 'other-arch') { $state[$key] = $stamp }
    if ($r[0] -ne 'other-arch') { $done += $r[1] }
  }
  return ,$done
}

function Library-Roots {
  $roots = New-Object System.Collections.Generic.List[string]
  foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam','HKLM:\SOFTWARE\Valve\Steam') {
    $p = (Get-ItemProperty $k -ErrorAction SilentlyContinue).InstallPath
    if ($p) { $roots.Add((Join-Path $p 'steamapps\common'))
      $vdf = Join-Path $p 'steamapps\libraryfolders.vdf'
      if (Test-Path $vdf) { foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $roots.Add((Join-Path ($m.Groups[1].Value -replace '\\\\','\') 'steamapps\common')) } } }
  }
  foreach ($p in 'C:\Program Files\Epic Games','C:\GOG Games','C:\Program Files (x86)\GOG Galaxy\Games','C:\Games') { $roots.Add($p) }
  $extra = Join-Path $Home5 'roots.txt'; if (Test-Path $extra) { foreach ($l in Get-Content $extra) { if ($l.Trim()) { $roots.Add($l.Trim()) } } }
  return $roots | Where-Object { Test-Path $_ } | Sort-Object -Unique
}

function Say([string]$text) {
  if ($Notify) { try { [void](New-Object -ComObject WScript.Shell).Popup($text, 20, 'PS5 GPU', 64) } catch {} }
  $text
}

if (-not (Test-Path $Home5)) { New-Item -ItemType Directory -Path $Home5 | Out-Null }
if ($Remove) {
  if (-not $Path) { 'usage: -Path <exe|dir> -Remove'; exit 2 }
  Say (((Remove-Kit $Path) -join "`n") + "`n(the automatic scan leaves this folder alone now; right-click > Prepare for PS5 GPU puts the kit back and lets the scan in again)"); exit 0
}
$state = Read-State; $kitver = Kit-Version
if ($Path) {
  if (-not (Test-Path $Path)) { Say "not found: $Path"; exit 2 }
  $d = if (Test-Path $Path -PathType Leaf) { Split-Path $Path -Parent } else { $Path }
  Write-Skip @(Read-Skip | Where-Object { $_ -ne $d.ToLower() -and -not $_.StartsWith($d.ToLower() + '\') })
  try { $msgs = Process-Tree $Path $state $kitver -Explicit } catch { Log "error: $($_.Exception.Message)"; Say "error: $($_.Exception.Message)"; exit 1 }
  Save-State $state
  if ($msgs.Count) { Say ("Prepared for the PS5 GPU:`n" + ($msgs -join "`n")) } else { Say "Nothing to do: no Direct3D program found in $Path" }
  exit 0
}
if ($Watch) {
  Log "watcher started (pid $PID)"
  $total = 0; foreach ($r in Library-Roots) { $total += (Process-Tree $r $state $kitver).Count }
  Save-State $state; if ($total) { Log "scan: $total program(s) prepared or refreshed" }
  $watchers = @{}; $pending = @{}
  $lastRoots = [DateTime]::MinValue; $lastFull = Get-Date
  while ($true) {
    if (((Get-Date) - $lastRoots).TotalMinutes -ge 10) {
      $lastRoots = Get-Date
      foreach ($r in Library-Roots) {
        if ($watchers.ContainsKey($r.ToLower())) { continue }
        try {
          $w = New-Object IO.FileSystemWatcher $r
          $w.IncludeSubdirectories = $true
          $w.NotifyFilter = [IO.NotifyFilters]'FileName, DirectoryName, LastWrite'
          $w.InternalBufferSize = 65536
          foreach ($ev in 'Created', 'Renamed', 'Changed') { Register-ObjectEvent -InputObject $w -EventName $ev -SourceIdentifier "p5kit|$ev|$r" | Out-Null }
          $w.EnableRaisingEvents = $true
          $watchers[$r.ToLower()] = $w
          Log "watching $r"
        } catch { Log "cannot watch $r : $($_.Exception.Message)" }
      }
    }
    $e = Wait-Event -Timeout 2
    while ($e) {
      $full = $e.SourceEventArgs.FullPath
      $root = ($e.SourceIdentifier -split '\|', 3)[2]
      Remove-Event -EventIdentifier $e.EventIdentifier
      if ($full -and $root -and ($full -match '\.exe$' -or (Test-Path -LiteralPath $full -PathType Container))) {
        $rel = $full.Substring([Math]::Min($full.Length, $root.TrimEnd('\').Length)).TrimStart('\')
        $top = ($rel -split '\\')[0]
        if ($top) { $pending[(Join-Path $root $top)] = Get-Date }
      }
      $e = Get-Event | Select-Object -First 1
    }
    foreach ($d in @($pending.Keys)) {
      if (((Get-Date) - $pending[$d]).TotalSeconds -lt 8) { continue }
      $pending.Remove($d)
      if (-not (Test-Path -LiteralPath $d)) { continue }
      try {
        $kitver = Kit-Version
        $msgs = Process-Tree $d $state $kitver
        Save-State $state
        if ($msgs.Count) { Log ("watch: prepared " + ($msgs -join '; ')) }
      } catch { Log "watch error in $d : $($_.Exception.Message)" }
    }
    if (((Get-Date) - $lastFull).TotalHours -ge 6) {
      $lastFull = Get-Date; $kitver = Kit-Version
      $total = 0; foreach ($r in Library-Roots) { $total += (Process-Tree $r $state $kitver).Count }
      Save-State $state; if ($total) { Log "scan: $total program(s) prepared or refreshed" }
    }
  }
}
if ($Scan) {
  $total = 0; foreach ($r in Library-Roots) { $total += (Process-Tree $r $state $kitver).Count }
  Save-State $state; if ($total) { Log "scan: $total program(s) prepared or refreshed" }
  exit 0
}
'usage: ps5-gamekit.ps1 -Scan | -Path <exe or folder> [-Remove] [-Notify]'
