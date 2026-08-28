#!/usr/bin/env bash
# Build TrimPod for NextUI's tg5040 and H700 platforms.
#
#   ./build.sh                 # incremental build for both platforms
#   ./build.sh clean           # clean build for both platforms
#   ./build.sh tg5040          # build only TrimUI Brick / Brick Pro
#   ./build.sh h700 clean      # clean build only Anbernic RG34XXSP
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
JOBS="$(nproc)"
CLEAN=0
REQUESTED=""

for arg in "$@"; do
    case "$arg" in
        clean) CLEAN=1 ;;
        tg5040|h700)
            [ -z "$REQUESTED" ] || {
                echo "Choose only one platform: tg5040 or h700." >&2
                exit 2
            }
            REQUESTED="$arg"
            ;;
        *)
            echo "Usage: $0 [tg5040|h700] [clean]" >&2
            exit 2
            ;;
    esac
done

if [ -n "$REQUESTED" ]; then
    PLATFORMS="$REQUESTED"
else
    PLATFORMS="tg5040 h700"
fi

build_platform() {
    platform="$1"
    case "$platform" in
        tg5040)
            base="ghcr.io/loveretro/tg5040-toolchain:latest"
            image="trimpod-toolchain-tg5040:latest"
            build_dir="build-trimpod"
            target_id=210
            ;;
        h700)
            base="ghcr.io/loveretro/h700-toolchain:latest"
            image="trimpod-toolchain-h700:latest"
            build_dir="build-trimpod-h700"
            target_id=302
            ;;
        *)
            echo "Unsupported build platform: $platform" >&2
            exit 2
            ;;
    esac

    echo ">> Preparing $platform toolchain image $image ..."
    docker build \
        --build-arg "BASE_IMAGE=$base" \
        --build-arg "TRIMPOD_PLATFORM=$platform" \
        -t "$image" -f "$ROOT/Dockerfile.trimpod" "$ROOT"

    # Run as the host user so bind-mounted build output is not owned by root.
    docker run --rm \
      --user "$(id -u):$(id -g)" -e HOME=/tmp \
      -v "$ROOT":/build -w /build "$image" bash -lc "
      set -e
      bash tools/build_trimpod_fonts.sh

      if [ $CLEAN -eq 1 ] || [ ! -f $build_dir/Makefile ]; then
        rm -rf $build_dir && mkdir -p $build_dir
        cd $build_dir
        ../tools/configure --target=$target_id --type=N
      else
        cd $build_dir
      fi
      make -j$JOBS
      make fullzip
    "

    echo ">> $platform done: $build_dir/trimpod ($build_dir/trimpod-full.zip)"
}

for platform in $PLATFORMS; do
    build_platform "$platform"
done
