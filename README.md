# AeroSLS OS - An AI Driven Single Level Storage OS

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

# File Manipulation Details

Data is written to the DB engine as key-value records in a DB_TABLE object. There's no external "file" to upload — you write individual fields directly. Other files such as executables and other file types use streaming. Two interfaces:

### Shell:

`insert <object> <key> <value>`

e.g. `insert employees name Alice`

`REST API (POST /api/record): { "object": "employees", "key": "name", "value": "Alice" }`

Each `DB_TABLE` stores keyed string fields. If you need a structured row, you insert multiple fields with a common prefix (e.g., alice_name, alice_dept), following the suffix-matching convention used by indexes, constraints, and aggregates.

For uploading binary programs, the format is chunked hex over `POST /api/program/upload` -`{"name":"…","hex":"deadbeef…","offset":N,"last":0|1}` - up to 1024 bytes per chunk. Similarly for streams

via `/api/stream/upload`.

# Here's a concrete breakdown:

**name:** the name of the `PROGRAM object` you previously created with `/api/program/create`. It's just a string identifier, e.g. `"myapp"`.

**hex:** the raw bytes of the binary file encoded as a continuous lowercase hex string (2 chars per byte). For example, the 4 bytes `0xDE 0xAD 0xBE 0xEF` become `"deadbeef"`. For a real ELF binary you'd do:

  `xxd -p mybinary | tr -d '\n'`

which produces something like `"7f454c460201010000000000..."` (ELF magic number followed by the rest of the file).

**offset:** byte position in the binary where this chunk begins. First chunk is 0.

**last:** 1 only on the final chunk; triggers the kernel to finalize `binary_size`, detect ELF vs flat format, and set status → ready.

### **Full example**: small binary that fits in one chunk (≤ 1024 bytes):

`POST /api/program/create`

`{"name": "myapp", "pages": 2}`

`POST /api/program/upload`

`{"name": "myapp", "hex": "7f454c46020101000000000000000000", "offset": 0, "last": 1}`

### Multi-chunk example (file > 1024 bytes):

`{"name": "myapp", "hex": "<first 2048 hex chars>",  "offset":    0, "last": 0}`

`{"name": "myapp", "hex": "<next  2048 hex chars>",  "offset": 1024, "last": 0}`

`{"name": "myapp", "hex": "<final chunk hex>",       "offset": 2048, "last": 1}`

---

Here's how arbitrary files (text, PDF, binary blobs, etc.) flow through the system:

## **The** `STREAM` **Object Type**

Non-executable, non-DB files use the `OBJ_TYPE_STREAM` path, which is distinct from `OBJ_TYPE_PROGRAM` (executables) and `OBJ_TYPE_DB_TABLE`.

### **Ingestion Path**

**1. Create a named stream object**

```
POST /api/stream/create
{ "name": "report.pdf", "mime": "application/pdf" }
```

This calls `stream_create()` in [stream.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/125df4672b/resources/app/out/vs/code/electron-browser/workbench/workbench.html), which:

- Allocates a slot in `stream_store[STREAM_MAX]`
- Registers it in the object catalog as `OBJ_TYPE_STREAM` (storage tier: **L3_SSD**)
- Seeds metadata records: `status=created`, `byte_size=0`, `mime_type=...`
- Assigns an NVMe LBA base: `STREAM_DATA_LBA_BASE + slot * STREAM_SECTORS_PER_SLOT`

**2. Upload the file in hex-encoded chunks**

```
POST /api/stream/upload
{ "name": "report.pdf", "hex": "255044462d...", "offset": 0, "last": 1 }
```

This calls `stream_write_chunk()`, which:

- Decodes hex → raw bytes via `hex_decode()`
- Allocates 4 KiB physical RAM frames on demand from the frame pool
- Copies bytes into `se->frames[frame_idx]` using frame-aligned writes
- On `last=1`: flushes all frames to NVMe synchronously, updates metadata records (`byte_size`, `status=ready`), persists the stream directory to NVMe

### **Serving/Download**

`GET /api/stream/<name>` triggers `http_respond_stream()`:

- Sends HTTP headers with the stored `mime_type` and `Content-Disposition: attachment`
- Sends content frame-by-frame (4 KiB each)
- If a frame is `NULL` (post-reboot, not in RAM yet) → **lazy-loads** it from NVMe via `stream_lazy_load_frame()`

### **Persistence**

A 4 KiB directory at `STREAM_DIR_LBA` on NVMe (magic `SLSSTRMX`) tracks all active streams. On `stream_init()` at boot, it reads this directory and re-registers all streams in the object catalog so they survive reboots.

### **Key difference from executables**


|                        | **PROGRAM**                            | **STREAM**                                |
| ---------------------- | -------------------------------------- | ----------------------------------------- |
| **Catalog type**       | `OBJ_TYPE_PROGRAM`                     | `OBJ_TYPE_STREAM`                         |
| **Storage tier**       | L2_DRAM                                | L3_SSD                                    |
| **Upload endpoint**    | `/api/program/upload`                  | `/api/stream/upload`                      |
| **On** `last=1`        | Detects ELF, maps into PML4, spawnable | Flush to NVMe, mark `ready`, downloadable |
| **Execute permission** | Yes                                    | No (guests can only read)                 |
| **Persistence**        | In-RAM binary store (lost on reboot)   | NVMe directory with lazy frame reload     |


The `mime_type` field is purely informational - the kernel doesn't interpret it; it just echoes it back in the `Content-Type` response header.

### **Checkpointing / Snapshots**

There is persistence at the L1/L2 memory tiers. 

**How it works:**


| Event                                                     | Hook                    | NVMe write                                     |
| --------------------------------------------------------- | ----------------------- | ---------------------------------------------- |
| `valloc` / `vfree` / `role_set`                           | `persist_catalog()`     | LBA 1024–1055: object_catalog[] + role_table[] |
| `insert` / `update` / `delete` (direct or tx-commit path) | `persist_records()`     | LBA 2048–2567: object_records[] (~232 KiB)     |
| `schema_set`                                              | `persist_schemas()`     | LBA 2568–2807: object_schemas[] (~116 KiB)     |
| final binary chunk upload                                 | `persist_programs()`    | LBA 4096–4240: service_binaries[] (~66 KiB)    |
| boot (after nvme_io_init)                                 | `persist_restore_all()` | reads all 4 regions back before stream_init()  |


**Safety:** Each region has a distinct magic header (`0xCAFE...01-04`). A struct-size check guards against format mismatches between kernel builds - wrong size = cold start for that subsystem, others still restore. Works correctly alongside the existing stream NVMe persistence (stream data is at LBA 65536+, these snapshots sit at LBA 1024–4240).

## AuroraSLS vs AeroSLS (persistence strategy)

**What Aurora does:**

- **Incremental checkpoints**: tracks dirty pages, only flushes changed pages to NVMe via `SLOS` (their custom object store on NVMe)
- **Partition model**: groups processes into SLS partitions that checkpoint together atomically
- **Page-oriented memsnap**: the unit of persistence is a 4 KiB page, not a data structure
- `slsctl checkpoint` triggers it; `slsctl restore` replays it at boot

**What AeroSLS already has that's relevant:**

- **A working WAL in journal.c and transaction.c::** logs mutations but never replays at boot
- **The stream directory pattern:** a fixed NVMe LBA with a magic header, written on every mutation, read back on `stream_init()`
- **The tier manager's auto-promote/demote loop in the AP poll:** a natural hook for periodic checkpoints

**Our Strategy for AeroSLS:**

Rather than full Aurora-style dirty-page tracking (which requires MMU-level write protection on catalog pages), a simpler fit  would be **three snapshot LBA ranges** modeled exactly on how streams already work:


| What                                | NVMe LBA           | Written when                     | Read back when                 |
| ----------------------------------- | ------------------ | -------------------------------- | ------------------------------ |
| object_catalog[] + role_table[]     | `CATALOG_SNAP_LBA` | every `valloc`/`vfree`           | new `catalog_init()` at boot   |
| object_records[] + object_schemas[] | `RECORD_SNAP_LBA`  | every `insert`/`update`/`delete` | same                           |
| `service_binaries[]` (programs)     | `PROG_SNAP_LBA`    | on `last=1` upload chunk         | new `loader_restore()` at boot |


This is essentially **Aurora's checkpoint concept but simplified**: instead of process memory snapshots, we're only persisting kernel metadata structs (which are small - `CATALOG_MAX_OBJECTS=256` entries fits in well under 1 MiB total).

**The main gap** is our boot replay path - `kernel.c` currently calls `stream_init()` but there's no equivalent `catalog_restore()` or `loader_restore()`. That's where we do our thing.

The WAL is also already doing half the job for DB records - we just needed a compact replay pass at boot instead of only being used for within-session ACID.
