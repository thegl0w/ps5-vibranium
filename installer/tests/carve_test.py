# SPDX-License-Identifier: MIT
"""expected_carve(): the VRAM carve-out the launch package's run_stage.sh checks for (4 GiB or 5 GiB)."""
import io
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import install  # noqa: E402

LINE = "grep -Eq '^%s-46fffffff : Reserved$' /proc/iomem\n"


def pkg(d, name, text):
    p = Path(d) / name
    with tarfile.open(p, "w:gz") as t:
        b = text.encode()
        i = tarfile.TarInfo("run_stage.sh"); i.size = len(b)
        t.addfile(i, io.BytesIO(b))
    return p


n = 0
with tempfile.TemporaryDirectory() as d:
    assert install.expected_carve(pkg(d, "a.tgz", "set -e\n" + LINE % "370000000")) == "370000000"; n += 1
    assert install.expected_carve(pkg(d, "b.tgz", "set -e\n" + LINE % "330000000")) == "330000000"; n += 1
    assert install.expected_carve(Path(d) / "missing.tgz") == "370000000"; n += 1
    for bad in ("set -e\n", LINE % "330000000" + LINE % "370000000", LINE % "2b0000000"):
        try:
            install.expected_carve(pkg(d, "bad.tgz", bad))
        except SystemExit:
            n += 1
        else:
            raise AssertionError("accepted: " + bad)
print(f"{n} carve cases passed")
