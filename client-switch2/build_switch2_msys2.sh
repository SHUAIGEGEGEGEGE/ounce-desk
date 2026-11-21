#!/usr/bin/env bash
# Run inside C:\Users\APPLE\msys64\usr\bin\bash.exe (once pkg.devkitpro.org is reachable without Cloudflare 403)
set -euo pipefail

if ! grep -q '^\[dkp-libs\]' /etc/pacman.conf; then
    cat >> /etc/pacman.conf <<'EOF'

[dkp-libs]
Server = https://pkg.devkitpro.org/packages

[dkp-windows]
Server = https://pkg.devkitpro.org/packages/windows/$arch/
EOF
fi

pacman -Sy --noconfirm --needed devkitA64 switch-tools general-tools \
    switch-sdl2 switch-sdl2_ttf switch-mbedtls switch-zlib

curl -fsSL -o /tmp/ounce-install.sh https://raw.githubusercontent.com/ouncebrew/ounce-devkit/main/install.sh
sh /tmp/ounce-install.sh

export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"

cd "$(dirname "$0")"
make clean && make -j4
ls -lh OunceDesk.nro
