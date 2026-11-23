# Per-file origin list

195 files. SHA-256 (first 16 hex digits) of the file as published. "Shared code" = the clone detector's
finding for that file (`tools/clonecheck.py`; see CLEANROOM.md for how each one is explained).

| File | SHA-256 | Origin | Licence | Shared code (clone detector) |
|---|---|---|---|---|
| `.github/ISSUE_TEMPLATE/bug_report.yml` | `66d9da1196256da4` | ours | MIT | - |
| `.github/ISSUE_TEMPLATE/config.yml` | `12785b373c783fa8` | ours | MIT | - |
| `.github/ISSUE_TEMPLATE/feature_request.yml` | `dd103975e0cb1807` | ours | MIT | - |
| `.gitignore` | `c7664b5b36551b63` | ours | MIT | - |
| `CONTRIBUTING.md` | `4673b86b6ac87a23` | ours | MIT | - |
| `LICENSE` | `aab31633bff4a0be` | ours (the MIT text of release/LICENSE) | MIT | - |
| `README.md` | `5254438b86e44ed1` | ours | MIT | - |
| `SECURITY.md` | `53dcddeedf3ed5b5` | ours | MIT | - |
| `cleanroom/directload-mit/CyanDirectLoad.cpp` | `1b62e949ae61539f` | clean-room rewrite from cleanroom/spec-directload.md (replaces MetalCyan's CyanDirectLoad) | MIT | MetalCyan:MetalCyan/CyanDirectLoad.cpp (1 runs, longest 4 lines) |
| `cleanroom/directload-mit/CyanDirectLoad.hpp` | `d2e555f6a333a229` | clean-room rewrite from cleanroom/spec-directload.md (replaces MetalCyan's CyanDirectLoad) | MIT | MetalCyan:MetalCyan/CyanDirectLoad.hpp (24 runs, longest 15 lines) |
| `cleanroom/directload-mit/NOTES.md` | `a9633ead6441f0e7` | clean-room rewrite from cleanroom/spec-directload.md (replaces MetalCyan's CyanDirectLoad) | MIT | - |
| `cleanroom/directload-mit/test_directload.cpp` | `4525a33f5523a1f6` | clean-room rewrite from cleanroom/spec-directload.md (replaces MetalCyan's CyanDirectLoad) | MIT | MetalCyan:tests/directload/host_trace.cpp (1 runs, longest 4 lines) |
| `cleanroom/p5eth-mit/NOTES.md` | `4941d510f2380ff5` | clean-room rewrite from cleanroom/spec-p5eth.md (replaces our earlier GPL driver) | MIT | - |
| `cleanroom/p5eth-mit/build.sh` | `99e410194f82ed01` | clean-room rewrite from cleanroom/spec-p5eth.md (replaces our earlier GPL driver) | MIT | - |
| `cleanroom/p5eth-mit/p5eth.cpp` | `83eb4786c1679b2f` | clean-room rewrite from cleanroom/spec-p5eth.md (replaces our earlier GPL driver) | MIT | GPL-ps5wifi:p5net.cpp (3 runs, longest 5 lines); GPL-ps5eth(old):p5eth.cpp (3 runs, longest 5 lines) |
| `cleanroom/p5eth-mit/p5eth.inf` | `b67075c5d6b0c3e9` | clean-room rewrite from cleanroom/spec-p5eth.md (replaces our earlier GPL driver) | MIT | - |
| `cleanroom/ps5reset-mit/LICENSE` | `eafbcf7dfbd0f73b` | clean-room rewrite from cleanroom/spec-icc-reset.md (replaces our earlier GPL library) | MIT | - |
| `cleanroom/ps5reset-mit/NOTES.md` | `359348f729894b1d` | clean-room rewrite from cleanroom/spec-icc-reset.md (replaces our earlier GPL library) | MIT | - |
| `cleanroom/ps5reset-mit/Ps5Icc.h` | `edd9c9ae8ef265b5` | clean-room rewrite from cleanroom/spec-icc-reset.md (replaces our earlier GPL library) | MIT | GPL-ps5reset(old):ps5_varhook.inc (8 runs, longest 11 lines) |
| `cleanroom/ps5reset-mit/Ps5IccMailbox.c` | `03e51eb9683c8509` | clean-room rewrite from cleanroom/spec-icc-reset.md (replaces our earlier GPL library) | MIT | - |
| `cleanroom/ps5reset-mit/Ps5IccResetSystemLib.c` | `8e14c665f46f63a1` | clean-room rewrite from cleanroom/spec-icc-reset.md (replaces our earlier GPL library) | MIT | ps5-hackintosh:uefi/Ps5Pkg/Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c (2 runs, longest 5 lines) |
| `cleanroom/ps5reset-mit/Ps5IccResetSystemLib.inf` | `5ca7f318454623f6` | clean-room rewrite from cleanroom/spec-icc-reset.md (replaces our earlier GPL library) | MIT | GPL-ps5reset(old):Ps5IccResetSystemLib.inf (2 runs, longest 4 lines) |
| `cleanroom/ps5reset-mit/Ps5IccVariables.c` | `d9af364373cd959b` | clean-room rewrite from cleanroom/spec-icc-reset.md (replaces our earlier GPL library) | MIT | - |
| `cleanroom/spec-directload.md` | `ec0f41d49774d0a8` | clean-room specification (written description; see CLEANROOM.md) | MIT | - |
| `cleanroom/spec-icc-reset.md` | `fecf9352a2385604` | clean-room specification (written description; see CLEANROOM.md) | MIT | - |
| `cleanroom/spec-p5eth.md` | `8b88a5fb0791d8ad` | clean-room specification (written description; see CLEANROOM.md) | MIT | - |
| `docs/CLEANROOM.md` | `7bd2d0536f72c979` | ours | MIT | - |
| `docs/CLONECHECK.md` | `92452921b9d39444` | ours | MIT | - |
| `docs/FAQ.md` | `391873a841e86a7e` | ours | MIT | - |
| `docs/ORIGINS.md` | `b386d77750f2af58` | ours | MIT | - |
| `docs/PINNED-ISSUE.md` | `410221b5318e8f3e` | ours | MIT | - |
| `firmware/build_stage.sh` | `cd22a1c06061297b` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/compile.sh` | `d0d10d429dcba40a` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/kms_linear.py` | `2153e4d9152f6017` | derived from ps5-hackintosh bringup/tools/kms_linear.py (rodri0x), credited in its header | MIT | ps5-hackintosh:bringup/tools/kms_linear.py (100 runs, longest 71 lines); ps5-hackintosh:bringup/tools/patch_tripwire.py (7 runs, longest 10 lines) |
| `firmware/kstub_table.py` | `0b198e3e82d1c753` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/make_bundle.py` | `b0b2f69d28e602a2` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/make_kpat.py` | `72742e77cae3280a` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/make_kstub.py` | `6c0727282c18a431` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/pack_package.sh` | `1eac547dbb42ec33` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/prepare.py` | `4b5cc6c7a7679fef` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/ps5warm/Ps5Warm.c` | `ad85b33c6106fa8d` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/ps5warm/Ps5Warm.h` | `d3c2be6f9105afde` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/ps5warm/Ps5WarmAsm.S` | `71dabdcb965b6732` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/run_stage.sh` | `0e971b00032240a4` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/windows_bds.inc` | `02b1cba9b73475fa` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/windows_halpatch.inc` | `3466f556a2956479` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `firmware/windows_kpat.inc` | `78e6502ff7887c02` | generated by firmware/make_kpat.py | MIT | - |
| `firmware/windows_kstub-19041.1288.inc` | `5a7039e50feede6a` | generated by firmware/make_kstub.py (our code, offsets, fingerprints) | MIT | - |
| `firmware/windows_kstub.inc` | `07b3ed136c01a999` | generated by firmware/make_kstub.py (our code, offsets, fingerprints) | MIT | - |
| `firmware/windows_kstub_install.inc` | `f3e726705558c325` | generated by firmware/make_kstub.py (our code, offsets, fingerprints) | MIT | - |
| `firmware/windows_trace.inc` | `139b74012efb0186` | ours (applied to EDK2 / ps5-hackintosh's UEFI build at build time) | MIT | - |
| `image/assemble-inside.sh` | `44f5f8bf0240f0b4` | ours | MIT | - |
| `image/base-inside.sh` | `f6fe5bc3a4ec9bfd` | ours | MIT | - |
| `image/build-base.sh` | `03d023469eb8ccb4` | ours | MIT | - |
| `image/build-image.sh` | `07419b96d6cbfe87` | ours | MIT | - |
| `image/overlay/etc/modprobe.d/ps5win-nobt.conf` | `25d31932899eb8fb` | ours | MIT | - |
| `image/overlay/etc/sysctl.d/20-ps5win-quiet.conf` | `24c67200cdb46cb9` | ours | MIT | - |
| `image/overlay/etc/systemd/system/getty@tty1.service.d/autologin.conf` | `71d4d9124afe8e56` | ours | MIT | - |
| `image/overlay/etc/systemd/system/ps5win-autostart.service` | `7fdc25002a21a217` | ours | MIT | - |
| `image/overlay/opt/ps5win/ps5win-autostart.sh` | `86d12e80b44b8857` | ours | MIT | - |
| `image/overlay/opt/ps5win/ps5win-start.sh` | `a76d5979f0903520` | ours | MIT | - |
| `image/packages.txt` | `3d41012be430dd9b` | ours | MIT | - |
| `installer/README.md` | `a7686224ed0e5139` | ours | MIT | - |
| `installer/autorun` | `25848da559abd56e` | ours | MIT | - |
| `installer/autorun.py` | `f0434498d416c697` | ours | MIT | - |
| `installer/build_payload.py` | `234a47c66943c9c2` | ours | MIT | - |
| `installer/check_kernel.py` | `f10b6505be87b1cb` | ours | MIT | - |
| `installer/deny.txt` | `d9d9b9234b18078e` | ours | MIT | - |
| `installer/find_iso.py` | `30c7d487465f864a` | ours | MIT | - |
| `installer/install.py` | `6c208cc3aa962ee1` | ours | MIT | - |
| `installer/lib/bcd.py` | `522cb1d5604648a7` | ours | MIT | - |
| `installer/lib/catalog.py` | `547fecaa8f03837a` | ours | MIT | - |
| `installer/lib/hive.py` | `ffe15260ac7fea2b` | ours | MIT | - |
| `installer/lib/udf.py` | `93b6c99df34ed326` | ours | MIT | - |
| `installer/lib/wim.py` | `cfa5ff5ee94f13c7` | ours | MIT | - |
| `installer/lib/wimapi.py` | `ac04da1723adf275` | ours | MIT | - |
| `installer/lib/writer.py` | `960803199c04f5a4` | ours | MIT | - |
| `installer/make_bcd.py` | `ee6e9fb789e90391` | ours | MIT | - |
| `installer/make_image_tree.py` | `5b1fee01d4511b59` | ours | MIT | - |
| `installer/make_unattend.py` | `27d872d773cd3785` | ours | MIT | - |
| `installer/offline_reg.py` | `7fcd547f4c2b06b3` | ours | MIT | - |
| `installer/payload/freeze-2026-11-23-r12.json` | `818d8bb76058f8ea` | ours | MIT | - |
| `installer/payload/freeze-2026-11-23-r13.json` | `7417bdd20e3c76b4` | ours | MIT | - |
| `installer/payload/manifest.txt` | `d063f838955aeac0` | ours | MIT | - |
| `installer/payload/pinned/autostart.cmd` | `1fadf108a7b3f183` | ours | MIT | - |
| `installer/payload/setup/SetupComplete.cmd` | `5e770d846efae507` | ours | MIT | - |
| `installer/payload/setup/firstlogon.cmd` | `f7abbad8c01383c7` | ours | MIT | - |
| `installer/payload/setup/ps5-setup.cmd` | `e4bfb98ffbf59dd0` | ours | MIT | - |
| `installer/payload/setup/readme-first.txt` | `9cf41488b2a1b7b6` | ours | MIT | - |
| `installer/payload/setup/security-report.ps1` | `c84cf21d99323747` | ours | MIT | - |
| `installer/payload/setup/wu-tasks-off.ps1` | `85392a918cf4c164` | ours | MIT | - |
| `installer/payload/setup/wu-tasks-system.cmd` | `4f9b67c6d0dc3d0c` | ours | MIT | - |
| `installer/plan_layout.py` | `f1b1772fe1cdbc7f` | ours | MIT | - |
| `installer/ps5win-answers.example.txt` | `c3d2133f11a0e261` | ours | MIT | - |
| `installer/tests/bcd_test.py` | `f5f82a0e149d56f4` | ours | MIT | - |
| `installer/tests/fixtures/answer-ok.txt` | `f007cc4dd251f9d9` | ours | MIT | - |
| `installer/tests/fixtures/answer-wrong.txt` | `0ffdb90fec0b5e25` | ours | MIT | - |
| `installer/tests/fixtures/answers-badkey.txt` | `df27e4a5b96f2068` | ours | MIT | - |
| `installer/tests/fixtures/answers-badname.txt` | `0a04d52f3f0a059f` | ours | MIT | - |
| `installer/tests/fixtures/answers-badtz.txt` | `cf7b54dd7e0dd9e4` | ours | MIT | - |
| `installer/tests/fixtures/answers-baduser.txt` | `6d42630afb90cd8b` | ours | MIT | - |
| `installer/tests/fixtures/answers-full.txt` | `b1921314f115f501` | ours | MIT | - |
| `installer/tests/fixtures/answers-install.txt` | `9a4aa0ebbd7fc5ef` | ours | MIT | - |
| `installer/tests/fixtures/answers-notarget.txt` | `c75cf650901e6c19` | ours | MIT | - |
| `installer/tests/fixtures/installed-disk.json` | `6080ff6223567e04` | ours | MIT | - |
| `installer/tests/fixtures/lsblk.json` | `1ae9ce295f1fd268` | ours | MIT | - |
| `installer/tests/fixtures/single-disk.json` | `22d51e78759f88f8` | ours | MIT | - |
| `installer/tests/fixtures/small-disk.json` | `a05d4bb305092a04` | ours | MIT | - |
| `installer/tests/fixtures/toshiba-image.json` | `b9caf6b57c0e9577` | ours | MIT | - |
| `installer/tests/fixtures/toshiba-noiso.json` | `e883921dd8cdce6e` | ours | MIT | - |
| `installer/tests/fixtures/two-disk.json` | `78a8b74417db2857` | ours | MIT | - |
| `installer/tests/overlay_test.py` | `eaf7a28eb48ac24b` | ours | MIT | - |
| `installer/tests/run_tests.sh` | `d36fb3cd2b4f2f43` | ours | MIT | - |
| `installer/tests/writer_test.py` | `8039ee57d30921af` | ours | MIT | - |
| `ps5gpu/README.md` | `d95d77d13a363a17` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/build.sh` | `fda837bd02f3a60b` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/include/ps5gpu_ioctl.h` | `26f83273968c5aab` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/kmd/ps5gpu.c` | `40a9f99959b4d259` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/lib/ps5drm.cpp` | `76035fb932481b82` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/lib/ps5drm.h` | `8762674bb217c496` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/mesa/ac_ps5_drm.cpp` | `92322925518735fe` | ours (Mesa build glue / winsys for libps5gpu) | MIT | - |
| `ps5gpu/mesa/configure.sh` | `87ef4283ff37bd2f` | ours (Mesa build glue / winsys for libps5gpu) | MIT | - |
| `ps5gpu/mesa/mesa-ps5.patch` | `7b2c9f793643aefe` | our changes to Mesa 26.3.1 | MIT (Mesa's licence) | - |
| `ps5gpu/mesa/mingw-cross.txt` | `e99a036034a3993b` | ours (Mesa build glue / winsys for libps5gpu) | MIT | - |
| `ps5gpu/mesa/shim/amdgpu.h` | `1c097bb1bcd9da6e` | ours (Mesa build glue / winsys for libps5gpu) | MIT | - |
| `ps5gpu/mesa/shim/poll.h` | `b5911ed8a92869fd` | ours (Mesa build glue / winsys for libps5gpu) | MIT | - |
| `ps5gpu/mesa/shim/sys/ioccom.h` | `11fb33c74894a6ff` | ours (Mesa build glue / winsys for libps5gpu) | MIT | - |
| `ps5gpu/mesa/shim/sys/ioctl.h` | `6d1852408c9c3306` | ours (Mesa build glue / winsys for libps5gpu) | MIT | - |
| `ps5gpu/mesa/shim/xf86drm.h` | `232223b22d51a03c` | ours (Mesa build glue / winsys for libps5gpu) | MIT | - |
| `ps5gpu/tool/backend.h` | `1ec947ffc9775f74` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/tool/backend_mock.cpp` | `53ac91e19a3a53c0` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | MetalCyan:tests/directload/host_trace.cpp (1 runs, longest 4 lines) |
| `ps5gpu/tool/backend_win.cpp` | `a8fe0f0cc5a3f248` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/tool/gpuboot-mock.cpp` | `af8997d9eaa60141` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/tool/layout.h` | `d48bed99830d2922` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/tool/libtest.cpp` | `f80b7c2b26a126c6` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/tool/ps5gpu.c` | `dabd969279d5a021` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/tool/regs.h` | `996064f95d1273ba` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/tool/stage2.cpp` | `63577c3e43b988d1` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/wddm/build.sh` | `87d32e108225ff09` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/wddm/gpuboot.cpp` | `a92e01aa76012a82` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/wddm/gpuboot.h` | `f83262218c8d7b81` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/wddm/kcompat/cstddef` | `7be17a72eae1a651` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/wddm/kcompat/cstdint` | `e69c37bc448f774a` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/wddm/kcompat/cstring` | `727ee0c9c897ae93` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/wddm/ntload.c` | `c38a1f7f45fc2a50` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/wddm/p5min.c` | `fe9a5ce9d6242adb` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/wddm/ps5wddm.cpp` | `7b226530d1787d64` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/wddm/ps5wddm.inf` | `7236c724477314ca` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | GPL-ps5eth(old):p5eth.inf (1 runs, longest 4 lines); GPL-ps5bd:p5bd.inf (1 runs, longest 4 lines); GPL-ps5wifi:p5net.inf (1 runs, longest 4 lines) |
| `ps5gpu/win/autostart.cmd` | `3913ef90801c5aaf` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/gdi-mode.cmd` | `bd45b22fcee8fb2d` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/gpu-recover.cmd` | `1718dfbcffdc6774` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/kit/kit-install.cmd` | `b226b245d403e0f4` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/kit/kit-install.ps1` | `c5ecf9400b59e739` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/kit/kit-menu.ps1` | `57c9884c67ee11b3` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/kit/ps5-gamekit.ps1` | `c77eb679d7f85ac6` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/setup/gamebar-off.ps1` | `4ee9c7174930e50d` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/setup/shader-cache-dirs.ps1` | `cd37a3fa75d9437e` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/start.cmd` | `3bd151849d2ad557` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/vkreg/ps5_icd.json` | `d0b4e635971133d6` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/vkreg/vk-install.cmd` | `8292ca4827c3b47c` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5gpu/win/wddm-mode.cmd` | `f14666905384b9d9` | ours (register names from Linux's MIT amdgpu headers, included at build, not copied) | MIT | - |
| `ps5wifi/devadd.c` | `6e82342535afc8e3` | ours (MIT; the only file shipped from the ps5wifi folder) | MIT | - |
| `release/KERNEL-SOURCE.md` | `f22df184bd5b6bcc` | ours | MIT | - |
| `release/LICENSE` | `ad707ae9500ee10a` | ours | MIT | - |
| `release/LONGRUN-TESTS.md` | `59b2942444fad772` | ours | MIT | - |
| `release/RELEASING.md` | `c4c1378fbd901174` | ours | MIT | - |
| `release/arch-packages.txt` | `fc7f617fabcfcab0` | ours | MIT | - |
| `release/export-allowlist.txt` | `27b3362666a039e6` | ours | MIT | - |
| `release/export.py` | `4635deb3753757b0` | ours | MIT | - |
| `release/kernel-7.2.3-p5f6b.config` | `b150c6149c3f07e9` | ours | MIT | - |
| `release/licenses/Apache-2.0.txt` | `074e6e32c86a4c0e` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/GPL-2.0-only.txt` | `aaf135472f81c5b4` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/LGPL-2.1-only.txt` | `5749785c8bdefafc` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/LGPL-2.1-or-later.txt` | `5749785c8bdefafc` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/LICENSE.amdgpu` | `572872598565dc35` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/README.md` | `f1006f9050ead320` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/dxvk-LICENSE.txt` | `a5cb1a6ded7d2d7e` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/edk2-License.txt` | `50ce20c9cfdb0e19` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/mesa-license.rst` | `0d1a0472ecc81830` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/mingw-w64-COPYING.txt` | `99a69660981156c2` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/ps5-hackintosh-LICENSE.txt` | `291f1f66abc3843c` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/vkd3d-proton-AUTHORS.txt` | `825ecce7c240aa48` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/vkd3d-proton-COPYING.txt` | `f51ad4bf5a3bd2b5` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/vulkan-loader-LICENSE.txt` | `43c0a37e6a0fa7ff` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/wimlib-COPYING` | `1829e594e80000d0` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/wimlib-COPYING.GPLv3` | `3972dc9744f6499f` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/wimlib-COPYING.LGPL` | `20e50fe7aae3e563` | third-party licence text, verbatim | as named in the file | - |
| `release/licenses/winpthreads-COPYING.txt` | `63263614cdd29f2f` | third-party licence text, verbatim | as named in the file | - |
| `release/longrun-record.ps1` | `7c3dfbdbede1d4d4` | ours | MIT | - |
| `release/ps5win.pub` | `d4ca4fef99291126` | ours | MIT | - |
| `tools/clonecheck.py` | `265ca2186ca977d3` | ours | MIT | - |
| `tools/origins.py` | `e711e8d63def3a2c` | ours | MIT | - |
| `winremote/build-ps5therm.sh` | `9e32519df37e1a08` | ours | MIT | - |
| `winremote/ps5guard.cmd` | `cf6a7b45aa6f706d` | ours | MIT | - |
| `winremote/ps5therm.c` | `f89ff7fd177914d8` | ours | MIT | - |
