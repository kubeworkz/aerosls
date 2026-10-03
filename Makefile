# ==============================================================================
#           AEROSLS UNIFIED CROSS-PLATFORM HARDWARE ARCHITECTURE MATRIX
# ==============================================================================

HOST_CXX    = g++
LLVM_CONFIG = llvm-config
ASN         = nasm
OBJCOPY     = objcopy

PLUGIN_CXXFLAGS = $(shell $(LLVM_CONFIG) --cxxflags) -fPIC -shared -fno-rtti
PLUGIN_LDFLAGS  = $(shell $(LLVM_CONFIG) --ldflags) -Wl,-z,defs
ALLOC_PLUGIN    = libSLSAllocationPassV2.so

# --- x86_64 Toolchain ---
X86_CC      ?= x86_64-elf-gcc
X86_LD      ?= x86_64-elf-ld
# -Wframe-larger-than: a kernel has no stack guard page and no way to grow the
# stack, so a large frame is not a style issue -- it is silent memory
# corruption. sls_shell_execute() carried a 276,032-byte frame against a 64 KiB
# stack for months; every shell command wrote ~208 KiB past stack_bottom into
# .bss, and it only surfaced once TCG started using the arena that lived there.
# This flag would have printed the number in seconds at any point.
#
# A WARNING, not an error, and deliberately so. This threshold is a fixed
# number, and a fixed number cannot express the property that actually matters:
# frames must fit the stack that EXISTS. Its job here is fast feedback -- a new
# offender shows up in the build output rather than as a fault address.
#
# The hard gate is tests/stack_frame_budget_check.sh, which derives the limit
# from the linked binary's own stack_top - stack_bottom. It runs in CI and
# blocks deploy.sh, so the enforcement lives where the relational invariant can
# actually be checked, and this flag stays advisory.
#
# For the record: both frames that originally justified the slack are long
# gone. sls_shell_execute() is 10,224 bytes and http_route() 5,456, each root-
# caused to a single oversized struct rather than the diffuse "hundreds of
# small locals" they were assumed to be. Nothing in the tree is near 16,384.
# ─── Build identity for the persistent translation cache ─────────────────────
# The QEMU-SLS translation cache stores HOST MACHINE CODE on NVMe and restores
# it across reboots. Its magic number proves only that SOME build wrote it, so
# restoring a cache produced by a different compiler or different code-gen
# flags would execute instructions built against assumptions that no longer
# hold -- with no fault to catch it.
#
# This stamp goes into the cache header and is compared on restore; a mismatch
# discards and cold-starts. Deliberately conservative: any commit invalidates
# the cache, even one that could not have changed code generation. A needless
# recompile costs one round. A wrongly-accepted cache costs arbitrary
# behaviour with no diagnostic.
#
# Falls back to a timestamp when git is unavailable (release tarball, CI
# without history), which errs toward cold-starting rather than toward
# accepting a cache whose provenance cannot be established.
AEROSLS_BUILD_ID := $(shell git rev-parse --short=12 HEAD 2>/dev/null || date -u +%Y%m%d%H%M%S)

# Build time in Unix seconds, the floor kernel/rtc.c refuses to believe a clock
# below. Taken from the commit date when there is one, so a reproducible build
# of an old commit gets that commit's floor rather than today's -- using the
# wall clock here would make the binary unreproducible AND would raise the
# floor above times that commit could legitimately see.
SLS_BUILD_EPOCH := $(shell git log -1 --format=%ct 2>/dev/null || date -u +%s)

# ─── SLS_SOFTMMU: the A/B knob for the Cross-ISA §5c measurement ─────────────
# The headline number -- 86 bytes of host code per guest load with QEMU's
# software MMU, 16 without, a 5.41x ratio -- comes from building the same
# sources twice with this one switch flipped.
#
# Until this existed the ON side required hand-editing tcg-internal.h, so the
# published ratio rested on a build that could not be reproduced. Half of it
# was measured on 2026-08-04 and had no way to be re-derived.
#
#   make x86-iso                    # softmmu OFF (default, the shipping config)
#   make x86-iso SLS_SOFTMMU=on     # softmmu ON  (the A/B comparison side)
#
# The define MUST reach both compilers. TCG_CFLAGS governs code generation in
# tcg.c and tcg-op-ldst.c; X86_CFLAGS governs the translation cache's identity
# stamp, which has to know which side it is on or an OFF-built cache will be
# accepted by an ON build and jumped into. See qemu_tcache_identity_of() in
# kernel/qemu_sls_tcache.h.
SLS_SOFTMMU ?= off
ifeq ($(SLS_SOFTMMU),on)
AB_DEFS = -DSLS_FORCE_SOFTMMU
else ifneq ($(SLS_SOFTMMU),off)
$(error SLS_SOFTMMU must be 'on' or 'off', got '$(SLS_SOFTMMU)')
endif

# ─── Rebuild when the CONFIGURATION changes, not only when sources do ────────
# make compares source timestamps against object timestamps. It has no idea
# that X86_CFLAGS or TCG_CFLAGS changed, so `make x86-iso SLS_SOFTMMU=on` over
# an existing tree recompiled 2 files out of 121 and produced a kernel with
# softmmu still OFF -- which then reported `softmmu=OFF` from a run that was
# supposed to be the ON side of the A/B. A build flag that silently does
# nothing is worse than no flag, because the output looks like a measurement.
#
# These stamps hold the current configuration. Objects depend on them, so
# changing the configuration makes the affected objects out of date exactly
# once. The stamp is only rewritten when the value actually differs, so a
# no-op invocation does not trigger a rebuild.
#
# Two stamps, because they have different blast radii:
#   AB_STAMP  -- SLS_SOFTMMU. Changes code generation everywhere; all objects.
#   BID_STAMP -- AEROSLS_BUILD_ID. Only reaches objects that include
#                kernel/qemu_sls_tcache.h, whose inline identity function
#                embeds it. Scoped so a commit rebuilds six files, not 121.
#
# BID_STAMP is not optional politeness. qemu_tcache_identity() is what stops a
# cache written by one build being executed by another, and it is derived from
# AEROSLS_BUILD_ID at compile time. Without this, an incremental build after a
# commit leaves qemu_sls_tcache.x86.o carrying the PREVIOUS commit's id: the
# kernel then accepts a cache from a build whose generated code it no longer
# entirely is. deploy.sh builds incrementally over a pulled tree, so that was
# the live path, not a hypothetical one.
AB_STAMP  := .build-config.stamp
BID_STAMP := .build-id.stamp
SLS_STAMP := .sls-frontend.stamp
#
# The `[ -f ... ] &&` is load-bearing. Without it, the default configuration
# (SLS_SOFTMMU=off, so AB_DEFS is empty) compares an empty variable against
# `cat` of a missing file -- which is also empty -- concludes nothing changed,
# and never creates the stamp. make then fails with "No rule to make target
# '.build-config.stamp'". Since clean removes the stamps, that broke
# `make clean && make x86-iso` completely. Caught by a scratch-directory
# reproduction of this exact logic, not by reading it.
$(shell v='$(AB_DEFS)';  [ -f $(AB_STAMP) ]  && [ "$$(cat $(AB_STAMP))"  = "$$v" ] || printf '%s' "$$v" > $(AB_STAMP))
$(shell v='$(AEROSLS_BUILD_ID):$(SLS_BUILD_EPOCH)'; [ -f $(BID_STAMP) ] && [ "$$(cat $(BID_STAMP))" = "$$v" ] || printf '%s' "$$v" > $(BID_STAMP))

# Objects whose translation unit pulls in the identity function.
# kernel/rtc.x86.o is here for SLS_BUILD_EPOCH, not AEROSLS_BUILD_ID. Same
# hazard: the epoch is baked in at compile time and is the floor below which
# rtc.c refuses to believe a clock. Without this dependency a commit moves the
# floor, rtc.c is not recompiled, and the kernel silently keeps an older one --
# which is the permissive direction, so nothing would ever look wrong.
BUILD_ID_OBJS = kernel/checkpoint_mgr.x86.o kernel/kernel.x86.o \
                kernel/qemu_sls_mmu.x86.o kernel/qemu_sls_pgo.x86.o \
                kernel/qemu_sls_tcache.x86.o kernel/qemu_sls_vm.x86.o \
                kernel/rtc.x86.o
$(BUILD_ID_OBJS): $(BID_STAMP)

X86_CFLAGS  = -ffreestanding -O2 -Wall -Wextra -mcmodel=small -mno-red-zone \
              -mno-sse -mno-sse2 -mno-mmx \
              -fno-pie -fno-pic -fno-tree-vectorize \
              -Wframe-larger-than=16384 \
              $(AB_DEFS) \
              -DAEROSLS_BUILD_ID='"$(AEROSLS_BUILD_ID)"' -DSLS_BUILD_EPOCH=$(SLS_BUILD_EPOCH)ull
X86_LDFLAGS = -T arch/x86/linker.ld -nostdlib --no-warn-rwx-segments

X86_ASM_SRC = arch/x86/boot.asm arch/x86/interrupt.asm arch/x86/switch_lazy.asm arch/x86/syscall.asm arch/x86/vector_crypto.asm arch/x86/process_enter.asm
X86_C_SRC   = kernel/kernel.c arch/x86/idt.c arch/x86/gdt.c arch/x86/vga.c kernel/scheduler.c arch/x86/lazy_fpu.c \
              arch/x86/walk_page_tables_x86.c \
              kernel/lockfree_map.c drivers/ahci.c drivers/pci.c drivers/nvme.c drivers/nvme_admin.c \
              kernel/frame_pool.c kernel/dashboard.c user/shell.c kernel/smp.c drivers/io_prio.c \
              net/consensus.c net/dspp.c net/dspp_checkpoint.c net/prefetch.c kernel/secure_api.c kernel/pte_migrate.c \
              kernel/timer.c kernel/flush_daemon.c \
              kernel/kernel_io.c kernel/syscall_dispatch.c \
              kernel/object_catalog.c kernel/transaction.c \
              kernel/ipc.c kernel/microkernel.c \
              kernel/tier_mgr.c kernel/query_engine.c \
              net/net.c net/arp.c net/ipv4.c net/tcp.c net/tcp_quota.c net/http.c net/http_rate_limit.c net/e1000.c net/udp.c net/dhcp.c net/inference.c \
              net/ollama_client.c \
              kernel/process.c arch/x86/user_paging.c arch/x86/device_irq.c arch/x86/ioapic.c kernel/cap.c kernel/chan.c \
              kernel/partition.c \
              kernel/boot_params.c kernel/node_reset.c \
              kernel/console.c kernel/console_service.c \
              kernel/env_service.c \
              kernel/env_console.c \
              kernel/boot_image.c \
              kernel/loader.c \
              kernel/simi_x86.c \
              kernel/simi_runtime.c \
              kernel/simi_translate.c \
              kernel/simi_interp.c \
              kernel/simi_ckpt.c \
              kernel/simi_ctx_migrate.c \
              kernel/service_registry.c \
              kernel/service_mesh.c \
              kernel/workload.c \
              kernel/workload_ctx.c \
              kernel/webapp.c \
              kernel/webapp_bundle.c \
              kernel/journal.c \
              kernel/lock_mgr.c \
              kernel/index_mgr.c \
              kernel/constraint.c \
              kernel/cursor.c \
              kernel/aggregate.c \
              kernel/mqt.c \
              kernel/stream.c \
              kernel/persist.c \
              kernel/rowstore.c \
              kernel/row_index.c \
              kernel/predicate.c \
              kernel/sql_parser.c \
              kernel/sql_exec.c \
              kernel/mvcc.c \
              kernel/row_constraint.c \
              kernel/row_journal.c \
              kernel/vecstore.c \
              kernel/vec_index.c kernel/sha256.c kernel/entropy.c kernel/rtc.c kernel/tls_platform.c kernel/tls_cert.c kernel/tls_store.c kernel/tls_server.c \
              kernel/vec_join.c \
              kernel/agent.c \
              kernel/agent_tools.c \
              kernel/checkpoint_mgr.c \
              kernel/checkpoint_delta.c \
              kernel/env_ckpt.c \
              kernel/env_storage.c \
              kernel/state_tree.c \
              kernel/failover.c \
              drivers/nvme_io.c \
              kernel/net_event.c \
              kernel/auth.c \
              kernel/group_profile.c \
              kernel/authlist.c \
              kernel/database.c \
              kernel/tenant.c \
              kernel/usage_metering.c \
              kernel/storage_quota.c \
              kernel/view.c \
              kernel/security_audit.c \
              kernel/msgqueue.c \
              kernel/qemu_sls_mmu.c \
              kernel/qemu_sls_tcache.c \
              kernel/qemu_sls_pgo.c \
              kernel/qemu_sls_vm.c \
              kernel/stubs.c

X86_OBJECTS = $(X86_ASM_SRC:.asm=.x86.o) $(X86_C_SRC:.c=.x86.o) arch/x86/trampoline.o
X86_BIN     = my_sls_kernel.bin
X86_ISO     = sls_operating_system.iso

# --- QEMU-SLS TCG Integration (Steps 2+) ---
QEMU_INC  = -I ../qemu/sls/include -I ../qemu/sls -I ../qemu/include \
            -I ../qemu/tcg -I ../qemu/tcg/x86_64 -I ../qemu/accel/tcg \
            -I ../qemu/target/i386
# TARGET_LONG_BITS: the guest's word size, 64 for our x86-64 guest. Set here
# rather than in a header because that is upstream's own mechanism --
# include/exec/target_long.h says "the build-system must ensure
# TARGET_LONG_BITS is defined directly" and #errors out otherwise. QEMU's meson
# sets it per target; we have exactly one guest target, so it is a constant.
#
# Step 6.1 (docs/AeroSLS-QEMU-SLS-Step6-x86-Frontend-Plan-v0.1.md): required
# before target/i386/tcg/translate.c will parse. Harmless to the existing TCG
# core objects, which is asserted rather than assumed -- they compile with 0
# errors either side of this change.
QEMU_DEFS = -include ../qemu/sls/sls-osdep.h \
            -DSLS_IN_KERNEL=1 -UCONFIG_PLUGIN -DCONFIG_TCG \
            -DTARGET_LONG_BITS=64
QEMU_WARN = -Wno-unused-parameter -Wno-unused-function \
            -Wno-unused-variable -Wno-unused-but-set-variable
TCG_CFLAGS = -ffreestanding -O2 -mcmodel=small -mno-red-zone \
             -mno-sse -mno-sse2 -mno-mmx \
             -fno-pie -fno-pic -fno-tree-vectorize \
             $(AB_DEFS) \
             $(QEMU_INC) $(QEMU_DEFS) $(QEMU_WARN)

TCG_OBJS = \
    tcg-objs/sls-runtime.x86.o \
    tcg-objs/sls-launcher.x86.o \
    tcg-objs/sls-elf64-loader.x86.o \
    tcg-objs/sls-x86-frontend.x86.o \
    tcg-objs/sls-tcg-wrappers.x86.o \
    tcg-objs/tcg.x86.o \
    tcg-objs/tcg-common.x86.o \
    tcg-objs/tcg-op.x86.o \
    tcg-objs/tcg-op-ldst.x86.o \
    tcg-objs/tcg-op-vec.x86.o \
    tcg-objs/tcg-op-gvec.x86.o \
    tcg-objs/optimize.x86.o \
    tcg-objs/region.x86.o \
    tcg-objs/tcg-runtime.x86.o \
    tcg-objs/tcg-runtime-gvec.x86.o \
    tcg-objs/sls-helper-stubs.x86.o

# tcg-objs/tci.x86.o deliberately NOT built.
#
# tci.c is the TCG *interpreter*. This build uses the native x86_64 backend:
# tcg.c includes "tcg-target.c.inc", resolved by QEMU_INC to tcg/x86_64, and
# CONFIG_TCG_INTERPRETER is defined nowhere. Two consequences make tci.c not
# merely unnecessary but harmful here:
#
#   1. It switches on INDEX_op_tci_* opcodes, which come from
#      tcg/tci/tcg-target-opc.h.inc via tcg-opc.h:183. With -I tcg/x86_64 that
#      include resolves to the x86_64 opcode list instead, so all fifteen are
#      undeclared -- which is exactly how this surfaced.
#   2. It defines tcg_qemu_tb_exec as a FUNCTION. Outside CONFIG_TCG_INTERPRETER
#      tcg.h declares that name as a function POINTER which tcg.c:1857 fills in
#      from the generated prologue. Linking both would be a symbol conflict.
#
# Nothing outside tci.c references its symbols (checked: tci_disas,
# print_insn_tci, tcg_qemu_tb_exec). Re-add it only alongside
# -DCONFIG_TCG_INTERPRETER, which switches the whole engine to the interpreter.

# accel/tcg is here for tcg-runtime.c and tcg-runtime-gvec.c, which define the
# ~200 helper_info_* metadata objects plus the helper functions they point at.
# tcg-op.c, tcg-op-ldst.c and tcg-op-gvec.c all reference those symbols through
# macro expansion (glue(helper_info_, NAME)), so they never appear as text in
# any source file and cannot be found by grepping for them -- they surface only
# at link time, all at once.
VPATH += ../qemu/sls ../qemu/tcg ../qemu/accel/tcg

# --- RISC-V 64-Bit Toolchain ---
RV_CC       = riscv64-unknown-elf-gcc
RV_LD       = riscv64-unknown-elf-ld
RV_CFLAGS   = -ffreestanding -O2 -Wall -Wextra -mcmodel=medany \
              -march=rv64gcv -mabi=lp64d -mno-relax -ffunction-sections -fdata-sections
RV_LDFLAGS  = -T arch/riscv/linker_riscv.ld -nostdlib --gc-sections

# Phase 9g: the M-mode variant (QEMU `-bios none -kernel` direct payload).
# Same sources, recompiled with -DRISCV_MMODE (UART console, mtvec trap
# vector, no-firmware exit) and linked at the DRAM base 0x80000000 -- the
# address the reset vector enters in a bare M-mode boot, where OpenSBI is
# absent (see arch/riscv/linker_riscv_m.ld and ISA doc §16 Phase 9g).
RV_CFLAGS_M  = $(RV_CFLAGS) -DRISCV_MMODE
RV_LDFLAGS_M = -T arch/riscv/linker_riscv_m.ld -nostdlib --gc-sections

RV_ASM_SRC  = arch/riscv/boot_riscv.S arch/riscv/context_riscv.S arch/riscv/vector_state.S \
              arch/riscv/trap_riscv.S
RV_C_SRC    = kernel/kernel_riscv.c arch/riscv/walk_page_tables_riscv.c \
              kernel/frame_pool.c kernel/dashboard.c kernel/pte_migrate.c arch/riscv/sbi.c \
              arch/riscv/plic.c arch/riscv/lazy_vector.c \
              kernel/simi_riscv.c kernel/object_catalog.c \
              arch/riscv/user_paging_riscv.c arch/riscv/trap_riscv.c

# arch/riscv/trap_riscv.c and arch/riscv/trap_riscv.S both map to
# trap_riscv.rv.o via the pattern rules below, so the C half (which
# defines riscv_trap_init/riscv_trap_dispatch/riscv_syscall_dispatch)
# was silently never built. Give the C file a unique object so both
# halves link.
RV_OBJECTS  = $(RV_ASM_SRC:.S=.rv.o) \
              $(filter-out arch/riscv/trap_riscv.rv.o,$(RV_C_SRC:.c=.rv.o)) \
              arch/riscv/trap_riscv_c.rv.o
RV_ELF      = sls_riscv_kernel.elf

# Same object list recompiled with -DRISCV_MMODE (distinct .m.rv.o names so
# both variants can coexist in one build tree).
RV_OBJECTS_M = $(RV_OBJECTS:.rv.o=.m.rv.o)
RV_ELF_M     = sls_riscv_kernel_m.elf

# Phase 9i: the UART-echo variant (distinct .e.rv.o names) -- same S-mode
# sources with -DKERNEL_UART_ECHO, which makes kernel_riscv_main skip the
# SIMI smoke (it powers the machine off) and instead spin with the PLIC-
# wired UART RX interrupt enabled, echoing every received character (see
# kernel/kernel_riscv.c and ISA doc §16 Phase 9i). S-mode only: the echo
# path uses sie.SEIE + the PLIC S-mode context, which bare M-mode lacks.
RV_OBJECTS_E = $(RV_OBJECTS:.rv.o=.e.rv.o)
RV_ELF_E     = sls_riscv_kernel_echo.elf

# The M-mode echo twin (distinct .m.e.rv.o names): the same echo sources
# with BOTH -DRISCV_MMODE and -DKERNEL_UART_ECHO -- the bare-metal build
# (linked at 0x80000000) whose PLIC path uses the M-mode context,
# mie.MEIE and mstatus.MIE (see kernel/kernel_riscv.c and ISA doc §16
# Phase 9i).
RV_OBJECTS_ME = $(RV_OBJECTS:.rv.o=.m.e.rv.o)
RV_ELF_ME     = sls_riscv_kernel_echo_m.elf

.PHONY: all clean x86-run riscv-run arm64-run arm64-teeth plugins

all: plugins x86-iso riscv-elf arm64-elf

plugins: compiler/SLSAllocationPassV2.cpp
	$(HOST_CXX) $(PLUGIN_CXXFLAGS) $(PLUGIN_LDFLAGS) $< -o $(ALLOC_PLUGIN) $(shell $(LLVM_CONFIG) --libs)

%.x86.o: %.asm
	$(ASN) -f elf64 $< -o $@

%.x86.o: %.c $(AB_STAMP)
	$(X86_CC) $(X86_CFLAGS) -c $< -o $@

# ── Header-only edits must relink the kernel ───────────────────────────────
# $(AB_STAMP) tracks the build CONFIGURATION and the pattern rule above names
# no header at all, so a header-only edit -- a new #define, a retuned constant
# -- compiles nothing and ships the PREVIOUS object. That is not hypothetical:
# CKPT_NUM_REGIONS grew 17 -> 18 when CKPT_REGION_ENV was added to
# kernel/checkpoint_delta.h, and checkpoint_delta.x86.o did not rebuild. The
# stale object's ckpt_mark_all_dirty() still wrote (1u << 17) - 1 and its
# ckpt_mark_dirty() still dropped region 17 as out of range, so a FULL
# checkpoint silently CLEARED the very bit the change existed to set. The
# environment region was never written, P1a's restore had nothing to replay,
# and every source-level clause stayed green -- the boot arm is what caught
# it, because it is the only thing that reads the mask the kernel actually
# computed (logged as `dirty=0x1ffff`, one bit short, in
# tests/env_checkpoint_restore_check.sh's B3).
#
# Named prerequisites, in the same spirit as the sls-i386-stub-class.h rule
# below and the embedded -bytes.h fixtures above: exactly the translation
# units that include checkpoint_delta.h, i.e. every consumer of the region
# NUMBERING. A change to the region map now invalidates precisely the objects
# whose behaviour it changes.
kernel/checkpoint_delta.x86.o kernel/checkpoint_mgr.x86.o kernel/env_ckpt.x86.o \
kernel/persist.x86.o kernel/qemu_sls_tcache.x86.o: kernel/checkpoint_delta.h

# ── kernel/tls_platform.c needs the vendored mbedTLS headers ───────────────
# An explicit rule rather than adding these to X86_CFLAGS, so vendor/mbedtls's
# headers are on the include path of exactly one file instead of all 200. The
# library ships library/common.h, library/constant_time_internal.h and other
# generic names; putting that directory on every translation unit's path is a
# collision waiting for the first kernel header with a matching name.
#
# Only include/ is added, never library/. include/ holds the mbedtls/-prefixed
# public headers and cannot collide; library/ holds the unprefixed internal
# ones and would.
#
# This rule is why the build broke at de65656: tls_platform.c was already in
# X86_C_SRC from an earlier commit, the allocator swap added an mbedTLS include
# to it, and I verified that only with host gcc and -I flags typed by hand --
# never through the Makefile that actually builds it. Compiling a file under
# different flags than the real build uses is not verification of the real
# build.
MBEDTLS_INC   = -I vendor/mbedtls/include -I vendor/mbedtls/shim
# vendor/mbedtls/shim/ is the freestanding libc-header stand-in for this
# build: the x86_64-elf cross toolchain ships no libc headers, but
# mbedTLS 3.6's public headers include <time.h> under HAVE_TIME_DATE
# (platform_util.h) and platform_time.h wants <inttypes.h> for
# mbedtls_ms_time_t unless MBEDTLS_PLATFORM_MS_TIME_TYPE_MACRO is set
# (it is, in sls_mbedtls_config.h). shim/time.h supplies struct tm only
# -- deliberately no time_t typedef, since a libc-equipped build (the
# deploy server) defines time_t itself and nothing here uses it; the
# shim is a no-op there and the struct tm supplier on a freestanding one.
# -U_FORTIFY_SOURCE is the root cause of a whole family of link errors, not a
# style preference. This toolchain defines _FORTIFY_SOURCE=2 by default, so gcc
# rewrites memcpy/memset/memmove with a compile-time-known size into
# __memcpy_chk and friends -- glibc functions absent from a freestanding link.
# It is also what produced __explicit_bzero_chk, which an earlier commit
# treated with ZEROIZE_ALT: a good fix for the wrong reason.
MBEDTLS_DEFS  = -DMBEDTLS_USER_CONFIG_FILE='"sls_mbedtls_config.h"' -U_FORTIFY_SOURCE \
                -Uunix -U__unix -U__unix__

kernel/tls_platform.x86.o: kernel/tls_platform.c $(AB_STAMP)
	$(X86_CC) $(X86_CFLAGS) $(MBEDTLS_INC) $(MBEDTLS_DEFS) -I kernel -c $< -o $@

# kernel/tls_cert.c is the second file to need them, for the certificate
# WRITER (x509_crt.h, pk.h, ecp.h). Same rule, same reasoning: include/ only,
# on exactly the two files that need it. The header comment above records that
# the build broke once because tls_platform.c's mbedTLS include was verified
# with hand-typed gcc flags instead of through this Makefile -- so tls_cert.c
# was built and its image-end measured through `make`, not by hand.
kernel/tls_cert.x86.o: kernel/tls_cert.c $(AB_STAMP)
	$(X86_CC) $(X86_CFLAGS) $(MBEDTLS_INC) $(MBEDTLS_DEFS) -I kernel -c $< -o $@

# Third and last of the mbedTLS-facing kernel files: the listener needs ssl.h.
kernel/tls_server.x86.o: kernel/tls_server.c $(AB_STAMP)
	$(X86_CC) $(X86_CFLAGS) $(MBEDTLS_INC) $(MBEDTLS_DEFS) -I kernel -I net -c $< -o $@

# ── The mbedTLS objects the kernel actually links ──────────────────────────
# THREE files, not all 107, because three is the measured closure of what
# kernel/tls_platform.c references today. There is no TLS server yet; adding
# the rest would link ~1 MB of code nothing calls and would put the image-end
# and frame-budget guards under load for no benefit. This list grows when a
# caller appears, not in anticipation of one.
#
# library/ IS on the include path here (unlike the tls_platform rule) because
# these sources include library/common.h and friends. That is safe for the
# vendored files themselves and unsafe for kernel files, which is why the two
# rules differ rather than sharing flags.
# Phase 3 needs the TLS server, X.509 write, EC key generation and PSA crypto.
# The dependency graph between those is dense enough that hand-picking a subset
# is a guessing game -- and I have guessed wrong about this link three times
# already this week. So: link the whole library first, MEASURE with the guards
# that exist for exactly this (kernel_image_end_check, stack_frame_budget_check),
# and trim from evidence.
#
# That inverts the "three files, not 107" argument from the previous commit,
# and deliberately. Then, three was the measured closure of what was actually
# called. Now nothing is called yet and the closure cannot be measured until a
# listener exists, so the honest sequence is measure-then-trim rather than
# trim-then-hope. The trimming is a real task, not an aspiration: it is what
# the image-end number is for.
MBEDTLS_SRC  = $(wildcard vendor/mbedtls/library/*.c)
MBEDTLS_OBJS = $(MBEDTLS_SRC:.c=.x86.o)

# libgcc supplies the compiler's own runtime helpers. bignum.c divides
# unsigned __int128 and gcc emits a call to __udivti3, which is not something
# a C file can provide -- it belongs to the compiler. -nostdlib excludes it
# along with libc, so it is named explicitly rather than by dropping -nostdlib
# and dragging a hosted libc in behind it.
LIBGCC := $(shell $(X86_CC) -print-libgcc-file-name 2>/dev/null)

$(MBEDTLS_OBJS): %.x86.o: %.c $(AB_STAMP)
	$(X86_CC) $(X86_CFLAGS) $(MBEDTLS_INC) -I vendor/mbedtls/library \
	          $(MBEDTLS_DEFS) -I kernel -c $< -o $@

$(TCG_OBJS): tcg-objs/%.x86.o: %.c $(AB_STAMP) $(SLS_STAMP)
	@mkdir -p tcg-objs
	$(X86_CC) $(TCG_CFLAGS) -c $< -o $@

# ─── Step 6.2: QEMU's own x86-64 guest frontend ──────────────────────────────
# docs/AeroSLS-QEMU-SLS-Step6-x86-Frontend-Plan-v0.1.md.
#
# M8 (Step 6.4): the decoder is the DEFAULT build. QEMU's own x86-64 guest
# frontend (translate.c through the Step 6.3 translator loop) is what
# deploy.sh builds and what runs in production, and the full M1-M8.5 gate
# (compiled elf elf-reject tls brkmmap rdclock futex sse2 faults) is the
# default build's regression. The retired 18-opcode C frontend remains
# selectable as a documented fixture:
#
#   make x86-iso                        # QEMU's decoder (default)
#   make x86-iso SLS_X86_FRONTEND=off   # the retired 18-opcode frontend
#
# The legacy config is kept compiling by CI (decoder-build's legacy-link
# step); it can no longer run the current fixture images, which begin with
# endbr64 (0xf3) and rep-prefixed ops the 18-opcode dispatcher never handled
# (iteration-26 doc).
#
# EXPLICIT PATH, NOT VPATH, and deliberately so. There are 20+ files named
# translate.c in the QEMU tree, one per guest architecture. Resolving this
# through VPATH and a %-stem would make the decoder we compile depend on VPATH
# search order, so adding an unrelated directory later could silently swap in
# another architecture's frontend and still build. The object is named
# i386-translate to keep that visible in the build log and in tcg-objs/.
#
# COMPILING_PER_TARGET goes ONLY here. It gates the helper 'tl' plumbing in
# exec/helper-head.h.inc, which is meaningful only for per-target files;
# defining it globally would apply a guest-word-size assumption to the
# generic TCG core, where it has no business.
SLS_X86_FRONTEND ?= on
ifeq ($(SLS_X86_FRONTEND),on)
TARGET_OBJS = tcg-objs/i386-translate.x86.o tcg-objs/translator.x86.o \
              tcg-objs/i386-helper-stubs.x86.o tcg-objs/i386-codefetch.x86.o \
              tcg-objs/i386-stub-class.x86.o
# M3: the decoder build's INVLPG hook (helper_flush_page) is wired into the C
# dispatcher's INVLPG decode, so the guest window's shadow PTEs are
# invalidated through the same function a TCG-translated INVLPG will call.
# The define gates that routing in sls-launcher.c; the default build does not
# link the helper layer and keeps its direct call.
#
# Step 6.3: TARGET_X86_64 and CONFIG_SYSTEM_ONLY make the decoder the 64-bit
# system-mode decoder it is upstream. Without TARGET_X86_64, translate.c's
# REX/CODE64/LMA machinery is compiled out and CPU_NB_REGS drops to 8 -- the
# decoder links and even runs, but as a 32-bit decoder that mis-decodes any
# REX-prefixed instruction. CONFIG_SYSTEM_ONLY gates gen_HLT() (emit.c.inc:
# 2054) -- without it, HLT translates to nothing and falls through.
TCG_CFLAGS += -DSLS_X86_FRONTEND=1 -DTARGET_X86_64 -DCONFIG_SYSTEM_ONLY
else ifneq ($(SLS_X86_FRONTEND),off)
$(error SLS_X86_FRONTEND must be 'on' or 'off', got '$(SLS_X86_FRONTEND)')
endif
# Frontend-flip stamp. TCG_CFLAGS differs between the two builds (the define
# above), but objects do not track their compile flags -- so flipping the
# frontend after an incremental build would silently reuse the previous
# build's sls-launcher.x86.o: the =on build would link a launcher that still
# calls qemu_sls_mmu_shadow_invlpg() directly, and the M3 helper routing would
# quietly not exist. Same mechanism and reasoning as AB_STAMP/BID_STAMP above;
# the write is here, after the ?=/ifeq block, so the recorded value is the
# effective one in BOTH builds (the SLS_STAMP variable itself is defined next
# to the other stamps, before the rules that depend on it).
$(shell v='$(SLS_X86_FRONTEND)'; [ -f $(SLS_STAMP) ] && [ "$$(cat $(SLS_STAMP))" = "$$v" ] || printf '%s' "$$v" > $(SLS_STAMP))

tcg-objs/i386-translate.x86.o: ../qemu/target/i386/tcg/translate.c $(AB_STAMP) $(SLS_STAMP)
	@mkdir -p tcg-objs
	$(X86_CC) $(TCG_CFLAGS) -DCOMPILING_PER_TARGET -c $< -o $@

# Step 6.3: x86_translate_code() calls translator_loop(), which lives here.
# This is the ONLY part of accel/tcg we take -- see the plan's 6.3 decision.
# cpu-exec.c, translate-all.c and cputlb.c stay out: sls-launcher.c keeps the
# execution loop and TB management, and cputlb.c is the software TLB that
# softmmu=OFF exists to bypass.
#
# No -DCOMPILING_PER_TARGET: verified it compiles identically with and without,
# and it is generic accel code, so the guest-word-size assumption has no place
# in it.
#
# Explicit rule even though 'translator.c' IS unique in the tree today and
# VPATH would resolve it. Uniformity within this group is worth one line: the
# neighbouring translate.c has 20+ namesakes, and a reader comparing the two
# rules should not have to work out why one is safe and the other is not.
tcg-objs/translator.x86.o: ../qemu/accel/tcg/translator.c $(AB_STAMP) $(SLS_STAMP)
	@mkdir -p tcg-objs
	$(X86_CC) $(TCG_CFLAGS) -c $< -o $@

# Step 6.4: bodies for the 765 helper_* symbols the decoder references.
# Generated by expanding target/i386/helper.h through QEMU's own DEF_HELPER
# machinery, so every stub carries the real signature and tracks upstream
# automatically -- there is no list here to go stale. Each halts naming itself,
# which is what turns "implement 765 helpers" into "run a binary, read the
# name, implement that one".
tcg-objs/i386-helper-stubs.x86.o: ../qemu/sls/sls-i386-helper-stubs.c $(AB_STAMP) $(SLS_STAMP)
	@mkdir -p tcg-objs
	$(X86_CC) $(TCG_CFLAGS) -DCOMPILING_PER_TARGET -c $< -o $@

# M7.5: the §4.5 permanent-unsupported classifier -- pure C, no QEMU
# headers, so the same file compiles in the host tests (aerosls2
# tests/unsupported_class_host_test.c) that pin it against the census.
tcg-objs/i386-stub-class.x86.o: ../qemu/sls/sls-i386-stub-class.c ../qemu/sls/sls-i386-stub-class.h $(AB_STAMP) $(SLS_STAMP)
	@mkdir -p tcg-objs
	$(X86_CC) $(TCG_CFLAGS) -c $< -o $@

# Guest instruction fetch for the softmmu=OFF path, plus the TB page-lock
# no-ops translator.c needs. Upstream gets both from cputlb.c (the soft MMU we
# exclude) or user-exec.c (1,271 lines of qemu-user process model). Ours reads
# straight through the GPA window, which IS what softmmu=OFF means.
# The compiled-guest fixture embeds guest-bytes.h (sls-launcher.c includes
# it), so a guest change that regenerates ONLY the header must still rebuild
# the launcher object. Without this dependency a header-only regen silently
# ships the previous guest -- the pattern rule below compiles sls-launcher.c
# from its .c + stamps alone. This rule adds the header as a prerequisite;
# it has no recipe, so the pattern rule still provides the compile.
# All EMBEDDED fixture headers are the same hazard as guest-bytes.h: a
# fixture change that regenerates ONLY a header must still rebuild the
# launcher object. This bit once: rdclock-bytes.h was regenerated after a
# fixture fix and make (dependent only on guest-bytes.h) shipped the stale
# bytes for four verification runs -- the gate kept failing for a defect
# that was already fixed, because the embedded fixture was still the old
# one. Every -bytes.h the launcher #includes is listed here.
tcg-objs/sls-launcher.x86.o: ../qemu/sls/guest/guest-bytes.h \
                             ../qemu/sls/guest/hello-bytes.h \
                             ../qemu/sls/guest/dynhello-bytes.h \
                             ../qemu/sls/guest/tls-bytes.h \
                             ../qemu/sls/guest/brkmmap-bytes.h \
                             ../qemu/sls/guest/rdclock-bytes.h \
                             ../qemu/sls/guest/futex-bytes.h \
                             ../qemu/sls/guest/sse2-bytes.h \
                             ../qemu/sls/guest/faults-bytes.h \
                             ../qemu/sls/guest/faults-entries.h

tcg-objs/i386-codefetch.x86.o: ../qemu/sls/sls-i386-codefetch.c $(AB_STAMP) $(SLS_STAMP)
	@mkdir -p tcg-objs
	$(X86_CC) $(TCG_CFLAGS) -DCOMPILING_PER_TARGET -c $< -o $@

arch/x86/trampoline.o: arch/x86/trampoline.asm
	$(ASN) -f bin $< -o arch/x86/trampoline.bin
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_arch_x86_trampoline_bin_start=trampoline_start \
		--redefine-sym _binary_arch_x86_trampoline_bin_end=trampoline_end \
		arch/x86/trampoline.bin arch/x86/trampoline.o

$(X86_BIN): $(X86_OBJECTS) $(TCG_OBJS) $(TARGET_OBJS) $(MBEDTLS_OBJS)
	$(X86_LD) $(X86_LDFLAGS) $(X86_OBJECTS) $(TCG_OBJS) $(TARGET_OBJS) $(MBEDTLS_OBJS) $(LIBGCC) -o $(X86_BIN)

x86-iso: $(X86_BIN)
	# ── Refuse to ship a sidecar archive that is not from THESE sources ─────
	# `sidecars.cpio` is tracked, so this recipe usually ships an archive packed
	# in an earlier session, and a `user/` change that was never re-packed is
	# invisible until boot time -- where it does NOT look like a packaging
	# problem, it looks like a kernel bug (P1a's create round trip answered by
	# an init with no ENV_REGISTER arm: "reply carries 28 bytes, short of the
	# 216-byte registration"). The archive carries the digest of what it was
	# packed from; a mismatch stops the build HERE. The rule is a digest and not
	# "is the archive older than user/*.rs" (a fresh clone writes the index in
	# path order, so every clean checkout would refuse); the tool explains.
	# The archive's own BYTES are checked as well (the archive-bytes tool against
	# the bytes stamp): the source digest cannot see a repack or a swap done out
	# of band -- user/ untouched, only the archive changed. And the RECORD is
	# checked too, so the stamps cannot have been refreshed independently of
	# each other: with only the two per-subject checks, a `user/` edit plus a
	# hand-run source digest over the old archive turned the first one green
	# while the old binaries still shipped. All three files come from one
	# `make selfhost-bootimage` run, and the record is what proves it.
	# And the archive's six binaries are checked against this tree's own BUILD
	# OUTPUT where the flattened `.bin` files exist: a record and its two stamps
	# can all be rewritten from a stale archive, but the build output is the one
	# half a hand cannot rewrite. (The e3_envs pack rebuilds all six sidecars and
	# overwrites the shared flattened paths, so any file named by a packed-variant
	# record is skipped -- not just init.)
	@if [ -s "$(SIDECAR_CPIO)" ]; then \
		want="$$(tools/sidecar_source_digest.sh)"; \
		have="$$(cat "$(SIDECAR_STAMP)" 2>/dev/null || true)"; \
		if [ "$$have" != "$$want" ]; then \
			echo "x86-iso: REFUSING to ship $(SIDECAR_CPIO) -- it was NOT packed from the user/ sources in this tree." >&2; \
			if [ -n "$$have" ]; then \
				echo "         $(SIDECAR_STAMP) records $$have" >&2; \
			else \
				echo "         $(SIDECAR_STAMP) is missing -- nothing here can say what the archive was packed from" >&2; \
			fi; \
			echo "         the user/ sources are now $$want" >&2; \
			echo "         A stale archive boots an init built from older sources, and the failure lands on" >&2; \
			echo "         the KERNEL (a create whose registration the manager answers with an error frame)." >&2; \
			echo "         Re-pack from these sources and retry:" >&2; \
			echo "             make selfhost-bootimage && make x86-iso" >&2; \
			echo "         Re-pack where the x86_64-unknown-none target is installed, and COMMIT the" >&2; \
			echo "         archive AND its stamps: all are tracked, and every host that does not build" >&2; \
			echo "         sidecars (CI's ISO job, deploy) ships exactly what is in the index." >&2; \
			exit 1; \
		fi; \
		want_sha="$$(tools/sidecar_archive_digest.sh "$(SIDECAR_CPIO)")"; \
		have_sha="$$(cat "$(SIDECAR_ARCHIVE_STAMP)" 2>/dev/null || true)"; \
		if [ "$$have_sha" != "$$want_sha" ]; then \
			echo "x86-iso: REFUSING to ship $(SIDECAR_CPIO) -- its BYTES are not the archive that was packed and stamped." >&2; \
			if [ -n "$$have_sha" ]; then \
				echo "         $(SIDECAR_ARCHIVE_STAMP) records $$have_sha" >&2; \
			else \
				echo "         $(SIDECAR_ARCHIVE_STAMP) is missing -- nothing here can say which archive bytes were packed" >&2; \
			fi; \
			echo "         the archive on disk hashes to $$want_sha" >&2; \
			echo "         So it was re-packed or replaced AFTER the stamp was written: a cpio built by" >&2; \
			echo "         hand from stale images changes these bytes and nothing else, while the SOURCES" >&2; \
			echo "         still match -- the stale-archive failure with nothing edited under user/." >&2; \
			echo "         Re-pack and re-stamp from these sources, then retry:" >&2; \
			echo "             make selfhost-bootimage && make x86-iso" >&2; \
			echo "         Commit the archive AND all three stamps ($(SIDECAR_STAMP), $(SIDECAR_ARCHIVE_STAMP), $(SIDECAR_STAMP_RECORD))." >&2; \
			exit 1; \
		fi; \
		want_record="$$(tools/sidecar_stamp_record.sh "$(SIDECAR_CPIO)")"; \
		have_record="$$(cat "$(SIDECAR_STAMP_RECORD)" 2>/dev/null || true)"; \
		if [ "$$have_record" != "$$want_record" ]; then \
			echo "x86-iso: REFUSING to ship $(SIDECAR_CPIO) -- its stamps were not all written by one packer run." >&2; \
			if [ -n "$$have_record" ]; then \
				echo "         $(SIDECAR_STAMP_RECORD) records a different sources/bytes pair than the instruments compute now." >&2; \
			else \
				echo "         $(SIDECAR_STAMP_RECORD) is missing -- nothing here can show the two stamps came from one run" >&2; \
			fi; \
			echo "         A stamp refreshed on its own still matches its subject while the other half describes the previous" >&2; \
			echo "         archive (a user/ edit plus a hand-run source digest was enough to ship the old binaries); the record" >&2; \
			echo "         is the one artifact both stamps are projections of, so this is the state it makes visible." >&2; \
			echo "         Re-pack from these sources and retry:" >&2; \
			echo "             make selfhost-bootimage && make x86-iso" >&2; \
			echo "         Commit the archive AND all three stamps ($(SIDECAR_STAMP), $(SIDECAR_ARCHIVE_STAMP), $(SIDECAR_STAMP_RECORD))." >&2; \
			exit 1; \
		fi; \
		tools/sidecar_build_output_check.sh --variant-record "$(SIDECAR_E3_CPIO).stamps" \
			--build boot/init.bin="$(SIDECAR_INIT_BIN)" \
			--build boot/dm.bin="$(SIDECAR_DM_BIN)" \
			--build boot/posix.bin="$(SIDECAR_POSIX_BIN)" \
			--build boot/ramdisk.bin="$(SIDECAR_RAMDISK_BIN)" \
			--build boot/net.bin="$(SIDECAR_NET_BIN)" \
			--build boot/e1000.bin="$(SIDECAR_E1000_BIN)" \
			"$(SIDECAR_CPIO)" || exit 1; \
		echo "[ISO] $(SIDECAR_CPIO) is from the user/ sources in this tree ($$want), its bytes are the stamped ones ($$want_sha), both stamps are one packer run's record, and its six binaries are this tree's flattened outputs where those exist"; \
	fi
	mkdir -p isodir/boot/grub
	cp $(X86_BIN) isodir/boot/
	# Phase 5: when the sidecars initrd exists, ship it so the Phase 5
	# GRUB menuentry can load it as a Multiboot2 module (build it with
	# `make selfhost-bootimage`). Without it, the ISO is byte-identical to
	# the pre-Phase-5 image and boots exactly as before.
	# The initrd is always embedded at the canonical name grub.cfg loads
	# (/boot/sidecars.cpio), whatever $(SIDECAR_CPIO)'s basename is — so the
	# E3 image (SIDECAR_CPIO=sidecars_e3.cpio) boots the Phase 5 entry too.
	@if [ -s "$(SIDECAR_CPIO)" ]; then cp "$(SIDECAR_CPIO)" isodir/boot/sidecars.cpio; echo "[ISO] including $(SIDECAR_CPIO) as /boot/sidecars.cpio"; fi
	cp grub.cfg isodir/boot/grub/
	grub-mkrescue --modules="normal multiboot multiboot2 iso9660 gfxterm font serial elf" \
	              -o $(X86_ISO) isodir
	rm -rf isodir

# Host port for `make x86-run`'s REST forward (guest 3000). `?=` so a host
# that also runs the CI runner, or the live cluster, can move it out of the
# way: 3001 is node 1's REST port AND the production backend, and a QEMU
# sitting there is not a failure -- it makes whatever trusts that port without
# probing test the wrong machine (tests/webapp_served_check.sh did exactly that
# on 2026-09-24). Test boots never use this port: they allocate from
# tests/free_port.sh's band (AEROSLS_FREE_PORT_RANGE), which refuses an overlap
# with 3001 by construction.
X86_HTTP_PORT ?= 3001

x86-run: x86-iso
	@if [ ! -f sls_storage.img ]; then qemu-img create -f raw sls_storage.img 10G; fi
	@if timeout 2 bash -c 'exec 3<>"/dev/tcp/127.0.0.1/$(X86_HTTP_PORT)"' 2>/dev/null; then \
		echo "x86-run: host port $(X86_HTTP_PORT) already has a listener — refusing" >&2; \
		echo "         rather than colliding (node 1's REST API and the production" >&2; \
		echo "         backend live on 3001). Move it: X86_HTTP_PORT=<free port> make x86-run" >&2; \
		exit 1; \
	fi
	qemu-system-x86_64 -cdrom $(X86_ISO) \
		-drive id=disk,file=sls_storage.img,if=none,format=raw \
		-device nvme,drive=disk,serial=slsdev0 \
		-netdev user,id=net0,hostfwd=tcp::$(X86_HTTP_PORT)-:3000 \
		-device e1000,netdev=net0,mac=52:54:00:12:34:01 \
		-vga std -display gtk \
		-m 4G -smp 4 -boot d -serial file:sls_kernel_debug.log

%.rv.o: %.S
	$(RV_CC) $(RV_CFLAGS) -c $< -o $@

%.rv.o: %.c
	$(RV_CC) $(RV_CFLAGS) -c $< -o $@

%.m.rv.o: %.S
	$(RV_CC) $(RV_CFLAGS_M) -c $< -o $@

%.m.rv.o: %.c
	$(RV_CC) $(RV_CFLAGS_M) -c $< -o $@

%.e.rv.o: %.S
	$(RV_CC) $(RV_CFLAGS) -DKERNEL_UART_ECHO -c $< -o $@

%.e.rv.o: %.c
	$(RV_CC) $(RV_CFLAGS) -DKERNEL_UART_ECHO -c $< -o $@

%.m.e.rv.o: %.S
	$(RV_CC) $(RV_CFLAGS_M) -DKERNEL_UART_ECHO -c $< -o $@

%.m.e.rv.o: %.c
	$(RV_CC) $(RV_CFLAGS_M) -DKERNEL_UART_ECHO -c $< -o $@

# Unique object for trap_riscv.c (see the RV_OBJECTS comment above), all
# four variants.
arch/riscv/trap_riscv_c.rv.o: arch/riscv/trap_riscv.c
	$(RV_CC) $(RV_CFLAGS) -c $< -o $@

arch/riscv/trap_riscv_c.m.rv.o: arch/riscv/trap_riscv.c
	$(RV_CC) $(RV_CFLAGS_M) -c $< -o $@

arch/riscv/trap_riscv_c.e.rv.o: arch/riscv/trap_riscv.c
	$(RV_CC) $(RV_CFLAGS) -DKERNEL_UART_ECHO -c $< -o $@

arch/riscv/trap_riscv_c.m.e.rv.o: arch/riscv/trap_riscv.c
	$(RV_CC) $(RV_CFLAGS_M) -DKERNEL_UART_ECHO -c $< -o $@

$(RV_ELF): $(RV_OBJECTS)
	$(RV_LD) $(RV_LDFLAGS) $(RV_OBJECTS) -o $(RV_ELF)

$(RV_ELF_M): $(RV_OBJECTS_M)
	$(RV_LD) $(RV_LDFLAGS_M) $(RV_OBJECTS_M) -o $(RV_ELF_M)

$(RV_ELF_E): $(RV_OBJECTS_E)
	$(RV_LD) $(RV_LDFLAGS) $(RV_OBJECTS_E) -o $(RV_ELF_E)

$(RV_ELF_ME): $(RV_OBJECTS_ME)
	$(RV_LD) $(RV_LDFLAGS_M) $(RV_OBJECTS_ME) -o $(RV_ELF_ME)

riscv-elf: $(RV_ELF) $(RV_ELF_M) $(RV_ELF_E) $(RV_ELF_ME)

# ── M4a: the minimal arm64 kernel (qemu-system-aarch64 -M virt) ────────
# Plan doc §6 M4 / §10.187: the first bootable AArch64 kernel, mirroring
# the RISC-V kernel's Phase 9 shape. sls_arm64_kernel.elf is linked at
# 0x40080000 (the virt DRAM + 2 MiB convention), boots a banner over the
# PL011, and powers off via PSCI SYSTEM_OFF (the SBI_SRST analog), so
# qemu exits rc=0. -mgeneral-regs-only is the arm64 analog of the x86
# kernel's -mno-sse: no FP/SIMD anywhere in the kernel.
AR_CC       = aarch64-linux-gnu-gcc
AR_LD       = aarch64-linux-gnu-ld
AR_CFLAGS   = -ffreestanding -O2 -Wall -Wextra -march=armv8-a -I. \
              -mgeneral-regs-only -fno-stack-protector -fno-pic -fno-pie \
              -ffunction-sections -fdata-sections
AR_LDFLAGS  = -T arch/arm64/linker_arm64.ld -nostdlib --gc-sections
AR_ASM_SRC  = arch/arm64/boot_arm64.S
# M4b: kernel/simi_arm.c joins the image — the §10.184 matrix's ARM
# build/link row flips NONE -> CHECK here. It needs the freestanding
# {memcpy, memset} pair (the §10.181 contract), provided by
# kernel/kernel_arm64.c.
AR_C_SRC    = kernel/kernel_arm64.c kernel/simi_arm.c arch/arm64/mmu.c \
              arch/arm64/uart_pl011.c arch/arm64/gic.c
AR_OBJECTS  = $(AR_ASM_SRC:.S=.ar64.o) $(AR_C_SRC:.c=.ar64.o)
AR_ELF      = sls_arm64_kernel.elf

%.ar64.o: %.S
	$(AR_CC) $(AR_CFLAGS) -c $< -o $@

%.ar64.o: %.c
	$(AR_CC) $(AR_CFLAGS) -c $< -o $@

$(AR_ELF): $(AR_OBJECTS)
	$(AR_LD) $(AR_LDFLAGS) $(AR_OBJECTS) -o $(AR_ELF)

arm64-elf: $(AR_ELF)

arm64-run: arm64-elf
	# The working M4a config, pinned empirically (§10.188): virtualization=on
	# (NOT secure) gives the virt machine the SMC PSCI conduit and enters
	# the payload at EL2; the head drops to EL1 and the smc routes to
	# QEMU's PSCI emulation, so qemu exits rc=0 (the SBI_SRST analog).
	qemu-system-aarch64 -M virt,virtualization=on -cpu cortex-a53 -m 1G \
		-kernel $(AR_ELF) -nographic -serial file:sls_arm64_boot.log -no-reboot

# §10.203 teeth: the EL0-only-by-construction machine check
# (-DARM64_TEETH_EL1_MEMTOUCH). Same sources, but kernel_arm64.c's main
# skips the gate sequence and instead attempts to run the mem_touch
# fixture at EL1: its baked user scratch (USER_SCRATCH_VA 0x10005000,
# unmapped in TTBR1) faults, the EL1h sync slot (boot_arm64.S
# arm64_sync_el1h) prints the Data Abort (EC=0x25, far=0x10005000) and
# halts. CI boots this ELF under a timeout and asserts the HANG (rc=124)
# + the [TEETH]/[SYNC] lines + the absence of any result/FAIL line.
# -Wno-unused-function: this variant never calls the gate-sequence
# functions (arm64_el1_entry/arm64_el0_done/arm64_wait_ticks/...), so
# the compiler's unused-static warnings are expected, not bugs. The
# object name ends in .ar64.o so `make clean`'s existing pattern removes
# it, and the teeth ELF matches `rm -f *.elf`.
AR_TEETH_ELF  = sls_arm64_kernel_teeth.elf
AR_TEETH_OBJS = $(filter-out kernel/kernel_arm64.ar64.o,$(AR_OBJECTS)) \
                kernel/kernel_arm64_teeth.ar64.o

kernel/kernel_arm64_teeth.ar64.o: kernel/kernel_arm64.c
	$(AR_CC) $(AR_CFLAGS) -DARM64_TEETH_EL1_MEMTOUCH -Wno-unused-function -c $< -o $@

$(AR_TEETH_ELF): $(AR_TEETH_OBJS)
	$(AR_LD) $(AR_LDFLAGS) $(AR_TEETH_OBJS) -o $(AR_TEETH_ELF)

arm64-teeth: $(AR_TEETH_ELF)

riscv-run: riscv-elf
	@if [ ! -f sls_storage_rv64.img ]; then qemu-img create -f raw sls_storage_rv64.img 10G; fi
	qemu-system-riscv64 -M virt -bios default -kernel $(RV_ELF) \
		-drive id=disk0,file=sls_storage_rv64.img,if=none,format=raw \
		-device virtio-blk-device,drive=disk0 \
		-m 4G -smp 4 -nographic -serial stdio

clean:
	# `rm -f *.o` matched NOTHING: all 127 objects are built next to their
	# sources (kernel/, net/, arch/x86/, drivers/, user/), none in the root.
	# So clean removed tcg-objs and left every kernel object in place, and
	# `make clean && make SLS_SOFTMMU=on` produced a MIXED kernel -- TCG
	# codegen from the new flag, identity stamp and kernel code from the old
	# one. A half-clean is worse than no clean, because it looks like a clean.
	find . -name '*.x86.o' -not -path './.git/*' -delete
	find . -name '*.rv.o'  -not -path './.git/*' -delete
	find . -name '*.ar64.o' -not -path './.git/*' -delete
	rm -f *.o *.bin *.iso *.elf *.img *.log *.cpio $(ALLOC_PLUGIN)
	rm -f $(AB_STAMP) $(BID_STAMP) $(SLS_STAMP)
	rm -rf tcg-objs

# ── User-space programs ────────────────────────────────────────────────────────
# Builds all .c files under user/examples/ into flat binaries using libsls.
# The binaries are position-independent (PIC) so the kernel can load them
# at any virtual address allocated by sys_sls_valloc().
#
# Prerequisites: gcc (host, x86-64), ld (GNU), xxd (vim-common — used only
# to regenerate the embedded-child header below)
#
# Usage:
#   make user-programs
#   python3 utils/program_upload.py --file user/examples/hello.bin --name hello
#   curl -X POST http://localhost:3001/api/program/spawn \
#        -H "Authorization: Bearer deadbeef01234567cafebabe76543210" \
#        -H "Content-Type: application/json" -d '{"name":"hello"}'

USER_CC      = gcc
USER_CFLAGS  = -m64 -O2 -std=c11 -ffreestanding -nostdlib \
               -fPIC -fno-plt -fno-stack-protector \
               -mno-sse -mno-sse2 -mno-avx \
               -Wall -Wextra -Iuser/libsls -Iuser/libaerocap
USER_LDFLAGS = -nostdlib -static -T user/libsls/user.ld
USER_SRCS    = $(wildcard user/examples/*.c)
USER_BINS    = $(USER_SRCS:.c=.bin)
USER_ELFS    = $(USER_SRCS:.c=.elf)

user/examples/%.elf: user/examples/%.c user/libsls/start.S user/libsls/sls.h user/libsls/user.ld
	$(USER_CC) $(USER_CFLAGS) $(USER_LDFLAGS) user/libsls/start.S $< -o $@

user/examples/%.bin: user/examples/%.elf
	objcopy -O binary $< $@
	@echo "[USER] Built $@ ($$(wc -c < $@) bytes)"

# cap_recycle_child_blob.h is a GENERATED artifact: part_recycle.c embeds
# cap_recycle_child's flat binary so it can upload it from ring-3 via
# sls_upload_binary() (see aerocap_provision.h — HTTP-uploaded objects are
# always partition 0 and would be denied by catalog_check_access() at spawn
# time, so the child object must be created and uploaded from ring-3).
# Regenerating it from the freshly-built .bin on every build means the
# embedded bytes can never drift from the program the Makefile actually
# compiled — editing the child's source automatically re-embeds it.
user/examples/cap_recycle_child_blob.h: user/examples/cap_recycle_child.bin
	@echo "[USER] Regenerating $@ from $<"
	@{ \
	  printf '/* user/examples/cap_recycle_child_blob.h - GENERATED by the Makefile. Do not edit.\n'; \
	  printf ' *\n'; \
	  printf ' * Embedded copy of cap_recycle_child.bin, uploaded from ring-3 by\n'; \
	  printf ' * part_recycle.c via sls_upload_binary() so the child object is born\n'; \
	  printf ' * inside a fresh partition (HTTP-uploaded objects are always partition\n'; \
	  printf ' * 0 and would be denied by catalog_check_access() at spawn time).\n'; \
	  printf ' *\n'; \
	  printf ' * Regenerated automatically by "make user-programs" whenever\n'; \
	  printf ' * cap_recycle_child.c changes; do not edit by hand.\n'; \
	  printf ' */\n'; \
	  printf 'static const unsigned char CAP_RECYCLE_CHILD_BLOB[] = {\n'; \
	  xxd -i $< | sed '1d' | head -n -2; \
	  printf '};\n'; \
	  printf '#define CAP_RECYCLE_CHILD_BLOB_LEN %su\n' "$$(wc -c < $<)"; \
	} > $@

# part_recycle embeds the blob header; rebuild it whenever the blob changes.
user/examples/part_recycle.elf: user/examples/cap_recycle_child_blob.h

# simi_recycle_tmo_blob.h is a GENERATED artifact: simi_recycle.c embeds
# tools/simi/tests/add.tmo (the 104-byte SIMI fixture whose main computes
# 2+3 -> r0=5) so it can upload it from ring-3 inside a fresh partition
# (HTTP-uploaded objects are always partition 0 and would be denied by
# catalog_check_access() at spawn time, so the SIMI object must be created
# and uploaded from ring-3). The .tmo is ITSELF generated by the SIMI host
# toolchain from add.simi, so this rule builds simi-asm on demand and
# reassembles the fixture before embedding — the embedded bytes can never
# drift from the source assembly, and a fresh checkout (tools/simi keeps
# build outputs untracked) still builds. Re-embedding on every build means
# editing add.simi automatically updates the ring-3 fixture.
tools/simi/simi-asm:
	$(MAKE) -C tools/simi simi-asm

tools/simi/tests/add.tmo: tools/simi/simi-asm tools/simi/tests/add.simi
	cd tools/simi && ./simi-asm tests/add.simi tests/add.tmo

user/examples/simi_recycle_tmo_blob.h: tools/simi/tests/add.tmo
	@echo "[USER] Regenerating $@ from $<"
	@{ \
	  printf '/* user/examples/simi_recycle_tmo_blob.h - GENERATED by the Makefile. Do not edit.\n'; \
	  printf ' *\n'; \
	  printf ' * Embedded copy of tools/simi/tests/add.tmo, uploaded from ring-3 by\n'; \
	  printf ' * simi_recycle.c via sls_upload_binary() so the SIMI object is born\n'; \
	  printf ' * inside a fresh partition (HTTP-uploaded objects are always\n'; \
	  printf ' * partition 0 and would be denied by catalog_check_access() at spawn\n'; \
	  printf ' * time). The .tmo is regenerated from tools/simi/tests/add.simi by\n'; \
	  printf ' * the SIMI host toolchain (simi-asm) as part of this rule, so the\n'; \
	  printf ' * embedded bytes can never drift from the source assembly.\n'; \
	  printf ' *\n'; \
	  printf ' * Regenerated automatically by "make user-programs" whenever add.simi\n'; \
	  printf ' * changes; do not edit by hand.\n'; \
	  printf ' */\n'; \
	  printf 'static const unsigned char SIMI_RECYCLE_TMO_BLOB[] = {\n'; \
	  xxd -i $< | sed '1d' | head -n -2; \
	  printf '};\n'; \
	  printf '#define SIMI_RECYCLE_TMO_BLOB_LEN %su\n' "$$(wc -c < $<)"; \
	} > $@

# simi_recycle embeds the blob header; rebuild it whenever the blob changes.
user/examples/simi_recycle.elf: user/examples/simi_recycle_tmo_blob.h

user-programs: $(USER_BINS)

.PHONY: user-programs

# ── Phase 5 self-hosted boot image ──────────────────────────────────────────
# Packages the init + Device Manager sidecar binaries into sidecars.cpio — a
# `newc` initrd (GRUB Multiboot2 module, U-Boot `-initrd`/`bootm`, QEMU
# `-initrd`) with each image at its manifest-declared physical address. The
# producer is the host tool aerosls-bootimage (user/bootimage/); the
# consumer contract lives in kernel/boot_image.h.
#
# BOTH sidecar binaries are produced HERE, end to end:
#   cargo build -p aerosls-init -p aerosls-dm --features target \
#       --target x86_64-unknown-none --release --bin init --bin dm
#       -> two ELFs whose first byte is `_start` (crt0.S via global_asm!,
#          linked by init.ld/dm.ld via build.rs — rustc's own rust-lld,
#          no cross-GCC needed)
#   aerosls-bootimage flatten   -> the flat binaries (PT_LOAD extraction,
#                          mini-objcopy) at SIDECAR_INIT_BIN/SIDECAR_DM_BIN
# If the cross target is not installed the target FAILS rather than reusing
# whatever ELFs happen to be on disk: the stamp it writes attests the PACKED
# BINARIES as well as the sources, so a run that did not build them must not
# write one. To pack deliberately prebuilt images, run aerosls-bootimage
# directly. The packaging itself is verified independently by the crate's
# golden tests:
#   cargo test -p aerosls-bootimage (user/Cargo.toml).
CARGO            ?= cargo
# Extra Cargo features for the init sidecar ONLY (init is built in its own
# cargo invocation below). Empty for the shipped Phase 5 image — that build is
# byte-for-byte unchanged. The POSIX-Environments E3 image sets this to
# `e3_envs` (see x86-iso-e3) so init spawns tenant environments; the feature is
# defined solely in user/init/Cargo.toml, hence init-only.
INIT_FEATURES    ?=
SIDECAR_INIT_ELF   ?= user/target/x86_64-unknown-none/release/init
SIDECAR_DM_ELF     ?= user/target/x86_64-unknown-none/release/dm
SIDECAR_POSIX_ELF ?= user/target/x86_64-unknown-none/release/posix
SIDECAR_RAMDISK_ELF ?= user/target/x86_64-unknown-none/release/ramdisk
SIDECAR_NET_ELF    ?= user/target/x86_64-unknown-none/release/network
SIDECAR_E1000_ELF  ?= user/target/x86_64-unknown-none/release/e1000_driver
SIDECAR_INIT_BIN   ?= user/target/x86_64-unknown-none/release/init.bin
SIDECAR_DM_BIN     ?= user/target/x86_64-unknown-none/release/dm.bin
SIDECAR_POSIX_BIN ?= user/target/x86_64-unknown-none/release/posix.bin
SIDECAR_RAMDISK_BIN ?= user/target/x86_64-unknown-none/release/ramdisk.bin
SIDECAR_NET_BIN    ?= user/target/x86_64-unknown-none/release/network.bin
SIDECAR_E1000_BIN  ?= user/target/x86_64-unknown-none/release/e1000_driver.bin
SIDECAR_CPIO       ?= sidecars.cpio
# The digest of the `user/` sources $(SIDECAR_CPIO) was packed from, written by
# selfhost-bootimage and verified by x86-iso. Derived from SIDECAR_CPIO (so the
# E3 image gets its own and the two cannot be confused), TRACKED for the same
# reason the archive is: the default `make x86-iso` ships the committed
# archive, and the check has to be able to answer for a tree that never built
# one. See tools/sidecar_source_digest.sh for why it is a digest and not an
# mtime comparison.
SIDECAR_STAMP      ?= $(SIDECAR_CPIO).digest
# The sha256 of $(SIDECAR_CPIO)'s OWN BYTES, written by selfhost-bootimage and
# verified by x86-iso next to the source stamp above. The source digest cannot
# see an archive re-packed or swapped out of band (user/ untouched, the stamp
# still matching), so the bytes are recorded separately. Derived from
# SIDECAR_CPIO like the digest (the E3 image gets its own pair), TRACKED for the
# same reason, and plain sha256sum so a host with no cargo can verify it. See
# tools/sidecar_archive_digest.sh for why BOTH stamps exist and neither suffices.
SIDECAR_ARCHIVE_STAMP ?= $(SIDECAR_CPIO).sha256
# The PACK-RUN RECORD: the one artifact the two stamps are DERIVED from, and
# the manifest of the six packed binaries. `selfhost-bootimage` runs
# tools/sidecar_stamp_record.sh once, which computes the source digest, the
# archive's sha256 and the digest of each `boot/*.bin` entry in a single
# invocation, and then projects each stamp out of the record (`sed -n
# 's/^sources //p'` / `s/^bytes //p'`). That is what keeps them from drifting
# independently: two stamp files written by two commands can each match their
# own subject while disagreeing with each other -- edit a `user/` source,
# hand-run the source digest over the old archive, and the source stamp is
# green again while the old binaries ship. tests/sidecar_stamp_check.sh clause
# K recomputes this record, requires the committed one to match, and requires
# each stamp to equal its field; clause L re-extracts the six binaries it names
# and requires them to be the archive's. Derived from SIDECAR_CPIO like the
# stamps (the E3 image gets its own), TRACKED for the same reason they are: the
# committed archive ships from trees that never run the packer.
SIDECAR_STAMP_RECORD ?= $(SIDECAR_CPIO).stamps

.PHONY: selfhost-bootimage
selfhost-bootimage:
	@echo "[SELFHOST] building the init + DM + POSIX + ramdisk + network + e1000_driver sidecars for x86_64-unknown-none..."
	@# A FAILED build here is FATAL -- do not relax it into a warning. The next
	@# steps pack $(SIDECAR_CPIO) and write $(SIDECAR_STAMP), and that stamp is
	@# what lets `x86-iso` ship the archive. A run that fell back to whatever
	@# binaries were already on disk (no x86_64-unknown-none target, a compile
	@# error) would stamp them as if this tree's sources produced them -- the
	@# stale-archive failure this whole mechanism exists to catch, one layer
	@# down, and harder to see: the SOURCES match, only the binaries inside are
	@# older. So: no build, no stamp. (To pack deliberately prebuilt images, run
	@# aerosls-bootimage directly; a stamp is only for an archive this target
	@# built.) tests/sidecar_stamp_check.sh clause G holds this line.
	@$(CARGO) build --quiet --manifest-path user/Cargo.toml -p aerosls-dm -p aerosls-sidecar -p aerosls-ramdisk -p aerosls-network -p aerosls-e1000-driver \
		--features target --target x86_64-unknown-none --release \
		--bin dm --bin posix --bin ramdisk --bin network --bin e1000_driver \
		|| { echo "[SELFHOST] REFUSING to pack and stamp: the sidecars did not build for x86_64-unknown-none." >&2; \
		     echo "           The stamp attests the packed binaries, and reusing existing ones would make it a lie." >&2; \
		     echo "           Install the target (rustup target add x86_64-unknown-none) and retry — see user/README.md." >&2; \
		     exit 1; }
	@# init is built separately so INIT_FEATURES can add e3_envs for the E3
	@# image without applying it to the other packages (which don't define it).
	@$(CARGO) build --quiet --manifest-path user/Cargo.toml -p aerosls-init \
		--features target $(if $(INIT_FEATURES),--features $(INIT_FEATURES)) --target x86_64-unknown-none --release \
		--bin init \
		|| { echo "[SELFHOST] REFUSING to pack and stamp: the init sidecar did not build for x86_64-unknown-none." >&2; \
		     echo "           Reusing an existing init.bin would stamp binaries this tree did not produce." >&2; \
		     exit 1; }
	@if [ -s "$(SIDECAR_INIT_ELF)" ]; then \
		$(CARGO) run --quiet --manifest-path user/Cargo.toml -p aerosls-bootimage -- \
			flatten --input "$(SIDECAR_INIT_ELF)" --output "$(SIDECAR_INIT_BIN)" \
			--load-vaddr 0x400000000000; \
	fi
	@if [ -s "$(SIDECAR_DM_ELF)" ]; then \
		$(CARGO) run --quiet --manifest-path user/Cargo.toml -p aerosls-bootimage -- \
			flatten --input "$(SIDECAR_DM_ELF)" --output "$(SIDECAR_DM_BIN)" \
			--load-vaddr 0x400000000000; \
	fi
	@if [ -s "$(SIDECAR_POSIX_ELF)" ]; then \
		$(CARGO) run --quiet --manifest-path user/Cargo.toml -p aerosls-bootimage -- \
			flatten --input "$(SIDECAR_POSIX_ELF)" --output "$(SIDECAR_POSIX_BIN)" \
			--load-vaddr 0x400000000000; \
	fi
	@if [ -s "$(SIDECAR_RAMDISK_ELF)" ]; then \
		$(CARGO) run --quiet --manifest-path user/Cargo.toml -p aerosls-bootimage -- \
			flatten --input "$(SIDECAR_RAMDISK_ELF)" --output "$(SIDECAR_RAMDISK_BIN)" \
			--load-vaddr 0x400000000000; \
	fi
	@if [ -s "$(SIDECAR_NET_ELF)" ]; then \
		$(CARGO) run --quiet --manifest-path user/Cargo.toml -p aerosls-bootimage -- \
			flatten --input "$(SIDECAR_NET_ELF)" --output "$(SIDECAR_NET_BIN)" \
			--load-vaddr 0x400000000000; \
	fi
	@if [ -s "$(SIDECAR_E1000_ELF)" ]; then \
		$(CARGO) run --quiet --manifest-path user/Cargo.toml -p aerosls-bootimage -- \
			flatten --input "$(SIDECAR_E1000_ELF)" --output "$(SIDECAR_E1000_BIN)" \
			--load-vaddr 0x400000000000; \
	fi
	@test -s "$(SIDECAR_INIT_BIN)" \
		|| { echo "[SELFHOST] missing init binary: $(SIDECAR_INIT_BIN)"; echo "           the flatten step produced nothing; is the cross target installed? (see user/README.md)"; exit 1; }
	@test -s "$(SIDECAR_DM_BIN)" \
		|| { echo "[SELFHOST] missing DM binary: $(SIDECAR_DM_BIN)"; echo "           the flatten step produced nothing; is the cross target installed? (see user/README.md)"; exit 1; }
	@test -s "$(SIDECAR_RAMDISK_BIN)" \
		|| { echo "[SELFHOST] missing ramdisk binary: $(SIDECAR_RAMDISK_BIN)"; echo "           the flatten step produced nothing; is the cross target installed? (see user/README.md)"; exit 1; }
	@test -s "$(SIDECAR_NET_BIN)" \
		|| { echo "[SELFHOST] missing network binary: $(SIDECAR_NET_BIN)"; echo "           the flatten step produced nothing; is the cross target installed? (see user/README.md)"; exit 1; }
	@test -s "$(SIDECAR_E1000_BIN)" \
		|| { echo "[SELFHOST] missing e1000 driver binary: $(SIDECAR_E1000_BIN)"; echo "           the flatten step produced nothing; is the cross target installed? (see user/README.md)"; exit 1; }
	$(CARGO) run --quiet --manifest-path user/Cargo.toml -p aerosls-bootimage -- \
		--init "$(SIDECAR_INIT_BIN)" --dm "$(SIDECAR_DM_BIN)" \
		--posix "$(SIDECAR_POSIX_BIN)" --ramdisk "$(SIDECAR_RAMDISK_BIN)" \
		--net "$(SIDECAR_NET_BIN)" --e1000 "$(SIDECAR_E1000_BIN)" -o "$(SIDECAR_CPIO)"
	@echo "[SELFHOST] boot image: $(SIDECAR_CPIO) (load as an initrd at the bootloader's module path)"
	@# The stamps, written HERE because this is the only target that packs an
	@# archive: `x86-iso` ships $(SIDECAR_CPIO) and refuses one whose stamps do
	@# not match the tree, so a `user/` change that is not re-packed stops at the
	@# ISO instead of at a boot. They attest the packed BINARIES as well as the
	@# sources, and the builds above are what make that true: they are FATAL, so
	@# these lines are only ever reached by a run that rebuilt all six sidecars
	@# from these sources. A host that reused existing binaries never gets here,
	@# and so never stamps them.
	@#
	@# ONE WRITE, FOUR FILES: sidecar_stamp_record.sh computes the source digest,
	@# the archive's sha256 and the digest of each of the six packed binaries in
	@# a single invocation into $(SIDECAR_STAMP_RECORD); the two stamp files are
	@# then PROJECTED out of that record -- views of one computation, not
	@# independent writes. That is the property that stops one stamp from being
	@# refreshed on its own (a `user/` edit plus a hand-run source digest) while
	@# the archive still holds the previous binaries; without the record both
	@# checks stayed green in exactly that state.
	@#
	@# The six binaries are passed for verification (--verify ENTRY=PATH): the
	@# record refuses to be written unless each archive entry is the flattened
	@# `$(SIDECAR_*_BIN)` file this run just produced, so the packer cannot bless
	@# images it did not build. tests/sidecar_stamp_check.sh clause L then
	@# re-extracts each entry from the archive, so the record cannot misstate
	@# which binaries are inside.
	@#
	@# The halves are, deliberately, different questions:
	@#   $(SIDECAR_STAMP)          is over the SOURCES -- recomputable with no
	@#                             cargo (even absent the archive), which is what
	@#                             lets x86-iso verify the COMMITTED archive in CI
	@#                             and on deploy, and what ties the images inside
	@#                             to the tree that built them.
	@#   $(SIDECAR_ARCHIVE_STAMP) is over the archive's own BYTES -- what catches
	@#                             a repack or swap done out of band, where the
	@#                             sources are untouched and only the archive
	@#                             changed. Also plain sha256sum, so also verifiable
	@#                             on a host with no cargo.
	@#   the `binary ` lines        name the six packed images themselves, each
	@#                             with the digest of its entry's data.
	@tools/sidecar_stamp_record.sh --verify boot/init.bin="$(SIDECAR_INIT_BIN)" --verify boot/dm.bin="$(SIDECAR_DM_BIN)" --verify boot/posix.bin="$(SIDECAR_POSIX_BIN)" --verify boot/ramdisk.bin="$(SIDECAR_RAMDISK_BIN)" --verify boot/net.bin="$(SIDECAR_NET_BIN)" --verify boot/e1000.bin="$(SIDECAR_E1000_BIN)" "$(SIDECAR_CPIO)" > "$(SIDECAR_STAMP_RECORD)"
	@sed -n 's/^sources //p' "$(SIDECAR_STAMP_RECORD)" > "$(SIDECAR_STAMP)"
	@sed -n 's/^bytes //p' "$(SIDECAR_STAMP_RECORD)" > "$(SIDECAR_ARCHIVE_STAMP)"
	@echo "[SELFHOST] stamp record: $(SIDECAR_STAMP_RECORD) (sources + archive bytes + the six packed binaries, verified against the flattened outputs -- the stamps below are views of this file)"
	@echo "[SELFHOST] stamp: $(SIDECAR_STAMP) (the digest of the user/ sources the packed binaries were built from; x86-iso verifies it)"
	@echo "[SELFHOST] stamp: $(SIDECAR_ARCHIVE_STAMP) (sha256 of the archive's own bytes -- a repack or swap out of band changes them; x86-iso verifies it too)"

# ── POSIX-Environments E3: the multi-instance-boot image ────────────────────
# The same six sidecars as `selfhost-bootimage`, except `init` is built with
# its `e3_envs` feature so that — after standing up the system POSIX — it
# spawns 2 tenant POSIX environments, each with its own ramdisk driver and a
# private R|W storage region carved from the frame pool via SYS_SLS_ALLOC_REGION
# (charged to init's partition). Each tenant POSIX formats its blank ramdisk on
# first mount (vfs mount_aerofs_or_format), so no init-side mkfs is needed.
#
# Everything lands in a SEPARATE cpio and a SEPARATE ISO (via variable
# overrides on the shared recipes), so the shipped sidecars.cpio /
# sls_operating_system.iso stay byte-for-byte identical to the Phase 5 image.
# tests/e3_multi_env_boot_check.sh boots $(X86_E3_ISO) and asserts both tenant
# environments come up at distinct storage addresses.
SIDECAR_E3_CPIO ?= sidecars_e3.cpio
X86_E3_ISO      ?= sls_operating_system_e3.iso

.PHONY: selfhost-bootimage-e3 x86-iso-e3
selfhost-bootimage-e3:
	@$(MAKE) selfhost-bootimage INIT_FEATURES=e3_envs SIDECAR_CPIO="$(SIDECAR_E3_CPIO)"

x86-iso-e3: selfhost-bootimage-e3
	@$(MAKE) x86-iso SIDECAR_CPIO="$(SIDECAR_E3_CPIO)" X86_ISO="$(X86_E3_ISO)"
	@echo "[ISO-E3] $(X86_E3_ISO) — init built with e3_envs (2 tenant POSIX environments)"

# ── SIMI host toolchain ─────────────────────────────────────────────────────
# Assembler/interpreter/disassembler/JIT-test for SIMI bytecode (tools/simi/).
# Builds with the host cc — no cross-compiler needed. See tools/simi/README.md.
simi-tools:
	$(MAKE) -C tools/simi all

simi-test: simi-tools
	$(MAKE) -C tools/simi test
	$(MAKE) -C tools/simi test-native

.PHONY: simi-tools simi-test

# ── aeroidl-consistency: the cross-language constant gate ──────────────────
# tools/aeroidl/tests/cross_lang_constants.rs greps every channel-flag and
# capability-permission constant across the kernel, C SDK, Rust runtime,
# mocks, and generated backends and fails if any disagrees with
# kernel/cap.h (the source of truth). This is the check that caught the
# CHAN_FLAG_NO_REPLY 0x0002 -> 0x0001 drift class: a constant changed in
# one language and silently wire-mismatched in the others.
#
# It runs inside `make aeroidl-check` (and thus CI on every push) and can
# be run standalone with:
#
#   make aeroidl-consistency
aeroidl-consistency:
	@echo "[AEROIDL] Cross-language constant consistency (kernel/cap.h is the source of truth)..."
	cargo test --quiet --manifest-path tools/aeroidl/Cargo.toml --test cross_lang_constants

# ── aeroidl-check: the Polyglot Nexus codegen gate ──────────────────────────
# Parses, type-checks, and generates all four backends (Rust client, Rust
# dispatcher, Common Lisp, C header) from every .aeroidl file under idl/,
# then COMPILES the generated code: cargo check on the Rust client +
# dispatcher, gcc/clang on the C header (skipped gracefully when no C
# compiler exists), and a paren-balance structural check on the Lisp. It
# also runs the full AeroIDL test suite (which includes
# cross_lang_constants) plus the dedicated aeroidl-consistency target, so
# constant drift across languages fails the gate like any other regression.
#
# This is the gate --check-all exists for. A codegen change that breaks any
# backend fails here with exit 1, so a generator regression cannot ship
# silently (it has caught real bugs: unbalanced Lisp parens, a missing C
# count param, and enum/arena type mismatches in the Rust dispatcher).
# Run it before merging any change to tools/aeroidl/ or idl/:
#
#   make aeroidl-check
#
# Requires the Rust toolchain (cargo) for the parser/compiler itself; the
# Lisp check is structural (no Lisp runtime needed).
aeroidl-check:
	@echo "[AEROIDL] Building the AeroIDL compiler..."
	cargo build --manifest-path tools/aeroidl/Cargo.toml
	@echo "[AEROIDL] Running the AeroIDL test suite..."
	cargo test --manifest-path tools/aeroidl/Cargo.toml
	@echo "[AEROIDL] Cross-language constants must agree with the kernel..."
	$(MAKE) -s aeroidl-consistency
	@echo "[AEROIDL] Checking all IDL files (all 4 backends must compile)..."
	@fail=0; for f in $$(ls idl/*.aeroidl 2>/dev/null); do \
		cargo run --quiet --manifest-path tools/aeroidl/Cargo.toml -- "$$f" --check-all || fail=1; \
	done; \
	if [ "$$fail" -ne 0 ]; then \
		echo "[AEROIDL] check-all FAILED"; exit 1; \
	fi; \
	echo "[AEROIDL] all IDL files: all backends OK"

# ── aeroidl-teeth: prove the aeroidl-check gate can FAIL ───────────────────
# tests/aeroidl_gate_smoke.sh breaks each of the four generators one at a
# time and requires `make aeroidl-check`'s check-all to fail, then restores
# the files byte-identically and requires it to pass again. A gate whose
# teeth are never shown to bite can go blind; this is the tooth-proving
# companion to the gate itself.
aeroidl-teeth:
	bash tests/aeroidl_gate_smoke.sh

.PHONY: aeroidl-check aeroidl-teeth aeroidl-consistency

# ── bundle: regenerate kernel/webapp_bundle.c from ../slsos-sim/dist ──────
# The committed kernel/webapp_bundle.c is CANONICAL -- CI and the deploy both
# build the kernel from it, and tests/webapp_bundle_guard_check.sh enforces
# it. Regenerate only after changing slsos-sim, review the diff, and COMMIT
# the result; never let a build host regenerate it.
#
# ─── Toolchain requirement: node 20+ (node 18 breaks the frontend build) ──
# `npm run build` inside ../slsos-sim dies on node 18 with the npm/cli#4828
# optional-dependencies bug:
#     Error: Cannot find native binding. ... @tailwindcss/oxide ...
#     https://github.com/npm/cli/issues/4828
# (npm 9 skips platform-specific native bindings; node 20+/npm 10+ installs
# them.) Proven on the 2026-08-18 reproducibility check: a fresh slsos-sim
# clone built fine under node 24 (via nvm) and failed under the system node
# 18. Use a modern node, e.g.  source ~/.nvm/nvm.sh && nvm use default
# (upstream workaround, if you must stay on node 18: delete package-lock.json
# and node_modules, then `npm i`).
#
# NOTE the `|| true` below: a frontend build failure is SILENTLY SWALLOWED,
# so `make bundle` proceeds with whatever ../slsos-sim/dist already happens
# to exist -- on a node-18 machine this target can quietly produce a STALE
# bundle. Check the npm output; the dist must be the one you intend to
# commit.
bundle:
	@echo "[BUNDLE] Generating kernel/webapp_bundle.c from slsos-sim/dist..."
	@cd ../slsos-sim && npm run build --silent 2>/dev/null || true
	@python3 tools/bundle_webapp.py ../slsos-sim/dist > kernel/webapp_bundle.c
	@echo "[BUNDLE] Done — $$(wc -l < kernel/webapp_bundle.c) lines generated."
	@echo "[BUNDLE] Reminder: commit the regenerated kernel/webapp_bundle.c -- the"
	@echo "          committed file is canonical (tests/webapp_bundle_guard_check.sh)."