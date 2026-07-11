Building a **Single-Level Store (SLS) operating system** removes the traditional barrier between volatile main memory (RAM) and non-volatile secondary storage (disk/SSD). In an SLS architecture, all data—including running processes, system configurations, and persistent user files—is treated as a **single, massive, flat address space**. Programs do not make standard file system calls (like `open`, `read`, or `write`). Instead, they assign data to memory pointers, and the operating system handles transparency and persistence implicitly under the hood via paging mechanics.

Historically implemented in legendary platforms like **Multics** and **IBM's OS/400 (IBM i)**, modern interest in SLS systems is surging due to ultra-low latency NVMe hardware and vast 64-bit address spaces.

Here is the architectural roadmap we took to build a mock SLS operating system.

---

## **1. Conceptual Architecture**

To construct an SLS, we must redesign core kernel resource allocation strategies:

- **Unified Address Space**: Everything is a segment or an object referenced by a global, unique virtual memory pointer. 
- **Implicit I/O**: Hard drives or SSDs are treated merely as the physical spillover mechanism for RAM. Disk operations are strictly triggered via **Page Faults**.
- **Persistent Execution State**: Because memory is persistent, saving a file is equivalent to leaving it in memory. If the system loses power, the kernel reloads the exact state of active application objects from the storage medium upon booting up.

## **2. Core Implementation Phases**

Building any OS from scratch requires structured phases. For an SLS system, we will modify standard ++**[OSDev Wiki procedures](https://wiki.osdev.org/Creating_an_Operating_System)**++ to implement transparent persistence.

### **Phase I: Bootstrapping and Environment**

### **Setup a Cross-Compiler**: Configure `gcc` or `clang` targeting our choice architecture (like x86_64 or ARM64) on a host system.

- **Write the Bootloader**: Code a minimal assembly routine to switch the processor from 16-bit real mode into long 64-bit protected mode.
- **Target Emulator**: Use tools like ++**[QEMU](https://www.qemu.org/)**++ to test our custom kernel without crashing physical machines.

### **Phase II: Memory Management (The SLS Engine)**

- **Global Descriptor Table (GDT)**: Establish segment definitions to dictate code and data bounds.
- **Page Table Mapping**: Set up 4-level or 5-level page tables mapping virtual addresses directly to physical memory.
- **Custom Page Fault Handler**: This is the heart of an SLS. When a program requests a memory address that isn't mapped to RAM, our handler must trap the `CR2` register fault, identify which storage block corresponds to that virtual address, fetch it from secondary storage, map it, and resume execution.

### **Phase III: Hardware & Object Layer**

- **Storage Drivers**: Implement a raw AHCI or NVMe storage driver capable of reading and writing fixed-size blocks (typically 4KB) based on an address mapping tree.
- **Object Directory Tracker**: Implement a thread-safe, resilient tree structure (like a B-Tree or an inverted page table) that permanently records which virtual address blocks map to physical sectors on our persistent disk.
- **Language Safety Integration**: Because classical hardware memory management can degrade performance, consider a modern approach like ++**[Microsoft's Midori research](https://wiki.c2.com/?SingleAddressSpaceOperatingSystem)**++, utilizing a type-safe language (such as Rust or specialized C#) to enforce object boundary security in software rather than pure hardware tables.

---

## **3. Key Technical Challenges to Solve**


| **Challenge**              | **Impact on SLS Design**                                         | **Remediation Strategy**                                                                    |
| -------------------------- | ---------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| **Pointer Corruption**     | If memory locations drift, references break across reboots.      | Use Object Capability mechanisms or immutable relative offsets within shared memory spaces. |
| **Power Loss Consistency** | Unwritten cache pages can cause data fragmentation upon crashes. | Implement a non-volatile journaling/shadowing mechanism to commit snapshots periodically.   |
| **Garbage Collection**     | Deleted pointers create "orphaned" memory spaces over time.      | Use global object tables with hardware-backed reference counting to sweep dead data blocks. |


### Next Steps to Launch

1. **Run:** git clone [git@github.com](mailto:git@github.com):kubeworkz/slsos.git slsos
2. **Open the folder:** Launch VS Code or CLion, and select **Open Folder** on the freshly populated root workspace directory.
3. **Run our cross-compilation loops:** You can execute our targets directly inside our integrated terminal window:
  - **For the x86_64 target:** Run `make x86-run`
  - **For the RISC-V target:** Run `make riscv-run`

The full structural suite is now perfectly formatted and readable. 
