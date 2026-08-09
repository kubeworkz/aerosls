#!/usr/bin/env bash
#
# kernel_image_end_check.sh — no allocated section may lie at or above
# _kernel_image_end, and the bootstrap stack must be strictly inside it.
#
# ─── The bug this locks down ───────────────────────────────────────────────
# arch/x86/linker.ld carried `*(.bootstrap_stack)` nested inside the .bss
# output section. On the real build that wildcard did not match: the section
# became an orphan, and ld places orphans after the output section they most
# resemble -- which is after the `. = ALIGN(4096); PROVIDE(_kernel_image_end)`
# statement, because statements between output sections are evaluated at their
# script position while orphans are inserted into the list afterwards.
#
# A live node reported:
#
#     bootstrap stack   [0x77e8000, 0x77f8000)
#     _kernel_image_end  0x77e8000
#
# _kernel_image_end landed exactly on stack_bottom. All 16 frames of the
# kernel's own 64 KiB stack were above the reservation frame_pool_init() makes,
# so alloc_raw_frame() was free to hand them out. It did. A stream upload
# memset one and filled it with 0xCD payload, the live stack became payload to
# its last byte, and the next `ret` took a non-canonical address and #GP'd --
# four layers and several wrong hypotheses away from the actual write.
#
# ─── Why a shell script and not a host test ────────────────────────────────
# This invariant is a property of the LINKED IMAGE. It cannot be reproduced in
# a host test: C cannot place a symbol at a chosen address, and the whole bug
# was ld's placement decision rather than any code's behaviour. Asserting it
# anywhere other than against the real ELF would be asserting something else.
#
# Usage:
#   tests/kernel_image_end_check.sh [path/to/kernel.bin]
#
# Defaults to my_sls_kernel.bin (Makefile's X86_BIN). Run it after `make`.
set -u

ELF="${1:-my_sls_kernel.bin}"
pass=0; fail=0
ok()   { echo "ok:   $1"; pass=$((pass+1)); }
bad()  { echo "FAIL: $1"; fail=$((fail+1)); }

if [ ! -f "$ELF" ]; then
    echo "kernel_image_end_check: '$ELF' not found."
    echo
    echo "  This check reads the LINKED image, so it needs a build. It is not"
    echo "  skippable in any meaningful sense -- the invariant it asserts is"
    echo "  exactly the one that was silently false for the entire life of the"
    echo "  project. Run 'make' first, or pass the image path as \$1."
    echo
    echo "  NOT RUN (this is a gap, not a pass)."
    exit 2
fi

for tool in readelf nm; do
    command -v "$tool" >/dev/null 2>&1 || { echo "kernel_image_end_check: $tool missing"; exit 2; }
done

echo "=== _kernel_image_end must cover every allocated section ==="
echo "    image: $ELF"
echo

# ─── the symbol ────────────────────────────────────────────────────────────
IMAGE_END="$(nm "$ELF" 2>/dev/null | awk '$3=="_kernel_image_end"{print $1; exit}')"
if [ -z "$IMAGE_END" ]; then
    bad "_kernel_image_end is not in the symbol table -- PROVIDE() only defines it
      when something references it, so either frame_pool.c stopped referencing
      it (in which case NOTHING reserves the kernel image) or the link changed"
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
END_DEC=$((0x$IMAGE_END))
ok "_kernel_image_end = 0x$IMAGE_END ($(( END_DEC / 1048576 )) MiB)"

# ─── every SHF_ALLOC section must end at or below it ───────────────────────
# readelf -SW prints flags in the last column; A = SHF_ALLOC. A section with no
# A does not occupy memory at run time and cannot be handed out.
viol=0
while read -r name addr size flags; do
    case "$flags" in *A*) ;; *) continue ;; esac
    [ "$size" = "0000000000000000" ] && continue
    s=$((0x$addr)); e=$((s + 0x$size))
    if [ "$e" -gt "$END_DEC" ]; then
        bad "section $name spans 0x$addr..$(printf '0x%x' "$e") -- ABOVE
      _kernel_image_end 0x$IMAGE_END. $(( (e - END_DEC + 4095) / 4096 )) frame(s) of
      live kernel memory are allocatable. This is an orphan section: add it to
      arch/x86/linker.ld by name, ABOVE the PROVIDE(_kernel_image_end)."
        viol=$((viol+1))
    fi
# readelf writes the section index as "[ 4]" for single digits and "[10]" for
# two, so awk splits the former into two fields and every column after it
# shifts by one. The first version of this parser tried to detect that by
# looking at the field contents and got it wrong: the generic sweep silently
# examined the wrong columns, matched no flags, and passed on an image whose
# stack WAS above the image end. The stack-specific checks below caught it; the
# sweep that was supposed to catch the general case did not.
#
# Strip the bracketed index first and the columns are fixed:
#   Name Type Address Off Size ES Flg Lk Inf Al
done < <(readelf -SW "$ELF" \
         | sed -nE 's/^ *\[[ 0-9]+\] +//p' \
         | awk 'NF >= 7 { print $1, $3, $5, $7 }')

[ "$viol" -eq 0 ] && ok "*** no allocated section lies above _kernel_image_end ***"

# ─── the bootstrap stack specifically ──────────────────────────────────────
# Named on its own because it is the one whose escape corrupts the running
# kernel rather than merely leaking a frame, and because it is the one that did.
SB="$(nm "$ELF" 2>/dev/null | awk '$3=="stack_bottom"{print $1; exit}')"
ST="$(nm "$ELF" 2>/dev/null | awk '$3=="stack_top"{print $1; exit}')"
if [ -z "$SB" ] || [ -z "$ST" ]; then
    bad "stack_bottom/stack_top are not exported from arch/x86/boot.asm -- without
      them frame_pool_init() cannot cross-check the reservation at runtime and
      this check cannot verify it at build time"
else
    sb=$((0x$SB)); st=$((0x$ST))
    if [ "$st" -le "$sb" ]; then
        bad "stack_top 0x$ST is not above stack_bottom 0x$SB"
    else
        ok "bootstrap stack [0x$SB,0x$ST) = $(( (st - sb) / 1024 )) KiB"
    fi
    if [ "$st" -gt "$END_DEC" ]; then
        bad "*** the bootstrap stack ends at 0x$ST, ABOVE _kernel_image_end
      0x$IMAGE_END -- $(( (st - END_DEC + 4095) / 4096 )) frame(s) of the kernel's OWN LIVE STACK
      are allocatable. This is the exact fault of roadmap section 9r. ***"
    else
        ok "*** the bootstrap stack is strictly inside the reserved region ***"
    fi
    if [ "$sb" -eq "$END_DEC" ]; then
        bad "stack_bottom equals _kernel_image_end exactly -- the signature of the
      orphan-placement bug: the symbol was computed at the end of .bss and the
      stack was then placed immediately after it"
    else
        ok "stack_bottom is not sitting exactly on the image end"
    fi
fi

echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
