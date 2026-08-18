/* user/examples/part_recycle.c — Phase 2 partition-teardown verification
 *
 * Proves that destroying a partition reclaims everything its processes
 * owned, via repeated create → assign → spawn-into → destroy cycles from
 * ring-3. Each cycle, THIS process (spawned via HTTP while uid 1000 was
 * unassigned, so it lives in the default partition and survives every
 * destroy below) does:
 *
 *   1. sls_partition_create() — a fresh partition.
 *   2. sls_partition_assign(uid 1000) — route uid 1000 into it. From this
 *      moment, any object created / process spawned with uid 1000 resolves
 *      to this partition (partition_get_for_uid).
 *   3. sls_obj_valloc() — a fresh OBJ_TYPE_PROGRAM object with a
 *      cycle-unique name and owner_uid=1000. partition_id=0 defaults to the
 *      owner's current partition — the ONLY way to get an object whose
 *      partition matches the partition catalog_check_access() will compare
 *      at spawn time (HTTP uploads always create partition-0 objects and
 *      would be denied).
 *   4. sls_upload_binary() — the embedded cap_recycle_child flat binary
 *      (cap_recycle_child_blob.h), completing the object.
 *   5. sls_program_spawn_nb_held() — the GATED async spawn. The dispatch
 *      threads spawn_owner_uid() (this process's real owner_uid = 1000), so
 *      the child inherits uid 1000 AND the object-partition check passes —
 *      the child is created IN THIS PARTITION.
 *   6. cap_chan_create(far_pid = child) while the child is HELD — its far
 *      end lands at frd=0/fwr=1 in ITS table.
 *   7. cap_recv_release(rd, block=1, child) — release + park in ONE
 *      syscall. The child runs LIVE: allocates two MEM caps (one kept),
 *      sends one ('Q'), and exits — its exit teardown reclaims the kept
 *      MEM arena frame, its page tables, its syscall stack, and its cap
 *      table. If a timer tick has not yet let the child run to its exit,
 *      the child is still SUSPENDED when we reach step 8, and
 *      partition_destroy KILLS it — process_kill's synchronous teardown
 *      reclaims exactly the same set. Either path is reclaimed; the boot
 *      check's frame-count stability assertion is the proof.
 *   8. Normal-path teardown: revoke the transferred MEM cap + our channel
 *      ends (the channel object dies with our last holder).
 *   9. sls_partition_destroy() — kills any remaining process in the
 *      partition (the suspended child), vfrees its catalog objects (the
 *      cycle's PROGRAM object — name slot freed for the next cycle), and
 *      partition_reclaim_all_frames() reclaims every frame the partition
 *      still owns (the Seed Kernel Phase 2 watermark fix made frames below
 *      the cap arena reclaimable — previously they were reported
 *      machine-owned and leaked forever).
 *
 * The partition lifecycle / object-catalog ABI is hidden behind the SDK —
 * sls_partition_create/assign/destroy come from libsls/sls.h, and
 * sls_obj_valloc / sls_upload_binary from the provisioning skin
 * (user/libaerocap/aerocap_provision.h). No raw syscall numbers or request
 * structs appear below.
 *
 * Build:
 *   make user-programs
 *   python3 utils/program_upload.py --file user/examples/part_recycle.bin \
 *                                   --name part_recycle
 *   (spawn part_recycle via POST /api/program/spawn with dave's token)
 *
 * The child (cap_recycle_child) is NOT uploaded separately: its binary is
 * embedded below and uploaded by this program at cycle time, so its object
 * is born inside the fresh partition. See cap_recycle_child_blob.h.
 */

#include <sls.h>
#include <aerocap.h>
#include <aerocap_provision.h>
#include "cap_recycle_child_blob.h"

/* The boot check spawns this program with dave's token → owner_uid 1000.
 * Children spawned by it inherit that uid (syscall_dispatch.c's
 * spawn_owner_uid), so assigning 1000 routes them into the fresh partition. */
#define DEMO_UID 1000
#define CYCLES   4

static void put_u32(uint32_t v) {
    char buf[12];
    int i = 11;
    buf[11] = 0;
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    sls_puts(&buf[i]);
}

/* "pcycle0" .. "pcycle3" — unique partition names keep the boot log
 * readable (names are not required to be unique; ids come from slot
 * reuse). */
static void build_name(char* out, const char* base, uint32_t n) {
    int i = 0;
    while (base[i]) { out[i] = base[i]; i++; }
    char digits[8];
    int d = 0;
    uint32_t v = n;
    do { digits[d++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (d > 0) out[i++] = digits[--d];
    out[i] = '\0';
}

static void make_name(char* out, uint32_t cycle) {
    build_name(out, "pcycle", cycle);
}

/* "cr_c0" .. "cr_c3" — the cycle's child PROGRAM object name. Unique per
 * cycle so it never collides with the catalog (destroy vfrees each one). */
static void make_child_name(char* out, uint32_t cycle) {
    const char* base = "cr_c";
    int i = 0;
    while (base[i]) { out[i] = base[i]; i++; }
    out[i] = (char)('0' + cycle);
    out[i + 1] = '\0';
}

int main(void) {
    sls_puts("[prec] partition teardown recycle test starting\n");
    int fail = 0;

    for (uint32_t cycle = 0; cycle < CYCLES; cycle++) {
        /* 1. Fresh partition (destroy reuses the slot). sls_partition_create
         * returns the id directly, or SLS_PARTITION_INVALID_ID on failure. */
        char part_name[32];   /* PARTITION_NAME_LEN */
        make_name(part_name, cycle);
        uint64_t part_id = sls_partition_create(part_name);
        if (part_id == SLS_PARTITION_INVALID_ID) {
            sls_puts("[prec] partition_create FAILED at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            return 1;
        }

        /* 2. Route uid 1000 into it. */
        if (sls_partition_assign(DEMO_UID, (uint32_t)part_id) != 0) {
            sls_puts("[prec] partition_assign FAILED at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            return 1;
        }

        /* 3. Create the child's PROGRAM object IN this partition: fresh
         * name, owner_uid 1000, partition_id 0 → defaults to uid 1000's
         * current partition. */
        char child_name[SLS_NAME_LEN];
        make_child_name(child_name, cycle);
        uint64_t obj_id = 0;
        if (sls_obj_valloc(child_name, SLS_OBJ_PROGRAM, 4 /* pages */,
                           DEMO_UID,
                           SLS_PERM_READ | SLS_PERM_EXECUTE | SLS_PERM_OWNER,
                           0 /* partition: default to owner's */,
                           &obj_id) != 0) {
            sls_puts("[prec] valloc FAILED at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            return 1;
        }

        /* 4. Upload the embedded child binary into that object (single
         * chunk, final). */
        if (sls_upload_binary(child_name, CAP_RECYCLE_CHILD_BLOB,
                              CAP_RECYCLE_CHILD_BLOB_LEN, 0, 1) != 0) {
            sls_puts("[prec] upload FAILED at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            return 1;
        }

        /* 5. Gated async spawn — child uid 1000, in this partition. */
        uint64_t child = sls_program_spawn_nb_held(child_name);
        if (child == 0) {
            sls_puts("[prec] spawn_nb_held FAILED at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            return 1;
        }

        /* 6. Provision the channel while the child is held: our rd=0/wr=1,
         * the child's far end frd=0/fwr=1 in ITS table. */
        cap_t rd = CAP_NONE, wr = CAP_NONE, frd = CAP_NONE, fwr = CAP_NONE;
        if (cap_chan_create((uint32_t)child, &rd, &wr, &frd, &fwr) != 0 ||
            rd == CAP_NONE || wr == CAP_NONE) {
            sls_puts("[prec] chan_create FAILED at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            return 1;
        }

        /* 7. Release + blocking recv in ONE syscall. The child runs,
         * allocates, sends 'Q', exits. */
        uint64_t cookie = 0;
        cap_t mem = CAP_NONE;
        if (cap_recv_release(rd, 1 /* block */, (uint32_t)child,
                             &cookie, &mem) != 0 || mem == CAP_NONE) {
            sls_puts("[prec] recv FAILED at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            return 1;
        }
        if (cookie != 0x51) {
            sls_puts("[prec] cookie mismatch at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            fail = 1;
        }

        /* 8. Normal-path teardown: the transferred MEM cap + our ends. */
        cap_revoke(mem);
        cap_revoke(rd);
        cap_revoke(wr);

        /* 9. Destroy the partition — kills the child if it is still
         * suspended (its exit teardown may already have run), vfrees its
         * catalog objects, reclaims its remaining frames. */
        if (sls_partition_destroy((uint32_t)part_id) != 0) {
            sls_puts("[prec] partition_destroy FAILED at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            fail = 1;
        }

        sls_puts("[prec] cycle ");
        put_u32(cycle);
        sls_puts(" ok\n");
    }

    /* ── Phase 14a (LPAR) destroy-time object story: binary-store probes ──
     *
     * After the 4 real cycles, run PROBE_N spawnless create → assign →
     * valloc → upload → destroy cycles to prove partition teardown frees
     * binary-store slots. The 4 real cycles each uploaded one child
     * binary (cr_c0..cr_c3) into a 16-slot store, and the HTTP upload of
     * THIS program consumed one more slot ("part_recycle"). WITHOUT the
     * destroy-time store free, only 16 - 1 - 4 = 11 slots remain, so
     * probe 12 (0-indexed 11) fails with "binary store full" — the
     * 13th probe is the tooth. WITH the fix, every destroy returns its
     * partition's slots and all 13 probes succeed. */
#define PROBE_N 13
    uint32_t probes_ok = 0;
    for (uint32_t p = 0; p < PROBE_N; p++) {
        char pname[32];
        build_name(pname, "pprobe", p);
        uint64_t pid2 = sls_partition_create(pname);
        int ok = 0;
        if (pid2 != SLS_PARTITION_INVALID_ID &&
            sls_partition_assign(DEMO_UID, (uint32_t)pid2) == 0) {
            char obj_name[SLS_NAME_LEN];
            build_name(obj_name, "pp", p);
            uint64_t oid = 0;
            if (sls_obj_valloc(obj_name, SLS_OBJ_PROGRAM, 4 /* pages */,
                               DEMO_UID,
                               SLS_PERM_READ | SLS_PERM_EXECUTE | SLS_PERM_OWNER,
                               0 /* partition: default to owner's */,
                               &oid) == 0) {
                if (sls_upload_binary(obj_name, CAP_RECYCLE_CHILD_BLOB,
                                      CAP_RECYCLE_CHILD_BLOB_LEN, 0, 1) == 0)
                    ok = 1;
            }
        }
        /* Destroy regardless — the teardown path is exactly what must
         * free the slot. */
        if (pid2 != SLS_PARTITION_INVALID_ID)
            sls_partition_destroy((uint32_t)pid2);
        if (ok) probes_ok++;
    }
    if (probes_ok != PROBE_N) {
        sls_puts("[prec] probe upload FAILED: ");
        put_u32(probes_ok);
        sls_puts("/");
        put_u32(PROBE_N);
        sls_puts(" ok\n");
        fail = 1;
    } else {
        sls_puts("[prec] probe DONE: ");
        put_u32(PROBE_N);
        sls_puts("/");
        put_u32(PROBE_N);
        sls_puts(" uploads ok\n");
    }

    /* ── Phase 14a per-object half: the vfree probe ─────────────────────
     *
     * Proves sys_sls_vfree() also frees the object's binary-store slot:
     * upload the full 400-byte child blob, vfree the object, then re-
     * valloc the same name and re-upload only 96 bytes. WITH the slot
     * free, the re-upload lands in a fresh slot and the kernel logs
     * total=96. WITHOUT it, loader_get_or_alloc() finds the still-active
     * slot by name and only grows sb->size, so the kernel logs total=400
     * — the boot check greps for the 96-byte line, the tooth. The probe
     * object is vfree'd again at the end so no residue remains. */
#define VFREE_PROBE_NAME "vfreeprobe"
#define VFREE_PROBE_REUPLOAD_LEN 96
    {
        uint64_t oid = 0;
        int probe_ok = 0;
        if (sls_obj_valloc(VFREE_PROBE_NAME, SLS_OBJ_PROGRAM, 4 /* pages */,
                           DEMO_UID,
                           SLS_PERM_READ | SLS_PERM_EXECUTE | SLS_PERM_OWNER,
                           0 /* partition: default to owner's */,
                           &oid) == 0 &&
            sls_upload_binary(VFREE_PROBE_NAME, CAP_RECYCLE_CHILD_BLOB,
                              CAP_RECYCLE_CHILD_BLOB_LEN, 0, 1) == 0) {
            /* First upload landed (kernel logs total=400). */
            if (sls_obj_vfree(VFREE_PROBE_NAME) == 0 &&
                sls_obj_valloc(VFREE_PROBE_NAME, SLS_OBJ_PROGRAM,
                               4 /* pages */, DEMO_UID,
                               SLS_PERM_READ | SLS_PERM_EXECUTE | SLS_PERM_OWNER,
                               0 /* partition */, &oid) == 0 &&
                sls_upload_binary(VFREE_PROBE_NAME, CAP_RECYCLE_CHILD_BLOB,
                                  VFREE_PROBE_REUPLOAD_LEN, 0, 1) == 0) {
                /* Re-upload landed; the kernel log must show total=96 for
                 * the slot to have been truly reset. Clean up: vfree the
                 * object again (which frees the fresh slot too). */
                if (sls_obj_vfree(VFREE_PROBE_NAME) == 0) probe_ok = 1;
            }
        }
        if (probe_ok) {
            sls_puts("[prec] vfree probe DONE\n");
        } else {
            sls_puts("[prec] vfree probe FAILED\n");
            fail = 1;
        }
    }

    /* Final cap dump so the boot check can assert the arena is fully
     * returned (objects active=0, arena free=16384/16384) after all the
     * spawn/destroy cycles. */
    cap_list();

    sls_puts(fail ? "[prec] done (FAILURES)\n"
                  : "[prec] DONE: 4/4 cycles passed\n");
    return fail;
}
