# Reproducible Switch 2 (kaiHEN) .nro build container using devkitPro + ouncebrew/ounce-devkit
FROM docker.io/devkitpro/devkita64:20260219

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git pkg-config \
    && rm -rf /var/lib/apt/lists/*

# devkitpro/devkita64 already pre-installs devkitA64, switch-tools, switch-sdl2,
# switch-sdl2_ttf, switch-mbedtls, and switch-zlib.
# Sync ONLY the [ounce] repository so transient pkg.devkitpro.org 403s cannot break CI.
RUN set -eu; \
    SIGNING_KEY=DFEAE7FA965D1E6C0D41D4D75DB9FDBFA4A4DE29; \
    curl -fsSL -o /tmp/ounce.gpg https://pkg.ouncebrew.org/ounce.gpg; \
    dkp-pacman-key --add /tmp/ounce.gpg; \
    dkp-pacman-key --lsign-key "$SIGNING_KEY"; \
    rm -f /tmp/ounce.gpg; \
    cp /opt/devkitpro/pacman/etc/pacman.conf /tmp/ounce-pacman.conf; \
    sed -i '/^\[dkp-libs\]/,/^$/d; /^\[dkp-linux\]/,/^$/d' /tmp/ounce-pacman.conf; \
    printf '\n[ounce]\nServer = https://pkg.ouncebrew.org\n' >> /tmp/ounce-pacman.conf; \
    dkp-pacman --config /tmp/ounce-pacman.conf -Sy --needed --noconfirm \
        --overwrite '/opt/devkitpro/libounce/*' \
        --overwrite '/opt/devkitpro/portlibs/switch2/*' \
        --overwrite '/opt/devkitpro/switch2vars.sh' \
        libounce ouncevk libkaihen; \
    rm -f /tmp/ounce-pacman.conf; \
    dkp-pacman -Q libounce ouncevk libkaihen switch-sdl2 switch-sdl2_ttf switch-mbedtls switch-zlib

WORKDIR /src
CMD ["make", "-j4"]
