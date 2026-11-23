# Clone-detector report

Shipped source files compared: 113 (export tree `public-export`; licence texts and Markdown excluded).
Corpora: MetalCyan (1558 files), ps5-hackintosh (113 files), GPL-ps5wifi (8 files), GPL-ps5bd (4 files), GPL-ps5eth(old) (4 files), GPL-ps5reset(old) (4 files), GPL-linux-survey (1 files), GPL-ps5-linux-patch (1 files), GPL-linux-amdgpu (636 files).
Method: comments, whitespace, case and trivial lines removed; shared runs of 4 consecutive lines (`tools/clonecheck.py`).

| Shipped file | Corpus | Corpus file | Shared 4-line runs | Longest shared run (lines) | Shingles in file |
|---|---|---|---|---|---|
| `firmware/kms_linear.py` | ps5-hackintosh | `bringup/tools/kms_linear.py` | 100 | 71 | 208 |
| `ps5wifi/devadd.c` | GPL-ps5wifi | `devadd.c` | 28 | 31 | 28 |
| `cleanroom/directload-mit/CyanDirectLoad.hpp` | MetalCyan | `MetalCyan/CyanDirectLoad.hpp` | 24 | 15 | 94 |
| `cleanroom/ps5reset-mit/Ps5Icc.h` | GPL-ps5reset(old) | `ps5_varhook.inc` | 8 | 11 | 109 |
| `firmware/kms_linear.py` | ps5-hackintosh | `bringup/tools/patch_tripwire.py` | 7 | 10 | 208 |
| `cleanroom/p5eth-mit/p5eth.cpp` | GPL-ps5wifi | `p5net.cpp` | 3 | 5 | 1062 |
| `cleanroom/p5eth-mit/p5eth.cpp` | GPL-ps5eth(old) | `p5eth.cpp` | 3 | 5 | 1062 |
| `cleanroom/ps5reset-mit/Ps5IccResetSystemLib.c` | ps5-hackintosh | `uefi/Ps5Pkg/Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c` | 2 | 5 | 89 |
| `cleanroom/ps5reset-mit/Ps5IccResetSystemLib.inf` | GPL-ps5reset(old) | `Ps5IccResetSystemLib.inf` | 2 | 4 | 18 |
| `cleanroom/directload-mit/CyanDirectLoad.cpp` | MetalCyan | `MetalCyan/CyanDirectLoad.cpp` | 1 | 4 | 233 |
| `cleanroom/directload-mit/test_directload.cpp` | MetalCyan | `tests/directload/host_trace.cpp` | 1 | 4 | 428 |
| `ps5gpu/tool/backend_mock.cpp` | MetalCyan | `tests/directload/host_trace.cpp` | 1 | 4 | 187 |
| `ps5gpu/wddm/ps5wddm.inf` | GPL-ps5eth(old) | `p5eth.inf` | 1 | 4 | 37 |
| `ps5gpu/wddm/ps5wddm.inf` | GPL-ps5bd | `p5bd.inf` | 1 | 4 | 37 |
| `ps5gpu/wddm/ps5wddm.inf` | GPL-ps5wifi | `p5net.inf` | 1 | 4 | 37 |
