# SPDX-License-Identifier: GPL-2.0-or-later
# UC.OS as a static site in a container: stage one builds the kernel, every program and the
# boot image from this checkout; stage two serves the result. Nothing is built on the host.
#   docker compose -f deploy/compose.yml up -d --build      (from the repository root)

# --- build: the same toolchain the project is developed with (clang, wasm-ld, llvm-ar, make)
FROM archlinux:latest AS build
RUN pacman -Syu --noconfirm --needed clang lld llvm make tar gzip curl unzip diffutils findutils gawk sed grep && pacman -Scc --noconfirm
# The image is a rolling release, so the compiler version moves between builds: it is
# printed here so that every build log says what it was built with.
RUN clang --version && wasm-ld --version
WORKDIR /src
COPY . .
# Doom's two data files are not in the repository. With DOOM_DATA=1 (the default) they are
# downloaded here and checked against pinned SHA-256 sums (tools/get-doom-data.sh); the
# site then serves them beside the image and the OS fetches one the first time Doom starts.
# With DOOM_DATA=0 the build has no game data and Doom says so.
ARG DOOM_DATA=1
RUN if [ "$DOOM_DATA" = 1 ]; then sh tools/get-doom-data.sh; fi
RUN make -j"$(nproc)" B=build-site dist

# --- serve: static files only
# caddy 2.11.9 on Alpine, pinned by digest (caddy:2-alpine of 2026-11-26)
FROM caddy@sha256:003a97808e781aa9f7e75cc2f43d34d461099031faf5e20faefb9f61126836f0
COPY deploy/Caddyfile /etc/caddy/Caddyfile
COPY --from=build /src/build-site/dist /srv/ucos
EXPOSE 8080
