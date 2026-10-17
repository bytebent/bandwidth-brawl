#!/bin/bash
# Installs Bandwidth Brawl for the current user and lets macOS load it.
cd "$(dirname "$0")" || exit 1
set -e
COMP=~/Library/Audio/Plug-Ins/Components
VST3=~/Library/Audio/Plug-Ins/VST3
mkdir -p "$COMP" "$VST3"

install_bundle () {   # $1 = folder, $2 = extension
  local dest="$1/Bandwidth Brawl.$2"
  rm -rf "$dest"
  cp -R "Bandwidth Brawl.$2" "$1/"
  xattr -dr com.apple.quarantine "$dest" 2>/dev/null || true
}

install_bundle "$COMP" component
install_bundle "$VST3" vst3
killall -9 AudioComponentRegistrar 2>/dev/null || true
echo
echo "Installed Bandwidth Brawl (AU + VST3) by Bytebent Audio."
echo "In Ableton: Preferences > Plug-Ins, make sure 'Use Audio Units' and/or 'Use VST3 Plug-In System Folders' are on, then click Rescan."
echo
read -n 1 -s -r -p "Press any key to close this window."
