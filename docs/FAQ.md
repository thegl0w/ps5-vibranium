# FAQ

**Which consoles work?** The launch PS5 (disc or digital). The PS5 Slim is untested (reports welcome). The PS5 Pro is
not supported.

**Which Windows?** Only the ISO named in the README (Windows 10 IoT Enterprise LTSC 2021, build 19044.1288, with the
SHA-256 given there). The installer checks the edition and kernel build and refuses anything else before writing.

**Does it touch the PS5's internal SSD or my PS5 games?** No. The internal NVMe is hidden from Windows and blocked three
ways (see Safety in the README). ps5win never reads or runs PS5 games.

**Can I install to an M.2 SSD in the expansion slot?** No, USB drives only, plugged directly into a PS5 port.

**Why is Windows Update off?** The firmware patches the Windows kernel for the PS5's CPU, matched to the kernel build.
An update could replace the kernel. Fixes come through ps5win releases. Microsoft Defender still updates its
definitions by itself when the console is online.

**Why is the screen black for a second after the desktop appears on the second start?** That is the switch to the full
GPU driver; a green rectangle near the top left just before it is the GPU's self-test.

**Can I play online games with anti-cheat?** No. Kernel anti-cheat does not work here and can get accounts banned; the
game kit skips those games.

**Why does PS5 OS say the console wasn't turned off properly, or check its storage first?** Expected and harmless:
Windows turns the console off or restarts it through its power controller directly, without PS5 OS's own shutdown, so
PS5 OS checks itself at the next start. ps5win never writes to the console's internal storage.

**120 Hz? Wi-Fi? Bluetooth? Blu-ray drive?** Not in this release. 120 Hz works in development; Wi-Fi needs a clean
driver build; Bluetooth and the Blu-ray drive are not done yet.

**Something went wrong.** See [Troubleshooting](../README.md#troubleshooting), then open a bug report with the logs the
template asks for.
