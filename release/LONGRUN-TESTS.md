# Long-lived install tests (before the release)

Why: the 9-boot trace-slot bug (2026-11-22) only showed after many starts of one install. These tests look for the
same class of problem: things that fill up or grow until a later boot fails.

## Setup (once)

1. A fresh install from the release image on a test drive (the Patriot), not the dev Sabrent.
2. Copy `longrun-record.ps1` to `C:\ps5\longrun\` and register it to run at every boot (elevated PowerShell):

   ```powershell
   $a = New-ScheduledTaskAction -Execute powershell.exe -Argument '-NoProfile -ExecutionPolicy Bypass -File C:\ps5\longrun\longrun-record.ps1'
   Register-ScheduledTask -TaskName ps5-longrun -Action $a -Trigger (New-ScheduledTaskTrigger -AtStartup) -User SYSTEM -RunLevel Highest
   ```

   Each boot appends one line to `C:\ps5\longrun\boots.csv`: boot number, time, uptime at record, C: free space, PS5BOOT
   free space, and the sizes of everything that grows (pagefile, hiberfil, crash dumps, Windows logs, CBS logs, temp,
   shader caches, our own logs).

## Test 1: 30+ boots on one install

- Cycle: Windows Start → Restart → start the PS5 Linux loader → the image starts Windows by itself. About 3 minutes
  per cycle; each one needs the loader started by hand. Mix in a few Shut downs (power off, then a normal start).
- Pass: every cycle reaches the desktop; PS5BOOT free space stops shrinking once all 9 trace slots exist (about
  24 MiB + 1 MiB per boot until then); no size column grows without bound.
- Afterwards: read `boots.csv`, plus `ps5-boot22.txt` and the trace slots on PS5BOOT (from the Mac, read-only).

## Test 2: nearly full USB drive

- Fill C: until about 1 GiB is free: `fsutil file createnew C:\ps5\longrun\fill.bin <bytes>` (compute the bytes from
  `Get-Volume C`). Restart twice.
- Pass: Windows still boots; the GPU driver starts; the firmware still starts Windows (boot log writes are best effort
  since 2026-11-22). Then delete `fill.bin`.
- Also fill **PS5BOOT** to near full from the Mac (a dummy file), restart once. Pass: Windows still starts (that was
  the second bug class: a failed log write must not stop the boot). Remove the dummy file afterwards.

## Test 3: growing files

Watch these columns over test 1: `pagefile.sys`, `hiberfil.sys` (hibernation should be off), `C:\Windows\Minidump`,
`C:\Windows\MEMORY.DMP`, `C:\Windows\Logs`, `C:\Windows\Logs\CBS`, `C:\Windows\Temp`, the user's `%TEMP%`, the DXVK /
vkd3d-proton / Mesa shader caches under the user's AppData, and `C:\ps5` (our own logs: autostart.log, start.log,
gamekit log, ps5guard log). Anything that grows every boot without a cap gets a cap or a rotation.

## Test 4: clock

The console's clock can be far off on a fresh install (it started at 2026-10-25 on one run). Set Windows' clock to
2015, restart, and check that the test-signed drivers still load (the certificate is valid from 2020; a planned
re-sign moves that to 2000).
