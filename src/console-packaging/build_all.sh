#!/usr/bin/env bash
# Linux SDK build helper. Merely writing this file does not invoke a toolchain.
set -Eeuo pipefail
umask 022
die() { printf 'build_all: %s\n' "$*" >&2; exit 1; }
usage() { printf 'Usage: bash build_all.sh --config /absolute/path/to/trusted-build.env\n'; }
if [[ ${1:-} == --help ]]; then usage; exit 0; fi
[[ $# == 2 && $1 == --config ]] || { usage >&2; exit 2; }
[[ $(uname -s) == Linux ]] || die 'Run later on the qualified Linux SDK host, not Windows.'
[[ $2 == /* && -f $2 && ! -L $2 ]] || die 'Configuration must be an absolute regular file.'
# The configuration is executable shell input; use only a file you control.
source "$2"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
SOURCE_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
PIN=0792144fe3a7b59908b0afdb2c01722e79040360
for var in TINA_TOOLCHAIN_BIN TINA_SYSROOT RETROARCH_SOURCE RETROARCH_CONFIGURE_ARGS BUILD_ROOT; do
    [[ -n ${!var:-} ]] || die "Missing $var"
    [[ ${!var} == /* && ${!var} != / && ${!var} != *[[:space:]]* ]] ||
        die "$var must be absolute, non-root, and contain no whitespace (upstream make limitation)."
done
[[ $SOURCE_ROOT != *[[:space:]]* ]] || die 'Put the sibling sources in a path without whitespace.'
TINA_TRIPLET=${TINA_TRIPLET:-aarch64-tina-linux}
[[ $TINA_TRIPLET == aarch64-tina-linux ]] || die 'Expected the aarch64-tina-linux SDK triplet.'
JOBS=${JOBS:-2}
[[ $JOBS =~ ^[1-9][0-9]*$ ]] && (( JOBS <= 256 )) || die 'JOBS must be 1..256.'
[[ -d $TINA_SYSROOT && -d $TINA_TOOLCHAIN_BIN && -d $RETROARCH_SOURCE ]] || die 'Missing explicit SDK/source directory.'
TINA_SYSROOT=$(cd -- "$TINA_SYSROOT" && pwd -P)
export TINA_SYSROOT
export PATH="$TINA_TOOLCHAIN_BIN:$PATH"
for tool in cmake make git python3 pkg-config install mktemp; do command -v "$tool" >/dev/null || die "Missing host tool: $tool"; done
for pair in 'CC:gcc' 'CXX:g++' 'AR:ar' 'RANLIB:ranlib' 'STRIP:strip' 'READELF:readelf'; do
    key=${pair%%:*}; suffix=${pair#*:}
    value="$TINA_TOOLCHAIN_BIN/$TINA_TRIPLET-$suffix"
    [[ -x $value ]] || die "Missing explicit SDK tool: $value"
    export "TINA_$key=$value"
done
[[ $("$TINA_CC" -dumpmachine) == "$TINA_TRIPLET" ]] || die 'Compiler target differs from the requested SDK.'
[[ -f $RETROARCH_CONFIGURE_ARGS && ! -L $RETROARCH_CONFIGURE_ARGS ]] || die 'Missing board configure-argument file.'
board_args=()
while IFS= read -r arg || [[ -n $arg ]]; do
    [[ $arg != *$'\r'* ]] || die 'Use LF line endings in the board configure file.'
    case "$arg" in ''|\#*) continue;; esac
    case "$arg" in
        --host*|--prefix*|--disable-threads*|--disable-dynamic*|--enable-vulkan*)
            die "Board option conflicts with managed build: $arg";;
        --enable-*|--disable-*|--with-*) board_args+=("$arg");;
        *) die "Invalid board configure argument (replace the template sentinel): $arg";;
    esac
done < "$RETROARCH_CONFIGURE_ARGS"
(( ${#board_args[@]} > 0 )) || die 'Supply explicit qualified board video/audio/input configure arguments.'
git -C "$RETROARCH_SOURCE" cat-file -e "$PIN^{commit}" || die 'Pinned RetroArch commit is not available locally.'

# Isolate pkg-config from host /usr and user CMake/package registries.
export PKG_CONFIG_SYSROOT_DIR="$TINA_SYSROOT"
export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR=${TINA_PKG_CONFIG_LIBDIR:-"$TINA_SYSROOT/usr/lib/pkgconfig:$TINA_SYSROOT/usr/share/pkgconfig:$TINA_SYSROOT/usr/lib/$TINA_TRIPLET/pkgconfig"}
python3 - "$TINA_SYSROOT" "$PKG_CONFIG_LIBDIR" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1]).resolve(strict=True)
for item in sys.argv[2].split(':'):
    if not item or not pathlib.Path(item).resolve().is_relative_to(root):
        raise SystemExit('pkg-config directories must stay inside the explicit target sysroot')
PY
export CC="$TINA_CC" CXX="$TINA_CXX" AR="$TINA_AR" RANLIB="$TINA_RANLIB" STRIP="$TINA_STRIP"
export CROSS_COMPILE="$TINA_TOOLCHAIN_BIN/$TINA_TRIPLET-"
export CFLAGS="--sysroot=$TINA_SYSROOT -mcpu=cortex-a53 -O2 -ffunction-sections -fdata-sections ${TINA_EXTRA_CFLAGS:-}"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="--sysroot=$TINA_SYSROOT -Wl,--gc-sections ${TINA_EXTRA_LDFLAGS:-}"
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH CMAKE_PREFIX_PATH CMAKE_TOOLCHAIN_FILE DESTDIR
export MAKEFLAGS= MFLAGS=
mkdir -p -- "$BUILD_ROOT"
BUILD_DIR=$(mktemp -d "$BUILD_ROOT/console-build.XXXXXXXX")
trap 'printf "build_all: failed; partial output retained at %s (no completion manifest)\n" "$BUILD_DIR" >&2' ERR
printf 'Build directory: %s\n' "$BUILD_DIR"
cmake -S "$SOURCE_ROOT/console-supervisor" -B "$BUILD_DIR/appliance" \
    -G 'Unix Makefiles' \
    -DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/cmake/aarch64-tina-linux.cmake" \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=OFF -DLAUNCHER_BUILD_TESTS=OFF \
    -DSESSIOND_BUILD_DAEMON=ON -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build "$BUILD_DIR/appliance" --parallel "$JOBS" --target \
    console-supervisor console-launcher console-sessiond console-ctl
PAYLOAD="$BUILD_DIR/payload"
cmake --install "$BUILD_DIR/appliance" --prefix "$PAYLOAD"

# Offline, private clone: never patch or clean the user's original RetroArch tree.
git clone --no-hardlinks --no-checkout -- "$RETROARCH_SOURCE" "$BUILD_DIR/retroarch"
git -C "$BUILD_DIR/retroarch" checkout --detach "$PIN"
python3 "$SOURCE_ROOT/retroarch-magicx-bridge/install.py" --retroarch "$BUILD_DIR/retroarch"
(
    cd -- "$BUILD_DIR/retroarch"
    ./configure --host="$TINA_TRIPLET" --prefix=/opt/console \
        --enable-threads --enable-dynamic --enable-networking --disable-vulkan "${board_args[@]}"
    make --jobs="$JOBS" HAVE_MAGICX_BRIDGE=1
)
install -m 0755 "$BUILD_DIR/retroarch/retroarch" "$PAYLOAD/bin/retroarch"
mkdir -p "$BUILD_DIR/unstripped" "$PAYLOAD/build-info"
for relative in bin/console-supervisor bin/console-sessiond bin/console-launcher bin/retroarch bootstrap/usr/magicx/bin/console-ctl; do
    binary="$PAYLOAD/$relative"
    install -m 0755 "$binary" "$BUILD_DIR/unstripped/${relative##*/}"
    "$TINA_STRIP" --strip-unneeded "$binary"
    "$TINA_READELF" -W -h -l -d --version-info "$binary" > "$PAYLOAD/build-info/${relative##*/}.elf.txt"
done
cp -- "$RETROARCH_CONFIGURE_ARGS" "$PAYLOAD/build-info/retroarch.configure.args"
cp -- "$BUILD_DIR/retroarch/config.log" "$PAYLOAD/build-info/retroarch-config.log"
"$TINA_CC" --version > "$PAYLOAD/build-info/compiler.txt"
python3 "$SCRIPT_DIR/package.py" seal-build --payload "$PAYLOAD" --triplet "$TINA_TRIPLET" --sysroot "$TINA_SYSROOT"
trap - ERR
printf '\nComplete, stripped payload: %s\nNo target executables were run.\n' "$PAYLOAD"
printf 'Next: sh %q --payload %q --output /absolute/path/to/new-package\n' "$SCRIPT_DIR/make_image.sh" "$PAYLOAD"
