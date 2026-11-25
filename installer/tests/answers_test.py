#!/usr/bin/env python3
"""Offline test of autorun.blank_password (the answer-file password line blanked after a successful install).
Writes only into a new temporary folder."""
import sys
import tempfile
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parent.parent), str(Path(__file__).resolve().parent.parent / "lib")]
import autorun  # noqa: E402

SECRET = "Hunter2-not-real"
cases = 0


def check(name, src, want_res, want_text=None, secret_gone=True):
    global cases
    d = Path(tempfile.mkdtemp())
    f = d / "ps5win-answers.txt"
    f.write_bytes(src)
    res = autorun.blank_password(f)
    got = f.read_bytes()
    assert res == want_res, f"{name}: result {res!r}, want {want_res!r}"
    if secret_gone:
        assert SECRET.encode() not in got, f"{name}: password still in the file"
    if want_text is not None:
        assert got == want_text, f"{name}: file is {got!r}"
    assert not (d / "ps5win-answers.txt.tmp").exists(), f"{name}: temp file left"
    cases += 1
    return got


C = b"# The installer blanked this password after using it. Set your password in Windows."
NOTE = C + b"\npassword="
check("plain LF", b"computer_name=PS5\npassword=" + SECRET.encode() + b"\ntime_zone=UTC\n", "blanked",
      b"computer_name=PS5\n" + NOTE + b"\ntime_zone=UTC\n")
check("CRLF + BOM kept", b"\xef\xbb\xbfuser_name=me\r\npassword = " + SECRET.encode() + b"\r\n", "blanked",
      b"\xef\xbb\xbfuser_name=me\r\n" + C + b"\r\npassword=\r\n")
check("no final newline", b"password=" + SECRET.encode(), "blanked", NOTE)
check("comment untouched", b"# password=example\nuser_name=me\n", "none", b"# password=example\nuser_name=me\n",
      secret_gone=False)
check("empty password", b"password=\n", "none", b"password=\n")
check("no password key", b"passwordhint=x\nuser_name=me\n", "none", b"passwordhint=x\nuser_name=me\n")
check("two password lines", b"password=a\npassword=" + SECRET.encode() + b"\n", "blanked", NOTE + b"\n" + NOTE + b"\n")
check("equals sign in password", b"password=" + SECRET.encode() + b"=x=\n", "blanked", NOTE + b"\n")
missing = Path(tempfile.mkdtemp()) / "nope.txt"
assert autorun.blank_password(missing).startswith("failed: cannot read"), "missing file"
cases += 1
# the blanked file must still parse as "no password" for make_unattend / install.read_kv
import install  # noqa: E402
d = Path(tempfile.mkdtemp())
f = d / "a.txt"
f.write_text("password=" + SECRET + "\n")
autorun.blank_password(f)
assert install.read_kv(f).get("password") == "", install.read_kv(f)
import make_unattend  # noqa: E402
assert make_unattend.read_answers(str(f)).get("password") == ""
cases += 1
print(f"{cases} answer-file cases passed")
