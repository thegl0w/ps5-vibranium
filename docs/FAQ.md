# FAQ

**Which consoles work?** The launch PS5 (disc or digital). The PS5 Slim is untested (reports welcome). The PS5 Pro is
not supported.

**Which firmware versions work?** Any firmware that can load Linux.

**How is this different from just booting Windows on a PS5?** ps5win brings its own GPU driver: a WDDM display
driver, Vulkan through Mesa RADV, and Direct3D 9 to 12 games through DXVK and vkd3d-proton. Without a GPU driver
Windows falls back to the basic display adapter: no acceleration and no games. See the table at the top of the README
for what works and what doesn't yet.

**How do I know this is real?** The source for every part is in this repository, with its licence and origin
(docs/ORIGINS.md, docs/CLEANROOM.md). Releases are signed (check the `.minisig` as the README describes). The demo video
shows a boot to the desktop with the GPU driver. Parts of the code and docs were written with AI help, as the README
says. Everything shipped was tested on a real console.

**Which Windows?** The ISO named in the README (Windows 10 IoT Enterprise LTSC 2021, build 19044.1288, with the
SHA-256 given there) is the tested one. Windows 10 22H2 and other 2004–22H2 media work as an experimental option
(`experimental=yes` in `ps5win-answers.txt`; tested once with Microsoft's 22H2 ISO). The installer checks the edition
and kernel before writing and refuses anything it can't patch. You bring the ISO and your own licence.

**Does it touch the PS5's internal SSD or my PS5 games?** No. The internal NVMe is hidden from Windows and blocked three
ways (see Safety in the README). ps5win never reads or runs PS5 games.

**Can I install to an M.2 SSD in the expansion slot?** No, USB drives only, plugged directly into a PS5 port.

**Why is Windows Update off?** The firmware patches the Windows kernel for the PS5's CPU, matched to the kernel build.
An update could replace the kernel. Fixes come through ps5win releases. Microsoft Defender still updates its
definitions by itself when the console is online.

**Why is the screen black for a second after the desktop appears on the second start?** That is the switch to the full
GPU driver; a green rectangle near the top left just before it is the GPU's self-test.

**Do I have to start the loader again after every Restart?** No (since 1.1). Start → Restart comes straight back into
Windows without leaving our firmware, and a blue screen restarts by itself the same way. After Shut down you start the
loader again as usual.

**Can I play online games with anti-cheat?** No. Kernel anti-cheat does not work here and can get accounts banned; the
game kit skips those games.

**Why does the console say it wasn't turned off properly, or check its storage when it next starts normally?**
Expected and harmless: Windows turns the console off through its power controller directly, without the console's own
shutdown, so the console checks itself at the next normal start. ps5win never writes to the console's internal storage.

**120 Hz? Wi-Fi? Bluetooth? Blu-ray drive?** Not in this release. 120 Hz works in development; Wi-Fi needs a clean
driver build; Bluetooth and the Blu-ray drive are not done yet.

**Something went wrong.** See [Troubleshooting](../README.md#troubleshooting), then open a bug report with the logs the
template asks for.
