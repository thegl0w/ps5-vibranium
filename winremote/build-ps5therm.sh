#!/bin/bash
# Build winremote/ps5therm.exe (Windows x64, no C runtime) with LLVM 18.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/ps5therm
llvm=/opt/homebrew/opt/llvm@18/bin
cat >build/ps5therm/kernel32.def <<'DEF_END'
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
cat >build/ps5therm/advapi32.def <<'DEF_END'
LIBRARY advapi32.dll
EXPORTS
OpenProcessToken
LookupPrivilegeValueW
AdjustTokenPrivileges
DEF_END
"$llvm/clang" -target x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -Werror -c winremote/ps5therm.c -o build/ps5therm/ps5therm.obj
"$llvm/llvm-dlltool" -m i386:x86-64 -d build/ps5therm/kernel32.def -l build/ps5therm/kernel32.lib
"$llvm/llvm-dlltool" -m i386:x86-64 -d build/ps5therm/advapi32.def -l build/ps5therm/advapi32.lib
"$llvm/lld-link" /entry:entry /subsystem:console /nodefaultlib /out:build/ps5therm/ps5therm.exe build/ps5therm/ps5therm.obj build/ps5therm/kernel32.lib build/ps5therm/advapi32.lib
