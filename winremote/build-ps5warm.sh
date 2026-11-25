#!/bin/bash
# SPDX-License-Identifier: MIT
# Build ps5warm.exe (Windows x64, no C runtime) with LLVM 18, as winremote/build-ps5therm.sh does for ps5therm.exe.
# Output: build/ps5warm/ps5warm.exe
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/ps5warm
llvm=/opt/homebrew/opt/llvm@18/bin
cat >build/ps5warm/kernel32.def <<'DEF_END'
LIBRARY kernel32.dll
EXPORTS
GetFirmwareEnvironmentVariableExW
SetFirmwareEnvironmentVariableExW
GetCurrentProcess
GetStdHandle
WriteFile
ExitProcess
GetCommandLineW
GetLastError
DEF_END
cat >build/ps5warm/advapi32.def <<'DEF_END'
LIBRARY advapi32.dll
EXPORTS
OpenProcessToken
LookupPrivilegeValueW
AdjustTokenPrivileges
DEF_END
"$llvm/clang" -target x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -Werror -c winremote/ps5warm.c -o build/ps5warm/ps5warm.obj
"$llvm/llvm-dlltool" -m i386:x86-64 -d build/ps5warm/kernel32.def -l build/ps5warm/kernel32.lib
"$llvm/llvm-dlltool" -m i386:x86-64 -d build/ps5warm/advapi32.def -l build/ps5warm/advapi32.lib
"$llvm/lld-link" /entry:entry /subsystem:console /nodefaultlib /out:build/ps5warm/ps5warm.exe build/ps5warm/ps5warm.obj build/ps5warm/kernel32.lib build/ps5warm/advapi32.lib
ls -la build/ps5warm/ps5warm.exe
