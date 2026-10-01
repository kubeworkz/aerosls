#!/usr/bin/env bash
# tests/env_register_pin_check.sh — guards the SEAM between the two sides of
# POSIX-Environments v0.2 P1a: the environment manager's registration (init,
# Rust) and the checkpoint record it becomes (kernel, C).
#
# ─── Why a pin, and why source-only ────────────────────────────────────────
# P1a's record has facts that exist on exactly one side. init owns the three
# frame-pool regions it allocated, the four messenger endpoints it holds and the
# registry names its sidecars were created under; the kernel owns the format
# stamps, the console binding and where it released each sidecar. The two
# halves meet over the wire, in `struct EnvCkptRegister` (kernel/env_ckpt.h) —
# which is BOTH the C struct env_ckpt_register_from() consumes and the exact
# bytes user/proto/src/env_proto.rs writes.
#
# That is a layout shared by two languages with no compiler in common. Nothing
# in either toolchain checks it: C compile-asserts the C struct's size, a Rust
# test asserts the Rust struct's size, and BOTH CAN BE RIGHT WHILE THE FIELDS
# DISAGREE. A `frames` written where a `kind` is read is not a compile error on
# either side — it is a record whose POSIX heap is filed as the ramdisk storage,
# which a restore would act on.
#
# So this guard reads both files and compares them to each other: the opcode,
# the sizes, the counts, the name length, every field offset's formula, and the
# order the two encoders lay the fields down in. Source-only, like
# tests/env_ckpt_check.sh, because all of it is statically visible — which is
# also why it runs in CI's verify job on every push.
#
# ─── The invariants ────────────────────────────────────────────────────────
#   A. The files that carry the two halves exist (else exit 2: the guard could
#      not be evaluated at all).
#   B. The C wire body is still compile-time-checked against the struct: the
#      `_Static_assert(sizeof(struct EnvCkptRegister) == ES...)` in
#      kernel/env_proto.h. Without it the C side's size is a hope.
#   C. The scalars agree ACROSS the two files: the ENV_REGISTER opcode, the
#      request and reply body sizes, the region/channel/task counts, and the
#      name length. These are the numbers a rename on one side silently breaks.
#   D. Every field OFFSET FORMULA agrees after prefix normalization. Comparing
#      formulas (not just values) is what catches a field inserted in the middle
#      on one side: the tail offsets shift, and only the side that moved says so.
#   E. The two encoders lay the fields down in the same ORDER. C's encoder and
#      Rust's are the two places a field can be written to the wrong offset even
#      though the constants agree.
#   F. The record's TASK ENTRIES come from the kernel's own array
#      (`task_entry[i]`), never from the registration — where a sidecar was
#      released is not a fact init holds, and a record that took it from the
#      wire would be recording init's guess.
#   G. init's region mapping is kind-labelled, in the record's order, with the
#      POSIX heap taken from `px_heap` (init's `struct Environment` declares its
#      regions in a different order from the record's, and that reordering is
#      where a swap would live).
#   H. The registration is REACHABLE: env_service_create() calls the register
#      round trip, the round trip parses the reply for its identity (and refuses
#      a reply about a different environment), resolves each named sidecar
#      through the kernel's own registry, and stamps the sequence from the
#      checkpoint manager.
#   I. init ANSWERS it: the manager has an ENV_REGISTER arm, and both of init's
#      dispatch loops size their reply buffer for a full registration (a 64-byte
#      buffer would make the manager decline to answer at all — silently).
#   J. The record layer's entry point that the registration becomes is declared
#      and defined — the seam's far end.
#
# ─── Teeth ────────────────────────────────────────────────────────────────
# tests/env_register_pin_check_smoke.sh mutates a copy of the two halves once
# per invariant and requires this guard to go red, then requires it green on the
# untouched tree.
#
# Optional argument: the repository root to inspect (the smoke's hermetic seam;
# every real caller passes nothing and gets this script's own parent).
#
# Exit: 0 all invariants hold, 1 one failed, 2 precondition missing.
set -u

ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT"

C_HDR="kernel/env_ckpt.h"
C_REC="kernel/env_ckpt.c"
C_PROTO="kernel/env_proto.h"
C_SVC_H="kernel/env_service.h"
C_SVC="kernel/env_service.c"
R_PROTO="user/proto/src/env_proto.rs"
R_MGR="user/init/src/env_manager.rs"
R_DEMO="user/init/src/demo.rs"

fail=0
ok()  { echo "ok:   $*"; }
bad() { echo "FAIL: $*"; fail=1; }

# ── helpers ───────────────────────────────────────────────────────────────

# C and Rust comments stripped: a definition's trailing prose is not part of
# the definition, and comparing it would make this guard fail on rewording.
strip_comments() { sed -E 's@/\*[^*]*\*/@@g; s@//.*$@@'; }

# A C `#define`'s right-hand side, with `\` continuations joined (several of
# the offsets are wrapped across two lines) and its comments removed.
c_expr() {  # <file> <macro>
    awk -v name="$2" '
        index($0, "#define " name " ") == 1 { grab = 1 }
        grab {
            line = $0
            sub(/\\[[:space:]]*$/, "", line)
            printf "%s", line
            if ($0 !~ /\\[[:space:]]*$/) { print ""; exit }
        }
    ' "$1" | sed -E "s/^#define[[:space:]]+$2[[:space:]]*//" | strip_comments
}

# A Rust `pub const NAME: usize = ...;`, `;`-terminated possibly across lines.
r_expr() {  # <file> <name>
    awk -v name="$2" '
        index($0, "pub const " name ":") == 1 || index($0, "pub const " name " ") == 1 { grab = 1 }
        grab {
            printf "%s", $0
            if (index($0, ";") > 0) { print ""; exit }
        }
    ' "$1" | sed -E "s/^pub const $2:[^=]*=[[:space:]]*//; s/;[[:space:]]*$//" | strip_comments
}

# Prefix-normalized, whitespace-free, unsuffixed form of an offset formula, so
# the C and Rust spellings of the same arithmetic become the same string.
c_canon() {
    strip_comments | sed -E \
        -e 's/ENV_CKPT_MAX_REGIONS/MAX_REGIONS/g' \
        -e 's/ENV_CKPT_MAX_CHANS/MAX_CHANS/g' \
        -e 's/ENV_CKPT_MAX_TASKS/MAX_TASKS/g' \
        -e 's/ENV_CKPT_NAME_LEN/NAME_LEN/g' \
        -e 's/ENV_REG_TASK_NAME_STRIDE/NAME_LEN/g' \
        -e 's/ENV_REG_OFF_/OFF_/g' \
        -e 's/ENV_REG_REGION_STRIDE/REGION_STRIDE/g' \
        -e 's/([0-9]+)[uU][lL]*/\1/g' \
        -e 's/[[:space:]]//g' \
        -e 's/^\((.*)\)$/\1/'
}
r_canon() {
    strip_comments | sed -E \
        -e 's/REGISTER_MAX_REGIONS/MAX_REGIONS/g' \
        -e 's/REGISTER_MAX_CHANS/MAX_CHANS/g' \
        -e 's/REGISTER_MAX_TASKS/MAX_TASKS/g' \
        -e 's/REGISTER_NAME_LEN/NAME_LEN/g' \
        -e 's/REG_OFF_/OFF_/g' \
        -e 's/REG_REGION_STRIDE/REGION_STRIDE/g' \
        -e 's/[[:space:]]//g'
}

# The leading number of a definition: `8u` -> `8`, `3u` -> `3`.
numonly() { strip_comments | sed -E 's/[^0-9].*$//'; }

cval() { c_expr "$C_HDR" "$1" | numonly; }
cpval() { c_expr "$C_PROTO" "$1" | numonly; }
rval() { r_expr "$R_PROTO" "$1" | numonly; }

# ── A. Preconditions ──────────────────────────────────────────────────────
for f in "$C_HDR" "$C_REC" "$C_PROTO" "$C_SVC_H" "$C_SVC" "$R_PROTO" "$R_MGR" "$R_DEMO"; do
    if [ ! -f "$f" ]; then
        echo "ABORT: $f is missing — cannot evaluate the P1a registration seam" >&2
        exit 2
    fi
done
ok "A. both halves of the registration are present ($C_HDR + $R_PROTO, and the paths that drive them)"

# ── B. The C body is a compile-time-checked image of the struct ───────────
if grep -qF "sizeof(struct EnvCkptRegister) == ENV_REGISTER_REPLY_BODY_SIZE" "$C_PROTO"; then
    ok "B. env_proto.h compile-asserts the wire body size against sizeof(struct EnvCkptRegister)"
else
    bad "B. the _Static_assert tying ENV_REGISTER_REPLY_BODY_SIZE to sizeof(struct EnvCkptRegister) is gone — the C side's body length is then a hope, and a field added to the struct silently overruns the encoder's buffer"
fi

# ── C. The scalars agree across the two files ─────────────────────────────
scalar_bad=""
agree() {  # <label> <c-value> <rust-value>
    if [ "$2" != "$3" ]; then
        scalar_bad="$scalar_bad $1(C=$2,Rust=$3)"
    fi
}
agree "opcode" "$(cpval ENV_REGISTER)" "$(rval ENV_REGISTER)"
agree "request-body" "$(cpval ENV_REGISTER_BODY_SIZE)" "$(rval REGISTER_BODY_SIZE)"
agree "max-regions" "$(cval ENV_CKPT_MAX_REGIONS)" "$(rval REGISTER_MAX_REGIONS)"
agree "max-chans" "$(cval ENV_CKPT_MAX_CHANS)" "$(rval REGISTER_MAX_CHANS)"
agree "max-tasks" "$(cval ENV_CKPT_MAX_TASKS)" "$(rval REGISTER_MAX_TASKS)"
agree "name-len" "$(cval ENV_CKPT_NAME_LEN)" "$(rval REGISTER_NAME_LEN)"
agree "region-stride" "$(cpval ENV_REG_REGION_STRIDE)" "$(rval REG_REGION_STRIDE)"
if [ -n "$scalar_bad" ]; then
    bad "C. the two halves disagree on:$scalar_bad — a registration built with one side's number and read with the other's is a record naming the wrong bytes"
else
    ok "C. opcode, sizes, counts, name length and region stride all agree across $C_PROTO and $R_PROTO"
fi

# ── D. Every offset formula agrees ────────────────────────────────────────
OFFSETS="OFF_PARTITION OFF_INDEX OFF_ENV_ID OFF_N_REGIONS OFF_REGIONS OFF_N_CHANS OFF_CHANS OFF_N_TASKS OFF_TASK_NAMES OFF_TASK_KINDS"
off_bad=""
for o in $OFFSETS; do
    cform=$(c_expr "$C_PROTO" "ENV_REG_$o" | c_canon)
    rform=$(r_expr "$R_PROTO" "REG_$o" | r_canon)
    if [ -z "$cform" ] || [ -z "$rform" ]; then
        off_bad="$off_bad $o(missing)"
    elif [ "$cform" != "$rform" ]; then
        off_bad="$off_bad $o(C='$cform' Rust='$rform')"
    fi
done
if [ -n "$off_bad" ]; then
    bad "D. these field offsets are computed differently on the two sides:$off_bad — the two sides would write and read DIFFERENT bytes for the same field"
else
    ok "D. all $(( $(echo $OFFSETS | wc -w) )) field offsets carry the same formula on both sides (a field inserted mid-body shifts the tail, and the tail says so)"
fi

# The body size formula must be the same arithmetic on both sides too — the
# belt to C's compile-time braces. A size that agreed by coincidence while the
# last field moved would be invisible to a value-only check.
size_c=$(c_expr "$C_PROTO" ENV_REGISTER_REPLY_BODY_SIZE | c_canon)
size_r=$(r_expr "$R_PROTO" REGISTER_REPLY_BODY_SIZE | r_canon)
if [ -n "$size_c" ] && [ "$size_c" = "$size_r" ]; then
    ok "D. the reply body size is the same arithmetic on both sides ($size_c)"
else
    bad "D. the reply body size is computed differently: C='$size_c' Rust='$size_r'"
fi

# ── E. The two encoders lay the fields down in the same order ─────────────
c_order=$(sed -n '/env_register_reply_encode/,/^}/p' "$C_PROTO" \
          | grep -oE 'ENV_REG_OFF_[A-Z_]+' | sed 's/ENV_REG_OFF_/OFF_/' | tr '\n' ' ')
r_order=$(sed -n '/pub fn encode(&self) -> \[u8; REGISTER_REPLY_BODY_SIZE\]/,/^    }/p' "$R_PROTO" \
          | grep -oE 'REG_OFF_[A-Z_]+' | sed 's/REG_OFF_/OFF_/' | tr '\n' ' ')
if [ -z "$c_order" ] || [ -z "$r_order" ]; then
    bad "E. could not find the field writes in one of the encoders — the guard's own anchors have moved"
elif [ "$c_order" != "$r_order" ]; then
    bad "E. the encoders write the fields in different orders (C: $c_order | Rust: $r_order) — every field past the first difference lands at the other side's offset"
else
    ok "E. both encoders write the fields in the same order ($(echo $c_order | wc -w) offsets)"
fi

# ── F. Task entries come from the KERNEL, never from the registration ─────
if grep -qF "rec.tasks[i].entry = task_entry ? task_entry[i] : 0;" "$C_REC"; then
    ok "F. env_ckpt_register_from() takes each task's entry from the kernel's own array"
else
    bad "F. the task entry no longer comes from task_entry[] — a record that took its entry from the registration would be recording where INIT guessed the sidecar runs, which is not a fact init holds"
fi

# ── G. init's region mapping is kind-labelled and in the record's order ────
map_bad=""
has_line() { grep -qF -- "$1" "$2"; }
# The mapping lines are matched as WHOLE LINES, leading whitespace included, so
# the module's own tests -- which write `RegisterRegion { base: env.px_heap,
# frames: POSIX_HEAP_FRAMES as u32, kind: REGION_POSIX_HEAP }` on one line --
# cannot satisfy a clause about the mapping under test.
grep_mapping() { grep -qxF -- "$1" "$R_MGR"; }
# The three kinds, each attached to the right base: POSIX heap <- px_heap (NOT
# the struct's field order), ramdisk heap <- rd_heap, storage <- rd_storage.
grep_mapping '        base: env.px_heap,' && grep_mapping '        kind: REGION_POSIX_HEAP,' \
    || map_bad="$map_bad posix-heap-base"
grep_mapping '        base: env.rd_heap,' && grep_mapping '        kind: REGION_RD_HEAP,' \
    || map_bad="$map_bad rd-heap-base"
grep_mapping '        base: env.rd_storage,' && grep_mapping '        kind: REGION_RD_STORAGE,' \
    || map_bad="$map_bad rd-storage-base"
# ...and the frames each base is reported with.
grep_mapping '        frames: POSIX_HEAP_FRAMES as u32,' || map_bad="$map_bad posix-frames"
grep_mapping '        frames: RD_HEAP_FRAMES as u32,' || map_bad="$map_bad rd-heap-frames"
grep_mapping '        frames: RD_STORAGE_FRAMES as u32,' || map_bad="$map_bad rd-storage-frames"
# init's four messenger ends and the two sidecar names it registered.
grep -qF 'r.chans = [env.ramdisk_r, env.ramdisk_w, env.posix_r, env.posix_w];' "$R_MGR" \
    || map_bad="$map_bad chans"
grep -qF 'r.set_task(0, rd.as_str(), TASK_RAMDISK_SIDECAR);' "$R_MGR" || map_bad="$map_bad rd-task"
grep -qF 'r.set_task(1, px.as_str(), TASK_POSIX_SIDECAR);' "$R_MGR" || map_bad="$map_bad px-task"
if [ -n "$map_bad" ]; then
    bad "G. init's registration is missing or mismapping:$map_bad — every one of these is a fact only init has, and a registration that drops one leaves the record describing some other environment"
else
    ok "G. init reports its three kind-labelled regions (POSIX heap from px_heap, the record's order, not the struct's), its four channels and both sidecar names"
fi

# ── H. The kernel's create path reaches the registration ──────────────────
reach_bad=""
has_line "int env_service_register_env(uint32_t partition, uint32_t index," "$C_SVC_H" \
    || reach_bad="$reach_bad not-declared"
has_line "int env_service_register_env(uint32_t partition, uint32_t index," "$C_SVC" \
    || reach_bad="$reach_bad not-defined"
grep -qF "(void)env_service_register_env(partition, index, *out_env_id, console_id);" "$C_SVC" \
    || reach_bad="$reach_bad create-does-not-register"
grep -qF "env_register_body_encode(body, env_id, partition);" "$C_SVC" \
    || reach_bad="$reach_bad no-request-body"
grep -qF "env_register_reply_parse(reply + ENV_FRAME_SIZE," "$C_SVC" \
    || reach_bad="$reach_bad no-reply-parse"
grep -qF "sidecar_registry_resolve(name, partition)" "$C_SVC" \
    || reach_bad="$reach_bad no-sidecar-resolve"
grep -qF "return proc_table[i].user_rip;" "$C_SVC" \
    || reach_bad="$reach_bad no-user-rip"
grep -qF "checkpoint_last_sequence()" "$C_SVC" \
    || reach_bad="$reach_bad no-sequence"
# The reply must be about the environment the request named, on all three
# fields the kernel can check it with.
grep -qF "if (reg.partition_id != partition || reg.env_id != env_id ||" "$C_SVC" \
    || reach_bad="$reach_bad no-identity-check"
grep -qF "reg.index != index) {" "$C_SVC" \
    || reach_bad="$reach_bad no-index-check"
grep -qF "env_ckpt_register_from(&reg, entries, console_id," "$C_SVC" \
    || reach_bad="$reach_bad no-record-call"
if [ -n "$reach_bad" ]; then
    bad "H. the create path does not reach the registration:$reach_bad — an environment created without one is live, reachable, and invisible to a checkpoint"
else
    ok "H. env_service_create() registers every environment it creates: round trip, identity-checked reply, kernel-resolved sidecar entries, checkpoint-stamped record"
fi

# ── I. init answers ENV_REGISTER, with room for the body ──────────────────
init_bad=""
grep -qF "ENV_REGISTER => {" "$R_MGR" \
    || init_bad="$init_bad no-arm"
grep -qF "pub fn environment_register_reply(env_id: u32, env: &Environment) -> RegisterReply {" "$R_MGR" \
    || init_bad="$init_bad no-builder"
grep -qF "let reg = environment_register_reply(env_id, &self.envs[pos].1);" "$R_MGR" \
    || init_bad="$init_bad arm-does-not-build"
# Both of init's dispatch loops (the supervisor loop and the unified boot's
# heartbeat loop) must be able to carry a full registration back.
n_reply_buf=$(grep -cF 'let mut reply = [0u8; aerosls_proto::env_proto::REPLY_MAX];' "$R_DEMO")
if [ "$n_reply_buf" -lt 2 ]; then
    init_bad="$init_bad reply-buffer($n_reply_buf of 2)"
fi
if [ -n "$init_bad" ]; then
    bad "I. init does not answer the registration properly:$init_bad — a manager that cannot send the body back leaves every environment unregistered, and a buffer smaller than the body makes it send NOTHING rather than something truncated"
else
    ok "I. init's manager answers ENV_REGISTER, and both dispatch loops size the reply for a full registration ($n_reply_buf of 2)"
fi

# ── J. The record layer's entry point is the one H calls ─────────────────
if grep -qF "int env_ckpt_register_from(const struct EnvCkptRegister* reg," "$C_HDR" &&
   grep -qF "int env_ckpt_register_from(const struct EnvCkptRegister* reg," "$C_REC"; then
    ok "J. env_ckpt_register_from() is declared in env_ckpt.h and defined in env_ckpt.c — the one entry point the create path calls, linked into the kernel by the Makefile's X86_C_SRC"
else
    bad "J. env_ckpt_register_from() is not both declared in env_ckpt.h and defined in env_ckpt.c"
fi

if [ "$fail" -ne 0 ]; then
    echo
    echo "env_register_pin_check: the two sides of the P1a registration are not in step."
    echo "  C:    $C_HDR / $C_PROTO / $C_SVC"
    echo "  Rust: $R_PROTO / $R_MGR"
    exit 1
fi
echo
echo "env_register_pin_check: the environment manager's registration and the kernel's record agree, field for field."
exit 0
