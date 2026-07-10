Building a **Single-Level Store (SLS) operating system** removes the traditional barrier between volatile main memory (RAM) and non-volatile secondary storage (disk/SSD). In an SLS architecture, all data—including running processes, system configurations, and persistent user files—is treated as a **single, massive, flat address space**. Programs do not make standard file system calls (like `open`, `read`, or `write`). Instead, they assign data to memory pointers, and the operating system handles transparency and persistence implicitly under the hood via paging mechanics. [[1](https://en.wikipedia.org/wiki/Single-level_store), [2](https://community.ibm.com/community/user/discussion/ibmi-single-level-storage), [3](https://news.ycombinator.com/item?id=33413090), [4](https://thei.blog/posts/single_level_storage_3/), [5](https://gunkies.org/wiki/Single-level_store)]

Historically implemented in legendary platforms like **Multics** and **IBM's OS/400 (IBM i)**, modern interest in SLS systems is surging due to ultra-low latency NVMe hardware and vast 64-bit address spaces. [[1](https://rcs.uwaterloo.ca/pubs/sosp21-aurora.pdf), [2](https://osadmins.com/en/ibm-i-os-400-the-database-operating-system/), [3](https://www.youtube.com/watch?v=P8i-AUHv1jM), [4](https://en.wikipedia.org/wiki/Single-level_store)]

Here is the architectural and practical roadmap to building an SLS operating system. [[1](https://thei.blog/posts/single_level_storage_3/)]

---

## **1. Conceptual Architecture**

To construct an SLS, we must redesign core kernel resource allocation strategies: [[1](https://thei.blog/posts/single_level_storage_3/)]

- **Unified Address Space**: Everything is a segment or an object referenced by a global, unique virtual memory pointer. [[1](https://www.itjungle.com/2020/11/23/frank-soltis-discusses-a-possible-future-for-single-level-storage/), [2](https://en.wikipedia.org/wiki/Single-level_store)]
- **Implicit I/O**: Hard drives or SSDs are treated merely as the physical spillover mechanism for RAM. Disk operations are strictly triggered via **Page Faults**. [[1](https://news.ycombinator.com/item?id=33413090), [2](https://gunkies.org/wiki/Single-level_store)]
- **Persistent Execution State**: Because memory is persistent, saving a file is equivalent to leaving it in memory. If the system loses power, the kernel reloads the exact state of active application objects from the storage medium upon booting up. [[1](https://rcs.uwaterloo.ca/pubs/sosp21-aurora.pdf), [2](https://en.wikipedia.org/wiki/Single-level_store), [3](https://osadmins.com/en/ibm-i-os-400-the-database-operating-system/), [4](https://dl.acm.org/doi/full/10.1145/3742425), [5](https://thei.blog/posts/single_level_storage_3/)]

## **2. Core Implementation Phases**

Building any OS from scratch requires structured phases. For an SLS system, you will modify standard ++**[OSDev Wiki procedures](https://wiki.osdev.org/Creating_an_Operating_System)**++ to implement transparent persistence. [[1](https://rcs.uwaterloo.ca/pubs/sosp21-aurora.pdf), [2](https://wiki.osdev.org/Creating_an_Operating_System), [3](https://www.youtube.com/watch?v=X82l37rAnP8)]

### **Phase I: Bootstrapping and Environment**

### **Setup a Cross-Compiler**: Configure `gcc` or `clang` targeting your choice architecture (like x86_64 or ARM64) on a host system.

- **Write the Bootloader**: Code a minimal assembly routine to switch the processor from 16-bit real mode into long 64-bit protected mode.
- **Target Emulator**: Use tools like ++**[QEMU](https://www.qemu.org/)**++ to test your custom kernel without crashing physical machines. [[1](https://www.youtube.com/watch?v=X82l37rAnP8), [2](https://wiki.osdev.org/Creating_an_Operating_System), [3](https://wiki.osdev.org/Getting_Started), [4](https://www.youtube.com/watch?v=2lHnFe0xy0Y&t=610), [5](https://www.instagram.com/reel/DQzw61Ckoim/?hl=en), [6](https://www.youtube.com/shorts/LgFjqEZ33Yc)]

### **Phase II: Memory Management (The SLS Engine)**

- **Global Descriptor Table (GDT)**: Establish segment definitions to dictate code and data bounds.
- **Page Table Mapping**: Set up 4-level or 5-level page tables mapping virtual addresses directly to physical memory.
- **Custom Page Fault Handler**: This is the heart of an SLS. When a program requests a memory address that isn't mapped to RAM, your handler must trap the `CR2` register fault, identify which storage block corresponds to that virtual address, fetch it from secondary storage, map it, and resume execution. [[1](https://news.ycombinator.com/item?id=33413090), [2](https://www.youtube.com/shorts/LgFjqEZ33Yc), [3](https://wiki.osdev.org/Creating_an_Operating_System), [4](https://gunkies.org/wiki/Single-level_store), [5](https://cs.uwaterloo.ca/~brecht/servers/readings/Summaries/Seltzer-OS/readings/kilburn-1961.html)]

### **Phase III: Hardware & Object Layer**

- **Storage Drivers**: Implement a raw AHCI or NVMe storage driver capable of reading and writing fixed-size blocks (typically 4KB) based on an address mapping tree. [[1](https://www.youtube.com/watch?v=P8i-AUHv1jM), [2](https://thei.blog/posts/single_level_storage_1/), [3](https://cgi.cse.unsw.edu.au/~reports/papers/9314.pdf), [4](https://newsletter.systemdesign.one/p/aws-s3-system-design)]
- **Object Directory Tracker**: Implement a thread-safe, resilient tree structure (like a B-Tree or an inverted page table) that permanently records which virtual address blocks map to physical sectors on your persistent disk. [[1](https://cs.uwaterloo.ca/~brecht/servers/readings/Summaries/Seltzer-OS/readings/kilburn-1961.html), [2](https://cgi.cse.unsw.edu.au/~reports/papers/9314.pdf)]
- **Language Safety Integration**: Because classical hardware memory management can degrade performance, consider a modern approach like ++**[Microsoft's Midori research](https://wiki.c2.com/?SingleAddressSpaceOperatingSystem)**++, utilizing a type-safe language (such as Rust or specialized C#) to enforce object boundary security in software rather than pure hardware tables. [[1](https://wiki.c2.com/?SingleAddressSpaceOperatingSystem), [2](https://www.sciencedirect.com/topics/computer-science/engineering-workstation)]

---

## **3. Key Technical Challenges to Solve**


| **Challenge**              | **Impact on SLS Design**                                         | **Remediation Strategy**                                                                    |
| -------------------------- | ---------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| **Pointer Corruption**     | If memory locations drift, references break across reboots.      | Use Object Capability mechanisms or immutable relative offsets within shared memory spaces. |
| **Power Loss Consistency** | Unwritten cache pages can cause data fragmentation upon crashes. | Implement a non-volatile journaling/shadowing mechanism to commit snapshots periodically.   |
| **Garbage Collection**     | Deleted pointers create "orphaned" memory spaces over time.      | Use global object tables with hardware-backed reference counting to sweep dead data blocks. |


## **4. Alternative: The Hybrid Approach**

If building a kernel entirely from assembly and C seems daunting, look into the architecture of modern academic research projects like ++**[The Aurora SLS Operating System](https://rcs.uwaterloo.ca/pubs/sosp21-aurora.pdf)**++. Instead of writing raw bare-metal code, you can write a **hypervisor extension** or customize the ++**[FreeBSD or Linux kernel virtual memory subsystem (Mach VM structures)](https://www.watchedkettle.com/assets/pdf/aurora-sosp.pdf)**++. This allows you to leverage existing hardware drivers while modifying the core OS paging logic to force transparent persistence at regular intervals. [[1](https://rcs.uwaterloo.ca/pubs/sosp21-aurora.pdf), [2](https://www.watchedkettle.com/assets/pdf/aurora-sosp.pdf)]

If you are interested, we can explore any part of this architecture further. Tell me:

- Do you want to build this **completely from scratch (bare metal)** or as a **custom fork of an existing microkernel**?
- What **programming language** (C, C++, Assembly, or Rust) are you planning to use?
- Which **hardware architecture** (x86_64 or ARM) are you targeting? [[1](https://www.youtube.com/watch?v=X82l37rAnP8), [2](https://www.instagram.com/reel/DQzw61Ckoim/?hl=en), [3](https://www.scribd.com/document/845420944/Types-of-Operating-Systems-Based-on-Architecture), [4](https://www.youtube.com/shorts/LgFjqEZ33Yc), [5](https://www.youtube.com/watch?v=eys5TpLWdgQ), [6](https://www.instagram.com/reel/DHeAeykgh0D/?hl=en), [7](https://www.youtube.com/watch?v=2lHnFe0xy0Y&t=610)]

### Next Steps to Launch

1. **Run the script:** Execute `python3 build_workspace.py` inside your local directory pool.
2. **Open the folder:** Launch VS Code or CLion, and select **Open Folder** on the freshly populated root workspace directory.
3. **Run your cross-compilation loops:** You can execute your targets directly inside your integrated terminal window:
  - **For the x86_64 target:** Run `make x86-run`
  - **For the RISC-V target:** Run `make riscv-run`

The full structural suite is now perfectly formatted and readable. Let me know if you need any further optimization passes as you begin building your project!
