# Clean-room record and code origins

This file keeps the evidence together: which parts were rewritten clean-room and from what, where every shipped file
comes from, and what an automated comparison against the code we read found.

## The three clean-room parts

Three parts of ps5win first followed code under other licences. Each was rewritten from a written description only:
one person read the original and wrote a specification of the hardware behaviour and the interface the rest of
ps5win needs; the rewrite was written from that specification without opening the original. The questions the
implementer asked and the answers are kept next to the specs.

| Part | Specification | Rewrite (MIT) | What it replaced |
|---|---|---|---|
| GPU microcode direct load | `cleanroom/spec-directload.md` | `cleanroom/directload-mit/` | MetalCyan's `CyanDirectLoad` (non-commercial licence) |
| Ethernet driver `p5eth` | `cleanroom/spec-p5eth.md` | `cleanroom/p5eth-mit/` | our first driver, which followed Linux's GPL `mts` driver |
| Restart / power-off library | `cleanroom/spec-icc-reset.md` | `cleanroom/ps5reset-mit/` | our first library, which followed Linux's GPL ICC code |

Each rewrite folder has a `NOTES.md` saying which documents were used, and that no GPL or MetalCyan source was opened.

## Per-file origins

[`ORIGINS.md`](ORIGINS.md): every published file with its SHA-256, origin and licence, and the comparison's finding
for it. In short: everything is ours (MIT) except the third-party licence texts, our patch to Mesa (MIT, Mesa's
licence), and `firmware/kms_linear.py`, which is derived from ps5-hackintosh (MIT, rodri0x) and says so in its header;
ps5-hackintosh's licence is in `release/licenses/`.

## Clone-detector report

[`CLONECHECK.md`](CLONECHECK.md), made by `tools/clonecheck.py`: every shipped source file compared with MetalCyan,
ps5-hackintosh's sources, the GPL code we used (our first Ethernet driver and reset library, the Wi-Fi and Blu-ray
drivers that do not ship, our notes on Linux modules, the ps5-linux kernel patch) and Linux's amdgpu driver. Comments,
whitespace, case and trivial lines are removed; any shared run of 4 consecutive lines is reported.

What it found, and why each finding is not copied code:

| Finding | Explanation |
|---|---|
| `firmware/kms_linear.py` and ps5-hackintosh's `kms_linear.py` / `patch_tripwire.py` | Derived from ps5-hackintosh (MIT), credited in the file header; licence shipped. |
| `cleanroom/directload-mit/CyanDirectLoad.hpp` and MetalCyan's header (longest shared run 15 lines) | Declarations only: struct field lists, function signatures and two timeout constants. `spec-directload.md` prescribes exactly this interface so the rewrite is a drop-in replacement for its callers. The implementation (`CyanDirectLoad.cpp`) shares one 4-line run: a status-to-name `switch`. |
| `cleanroom/directload-mit/test_directload.cpp`, `ps5gpu/tool/backend_mock.cpp` and MetalCyan's test harness | One 4-line run each: a mock register-access pattern prescribed by the same interface. |
| `cleanroom/ps5reset-mit/Ps5Icc.h` and our earlier GPL variable hook | The `PS5_THERMAL` structure: a data layout of our own design, prescribed by `spec-icc-reset.md` so both versions answer the same variable. |
| `cleanroom/ps5reset-mit/*` and ps5-hackintosh's `Ps5ResetSystemLib` / our earlier `.inf` | EDK2 library boilerplate (MIT/BSD on both sides). |
| `cleanroom/p5eth-mit/p5eth.cpp` and our earlier drivers (longest run 5 lines) | NDIS API set-up lines (pool parameters, buffer fields) that every NDIS miniport writes the same way. |
| `ps5gpu/wddm/ps5wddm.inf` and our other INFs | INF boilerplate of our own drivers. |

No shipped file shares code with Linux's amdgpu driver or the ps5-linux kernel patch. Register names and offsets come
from Linux's amdgpu register headers, which are MIT-licensed and are included at build time, not copied.

Not covered: NootedRed (not compared; MetalCyan, which was compared, is the code we read).
