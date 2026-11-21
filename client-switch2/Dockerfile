# Reproducible Switch 2 (kaiHEN) .nro build container using devkitPro + ouncebrew/ounce-devkit
FROM docker.io/devkitpro/devkita64:latest

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git \
    && rm -rf /var/lib/apt/lists/*

# Install Switch portlibs (compatible unchanged with libounce)
RUN dkp-pacman -Syu --noconfirm \
        switch-sdl2 \
        switch-sdl2_ttf \
        switch-mbedtls \
        switch-zlib \
    && dkp-pacman -Scc --noconfirm

# Install ouncebrew/ounce-devkit (libounce, ouncevk, libkaihen)
RUN git clone --depth 1 https://github.com/ouncebrew/ounce-devkit.git /tmp/ounce-devkit \
    && cd /tmp/ounce-devkit \
    && sh install.sh \
    && rm -rf /tmp/ounce-devkit

WORKDIR /src
CMD ["make", "-j4"]
