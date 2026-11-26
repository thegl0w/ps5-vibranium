# Contributing to PS5 Vibranium

Thank you for helping. Please read this first; it is short.

## Licence

Contributions are accepted under the MIT licence (inbound = outbound): by sending a change you agree that it is
published under the same MIT licence as the rest of the project. The exception is files that are already GPL-2.0;
changes to those stay GPL-2.0.

## AI-assisted contributions

If a tool such as an AI assistant wrote or substantially shaped your change, say so in the pull request.

## Evidence

Changes need evidence. Say what you tested and on which console and firmware, and attach the logs or output (for
example `C:\ps5\gpu\autostart.log`, `start.log`, the firmware trace from the `PS5BOOT` partition, or the installer
log). Changes nobody could test on a PS5 are welcome as drafts, marked as untested.

## Not accepted

- Anything that touches PS5 games or game packages.
- Anything for the console's own system software or its protections. The PS5 Linux loader is a separate project.
- Sony files of any kind (firmware, keys, system software).
- Windows files (system files, ISOs, drivers from Windows).
- Personal data: serial numbers, account names, network names, MAC or IP addresses, in code, logs or screenshots.
  Remove them before you attach a log.

## Forks

If you publish your own builds, please give them a different name, so users can tell them apart from official
PS5 Vibranium releases.
