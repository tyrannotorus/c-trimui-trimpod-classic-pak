#!/usr/bin/env bash
# Build Rockbox (Trimpod) as an SDL application for the TrimUI Brick / NextUI (tg5040).
# Cross-compiles inside the Trimpod toolchain image. Output lands in build-trimpod/.
#
#   ./build.sh            # incremental build
#   ./build.sh clean      # wipe build dir and reconfigure from scratch
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMG="trimpod-toolchain:latest"
BASE="ghcr.io/loveretro/tg5040-toolchain:latest"
BUILD_DIR="build-trimpod"
TARGET_ID=210          # retro-handheld (TrimUI Brick SDL app), see tools/configure
JOBS="$(nproc)"

# Build the derived toolchain image once (cached thereafter).
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo ">> Building toolchain image $IMG ..."
  docker build -t "$IMG" -f "$ROOT/Dockerfile.trimpod" "$ROOT"
fi

CLEAN=0
[ "${1:-}" = "clean" ] && CLEAN=1

# Run the container as the host user so files written under the bind-mounted
# /build are owned by you, not root (Docker bind mounts map container uid 0 ->
# host uid 0).  HOME=/tmp gives the non-root user a writable home for any tool
# that wants one.  This replaces the post-build sudo chown.
docker run --rm \
  --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$ROOT":/build -w /build "$IMG" bash -lc "
  set -e
  # /usr/local/bin (our sdl2-config wrapper, baked into the image) must win over
  # the sysroot's broken sdl2-config, so do NOT prepend \$SYSROOT/usr/bin here.
  if [ $CLEAN -eq 1 ] || [ ! -f $BUILD_DIR/Makefile ]; then
    rm -rf $BUILD_DIR && mkdir -p $BUILD_DIR
    cd $BUILD_DIR
    ../tools/configure --target=$TARGET_ID --type=N
  else
    cd $BUILD_DIR
  fi
  make -j$JOBS
  # Produce the runtime zip package.sh assembles the .pak from.
  make fullzip
  # uMTP-Responder (USB file transfer), built out of tree from lib/umtprd.
  # cp -a keeps the sources' mtimes, so an unchanged tree doesn't rebuild.
  cd /build
  mkdir -p $BUILD_DIR/umtprd
  cp -a lib/umtprd/. $BUILD_DIR/umtprd/
  make -C $BUILD_DIR/umtprd -j$JOBS CC=\${CROSS_COMPILE}gcc umtprd
"

echo ">> Done. Binary: $BUILD_DIR/trimpod (runtime zip: $BUILD_DIR/trimpod-full.zip)"
