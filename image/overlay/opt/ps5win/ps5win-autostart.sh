#!/bin/bash
# SPDX-License-Identifier: MIT
# Runs on the console screen (tty1) at every start of this disk's Linux:
#  - Windows installed (/etc/ps5win.conf): count down, then start Windows; any key during the countdown stays in Linux.
#  - not installed yet: hand over to the installer (/opt/ps5win/installer/autorun), which finds the ISO and installs.
set -u
setfont ter-v32b 2>/dev/null || true
clear
echo "PS5 Windows"
echo
if [ -f /etc/ps5win.conf ]; then
  secs=10
  echo "Windows starts in $secs seconds. Press any key to stay in Linux."
  while [ $secs -gt 0 ]; do
    if read -r -s -n 1 -t 1 _; then
      echo; echo "Staying in Linux. Start Windows later with: /opt/ps5win/ps5win-start.sh"
      exit 0
    fi
    secs=$((secs - 1)); printf '\r%2d ' $secs
  done
  echo
  exec /opt/ps5win/ps5win-start.sh
fi
if [ -x /opt/ps5win/installer/autorun ]; then
  exec /opt/ps5win/installer/autorun
fi
echo "Windows is not installed and the installer is missing from this image."
exit 1
