# AeroSLS OS - A Novel Flat Memory Operating system

---

## **1. Build & Run Guide**

This section covers everything needed to compile AeroSLS, boot it in QEMU, and connect the Navigator to the live kernel's REST API.

### Prerequisites

Install the following tools on Ubuntu/Debian:

```bash
sudo apt update
sudo apt install -y \
    nasm \
    grub-pc-bin grub-common grub-efi-amd64-bin xorriso \
    qemu-system-x86 \
    gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu \
    build-essential python3
```

> `grub-efi-amd64-bin` is required so `grub-mkrescue` produces a **UEFI + BIOS hybrid ISO** that boots on any modern machine without enabling CSM.

The cross-compiler is expected as `x86_64-elf-gcc`. If your distro provides it as `x86_64-linux-gnu-gcc` instead, create a symlink:

```bash
sudo ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc
sudo ln -sf /usr/bin/x86_64-linux-gnu-ld  /usr/local/bin/x86_64-elf-ld
```

For the RISC-V target, also install:

```bash
sudo apt install -y gcc-riscv64-unknown-elf
```

### Step 1: Clone and build the ISO

```bash
git clone https://github.com/kubeworkz/slsos.git
cd slsos
make x86-iso
```

This produces `sls_operating_system.iso` — a UEFI + BIOS bootable GRUB2 disc image with the kernel binary and the full Navigator SPA **embedded inside the kernel itself**.

> **Rebuilding the Navigator UI:** if you change `slsos-sim/src/`, regenerate the embedded bundle and rebuild the ISO:
>
> ```bash
> make bundle   # runs npm build in slsos-sim/, regenerates kernel/webapp_bundle.c
> make x86-iso
> ```

### Step 2: Create a persistent storage image (first run only)

```bash
qemu-img create -f raw sls_storage.img 2G
```

### Step 3: Boot in QEMU

```bash
qemu-system-x86_64 \
  -cdrom sls_operating_system.iso \
  -drive id=disk,file=sls_storage.img,if=none,format=raw \
  -device nvme,drive=disk,serial=slsdev0 \
  -netdev user,id=net0,hostfwd=tcp::3001-:3000 \
  -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
  -m 1G -smp 2 -boot d \
  -serial file:sls_kernel_debug.log \
  -display none -daemonize
```

Or use the Makefile shortcut (interactive, with display):

```bash
make x86-run
```

The kernel boots in ~3 seconds. The serial log is written to `sls_kernel_debug.log`. A clean boot looks like:

```
[HW] CPU GenuineIntel family=6 model=158
[HW] Memory map:
[HW]   0000000000100000 +   1023 MiB  RAM
[HW]   ...
[HW] Usable RAM: 1023 MiB
[E1000] MAC 52:54:00:12:34:01
[DHCP] Starting DISCOVER...
[DHCP] Bound: 10.0.2.15  gw 10.0.2.2
[NET] e1000 RX/TX rings online.
[HTTP] Listening on port 3000.
```

### Step 4: Verify the REST API

The kernel exposes a full REST API on guest port 3000, forwarded to host port 3001:

```bash
# System health
curl http://localhost:3001/api/health

# Object catalog + WAL stats
curl http://localhost:3001/api/scan

# Microkernel service status
curl http://localhost:3001/api/services

# Storage tiers (L1 cache / L2 DRAM / L3 SSD)
curl http://localhost:3001/api/tiers

# Authenticate (returns a lease token)
curl -X POST http://localhost:3001/auth/token \
     -H "Content-Type: application/json" \
     -d '{"email":"dave@gridworkz.com","password":"any"}'
```

### Step 5: Open the Navigator

The Navigator SPA is **embedded in the kernel** and served directly at port 3000. Open it in a browser:

```
http://localhost:3001/
```

The **Address Space Map**, **WAL Log**, **Service Monitor**, and **Query Console** tabs all read live data from the running kernel.

> **Alternatively** — the `slsos-sim` repo runs a Node.js development server that proxies the same API routes to the kernel and adds an AI query panel:
>
> ```bash
> cd slsos-sim && npm install && npm run dev -- --port 3000 --host
> ```

### Step 6: Stop the kernel

```bash
pkill -f qemu-system-x86_64
```

---

## **2. Real Hardware**

AeroSLS boots on any x86-64 machine with an Intel e1000/e1000e NIC (available as a ~$10 PCIe card, or built into many server boards).

### Flash to USB

```bash
sudo dd if=sls_operating_system.iso of=/dev/sdX bs=4M status=progress
```

Replace `/dev/sdX` with your USB drive (`lsblk` to identify it). The ISO is a hybrid image — it boots from both **USB** and **optical disc** in both **UEFI native** and **BIOS/CSM** modes.

### Boot

Select the USB from your machine's boot menu (usually F12 or Del at POST). The GRUB menu appears with a 3-second timeout. No keyboard input needed — the default entry boots automatically.

### What the serial log shows on real hardware

Connect a 3.3V USB-UART adapter to COM1 (pin 3 = TX out, pin 5 = GND) and open a terminal at 38400 baud. You will see:

```
[HW] CPU GenuineIntel family=6 model=158   ← your actual CPU
[HW] Memory map:
[HW]   0000000000100000 +  16384 MiB  RAM  ← real detected RAM
[HW]   ...
[E1000] MAC aa:bb:cc:dd:ee:ff              ← read from NIC EEPROM
[DHCP] Bound: 192.168.1.42  gw 192.168.1.1 ← IP from your router
[HTTP] Listening on port 3000.
```

Then open `http://192.168.1.42:3000/` in a browser on any machine on the same network.

### Configuring a static IP (optional)

If you want a fixed IP instead of DHCP, edit `include/config.h` before building:

```c
// include/config.h
#define KERNEL_STATIC_IP  0x6401A8C0UL  // 192.168.1.100
#define KERNEL_STATIC_GW  0x0101A8C0UL  // 192.168.1.1
```

The DHCP client still runs first and wins if a server responds. The static values are the fallback if DHCP times out (~3 seconds).

---

## **3. API Reference**


| Method | Endpoint              | Description                                |
| ------ | --------------------- | ------------------------------------------ |
| GET    | `/api/health`         | Kernel liveness probe                      |
| GET    | `/api/scan`           | Object catalog, WAL stats, service summary |
| GET    | `/api/objects`        | All SLS objects                            |
| GET    | `/api/objects/<name>` | Single object by name                      |
| GET    | `/api/services`       | Microkernel service list + IPC stats       |
| GET    | `/api/wal`            | Write-Ahead Log entries                    |
| GET    | `/api/tiers`          | Storage tier contents (L1/L2/L3)           |
| GET    | `/api/processes`      | Ring-3 process table                       |
| GET    | `/api/query?q=<sql>`  | SQL-style query engine                     |
| POST   | `/api/valloc`         | Allocate a virtual memory object           |
| POST   | `/api/record`         | Write a record to the object store         |
| POST   | `/api/tx/begin`       | Begin a transaction                        |
| POST   | `/api/tx/commit`      | Commit a transaction                       |
| POST   | `/api/tx/rollback`    | Roll back a transaction                    |
| POST   | `/auth/token`         | Issue a lease token (email → role)         |
| GET    | `/auth/verify`        | Validate a Bearer token                    |


Demo tokens (no real password check):


| Email                 | Role       | Token                              |
| --------------------- | ---------- | ---------------------------------- |
| `dave@gridworkz.com`  | `DB_ADMIN` | `deadbeef01234567cafebabe76543210` |
| `bob@vance.com`       | `APP_USER` | `cafebabe7654321089abcdef01234567` |
| `carol@gridworkz.com` | `DB_ADMIN` | `feedf00dabcdef0112345678deadc0de` |
| `guest@sandbox.com`   | `GUEST`    | `deadc0de9988776655443322aabbccdd` |


---

# Flatter is Better

