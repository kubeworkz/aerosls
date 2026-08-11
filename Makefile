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
X86_CC      = x86_64-elf-gcc
X86_LD      = x86_64-elf-ld
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
              kernel/process.c arch/x86/user_paging.c \
              kernel/partition.c \
              kernel/boot_params.c kernel/node_reset.c \
              kernel/console.c \
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
              kernel/vec_index.c kernel/sha256.c kernel/entropy.c kernel/rtc.c kernel/tls_platform.c \
              kernel/vec_join.c \
              kernel/agent.c \
              kernel/agent_tools.c \
              kernel/checkpoint_mgr.c \
              kernel/checkpoint_delta.c \
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

$(TCG_OBJS): tcg-objs/%.x86.o: %.c $(AB_STAMP) $(SLS_STAMP)
	@mkdir -p tcg-objs
	$(X86_CC) $(TCG_CFLAGS) -c $< -o $@

# ─── Step 6.2: QEMU's own x86-64 guest frontend ──────────────────────────────
# docs/AeroSLS-QEMU-SLS-Step6-x86-Frontend-Plan-v0.1.md.
#
# OFF BY DEFAULT, and it must stay that way until Step 6.4. translate.c
# references 765 helper_* symbols against sls-helper-stubs.c's ~130, so linking
# it today fails. Gating it keeps the default build -- and therefore deploy.sh
# -- exactly as it was, while the work proceeds behind a flag:
#
#   make x86-iso                        # unchanged, our 18-opcode frontend
#   make x86-iso SLS_X86_FRONTEND=on    # QEMU's decoder; will not link yet
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
SLS_X86_FRONTEND ?= off
ifeq ($(SLS_X86_FRONTEND),on)
TARGET_OBJS = tcg-objs/i386-translate.x86.o tcg-objs/translator.x86.o \
              tcg-objs/i386-helper-stubs.x86.o tcg-objs/i386-codefetch.x86.o
# M3: the decoder build's INVLPG hook (helper_flush_page) is wired into the C
# dispatcher's INVLPG decode, so the guest window's shadow PTEs are
# invalidated through the same function a TCG-translated INVLPG will call.
# The define gates that routing in sls-launcher.c; the default build does not
# link the helper layer and keeps its direct call.
TCG_CFLAGS += -DSLS_X86_FRONTEND=1
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
tcg-objs/translator.x86.o: ../qemu/accel/tcg/translator.c $(AB_STAMP)
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

# Guest instruction fetch for the softmmu=OFF path, plus the TB page-lock
# no-ops translator.c needs. Upstream gets both from cputlb.c (the soft MMU we
# exclude) or user-exec.c (1,271 lines of qemu-user process model). Ours reads
# straight through the GPA window, which IS what softmmu=OFF means.
tcg-objs/i386-codefetch.x86.o: ../qemu/sls/sls-i386-codefetch.c $(AB_STAMP) $(SLS_STAMP)
	@mkdir -p tcg-objs
	$(X86_CC) $(TCG_CFLAGS) -DCOMPILING_PER_TARGET -c $< -o $@

arch/x86/trampoline.o: arch/x86/trampoline.asm
	$(ASN) -f bin $< -o arch/x86/trampoline.bin
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_arch_x86_trampoline_bin_start=trampoline_start \
		--redefine-sym _binary_arch_x86_trampoline_bin_end=trampoline_end \
		arch/x86/trampoline.bin arch/x86/trampoline.o

$(X86_BIN): $(X86_OBJECTS) $(TCG_OBJS) $(TARGET_OBJS)
	$(X86_LD) $(X86_LDFLAGS) $(X86_OBJECTS) $(TCG_OBJS) $(TARGET_OBJS) -o $(X86_BIN)

x86-iso: $(X86_BIN)
	mkdir -p isodir/boot/grub
	cp $(X86_BIN) isodir/boot/
	cp grub.cfg isodir/boot/grub/
	grub-mkrescue --modules="normal multiboot2 iso9660 gfxterm font" \
	              -o $(X86_ISO) isodir
	rm -rf isodir

x86-run: x86-iso
	@if [ ! -f sls_storage.img ]; then qemu-img create -f raw sls_storage.img 10G; fi
	qemu-system-x86_64 -cdrom $(X86_ISO) \
		-drive id=disk,file=sls_storage.img,if=none,format=raw \
		-device nvme,drive=disk,serial=slsdev0 \
		-netdev user,id=net0,hostfwd=tcp::3001-:3000 \
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
	rm -f *.o *.bin *.iso *.elf *.img *.log $(ALLOC_PLUGIN)
	rm -f $(AB_STAMP) $(BID_STAMP) $(SLS_STAMP)
	rm -rf tcg-objs

# ── User-space programs ────────────────────────────────────────────────────────
# Builds all .c files under user/examples/ into flat binaries using libsls.
# The binaries are position-independent (PIC) so the kernel can load them
# at any virtual address allocated by sys_sls_valloc().
#
# Prerequisites: gcc (host, x86-64), ld (GNU)
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
               -Wall -Wextra -Iuser/libsls
USER_LDFLAGS = -nostdlib -static -T user/libsls/user.ld
USER_SRCS    = $(wildcard user/examples/*.c)
USER_BINS    = $(USER_SRCS:.c=.bin)
USER_ELFS    = $(USER_SRCS:.c=.elf)

user/examples/%.elf: user/examples/%.c user/libsls/start.S user/libsls/sls.h user/libsls/user.ld
	$(USER_CC) $(USER_CFLAGS) $(USER_LDFLAGS) user/libsls/start.S $< -o $@

user/examples/%.bin: user/examples/%.elf
	objcopy -O binary $< $@
	@echo "[USER] Built $@ ($$(wc -c < $@) bytes)"

user-programs: $(USER_BINS)

.PHONY: user-programs

# ── SIMI host toolchain ─────────────────────────────────────────────────────
# Assembler/interpreter/disassembler/JIT-test for SIMI bytecode (tools/simi/).
# Builds with the host cc — no cross-compiler needed. See tools/simi/README.md.
simi-tools:
	$(MAKE) -C tools/simi all

simi-test: simi-tools
	$(MAKE) -C tools/simi test
	$(MAKE) -C tools/simi test-native

.PHONY: simi-tools simi-test

bundle:
	@echo "[BUNDLE] Generating kernel/webapp_bundle.c from slsos-sim/dist..."
	@cd ../slsos-sim && npm run build --silent 2>/dev/null || true
	@python3 tools/bundle_webapp.py ../slsos-sim/dist > kernel/webapp_bundle.c
	@echo "[BUNDLE] Done — $$(wc -l < kernel/webapp_bundle.c) lines generated."
	find . -name "*.o" -type f -delete
	find . -name "*.bin" -type f -delete