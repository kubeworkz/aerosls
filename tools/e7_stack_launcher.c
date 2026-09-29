/*
 * tools/e7_stack_launcher.c — start a static ET_EXEC on an initial stack this
 * program builds itself, so the auxv is CHOSEN rather than inherited.
 *
 * ─── Why this exists ───────────────────────────────────────────────────────
 * tools/linux_syscall_census.sh refuses to write a census until the candidate
 * declares CANDIDATE_VDSO_SYSCALLS, because the trace it takes runs on a Linux
 * whose kernel maps a vDSO and therefore does not show the syscalls the vDSO
 * satisfies in userspace. The shim has no vDSO (design §5.2 omits
 * AT_SYSINFO_EHDR), so those calls become real traps on AeroSLS.
 *
 * A declaration is a claim. This program is how the claim is CHECKED: it starts
 * the candidate with an auxv that has no AT_SYSINFO_EHDR in it, and a census of
 * THAT run must show each declared syscall as a real entry. The declaration is
 * then measured rather than asserted.
 *
 * ─── Why a loader and not PTRACE ───────────────────────────────────────────
 * The obvious way to remove one auxv entry is to trace the target from
 * PTRACE_TRACEME, stop at the exec, and blank the entry on the stack. That
 * cannot work under the census: the run is already traced by `strace`, and
 * PTRACE_TRACEME while already traced fails with EPERM. A second process that
 * traced the target instead would leave the census unable to see the target's
 * syscalls at all.
 *
 * So this program does what the kernel's ELF loader does for a static ET_EXEC —
 * map the PT_LOAD segments, copy them in, apply their protections and
 * GNU_RELRO, build argc/argv/envp/auxv at the top of a fresh stack — and jumps
 * to e_entry. Everything about the target's start state is then explicit in
 * this file, which is the point: the thing under test is the auxv, and here the
 * auxv is a literal in the source.
 *
 * ─── What it does NOT do ───────────────────────────────────────────────────
 * No PT_INTERP (refused: that needs ld.so, and E7's candidates are static), no
 * PT_DYNAMIC relocation processing (a static ET_EXEC has no dynamic relocations
 * to process), no address randomisation (the target is ET_EXEC; its addresses
 * are fixed by the link), and no VDSO. It also does no capability/EHDR
 * assistance: it is a loader, not a kernel.
 *
 * ─── Be exact about "no vDSO" ──────────────────────────────────────────────
 * The vDSO IMAGE is still mapped in this process — the kernel mapped it when it
 * exec'd the LAUNCHER, and nothing here unmaps it. What is gone is the only
 * handle a static libc uses to find it: AT_SYSINFO_EHDR in the auxv. That is
 * the condition AeroSLS actually presents (no vDSO mapped, no auxv entry), and
 * it is the same condition as far as musl is concerned — musl reaches the vDSO
 * through `__vdsosym`, which scans `libc.auxv` and nothing else. Stated because
 * the difference is real even though nothing measured here can see it: a libc
 * that hunted for the vDSO by scanning /proc/self/maps, or the address space,
 * would still find one under this launcher.
 *
 * ─── The marker ────────────────────────────────────────────────────────────
 * One `write(2, "e7:enter-no-vdso\n")` happens immediately before the jump, and
 * NOTHING else runs between that write and the target's first syscall. That
 * line is the boundary in the strace output: everything after it is the
 * target's own syscalls, so a census of this run is comparable row-for-row with
 * the plain census instead of being polluted by the loader's opens and mmaps.
 *
 * Build (no dependencies beyond libc):
 *     cc -O2 -Wall -Wextra -o e7_stack_launcher tools/e7_stack_launcher.c
 *
 * Run:
 *     e7_stack_launcher ./some-static-binary [args...]
 *
 * Exit: 2 for a usage/load error (nothing was entered), else the target's own
 * exit status, because the target replaces this process.
 */

#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define STACK_SIZE (8u * 1024u * 1024u)
#define MARKER "e7:enter-no-vdso\n"

extern char **environ;

static long page_size;
static int verbose;

static void die(const char *what)
{
	fprintf(stderr, "e7_stack_launcher: %s: %s\n", what, strerror(errno));
	exit(2);
}

static void die_msg(const char *msg)
{
	fprintf(stderr, "e7_stack_launcher: %s\n", msg);
	exit(2);
}

static uintptr_t page_floor(uintptr_t a) { return a & ~(uintptr_t)(page_size - 1); }
static uintptr_t page_ceil(uintptr_t a) { return page_floor(a + page_size - 1); }

static void read_exact(int fd, void *buf, size_t len, off_t off)
{
	ssize_t n;
	while (len) {
		n = pread(fd, buf, len, off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("pread of the target image");
		}
		if (n == 0)
			die_msg("the target image ended before the bytes it claims");
		buf = (char *)buf + n;
		len -= (size_t)n;
		off += n;
	}
}

/* Load the target the way the kernel loads a static ET_EXEC, minus the vDSO. */
static uintptr_t load_target(const char *path, Elf64_Ehdr *eh, Elf64_Phdr **ph_out,
			     size_t *phnum_out, uintptr_t *phdr_addr_out,
			     uintptr_t *relro_start, uintptr_t *relro_len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		die(path);

	read_exact(fd, eh, sizeof *eh, 0);
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
		die_msg("the target is not an ELF file");
	if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB)
		die_msg("the target is not a 64-bit little-endian ELF");
	if (eh->e_machine != EM_X86_64)
		die_msg("the target is not x86-64");
	if (eh->e_type != ET_EXEC)
		die_msg("the target is not ET_EXEC — this launcher exists for the static "
			"non-PIE case (§5.1); ET_DYN would need load-bias self-relocation");
	if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0)
		die_msg("the target has no usable program header table");

	Elf64_Phdr *ph = calloc(eh->e_phnum, sizeof *ph);
	if (!ph)
		die("calloc of the program header table");
	read_exact(fd, ph, (size_t)eh->e_phnum * sizeof *ph, (off_t)eh->e_phoff);

	/* Pass 1: the page ranges. One map per non-overlapping run of pages, in
	 * ascending order, so two segments that share a page do not fight over it
	 * (they are contiguous in the file image, and both are copied in below). */
	uintptr_t mapped_end = 0, prev_start = 0;
	size_t i;
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (ph[i].p_memsz == 0)
			continue;
		uintptr_t vs = (uintptr_t)ph[i].p_vaddr;
		if (vs < prev_start)
			die_msg("the target's PT_LOAD segments are not in ascending order");
		prev_start = vs;
		uintptr_t s = page_floor(vs), e = page_ceil(vs + ph[i].p_memsz);
		if (e <= mapped_end)
			continue;
		if (s < mapped_end)
			s = mapped_end;
		void *got = mmap((void *)s, e - s, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (got == MAP_FAILED)
			die("mmap of a target segment (is that address range already "
			    "in use? this launcher must be built as a PIE, or run with "
			    "the target's addresses free)");
		mapped_end = e;
	}
	if (mapped_end == 0)
		die_msg("the target has no loadable segment");

	/* Pass 2: the file bytes. Anonymous pages are zero, which is the bss. */
	for (i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD && ph[i].p_filesz)
			read_exact(fd, (void *)(uintptr_t)ph[i].p_vaddr,
				   (size_t)ph[i].p_filesz, (off_t)ph[i].p_offset);

	/* The phdrs the target reads through AT_PHDR must be inside a segment
	 * that was really mapped from the file — otherwise the target would
	 * follow AT_PHDR into unmapped memory and die somewhere confusing. */
	uintptr_t phdr_addr = 0;
	uintptr_t want = (uintptr_t)eh->e_phoff;
	uintptr_t want_end = want + (uintptr_t)eh->e_phnum * sizeof(Elf64_Phdr);
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0)
			continue;
		if (want >= ph[i].p_offset &&
		    want_end <= (uintptr_t)ph[i].p_offset + ph[i].p_filesz) {
			phdr_addr = (uintptr_t)ph[i].p_vaddr + (want - ph[i].p_offset);
			break;
		}
	}
	if (!phdr_addr)
		die_msg("the target's program headers are not inside a loadable segment");

	/* Pass 3: protections, in program-header order — for a page two segments
	 * share, the later header wins, which is what the kernel's per-segment
	 * mprotect does too. */
	uintptr_t relro_s = 0, relro_l = 0;
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == PT_INTERP)
			die_msg("the target has a PT_INTERP: it is dynamically linked, "
				"and §5.1 refuses that up front");
		if (ph[i].p_type == PT_GNU_RELRO) {
			relro_s = (uintptr_t)ph[i].p_vaddr;
			relro_l = (uintptr_t)ph[i].p_memsz;
			continue;
		}
		if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
			continue;
		int prot = 0;
		if (ph[i].p_flags & PF_R) prot |= PROT_READ;
		if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
		if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
		uintptr_t s = page_floor((uintptr_t)ph[i].p_vaddr);
		uintptr_t e = page_ceil((uintptr_t)ph[i].p_vaddr + ph[i].p_memsz);
		if (mprotect((void *)s, e - s, prot) != 0)
			die("mprotect of a target segment");
	}
	/* GNU_RELRO after the segments, as the kernel does for the main
	 * executable: .data.rel.ro and .got become read-only before entry. */
	if (relro_l)
		if (mprotect((void *)page_floor(relro_s),
			     page_ceil(relro_s + relro_l) - page_floor(relro_s),
			     PROT_READ) != 0)
			die("mprotect of GNU_RELRO");

	close(fd);

	*ph_out = ph;
	*phnum_out = eh->e_phnum;
	*phdr_addr_out = phdr_addr;
	*relro_start = relro_s;
	*relro_len = relro_l;
	if (verbose)
		fprintf(stderr, "e7_stack_launcher: %s loaded at %#lx .. %#lx, phdrs at %#lx\n",
			path, (unsigned long)page_floor((uintptr_t)ph[0].p_vaddr),
			(unsigned long)mapped_end, (unsigned long)phdr_addr);
	return (uintptr_t)eh->e_entry;
}

/* The stack the target will see. Strings live above the tables, so every
 * pointer in the tables stays valid while the tables are written. */
static uintptr_t build_stack(int argc, char **argv, int envc, char **envp,
			     uintptr_t entry, const Elf64_Ehdr *eh, size_t phnum,
			     uintptr_t phdr_addr)
{
	void *region = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED)
		die("mmap of the target stack");

	uintptr_t top = (uintptr_t)region + STACK_SIZE;
	char **argv_copy = calloc((size_t)argc, sizeof *argv_copy);
	char **envp_copy = calloc((size_t)envc ? (size_t)envc : 1, sizeof *envp_copy);
	if (!argv_copy || !envp_copy)
		die("calloc of the argv/envp copy");

	/* Strings, downward from the top of the region. */
	for (int i = envc - 1; i >= 0; i--) {
		size_t n = strlen(envp[i]) + 1;
		top -= n;
		memcpy((void *)top, envp[i], n);
		envp_copy[i] = (char *)top;
	}
	for (int i = argc - 1; i >= 0; i--) {
		size_t n = strlen(argv[i]) + 1;
		top -= n;
		memcpy((void *)top, argv[i], n);
		argv_copy[i] = (char *)top;
	}
	static const char platform[] = "x86_64";
	top -= sizeof platform;
	memcpy((void *)top, platform, sizeof platform);
	uintptr_t platform_addr = top;

	size_t execfn_n = strlen(argv[0]) + 1;
	top -= execfn_n;
	memcpy((void *)top, argv[0], execfn_n);
	uintptr_t execfn_addr = top;

	/* AT_RANDOM's 16 bytes. Taken fresh; the launcher's own auxv is a
	 * fallback for a kernel without getrandom(2). */
	top -= 16;
	if (getrandom((void *)top, 16, 0) != 16) {
		unsigned long self = getauxval(AT_RANDOM);
		if (!self)
			die_msg("no AT_RANDOM to seed the target's stack canary from");
		memcpy((void *)top, (void *)self, 16);
	}
	uintptr_t random_addr = top;

	/* Tables, below the strings. */
	const unsigned long hwcap = getauxval(AT_HWCAP);
	const unsigned long hwcap2 = getauxval(AT_HWCAP2);
	const unsigned int naux = 18;
	size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + 2u * (naux + 1u);
	uintptr_t sp = (top - words * sizeof(uint64_t)) & ~(uintptr_t)15;
	uint64_t *w = (uint64_t *)sp;

	*w++ = (uint64_t)argc;
	for (int i = 0; i < argc; i++)
		*w++ = (uint64_t)(uintptr_t)argv_copy[i];
	*w++ = 0;
	for (int i = 0; i < envc; i++)
		*w++ = (uint64_t)(uintptr_t)envp_copy[i];
	*w++ = 0;

	*w++ = AT_PHDR;     *w++ = phdr_addr;
	*w++ = AT_PHENT;    *w++ = eh->e_phentsize;
	*w++ = AT_PHNUM;    *w++ = phnum;
	*w++ = AT_PAGESZ;   *w++ = (uint64_t)page_size;
	*w++ = AT_BASE;     *w++ = 0;            /* no interpreter */
	*w++ = AT_FLAGS;    *w++ = 0;
	*w++ = AT_ENTRY;    *w++ = entry;
	*w++ = AT_UID;      *w++ = (uint64_t)getuid();
	*w++ = AT_EUID;     *w++ = (uint64_t)geteuid();
	*w++ = AT_GID;      *w++ = (uint64_t)getgid();
	*w++ = AT_EGID;     *w++ = (uint64_t)getegid();
	*w++ = AT_SECURE;   *w++ = 0;
	*w++ = AT_RANDOM;   *w++ = random_addr;
	*w++ = AT_HWCAP;    *w++ = hwcap;
	*w++ = AT_HWCAP2;   *w++ = hwcap2;
	*w++ = AT_CLKTCK;   *w++ = 100;
	*w++ = AT_PLATFORM; *w++ = platform_addr;
	*w++ = AT_EXECFN;   *w++ = execfn_addr;
	/* AT_SYSINFO_EHDR is deliberately absent: that absence IS the test. */
	*w++ = AT_NULL;     *w++ = 0;

	if ((size_t)(w - (uint64_t *)sp) != words)
		die_msg("internal error: the auxv table was not built to its own length");
	if (sp < (uintptr_t)region || sp + words * 8 > top)
		die_msg("internal error: the crafted stack is not inside its region");

	if (verbose) {
		fprintf(stderr, "e7_stack_launcher: stack at %#lx, argc=%d envc=%d, "
			"%u auxv entries, AT_SYSINFO_EHDR NOT set\n",
			(unsigned long)sp, argc, envc, naux);
		fprintf(stderr, "e7_stack_launcher: AT_PHDR=%#lx AT_ENTRY=%#lx AT_PAGESZ=%#lx\n",
			(unsigned long)phdr_addr, (unsigned long)entry,
			(unsigned long)page_size);
	}
	return sp;
}

__attribute__((noreturn))
static void enter_target(uintptr_t entry, uintptr_t sp)
{
	/* rdi = stack, rsi = entry, named explicitly so the rdx/rbp zeroing
	 * between them cannot clobber either. Nothing runs after this. */
	__asm__ __volatile__(
		"movq %%rdi, %%rsp\n\t"
		"xorl %%ebp, %%ebp\n\t"
		"xorl %%edx, %%edx\n\t"
		"jmpq *%%rsi\n\t"
		: : "D"(sp), "S"(entry) : "memory");
	__builtin_unreachable();
}

int main(int argc, char **argv)
{
	if (argc < 2 ||
	    strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		fprintf(stderr,
			"usage: e7_stack_launcher <static-ET_EXEC> [args...]\n"
			"  Starts the target with an initial stack built by this program\n"
			"  that has NO AT_SYSINFO_EHDR: the vDSO image stays mapped here\n"
			"  (this process is the launcher), but a static libc's only handle\n"
			"  on it is gone, which is the condition AeroSLS presents. Writes a\n"
			"  '" MARKER "' marker to fd 2 immediately before entry.\n"
			"  E7_LAUNCHER_VERBOSE=1 prints the load addresses and the auxv.\n");
		return 2;
	}

	verbose = getenv("E7_LAUNCHER_VERBOSE") != NULL;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		die_msg("cannot determine the page size");

	Elf64_Ehdr eh;
	Elf64_Phdr *ph = NULL;
	size_t phnum = 0;
	uintptr_t phdr_addr = 0, relro_s = 0, relro_l = 0;
	uintptr_t entry = load_target(argv[1], &eh, &ph, &phnum, &phdr_addr,
				      &relro_s, &relro_l);

	int envc = 0;
	while (environ[envc])
		envc++;

	uintptr_t sp = build_stack(argc - 1, argv + 1, envc, environ, entry,
				   &eh, phnum, phdr_addr);

	/* The boundary line: the launcher's last syscall before the target's
	 * first. A census splits the trace here, so the loader's own opens and
	 * mmaps stay out of the target's row set. */
	if (write(2, MARKER, sizeof MARKER - 1) < 0)
		die("write of the entry marker");

	enter_target(entry, sp);
}
