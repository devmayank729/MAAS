#!/bin/sh
# One-command setup for Linux and macOS:   ./install.sh
# Detects the OS, makes sure a C/C++ compiler exists (installs one if it can),
# builds maas, installs it to ~/.maas/bin and puts that on your PATH.
# Windows is not supported by this script yet (see SETUP.md).
set -e
cd "$(dirname "$0")"

say() { printf '%s\n' "  $*"; }
fail() { printf '%s\n' "  ✗ $*" >&2; exit 1; }

OS=$(uname -s)
case "$OS" in
  Linux) PLATFORM=linux ;;
  Darwin) PLATFORM=macos ;;
  MINGW*|MSYS*|CYGWIN*) fail "This is Windows. install.sh does not support Windows yet; see SETUP.md." ;;
  *) fail "Unsupported system: $OS" ;;
esac
if [ "$PLATFORM" = linux ] && grep -qi microsoft /proc/version 2>/dev/null; then
  say "⚠ WSL detected: other devices usually cannot reach a node running inside WSL."
fi
say "System: $PLATFORM ($(uname -m))"

SUDO=""
[ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1 && SUDO="sudo"

need_compiler() { ! command -v c++ >/dev/null 2>&1 || ! command -v cc >/dev/null 2>&1; }
if need_compiler; then
  say "No C/C++ compiler found; installing one..."
  if [ "$PLATFORM" = macos ]; then
    xcode-select --install 2>/dev/null || true
    fail "A window asking to install the Command Line Tools should have opened. Finish it, then run ./install.sh again."
  elif command -v apt-get >/dev/null 2>&1; then $SUDO apt-get update -qq && $SUDO apt-get install -y build-essential
  elif command -v dnf >/dev/null 2>&1; then $SUDO dnf install -y gcc gcc-c++ make
  elif command -v yum >/dev/null 2>&1; then $SUDO yum install -y gcc gcc-c++ make
  elif command -v pacman >/dev/null 2>&1; then $SUDO pacman -Sy --noconfirm base-devel
  elif command -v zypper >/dev/null 2>&1; then $SUDO zypper install -y gcc gcc-c++ make
  elif command -v apk >/dev/null 2>&1; then $SUDO apk add build-base linux-headers
  else fail "Could not find a package manager. Install a C++17 compiler (g++ or clang++) and run again."
  fi
  need_compiler && fail "Compiler installation did not work. Install g++ or clang++ and run again."
fi

say "Building maas (takes 1-3 minutes on an old laptop)..."
sh ./build.sh >/tmp/maas-build.log 2>&1 || { tail -20 /tmp/maas-build.log; fail "Build failed (full log: /tmp/maas-build.log)."; }

BIN_DIR="${MAAS_HOME:-$HOME/.maas}/bin"
mkdir -p "$BIN_DIR"
cp build/maas "$BIN_DIR/maas.new" && mv "$BIN_DIR/maas.new" "$BIN_DIR/maas"
chmod 755 "$BIN_DIR/maas"
say "Installed $BIN_DIR/maas"

LINE="export PATH=\"$BIN_DIR:\$PATH\""
for rc in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
  if [ -f "$rc" ] || [ "$rc" = "$HOME/.profile" ]; then
    grep -qs "$BIN_DIR" "$rc" || printf '\n# MAAS\n%s\n' "$LINE" >> "$rc"
  fi
done

if ! command -v python3 >/dev/null 2>&1; then
  say "⚠ No python3 found. Scripts need a runtime: run  maas runtime install python  (no admin needed)."
fi

say "✓ Done. Open a new terminal (or run: $LINE)"
say "  Start this machine as a node:   maas start"
