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
X86_CFLAGS  = -ffreestanding -O2 -Wall -Wextra -mcmodel=small -mno-red-zone \
              -mno-sse -mno-sse2 -mno-mmx \
              -fno-pie -fno-pic -fno-tree-vectorize
X86_LDFLAGS = -T arch/x86/linker.ld -nostdlib --no-warn-rwx-segments

X86_ASM_SRC = arch/x86/boot.asm arch/x86/interrupt.asm arch/x86/switch_lazy.asm arch/x86/syscall.asm arch/x86/vector_crypto.asm arch/x86/process_enter.asm
X86_C_SRC   = kernel/kernel.c arch/x86/idt.c arch/x86/gdt.c kernel/scheduler.c arch/x86/lazy_fpu.c \
              arch/x86/walk_page_tables_x86.c \
              kernel/lockfree_map.c drivers/ahci.c drivers/pci.c drivers/nvme.c drivers/nvme_admin.c \
              kernel/frame_pool.c kernel/dashboard.c user/shell.c kernel/smp.c drivers/io_prio.c \
              net/consensus.c net/prefetch.c kernel/secure_api.c kernel/pte_migrate.c \
              kernel/timer.c kernel/flush_daemon.c \
              kernel/kernel_io.c kernel/syscall_dispatch.c \
              kernel/object_catalog.c kernel/transaction.c \
              kernel/ipc.c kernel/microkernel.c \
              kernel/tier_mgr.c kernel/query_engine.c \
              net/net.c net/arp.c net/ipv4.c net/tcp.c net/http.c net/e1000.c net/udp.c net/dhcp.c \
              kernel/process.c arch/x86/user_paging.c \
              kernel/loader.c \
              kernel/webapp.c \
              kernel/webapp_bundle.c \
              kernel/journal.c \
              kernel/lock_mgr.c \
              kernel/index_mgr.c \
              kernel/net_event.c \
              kernel/auth.c \
              kernel/stubs.c

X86_OBJECTS = $(X86_ASM_SRC:.asm=.x86.o) $(X86_C_SRC:.c=.x86.o) arch/x86/trampoline.o
X86_BIN     = my_sls_kernel.bin
X86_ISO     = sls_operating_system.iso

# --- RISC-V 64-Bit Toolchain ---
RV_CC       = riscv64-unknown-elf-gcc
RV_LD       = riscv64-unknown-elf-ld
RV_CFLAGS   = -ffreestanding -O2 -Wall -Wextra -mcmodel=medany \
              -march=rv64gc -mabi=lp64d -mno-relax -ffunction-sections -fdata-sections
RV_LDFLAGS  = -T arch/riscv/linker_riscv.ld -nostdlib --gc-sections

RV_ASM_SRC  = arch/riscv/boot_riscv.S arch/riscv/context_riscv.S arch/riscv/vector_state.S
RV_C_SRC    = kernel/kernel_riscv.c arch/riscv/walk_page_tables_riscv.c drivers/pci.c \
              kernel/frame_pool.c kernel/dashboard.c kernel/pte_migrate.c arch/riscv/sbi.c \
              arch/riscv/plic.c arch/riscv/lazy_vector.c

RV_OBJECTS  = $(RV_ASM_SRC:.S=.rv.o) $(RV_C_SRC:.c=.rv.o)
RV_ELF      = sls_riscv_kernel.elf

.PHONY: all clean x86-run riscv-run plugins

all: plugins x86-iso riscv-elf

plugins: compiler/SLSAllocationPassV2.cpp
	$(HOST_CXX) $(PLUGIN_CXXFLAGS) $(PLUGIN_LDFLAGS) $< -o $(ALLOC_PLUGIN) $(shell $(LLVM_CONFIG) --libs)

%.x86.o: %.asm
	$(ASN) -f elf64 $< -o $@

%.x86.o: %.c
	$(X86_CC) $(X86_CFLAGS) -c $< -o $@

arch/x86/trampoline.o: arch/x86/trampoline.asm
	$(ASN) -f bin $< -o arch/x86/trampoline.bin
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_arch_x86_trampoline_bin_start=trampoline_start \
		--redefine-sym _binary_arch_x86_trampoline_bin_end=trampoline_end \
		arch/x86/trampoline.bin arch/x86/trampoline.o

$(X86_BIN): $(X86_OBJECTS)
	$(X86_LD) $(X86_LDFLAGS) $(X86_OBJECTS) -o $(X86_BIN)

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
		-m 4G -smp 4 -boot d -serial file:sls_kernel_debug.log

%.rv.o: %.S
	$(RV_CC) $(RV_CFLAGS) -c $< -o $@

%.rv.o: %.c
	$(RV_CC) $(RV_CFLAGS) -c $< -o $@

$(RV_ELF): $(RV_OBJECTS)
	$(RV_LD) $(RV_LDFLAGS) $(RV_OBJECTS) -o $(RV_ELF)

riscv-elf: $(RV_ELF)

riscv-run: riscv-elf
	@if [ ! -f sls_storage_rv64.img ]; then qemu-img create -f raw sls_storage_rv64.img 10G; fi
	qemu-system-riscv64 -M virt -bios default -kernel $(RV_ELF) \
		-drive id=disk0,file=sls_storage_rv64.img,if=none,format=raw \
		-device virtio-blk-device,drive=disk0 \
		-m 4G -smp 4 -nographic -serial stdio

clean:
	rm -f *.o *.bin *.iso *.elf *.img *.log $(ALLOC_PLUGIN)

bundle:
	@echo "[BUNDLE] Generating kernel/webapp_bundle.c from slsos-sim/dist..."
	@cd ../slsos-sim && npm run build --silent 2>/dev/null || true
	@python3 tools/bundle_webapp.py ../slsos-sim/dist > kernel/webapp_bundle.c
	@echo "[BUNDLE] Done — $$(wc -l < kernel/webapp_bundle.c) lines generated."
	find . -name "*.o" -type f -delete
	find . -name "*.bin" -type f -delete