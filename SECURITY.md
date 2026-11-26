# Security policy

## Reporting a vulnerability

Please report security problems **privately**, through GitHub's private vulnerability reporting: the **Security** tab
of this repository, then **Report a vulnerability**. Do not open a public issue, discussion or pull request for a
security bug; those are visible to everyone before a fix exists.

Include what you found, how to reproduce it, which PS5 Vibranium release and which Windows build you used, and what an
attacker could do with it.

## In scope

- The kernel drivers: `ps5wddm` (display/GPU), `ps5gpu` (kernel shim) and `p5eth` (Ethernet)
- `libps5gpu` and the user-mode GPU tools (`ps5gpu2.exe`)
- The UEFI firmware, the launch package and the Linux launcher
- The installer (the image's Linux system and `/opt/ps5win/installer`)

Out of scope: Windows itself, Mesa, DXVK, vkd3d-proton, the PS5 Linux loader and the PS5's own software. Please report
problems in those to their own projects.

## Response

This is a volunteer project: best-effort response; no guarantees about how quickly a report is answered or fixed.
