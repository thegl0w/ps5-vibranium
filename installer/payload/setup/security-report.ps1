# PS5 Windows: what this install exposes, written once at the end of the first-start setup to
# C:\ps5\security-report.txt (read from Linux or the Mac, read-only). Reports only; changes nothing.
$out = 'C:\ps5\security-report.txt'
$r = New-Object System.Collections.Generic.List[string]
function Section($t) { $r.Add(''); $r.Add("== $t") }
function Try-Add($block) { try { & $block | ForEach-Object { $r.Add([string]$_) } } catch { $r.Add("  (failed: $($_.Exception.Message))") } }
$r.Add("PS5 Windows security report $(Get-Date -Format s) as $([Security.Principal.WindowsIdentity]::GetCurrent().Name)")

Section 'Accounts: blank passwords allowed only at the console (LimitBlankPasswordUse 1 = yes)'
Try-Add { "  LimitBlankPasswordUse = $((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Lsa').LimitBlankPasswordUse)" }
Try-Add { Get-LocalUser | ForEach-Object { "  user {0}: enabled {1}, password required {2}" -f $_.Name, $_.Enabled, $_.PasswordRequired } }

Section 'Listening TCP ports (owning process)'
Try-Add {
  Get-NetTCPConnection -State Listen | Sort-Object LocalPort | ForEach-Object {
    $p = Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue
    "  {0}:{1}  pid {2} {3}" -f $_.LocalAddress, $_.LocalPort, $_.OwningProcess, $(if ($p) { $p.ProcessName } else { '?' })
  }
}
Section 'Listening UDP endpoints (owning process)'
Try-Add {
  Get-NetUDPEndpoint | Sort-Object LocalPort | ForEach-Object {
    $p = Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue
    "  {0}:{1}  pid {2} {3}" -f $_.LocalAddress, $_.LocalPort, $_.OwningProcess, $(if ($p) { $p.ProcessName } else { '?' })
  }
}

Section 'Firewall profiles'
Try-Add { Get-NetFirewallProfile | ForEach-Object { "  {0}: enabled {1}, inbound {2}, outbound {3}" -f $_.Name, $_.Enabled, $_.DefaultInboundAction, $_.DefaultOutboundAction } }

Section 'Remote Desktop (fDenyTSConnections 1 = off)'
Try-Add { "  fDenyTSConnections = $((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server').fDenyTSConnections)" }

Section 'OpenSSH server'
Try-Add { Get-WindowsCapability -Online -Name 'OpenSSH.Server*' | ForEach-Object { "  {0}: {1}" -f $_.Name, $_.State } }
Try-Add { $s = Get-Service sshd -ErrorAction SilentlyContinue; if ($s) { "  service sshd: $($s.Status), start $($s.StartType)" } else { '  service sshd: not installed' } }

Section 'SMB server'
Try-Add { $s = Get-Service LanmanServer; "  LanmanServer: $($s.Status), start $($s.StartType)" }
Try-Add { Get-SmbShare | ForEach-Object { "  share {0} -> {1}" -f $_.Name, $_.Path } }

Section 'Microsoft Defender'
Try-Add {
  $m = Get-MpComputerStatus
  "  real-time protection {0}, antivirus signatures {1} from {2:yyyy-MM-dd} ({3} days old)" -f $m.RealTimeProtectionEnabled, $m.AntivirusSignatureVersion, $m.AntivirusSignatureLastUpdated, $m.AntivirusSignatureAge
}

Section 'Windows Update + Delivery Optimization (expected: Start 4; the kernel stub is tied to this kernel)'
Try-Add { foreach ($s in 'wuauserv','UsoSvc','WaaSMedicSvc','DoSvc') { "  $s Start = $((Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$s").Start)" } }

Try-Add { "  DODownloadMode (policy) = $((Get-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeliveryOptimization' -ErrorAction SilentlyContinue).DODownloadMode)" }

Set-Content -Path $out -Value $r -Encoding UTF8
"written: $out"
