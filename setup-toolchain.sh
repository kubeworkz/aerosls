#!/usr/bin/env bash
# setup-toolchain.sh — builds the toolchain AeroSLS's Makefile expects:
# x86_64-elf-gcc/ld (cross-compiler + linker), nasm, qemu-system-x86_64,
# grub-mkrescue. Run this inside WSL2 (Ubuntu) or native Linux.
#
# x86_64-elf-gcc isn't a normal apt package — building it from source is
# the standard OSDev.org recipe (https://wiki.osdev.org/GCC_Cross-Compiler).
# This script automates that. Takes ~20-40 minutes, mostly compiling gcc.
#
# Usage:
#   chmod +x setup-toolchain.sh
#   ./setup-toolchain.sh
#   # then add this to your ~/.bashrc (the script prints the exact line):
#   export PATH="$HOME/opt/cross/bin:$PATH"

set -euo pipefail

BINUTILS_VERSION=2.42
GCC_VERSION=13.2.0
TARGET=x86_64-elf
PREFIX="$HOME/opt/cross"

# JOBS: override with `JOBS=2 ./setup-toolchain.sh` if this still gets
# killed. GCC's build is memory-hungry (~1-2GB per parallel compiler
# instance) — `nproc` parallelism assumes as much RAM as you have cores,
# which WSL2 often doesn't have room for. Rule of thumb: JOBS <= RAM_GB/2.
JOBS="${JOBS:-$(( $(nproc) > 4 ? 4 : $(nproc) ))}"

echo "==> Available memory:"
free -h
echo "==> Building with JOBS=$JOBS (override with: JOBS=2 ./setup-toolchain.sh)"

echo "==> Installing apt prerequisites (build tools, QEMU, GRUB, nasm)..."
sudo apt-get update
sudo apt-get install -y \
    build-essential bison flex libgmp-dev libmpc-dev libmpfr-dev texinfo \
    nasm qemu-system-x86 grub-pc-bin grub-common xorriso mtools \
    wget curl

mkdir -p "$HOME/src" "$PREFIX"
cd "$HOME/src"

echo "==> Downloading binutils $BINUTILS_VERSION..."
if [ ! -f "binutils-$BINUTILS_VERSION.tar.gz" ]; then
    wget "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.gz"
fi
tar xf "binutils-$BINUTILS_VERSION.tar.gz"

if [ -x "$PREFIX/bin/$TARGET-ld" ]; then
    echo "==> binutils already built and installed ($PREFIX/bin/$TARGET-ld exists) — skipping."
else
    echo "==> Building binutils (target=$TARGET)..."
    mkdir -p "build-binutils-$TARGET"
    cd "build-binutils-$TARGET"
    ../"binutils-$BINUTILS_VERSION"/configure \
        --target="$TARGET" --prefix="$PREFIX" \
        --with-sysroot --disable-nls --disable-werror
    make -j"$JOBS"
    make install
    cd "$HOME/src"
fi

echo "==> Downloading gcc $GCC_VERSION..."
if [ ! -f "gcc-$GCC_VERSION.tar.gz" ]; then
    wget "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.gz"
fi
tar xf "gcc-$GCC_VERSION.tar.gz"

# A previous partial/killed build can leave an inconsistent build dir —
# wipe and start clean rather than resume, since `make` can't always tell
# a truncated .o file from a good one after a SIGTERM mid-compile.
if [ -d "build-gcc-$TARGET" ]; then
    echo "==> Removing previous (possibly incomplete) build-gcc-$TARGET directory..."
    rm -rf "build-gcc-$TARGET"
fi

echo "==> Building gcc (target=$TARGET, C only — no C++, no libc/headers)..."
# C only: this kernel is pure C (see Makefile's X86_C_SRC), so building the
# C++ front end too would roughly double compile time and peak memory for
# zero benefit.
export PATH="$PREFIX/bin:$PATH"
mkdir -p "build-gcc-$TARGET"
cd "build-gcc-$TARGET"
../"gcc-$GCC_VERSION"/configure \
    --target="$TARGET" --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j"$JOBS" all-gcc
make -j"$JOBS" all-target-libgcc
make install-gcc
make install-target-libgcc

echo ""
echo "==> Done. Add this to your ~/.bashrc (or ~/.zshrc), then re-open your shell:"
echo ""
echo "    export PATH=\"$PREFIX/bin:\$PATH\""
echo ""
echo "==> Verify with:"
echo "    x86_64-elf-gcc --version"
echo "    nasm -v"
echo "    qemu-system-x86_64 --version"
echo "    grub-mkrescue --version"
echo ""
echo "==> Then from the aerosls2 project root:"
echo "    make x86-run"
