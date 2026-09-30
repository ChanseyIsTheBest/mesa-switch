#!/usr/bin/env bash
# Builds this tree (mesa-switch + the nfsmw-nx driver changes) with the repository's MSYS2 build,
# build-unified.sh, after checking everything that build needs. Prints what to pass to
# UnleashedRecomp at the end.
#
# Run it from a regular MSYS2 installation (the one at C:/msys64, with the CLANGARM64, CLANG64 and
# UCRT64 environments), not from devkitPro's own MSYS2 (C:/devkitPro/msys2), whose pacman only has
# MSYS and devkitPro packages. devkitPro can stay where it is (C:/devkitPro or C:/msys64/opt/devkitpro).
#
#   bash build-nfsmw.sh --enable-msys2-repos add MSYS2's standard package repositories to this MSYS2 (for
#                                            devkitPro's own MSYS2, which ships without them; no new install)
#   bash build-nfsmw.sh --docker             build with the repository's Docker build (build-switch.sh)
#                                            instead: needs only Docker Desktop, nothing from MSYS2
#   bash build-nfsmw.sh --check              only check, print what is missing and how to get it
#   bash build-nfsmw.sh --install-packages   install the missing MSYS2 packages with pacman
#   bash build-nfsmw.sh --setup-rust         install the Rust toolchain the build expects, into build/deps
#   bash build-nfsmw.sh                      check, then build
#
# The options can be combined. Overrides: DEVKITPRO (devkitPro folder), and everything build-unified.sh
# accepts (BUILD_DIR, DESTDIR, MESA_SWITCH_RUSTC, CROSS_FILE, NATIVE_FILE, HOST_CROSS_FILE,
# MESA_OPTIMIZATION...).
#
# What the repository's MSYS2 build uses (an ARM64 MSYS2 driving x64 host tools, as on a
# Windows-on-ARM PC):
#   CLANGARM64  meson, ninja, python (+ mako, pyyaml, packaging, setuptools), clang, bindgen, cbindgen
#               (on an x64 PC the same tools come from CLANG64: ARM64 programs do not run there)
#   CLANG64     clang, llvm, SPIRV-LLVM-Translator, SPIRV-Tools, pkgconf, cmake -> host tools (mesa_clc)
#   UCRT64      gcc                                                            -> links Rust proc macros
#   MSYS        git, flex, bison, pkgconf
#   devkitPro   devkitA64, libnx, switch-zlib, switch-libzstd, switch-libexpat
#   Rust        x86_64-pc-windows-gnu host toolchain + aarch64-unknown-linux-gnu std (--setup-rust)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

check_only=0
use_docker=0
enable_repos=0
install_packages=0
setup_rust=0
passthrough=()
for arg in "$@"; do
    case "$arg" in
        --check) check_only=1 ;;
        --install-packages) install_packages=1 ;;
        --setup-rust) setup_rust=1 ;;
        --docker) use_docker=1 ;;
        --enable-msys2-repos) enable_repos=1 ;;
        -h|--help) sed -n '2,33p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) passthrough+=("$arg") ;;
    esac
done

# ---------------------------------------------------------------- Docker route
# build-switch.sh runs the whole build in a Linux container (devkitA64, LLVM, Rust, bindgen, all in
# the image), so nothing has to be installed on Windows besides Docker Desktop. Its output,
# builddir-switch/src/nouveau/vulkan/libvulkan.a, is the layout UnleashedRecomp's build script
# was written for.
if [ "$use_docker" = 1 ]; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "docker was not found. Install Docker Desktop (and start it), or use the MSYS2 route." >&2
        exit 2
    fi
    if ! docker info >/dev/null 2>&1; then
        echo "Docker is installed but not running: start Docker Desktop and run this again." >&2
        exit 2
    fi
    bash "$ROOT/build-switch.sh"
    lib="$ROOT/builddir-switch/src/nouveau/vulkan/libvulkan.a"
    root_shown="$ROOT"
    command -v cygpath >/dev/null 2>&1 && root_shown="$(cygpath -m "$ROOT")"
    echo
    echo "Driver: $lib"
    echo "UnleashedRecomp: rm -rf build && NVK_ROOT=$root_shown tools/build-switch.sh"
    exit 0
fi

if [ -z "${MSYSTEM:-}" ] || ! command -v cygpath >/dev/null 2>&1; then
    echo "Run this from an MSYS2 shell (build-unified.sh refuses to run elsewhere)." >&2
    echo "On Linux, use the Docker build instead: ./build-switch.sh (see nfsmw/README.md)." >&2
    exit 2
fi

MSYS_WIN="$(cygpath -m /)"      # Windows path of this MSYS2, e.g. C:/msys64
MSYS_WIN="${MSYS_WIN%/}"        # cygpath prints the root with a trailing slash
ROOT_WIN="$(cygpath -m "$ROOT")"

have() { command -v "$1" >/dev/null 2>&1; }
version_ge() { # version_ge A B -> true if A >= B
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]
}

# ---------------------------------------------------------------- the right MSYS2?
# The build needs CLANGARM64/CLANG64/UCRT64 packages. devkitPro's own MSYS2 does not have those
# repositories, and its /clang64, /ucrt64 paths (used by the build's wrappers) would be empty.
has_repo() { grep -q "^\[$1\]" /etc/pacman.conf 2>/dev/null; }

# Native tools (meson, python, bindgen, cbindgen, and the clang bindgen asks for its headers) come from
# CLANGARM64 on a Windows-on-ARM PC and from CLANG64 on an x64 PC: ARM64 programs do not run on x64
# Windows ("Exec format error"). The repository's MSYS2 files were written on an ARM64 PC.
# NFSMW_TOOLS_ENV=clang64|clangarm64 overrides the detection.
runs() { "$@" >/dev/null 2>&1; }
tools_env="${NFSMW_TOOLS_ENV:-}"
if [ -z "$tools_env" ]; then
    case "${PROCESSOR_IDENTIFIER:-} ${PROCESSOR_ARCHITECTURE:-} ${PROCESSOR_ARCHITEW6432:-}" in
        *ARMv*|*ARM64*) tools_env=clangarm64 ;;
        *) tools_env=clang64 ;;
    esac
    # Installed ARM64 programs that do not start mean an x64 PC, whatever the variables say.
    if [ "$tools_env" = clangarm64 ] && [ -x /clangarm64/bin/clang.exe ] && ! runs /clangarm64/bin/clang.exe --version; then
        tools_env=clang64
    fi
fi
if [ "$tools_env" = clangarm64 ]; then
    tools_pkg=mingw-w64-clang-aarch64
else
    tools_pkg=mingw-w64-clang-x86_64
fi
TOOLS="/$tools_env/bin"

# What the build really needs from MSYS2 is packages: NVK compiles two OpenCL kernels
# (src/nouveau/vulkan/cl/*.cl) with mesa_clc, a host tool built from this tree with LLVM, Clang,
# SPIRV-LLVM-Translator and SPIRV-Tools. devkitPro's own MSYS2 is a normal MSYS2 whose pacman.conf
# only lists MSYS's and devkitPro's repositories; adding MSYS2's standard ones makes those
# packages installable there, without installing another MSYS2.
needed_repos="clang64 ucrt64"
[ "$tools_env" = clangarm64 ] && needed_repos="clangarm64 clang64 ucrt64"
missing_repos=""
for repo in $needed_repos; do
    has_repo "$repo" || missing_repos="$missing_repos $repo"
done
if [ "$enable_repos" = 1 ] && [ -n "$missing_repos" ]; then
    echo "== Adding MSYS2's package repositories ($missing_repos ) to $MSYS_WIN/etc/pacman.conf"
    if [ ! -w /etc/pacman.conf ]; then
        echo "Cannot write /etc/pacman.conf: start this shell as administrator, or fix the folder's permissions." >&2
        exit 2
    fi
    [ -f /etc/pacman.conf.nfsmw-backup ] || cp /etc/pacman.conf /etc/pacman.conf.nfsmw-backup
    mirrorlist=/etc/pacman.d/mirrorlist.mingw
    if [ ! -f "$mirrorlist" ] || ! grep -q 'mingw/\$repo' "$mirrorlist"; then
        mirrorlist=/etc/pacman.d/mirrorlist.nfsmw-mingw
        mkdir -p /etc/pacman.d
        printf '%s\n' \
            '# Added by build-nfsmw.sh: MSYS2 mirrors for the mingw package repositories.' \
            'Server = https://mirror.msys2.org/mingw/$repo/' \
            'Server = https://repo.msys2.org/mingw/$repo/' > "$mirrorlist"
    fi
    for repo in $missing_repos; do
        printf '\n[%s]\nInclude = %s\n' "$repo" "$mirrorlist" >> /etc/pacman.conf
    done
    echo "   (the previous file is kept as /etc/pacman.conf.nfsmw-backup)"
    # devkitPro's server is sometimes slow; its databases are not needed here.
    pacman -Sy || echo "== Some package databases failed to download (see above); continuing with the ones that did."
    # Packages from these repositories are signed by MSYS2; make sure its keys are current.
    pacman -S --needed msys2-keyring || true
    rest=()
    [ "$check_only" = 1 ] && rest+=(--check)
    [ "$install_packages" = 1 ] && rest+=(--install-packages)
    [ "$setup_rust" = 1 ] && rest+=(--setup-rust)
    exec bash "${BASH_SOURCE[0]}" "${rest[@]}" "${passthrough[@]}"
fi

if ! has_repo clang64 && ! has_repo ucrt64 && [ ! -x /clang64/bin/clang.exe ]; then
    echo "This MSYS2 ($MSYS_WIN) has no access to the CLANGARM64/CLANG64/UCRT64 packages the build needs" >&2
    echo "(LLVM, Clang and SPIRV tools for mesa_clc, which compiles NVK's two OpenCL kernels; plus meson, python" >&2
    echo "and bindgen). It looks like devkitPro's own MSYS2, which ships with only the MSYS and devkitPro repositories." >&2
    echo >&2
    echo "Any one of these works:" >&2
    echo "  1. Keep this MSYS2 and add MSYS2's standard package repositories to it (nothing else to install):" >&2
    echo "       bash build-nfsmw.sh --enable-msys2-repos --install-packages" >&2
    echo "       bash build-nfsmw.sh" >&2
    echo "  2. Docker Desktop, if you have it: bash build-nfsmw.sh --docker" >&2
    echo "  3. A separate MSYS2 from https://www.msys2.org, in C:\\msys64." >&2
    exit 2
fi

if ! has_repo "$tools_env" && [ ! -x "$TOOLS/clang.exe" ]; then
    echo "The ${tools_env} repository is not enabled in /etc/pacman.conf: run bash build-nfsmw.sh --enable-msys2-repos" >&2
    echo "(or uncomment its [$tools_env] section and run pacman -Sy), then run this again." >&2
    exit 2
fi

# The repository's MSYS2 files use the CLANGARM64 tools (meson, ninja, python, clang, bindgen,
# cbindgen). Put them first whatever shell of this MSYS2 this was started from.
if [ -d "$TOOLS" ]; then
    export PATH="$TOOLS:$PATH"
fi

missing_packages=()
missing_dkp_packages=()
problems=()
need_package() { missing_packages+=("$1"); }
need_dkp_package() { missing_dkp_packages+=("$1"); }
problem() { problems+=("$1"); }

# ---------------------------------------------------------------- MSYS tools
have git   || need_package git
have flex  || need_package flex
have bison || need_package bison
[ -x /usr/bin/pkg-config ] || need_package pkgconf   # used by switch-pkg-config-wrapper.sh

# ---------------------------------------------------------------- CLANGARM64
# The repository's scripts expect the native (CLANGARM64) meson and python, not MSYS's.
{ [ -x "$TOOLS/meson" ] || [ -x "$TOOLS/meson.exe" ]; } || need_package "$tools_pkg-meson"
[ -x "$TOOLS/ninja.exe" ] || need_package "$tools_pkg-ninja"
runs "$TOOLS/clang.exe" --version || need_package "$tools_pkg-clang"
if runs "$TOOLS/python.exe" -c "pass"; then
    for module in mako:python-mako yaml:python-yaml packaging:python-packaging setuptools:python-setuptools; do
        runs "$TOOLS/python.exe" -c "import ${module%%:*}" || need_package "$tools_pkg-${module#*:}"
    done
else
    need_package "$tools_pkg-python"
    need_package "$tools_pkg-python-mako"
    need_package "$tools_pkg-python-yaml"
    need_package "$tools_pkg-python-packaging"
    need_package "$tools_pkg-python-setuptools"
fi
if [ -x "$TOOLS/meson" ] || [ -x "$TOOLS/meson.exe" ]; then
    meson_version="$(meson --version 2>/dev/null || true)"
    if [ -n "$meson_version" ]; then
        version_ge "$meson_version" 1.7.0 ||
            problem "meson $meson_version is too old (1.7 or newer): pacman -Syu $tools_pkg-meson"
    fi
fi

# ---------------------------------------------------------------- CLANG64 host tools (mesa_clc)
[ -x /clang64/bin/clang.exe ]       || need_package mingw-w64-clang-x86_64-clang
[ -x /clang64/bin/llvm-config.exe ] || need_package mingw-w64-clang-x86_64-llvm
[ -f /clang64/lib/pkgconfig/LLVMSPIRVLib.pc ] || need_package mingw-w64-clang-x86_64-spirv-llvm-translator
[ -f /clang64/lib/pkgconfig/SPIRV-Tools.pc ]  || need_package mingw-w64-clang-x86_64-spirv-tools
[ -x /clang64/bin/pkg-config.exe ]  || [ -x /clang64/bin/pkgconf.exe ] || need_package mingw-w64-clang-x86_64-pkgconf
[ -x /clang64/bin/cmake.exe ]       || need_package mingw-w64-clang-x86_64-cmake

# ---------------------------------------------------------------- linker for Rust proc macros (x64 GNU)
native_linker="${MESA_SWITCH_NATIVE_LINKER:-}"
if [ -z "$native_linker" ]; then
    for candidate in /ucrt64/bin/gcc.exe /mingw64/bin/gcc.exe; do
        if [ -x "$candidate" ]; then
            native_linker="$(cygpath -m "$candidate")"
            break
        fi
    done
fi
[ -n "$native_linker" ] || need_package mingw-w64-ucrt-x86_64-gcc

# ---------------------------------------------------------------- devkitPro (wherever it is)
dkp_posix=""
for candidate in "${DEVKITPRO:-}" /opt/devkitpro /c/devkitPro "$MSYS_WIN/opt/devkitpro"; do
    [ -n "$candidate" ] || continue
    path="$(cygpath -u "$candidate" 2>/dev/null || echo "$candidate")"
    if [ -e "$path/devkitA64/bin/aarch64-none-elf-gcc.exe" ] || [ -e "$path/devkitA64/bin/aarch64-none-elf-gcc" ]; then
        dkp_posix="$path"
        break
    fi
done
if [ -z "$dkp_posix" ]; then
    need_dkp_package switch-dev
else
    [ -f "$dkp_posix/libnx/lib/libnx.a" ]                 || need_dkp_package libnx
    [ -f "$dkp_posix/portlibs/switch/lib/libz.a" ]        || need_dkp_package switch-zlib
    [ -f "$dkp_posix/portlibs/switch/lib/libzstd.a" ]     || need_dkp_package switch-libzstd
    [ -f "$dkp_posix/portlibs/switch/lib/libexpat.a" ]    || need_dkp_package switch-libexpat
fi
dkp_win="${dkp_posix:+$(cygpath -m "$dkp_posix")}"

# ---------------------------------------------------------------- Rust
# build-unified.sh and the rustc wrappers look for MESA_SWITCH_RUSTC, or a rustup installation
# inside the tree at build/deps/{cargo,rustup}. The same rustc builds the Switch code
# (--target aarch64-unknown-linux-gnu) and the proc macros it loads, which the x64 GCC links:
# so its host must be x86_64-pc-windows-gnu.
DEPS_CARGO="$ROOT/build/deps/cargo"
DEPS_RUSTUP="$ROOT/build/deps/rustup"

if [ "$setup_rust" = 1 ] && [ ! -x "$DEPS_CARGO/bin/rustc.exe" ]; then
    echo "== Installing Rust (x86_64-pc-windows-gnu host, aarch64-unknown-linux-gnu std) into build/deps"
    mkdir -p "$ROOT/build/deps"
    installer="$ROOT/build/deps/rustup-init.exe"
    curl -fL --proto '=https' --tlsv1.2 -o "$installer" \
        https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-gnu/rustup-init.exe
    RUSTUP_HOME="$(cygpath -m "$DEPS_RUSTUP")" CARGO_HOME="$(cygpath -m "$DEPS_CARGO")" "$installer" -y --no-modify-path \
        --default-host x86_64-pc-windows-gnu --default-toolchain stable --profile minimal
    RUSTUP_HOME="$(cygpath -m "$DEPS_RUSTUP")" CARGO_HOME="$(cygpath -m "$DEPS_CARGO")" "$DEPS_CARGO/bin/rustup.exe" \
        target add aarch64-unknown-linux-gnu
fi

rustc_bin="${MESA_SWITCH_RUSTC:-}"
if [ -z "$rustc_bin" ] && [ -x "$DEPS_CARGO/bin/rustc.exe" ]; then
    export RUSTUP_HOME="$(cygpath -m "$DEPS_RUSTUP")" CARGO_HOME="$(cygpath -m "$DEPS_CARGO")"
    rustc_bin="$(cygpath -m "$DEPS_CARGO/bin/rustc.exe")"
fi
if [ -z "$rustc_bin" ] && have rustc; then
    rustc_bin="$(cygpath -m "$(command -v rustc)")"
fi

rustc_version=""
rustc_host=""
if [ -z "$rustc_bin" ]; then
    problem "No Rust toolchain. Run: bash build-nfsmw.sh --setup-rust"
else
    rustc_info="$("$rustc_bin" -vV 2>/dev/null || true)"
    rustc_version="$(printf '%s\n' "$rustc_info" | awk '/^release:/ {print $2}')"
    rustc_host="$(printf '%s\n' "$rustc_info" | awk '/^host:/ {print $2}')"
    if [ -z "$rustc_version" ]; then
        problem "$rustc_bin does not run. Run: bash build-nfsmw.sh --setup-rust"
    else
        version_ge "${rustc_version%%-*}" 1.85.0 ||
            problem "Rust $rustc_version is too old (1.85 or newer). Run: bash build-nfsmw.sh --setup-rust"
        [ "$rustc_host" = x86_64-pc-windows-gnu ] ||
            problem "Rust host is $rustc_host; the build needs x86_64-pc-windows-gnu (its proc macros are linked with the x64 GCC). Run: bash build-nfsmw.sh --setup-rust"
        sysroot="$("$rustc_bin" --print sysroot 2>/dev/null || true)"
        if [ -n "$sysroot" ] && [ ! -d "$(cygpath -u "$sysroot")/lib/rustlib/aarch64-unknown-linux-gnu" ]; then
            problem "Rust has no aarch64-unknown-linux-gnu std. Run: bash build-nfsmw.sh --setup-rust (or rustup target add aarch64-unknown-linux-gnu)"
        fi
    fi
fi

# ---------------------------------------------------------------- bindgen (0.71.1+, not 0.72.0)
bindgen_ok() {
    local v
    v="$("$1" --version 2>/dev/null | awk '{print $2}')"
    [ -n "$v" ] && [ "$v" != 0.72.0 ] && version_ge "$v" 0.71.1
}
bindgen_bin=""
for candidate in "${MESA_SWITCH_BINDGEN:-}" "$TOOLS/bindgen.exe" "$DEPS_CARGO/bin/bindgen.exe" "$(command -v bindgen 2>/dev/null || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ] && bindgen_ok "$candidate"; then
        bindgen_bin="$candidate"
        break
    fi
done
if [ -z "$bindgen_bin" ]; then
    if runs "$TOOLS/bindgen.exe" --version; then
        problem "bindgen $("$TOOLS/bindgen.exe" --version 2>/dev/null | awk '{print $2}') cannot build Mesa (needs 0.71.1+, not 0.72.0). After --setup-rust: RUSTUP_HOME=\"$(cygpath -m "$DEPS_RUSTUP")\" CARGO_HOME=\"$(cygpath -m "$DEPS_CARGO")\" \"$DEPS_CARGO/bin/cargo.exe\" install bindgen-cli --version 0.71.1"
    else
        need_package "$tools_pkg-rust-bindgen"
    fi
else
    export MESA_SWITCH_BINDGEN="$bindgen_bin"
    # An x64 bindgen (installed with cargo) needs the x64 libclang.
    if [ "$bindgen_bin" != "$TOOLS/bindgen.exe" ] && [ -z "${LIBCLANG_PATH:-}" ] && [ -f /clang64/bin/libclang.dll ]; then
        export LIBCLANG_PATH="$(cygpath -m /clang64/bin)"
    fi
fi

# ---------------------------------------------------------------- cbindgen
cbindgen_bin=""
for candidate in "$TOOLS/cbindgen.exe" "$DEPS_CARGO/bin/cbindgen.exe" "$(command -v cbindgen 2>/dev/null || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ] && runs "$candidate" --version; then
        cbindgen_bin="$(cygpath -m "$candidate")"
        break
    fi
done
[ -n "$cbindgen_bin" ] || need_package "$tools_pkg-cbindgen"

# ---------------------------------------------------------------- report
if [ "${#missing_packages[@]}" -gt 0 ] && [ "$install_packages" = 1 ]; then
    echo "== pacman -S --needed ${missing_packages[*]}"
    if ! pacman -S --needed "${missing_packages[@]}"; then
        echo "pacman could not install everything. If it said \"target not found\", update the package" >&2
        echo "databases first (pacman -Syu, then run this again)." >&2
        exit 2
    fi
    echo "== Packages installed; checking again"
    rest=()
    [ "$check_only" = 1 ] && rest+=(--check)
    exec bash "${BASH_SOURCE[0]}" "${rest[@]}" "${passthrough[@]}"
fi

if [ "${#missing_packages[@]}" -gt 0 ]; then
    echo "Missing MSYS2 packages. Install them with:" >&2
    echo "  pacman -S --needed ${missing_packages[*]}" >&2
    echo "(or run: bash build-nfsmw.sh --install-packages)" >&2
fi
if [ "${#missing_dkp_packages[@]}" -gt 0 ]; then
    echo "Missing devkitPro packages${dkp_posix:+ in $dkp_win}. Install them with devkitPro's pacman" >&2
    echo "(devkitPro's MSYS2 shell, or dkp-pacman):" >&2
    echo "  pacman -S --needed ${missing_dkp_packages[*]}" >&2
    [ -n "$dkp_posix" ] || echo "(or set DEVKITPRO to where devkitPro is installed)" >&2
fi
if [ "${#problems[@]}" -gt 0 ]; then
    echo "Other problems:" >&2
    for p in "${problems[@]}"; do
        echo "  - $p" >&2
    done
fi
if [ "${#missing_packages[@]}" -gt 0 ] || [ "${#missing_dkp_packages[@]}" -gt 0 ] || [ "${#problems[@]}" -gt 0 ]; then
    exit 2
fi

# ---------------------------------------------------------------- build files for this machine
# The repository's MSYS2 files hardcode C:/msys64 and C:/msys64/opt/devkitpro. Write copies with the
# paths found here (this MSYS2, this devkitPro, these tools) and point build-unified.sh at them.
GEN="$ROOT/build/nfsmw-msys2"
mkdir -p "$GEN"
GEN_WIN="$(cygpath -m "$GEN")"

adapt() { # adapt <in> <out>: rewrite the hardcoded MSYS2 and devkitPro paths
    sed -e "s|C:/msys64/opt/devkitpro|$dkp_win|g" \
        -e "s|C:/msys64/|$MSYS_WIN/|g" \
        "$1" > "$2"
}

if [ -z "${CROSS_FILE:-}" ]; then
    adapt "$ROOT/switch_cross_file_msys2.txt" "$GEN/cross.txt"
    # Wrappers referenced through @DIRNAME@: the adapted copies live next to the cross file; the Rust
    # wrapper finds build/deps through MESA_SWITCH_RUSTC, RUSTUP_HOME and CARGO_HOME.
    adapt "$ROOT/rustc-switch-wrapper.sh" "$GEN/rustc-switch-wrapper.sh"
    adapt "$ROOT/bindgen-switch-wrapper.sh" "$GEN/bindgen-switch-wrapper.sh"
    sed -i "s|/clangarm64/bin/|$TOOLS/|g" "$GEN/bindgen-switch-wrapper.sh"
    cp "$ROOT/bindgen-atomic-shim.h" "$GEN/bindgen-atomic-shim.h"
    adapt "$ROOT/switch-pkg-config-wrapper.sh" "$GEN/switch-pkg-config-wrapper.sh"
    sed -i "s|^export PKG_CONFIG_LIBDIR=.*|export PKG_CONFIG_LIBDIR=$dkp_posix/portlibs/switch/lib/pkgconfig|" \
        "$GEN/switch-pkg-config-wrapper.sh"
    export CROSS_FILE="$GEN/cross.txt"
fi
if [ -z "${NATIVE_FILE:-}" ]; then
    adapt "$ROOT/switch_native_tools_msys2.txt" "$GEN/native.txt"
    sed -i "s|^cbindgen = .*|cbindgen = '$cbindgen_bin'|" "$GEN/native.txt"
    export NATIVE_FILE="$GEN/native.txt"
fi
if [ -z "${HOST_CROSS_FILE:-}" ]; then
    adapt "$ROOT/windows_x64_host_cross_file_msys2.txt" "$GEN/host.txt"
    export HOST_CROSS_FILE="$GEN/host.txt"
fi

export MESA_SWITCH_RUSTC="$rustc_bin"
export MESA_SWITCH_NATIVE_LINKER="$native_linker"
export DEVKITPRO="$dkp_posix"

echo "All prerequisites found."
echo "  MSYS2:     $MSYS_WIN (native tools from ${tools_env^^})"
echo "  devkitPro: $dkp_win"
echo "  rustc:     $rustc_bin ($rustc_version, host $rustc_host)"
echo "  bindgen:   $bindgen_bin"
echo "  cbindgen:  $cbindgen_bin"
echo "  linker for proc macros: $native_linker"
echo "  build files: $CROSS_FILE, $NATIVE_FILE, $HOST_CROSS_FILE"
if [ "$check_only" = 1 ]; then
    exit 0
fi

# ---------------------------------------------------------------- build
# build-unified.sh takes the NVK build id and SOURCE_DATE_EPOCH from git. A tree extracted from
# a zip has no .git, so give it one (the build id then changes whenever you commit a change,
# which also invalidates the pipeline caches of the games that use the driver: intended).
if ! git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1; then
    echo "== Creating a git repository for the build id (first run only)"
    git -C "$ROOT" init -q
    printf '%s\n' 'build/' 'builddir-*/' 'mesa-unified-install*/' 'dist/' >> "$ROOT/.git/info/exclude"
    git -C "$ROOT" add -A
    git -C "$ROOT" -c user.name=local -c user.email=local@localhost \
        commit -q -m "mesa-switch 26.2.2 with the nfsmw-nx driver changes"
fi

export ALLOW_DIRTY="${ALLOW_DIRTY:-1}"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" show -s --format=%ct HEAD)}"

bash "$ROOT/build-unified.sh" "${passthrough[@]}"

sdk="${DESTDIR:-$ROOT/mesa-unified-install}/opt/devkitpro/portlibs/switch"
echo
echo "Driver: $(cygpath -m "$sdk")/lib/libvulkan.a"
if [ "$tools_env" = clang64 ]; then
    # UnleashedRecomp's script defaults to CLANGARM64's clang, cmake and ninja (an ARM64 PC); on an x64
    # PC point it at CLANG64's, which this build has installed.
    echo "UnleashedRecomp (from its folder, in this MSYS2):"
    echo "  rm -rf build && CLANGARM64=/clang64/bin CLANG64=/clang64/bin NVK_ROOT=$ROOT_WIN tools/build-switch.sh"
else
    echo "UnleashedRecomp: rm -rf build && NVK_ROOT=$ROOT_WIN tools/build-switch.sh"
fi
