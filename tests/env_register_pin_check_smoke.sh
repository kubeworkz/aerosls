#!/usr/bin/env bash
# tests/env_register_pin_check_smoke.sh — the TEETH for
# tests/env_register_pin_check.sh.
#
# ─── What a teeth check is for, and why this one is source-only ────────────
# A guard that has never been seen to fail is a guard nobody can trust, and the
# pin guard's subject is a layout no compiler checks: the kernel's
# `struct EnvCkptRegister` and the bytes user/proto/src/env_proto.rs writes for
# it. So every tooth below breaks ONE way the two sides can drift in a throwaway
# copy of the tree and requires the guard to (a) exit 1 and (b) name THAT
# invariant. Requiring the right named clause is the half that matters: a guard
# that reddens on everything, or on the wrong clause, would pass a tooth that
# only checked the exit code.
#
# There are two kinds of tooth here, and the second is the harder one:
#
#   * the numbers drift (an opcode, a count, a body size, a field offset) —
#     the failure a reader expects; and
#   * the arithmetic AGREES while the fields move: swapping two write lines in
#     one encoder (tooth E) leaves every constant correct and every offset
#     formula identical, and produces a body whose index and env_id are
#     transposed. That is the drift this guard exists for, and only the
#     encoder-order clause catches it.
#
# The last tooth is the vacuity control — the guard must be GREEN on the real,
# unmutated tree. Without it, a guard that always failed would pass every other
# tooth in this file.
#
# The hermetic seam is the guard's own optional root argument: the guard
# inspects a repository root, defaulting to its own parent. So this smoke builds
# a minimal root containing exactly the files the guard reads, mutates one line,
# and runs the real guard against it — no network, no build, no boot. It is
# source-only by construction (bash/sed/cp/python3 only), so its teeth are
# proven on CI's verify job on every push.
#
# Exit: 0 every tooth bit, 1 a tooth did not.
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
GUARD="$ROOT/tests/env_register_pin_check.sh"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found — run from the repo root." >&2; exit 2; }

W="$(mktemp -d)"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# The files the guard reads. Kept in one list so a tooth can never pass because
# the copy it mutated was missing something the guard needed.
SEED_FILES=(
    kernel/env_ckpt.h
    kernel/env_ckpt.c
    kernel/env_proto.h
    kernel/env_service.h
    kernel/env_service.c
    user/proto/src/env_proto.rs
    user/init/src/env_manager.rs
    user/init/src/demo.rs
)

seed() {   # seed <dir>
    rm -rf "$1"
    mkdir -p "$1/kernel" "$1/user/proto/src" "$1/user/init/src"
    local f
    for f in "${SEED_FILES[@]}"; do
        cp "$ROOT/$f" "$1/$f" || return 1
    done
    return 0
}

passed=0
failed=0
tooth() {   # tooth <name> <expected-clause> <dir>; the mutation has already run
    local name="$1" clause="$2" dir="$3" out rc
    out="$(bash "$GUARD" "$dir" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: ${clause}\."; then
        echo "ok:   $name bites (guard exits 1 naming '$clause')"
        passed=$((passed + 1))
    else
        echo "FAIL: $name did NOT bite — exit $rc, expected 1 with 'FAIL: $clause.'"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

echo "env_register_pin_check_smoke — teeth for the C/Rust registration pin"
echo "===================================================================="
echo

# ── A. A missing half is a refusal to evaluate, not a pass or a failure ────
seed "$W/a"; rm -f "$W/a/user/proto/src/env_proto.rs"
out="$(bash "$GUARD" "$W/a" 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s\n' "$out" | grep -q "^ABORT:"; then
    echo "ok:   A. a missing env_proto.rs aborts (exit 2) instead of reporting the two sides in step"
    passed=$((passed + 1))
else
    echo "FAIL: A. a missing file did not abort (exit $rc)"; echo "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── B. The C body stops being compile-time-checked against the struct ──────
seed "$W/b"
sed -i '/_Static_assert(sizeof(struct EnvCkptRegister)/,+1d' "$W/b/kernel/env_proto.h"
tooth "B. the struct-size _Static_assert removed" "B" "$W/b"

# ── C. The scalars drift between the two files ────────────────────────────
seed "$W/c1"
sed -i 's/^#define ENV_REGISTER 3 /#define ENV_REGISTER 4 /' "$W/c1/kernel/env_proto.h"
tooth "C. the C side's ENV_REGISTER opcode moved (3 -> 4)" "C" "$W/c1"

seed "$W/c2"
sed -i 's/^pub const ENV_REGISTER: u16 = 3;$/pub const ENV_REGISTER: u16 = 4;/' "$W/c2/user/proto/src/env_proto.rs"
tooth "C. the Rust side's ENV_REGISTER opcode moved (3 -> 4)" "C" "$W/c2"

seed "$W/c3"
sed -i 's/^pub const REGISTER_MAX_CHANS: usize = 4;$/pub const REGISTER_MAX_CHANS: usize = 5;/' "$W/c3/user/proto/src/env_proto.rs"
tooth "C. the Rust side carries five channels where the record holds four" "C" "$W/c3"

seed "$W/c4"
sed -i 's/^#define ENV_CKPT_NAME_LEN     24u$/#define ENV_CKPT_NAME_LEN     32u/' "$W/c4/kernel/env_ckpt.h"
tooth "C. the record's task-name field widened without the wire body following" "C" "$W/c4"

seed "$W/c5"
sed -i 's/^#define ENV_REGISTER_BODY_SIZE 8u$/#define ENV_REGISTER_BODY_SIZE 12u/' "$W/c5/kernel/env_proto.h"
tooth "C. the request body size changed on the C side only" "C" "$W/c5"

# ── D. An offset FORMULA moves even though nothing else does ───────────────
seed "$W/d1"
sed -i 's/^#define ENV_REG_OFF_CHANS      (ENV_REG_OFF_N_CHANS + 4u)$/#define ENV_REG_OFF_CHANS      (ENV_REG_OFF_N_CHANS)/' "$W/d1/kernel/env_proto.h"
tooth "D. the C channel offset drops its own field width" "D" "$W/d1"

seed "$W/d2"
sed -i 's/^pub const REG_OFF_TASK_NAMES: usize = REG_OFF_N_TASKS + 4;$/pub const REG_OFF_TASK_NAMES: usize = REG_OFF_N_TASKS + 8;/' "$W/d2/user/proto/src/env_proto.rs"
tooth "D. the Rust task-name block starts two fields late" "D" "$W/d2"

seed "$W/d3"
python3 - "$W/d3/user/proto/src/env_proto.rs" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("pub const REGISTER_REPLY_BODY_SIZE: usize = REG_OFF_TASK_KINDS + REGISTER_MAX_TASKS * 4;",
              "pub const REGISTER_REPLY_BODY_SIZE: usize = REG_OFF_TASK_KINDS + REGISTER_MAX_TASKS * 8;")
open(p, "w").write(s)
PY
tooth "D. the Rust body-size formula counts the kind array twice over" "D" "$W/d3"

# ── E. The constants all agree and the FIELDS still move ──────────────────
# The hard tooth: every offset formula and every scalar is unchanged; only the
# order the fields are written in differs, so a body is produced whose index
# and env_id are transposed.
seed "$W/e1"
python3 - "$W/e1/user/proto/src/env_proto.rs" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
a = next(i for i, l in enumerate(lines) if "put_u32(&mut b, REG_OFF_INDEX," in l)
b = next(i for i, l in enumerate(lines) if "put_u32(&mut b, REG_OFF_ENV_ID," in l)
lines[a], lines[b] = lines[b], lines[a]
open(p, "w").write("\n".join(lines))
PY
tooth "E. the Rust encoder transposes index and env_id" "E" "$W/e1"

seed "$W/e2"
python3 - "$W/e2/kernel/env_proto.h" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().split("\n")
a = next(i for i, l in enumerate(lines) if "ENV_REG_OFF_INDEX,     r->index);" in l)
b = next(i for i, l in enumerate(lines) if "ENV_REG_OFF_ENV_ID,    r->env_id);" in l)
lines[a], lines[b] = lines[b], lines[a]
open(p, "w").write("\n".join(lines))
PY
tooth "E. the C encoder transposes index and env_id" "E" "$W/e2"

# ── F. The task entry stops coming from the kernel ────────────────────────
seed "$W/f"
sed -i 's/^        rec.tasks\[i\].entry = task_entry ? task_entry\[i\] : 0;$/        rec.tasks[i].entry = reg->task_kind[i];/' "$W/f/kernel/env_ckpt.c"
tooth "F. the record takes its task entries from the registration" "F" "$W/f"

seed "$W/j"
sed -i 's/^int env_ckpt_register_from(const struct EnvCkptRegister\* reg,$/static int env_ckpt_register_from_unused(const struct EnvCkptRegister* reg,/' "$W/j/kernel/env_ckpt.c"
tooth "J. the record layer's entry point is renamed out from under its caller" "J" "$W/j"

# ── G. init's region mapping loses its kinds ───────────────────────────────
seed "$W/g1"
sed -i '0,/^        kind: REGION_POSIX_HEAP,$/s//        kind: REGION_RD_HEAP,/' "$W/g1/user/init/src/env_manager.rs"
tooth "G. init reports its POSIX heap under the ramdisk heap's kind" "G" "$W/g1"

seed "$W/g2"
sed -i 's/^        base: env\.px_heap,$/        base: env.rd_heap,/' "$W/g2/user/init/src/env_manager.rs"
tooth "G. init reports the ramdisk heap's base as the POSIX heap" "G" "$W/g2"

seed "$W/g3"
sed -i 's/^    r.chans = \[env.ramdisk_r, env.ramdisk_w, env.posix_r, env.posix_w\];$/    r.chans = [env.posix_r, env.posix_w, env.ramdisk_r, env.ramdisk_w];/' "$W/g3/user/init/src/env_manager.rs"
tooth "G. init swaps the two sidecars' messenger ends" "G" "$W/g3"

# ── H. The kernel's create path stops reaching the registration ────────────
seed "$W/h1"
sed -i '/^        (void)env_service_register_env(partition, index, \*out_env_id, console_id);$/d' "$W/h1/kernel/env_service.c"
tooth "H. the create path no longer registers anything" "H" "$W/h1"

seed "$W/h2"
sed -i 's/^    if (reg.partition_id != partition || reg.env_id != env_id ||$/    if (reg.partition_id != partition || reg.env_id != env_id) {/' "$W/h2/kernel/env_service.c"
tooth "H. the reply's index is no longer checked against the one created" "H" "$W/h2"

seed "$W/h3"
sed -i 's/^            return proc_table\[i\].user_rip;$/            return 0;/' "$W/h3/kernel/env_service.c"
tooth "H. the sidecar entry stops being read from the process table" "H" "$W/h3"

seed "$W/h4"
sed -i 's/                                    checkpoint_last_sequence());/                                    0);/' "$W/h4/kernel/env_service.c"
tooth "H. the record stops being stamped with the checkpoint epoch" "H" "$W/h4"

# ── I. init stops answering, or answers into too small a buffer ────────────
seed "$W/i1"
sed -i '/^            ENV_REGISTER => {$/,/^            }$/d' "$W/i1/user/init/src/env_manager.rs"
tooth "I. the manager's ENV_REGISTER arm removed" "I" "$W/i1"

seed "$W/i2"
python3 - "$W/i2/user/init/src/demo.rs" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("let mut reply = [0u8; aerosls_proto::env_proto::REPLY_MAX];",
              "let mut reply = [0u8; 64];", 1)
open(p, "w").write(s)
PY
tooth "I. one dispatch loop's reply buffer shrank below a registration" "I" "$W/i2"

# ── The vacuity control ───────────────────────────────────────────────────
# The guard must be GREEN on the untouched tree. Without this arm, a guard that
# failed unconditionally would pass every tooth above.
out="$(bash "$GUARD" "$ROOT" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   control. the untouched tree passes ($(printf '%s\n' "$out" | grep -c '^ok:' ) clauses green)"
    passed=$((passed + 1))
else
    echo "FAIL: control. the untouched tree does NOT pass — the guard is not measuring what it claims"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

echo
echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
