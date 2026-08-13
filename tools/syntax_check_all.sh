#!/usr/bin/env bash
# tools/syntax_check_all.sh -- Operational Phase A
# (docs/AeroSLS-Operational-MVP-Roadmap-v0.1.md).
#
# gcc -fsyntax-only across every file in the Makefile's own X86_C_SRC list
# -- extracted via `make -pn`, not copied by hand into this script, so it
# can't silently drift out of sync with the Makefile the way a hardcoded
# file list would as sources get added or removed.
#
# This has been this project's actual verification method throughout its
# whole history (every phase of every roadmap doc in docs/ was checked this
# way): no real x86_64-elf-gcc cross-compiler is available in most dev
# environments, including CI, so this is host gcc with matching freestanding
# flags doing a syntax/type check, not a full cross-build. Only compile
# ERRORS fail this script -- warnings are printed but don't affect the exit
# code, since isolated single-file checks routinely produce warnings from
# headers that would normally arrive transitively in the real multi-file
# build (implicit-declaration warnings for functions declared in headers
# this one file doesn't itself include, mainly) -- the same tradeoff every
# phase's own manual verification already accepted, not a new one invented
# here just to make this script pass.
set -u
cd "$(dirname "$0")/.."

CFLAGS="-ffreestanding -O2 -Wall -Wextra -mcmodel=small -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -fno-pie -fno-pic -fno-tree-vectorize -I."

# Three kernel files include the vendored mbedTLS headers and therefore
# need the dedicated flags the Makefile gives them (Makefile lines ~380-413:
# MBEDTLS_INC, MBEDTLS_DEFS, -I kernel, and -I net for tls_server). Checking
# them under the flat CFLAGS would fail with "mbedtls/...: No such file",
# exactly the de65656 failure mode the Makefile comment warns about --
# verifying a file under different flags than the real build uses is not
# verification of the real build. The Makefile is the source of truth for
# these; when it changes, this list must change with it.
MBEDTLS_INC="-I vendor/mbedtls/include -I vendor/mbedtls/shim"
# Single-quoted assignment so the double quotes around sls_mbedtls_config.h
# stay LITERAL in the variable. Expanded unquoted below, the word reaches
# gcc as  -DMBEDTLS_USER_CONFIG_FILE="sls_mbedtls_config.h"  -- exactly the
# argv the Makefile's own shell produces. A mangled -D here silently drops
# the user config, which un-defines MBEDTLS_PLATFORM_MS_TIME_TYPE_MACRO and
# makes platform_time.h demand a real time_t that shim/time.h deliberately
# does not provide -- an error that names the wrong file.
MBEDTLS_DEFS='-DMBEDTLS_USER_CONFIG_FILE="sls_mbedtls_config.h" -U_FORTIFY_SOURCE -Uunix -U__unix -U__unix__'
MBEDTLS_CFLAGS="$MBEDTLS_INC $MBEDTLS_DEFS -I kernel"

files="$(make -pn 2>/dev/null | awk -F'= ' '/^X86_C_SRC =/{print $2; exit}')"
if [ -z "$files" ]; then
    echo "FAIL  could not extract X86_C_SRC from the Makefile (is 'make' installed? has the Makefile's variable name changed?)"
    exit 1
fi

fail=0
count=0
for f in $files; do
    count=$((count + 1))
    # -include string.h: matches every isolated check done by hand
    # throughout this project (string.h routinely arrives transitively in
    # the real build but not in a single-file check) -- avoids ~20 spurious
    # implicit-declaration warnings for strlen/strcmp/memcpy that aren't
    # real problems.
    # tls_platform/tls_cert/tls_server get the Makefile's mbedTLS flags;
    # everything else gets the flat CFLAGS.
    case "$f" in
        kernel/tls_platform.c) check="$CFLAGS $MBEDTLS_CFLAGS" ;;
        kernel/tls_cert.c)     check="$CFLAGS $MBEDTLS_CFLAGS" ;;
        kernel/tls_server.c)   check="$CFLAGS $MBEDTLS_CFLAGS -I net" ;;
        *)                     check="$CFLAGS" ;;
    esac
    if ! out=$(gcc -fsyntax-only $check -include string.h "$f" 2>&1); then
        echo "FAIL  $f"
        echo "$out" | sed 's/^/      /'
        fail=$((fail + 1))
    fi
done

echo ""
echo "$((count - fail))/$count files syntax-checked clean, $fail failed"
[ "$fail" -eq 0 ]
