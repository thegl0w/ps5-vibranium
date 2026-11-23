# PS5 Windows: switch off Windows Update's scheduled tasks (the installer already set the update policies and the
# services wuauserv / UsoSvc / WaaSMedicSvc to disabled offline). Task part of ps5gpu/win/wu-off.ps1, without its
# "servicing is running" abort: at the end of Windows Setup the servicing stack may still be busy, and disabling
# tasks does not touch it. Why updates stay off: the firmware's kernel stub is tied to one ntoskrnl build.
$log = 'C:\ps5\wu-tasks-off.log'
Set-Content -Path $log -Value "wu-tasks-off $(Get-Date -Format s)"
$failed = 0
foreach ($tp in '\Microsoft\Windows\WindowsUpdate\','\Microsoft\Windows\UpdateOrchestrator\','\Microsoft\Windows\WaaSMedic\','\Microsoft\Windows\UpdateAssistant\') {
  Get-ScheduledTask -TaskPath $tp -ErrorAction SilentlyContinue | ForEach-Object {
    try { Disable-ScheduledTask -InputObject $_ -ErrorAction Stop | Out-Null; Add-Content $log "task off $($_.TaskPath)$($_.TaskName)" }
    catch { $failed++; Add-Content $log "task FAILED $($_.TaskPath)$($_.TaskName): $($_.Exception.Message)" }
  }
}
sc.exe failure wuauserv reset= 0 actions= "" | Out-Null
foreach ($s in 'wuauserv','UsoSvc','WaaSMedicSvc','DoSvc') {
  $v = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$s" -Name Start -ErrorAction SilentlyContinue).Start
  Add-Content $log "service $s Start=$v"
  if ($v -ne 4) { $failed++ }
}
Add-Content $log "done, $failed problem(s) (as $([Security.Principal.WindowsIdentity]::GetCurrent().Name))"
Set-Content -Path 'C:\ps5\wu-tasks-off.done' -Value $failed -NoNewline
exit $failed
