;;; calc_guest.wat — the Wasm-sidecar guest module.
;;;
;;; This module is the "function written in WebAssembly" in the polyglot call
;;; chain: it is the CALLER of the CalculatorService running in the Lisp
;;; sidecar. It orchestrates two calls entirely from inside wasm:
;;;
;;;   1. add(2, 3) -> 5                    (inline args, no arena data)
;;;   2. latency benchmark: 1000 add() round trips into Lisp (the host
;;;      times each call with rdtsc inside the call_add import and reports
;;;      median/p99 after run)
;;;   3. latency benchmark: 100 sqrt_batch calls, each moving a 4096-f64
;;;      array through the shared arena by MEM cap (the host times each
;;;      call_sqrt_batch import and reports a second median/p99 line)
;;;   4. sqrt_batch over N=4096 f64s       (arena array handed by capability)
;;;
;;; The array itself never crosses the wire: the guest writes it into the
;;; shared arena through host_* imports (which address the shared mapping
;;; directly) and hands the callee a MEM cap. Verification reads the output
;;; array straight back from the shared arena — zero serialization.
;;;
;;; Host imports (implemented by the Rust sidecar host):
;;;   host.call_add(i32 a, i32 b) -> i64   ; hi=status, lo=result
;;;   host.call_sqrt_batch(i32 count, i32 input_cap) -> i64 ; hi=status, lo=out_cap
;;;   host.call_reverse(i32 cap, i32 len) -> i64 ; hi=status, lo=out_cap (string path)
;;;   host.write_f64(i32 cap, i32 idx, i64 bits)
;;;   host.read_f64(i32 cap, i32 idx) -> i64
;;;   host.write_u8(i32 cap, i32 idx, i32 byte)
;;;   host.read_u8(i32 cap, i32 idx) -> i32
;;;   host.arena_alloc(i32 size) -> i32 cap
;;;   host.arena_free(i32 cap)
;;;   host.log(i32 ptr, i32 len)

(module
  (import "host" "call_add" (func $call_add (param i32 i32) (result i64)))
  (import "host" "call_sqrt_batch" (func $call_sqrt_batch (param i32 i32) (result i64)))
  (import "host" "call_reverse" (func $call_reverse (param i32 i32) (result i64)))
  (import "host" "call_heavy_reduce" (func $call_heavy_reduce (param i32 i32) (result i64)))
  (import "host" "await_async_result" (func $await_async_result (result i64)))
  (import "host" "is_shm" (func $is_shm (result i32)))
  (import "host" "write_f64" (func $write_f64 (param i32 i32 i64)))
  (import "host" "read_f64" (func $read_f64 (param i32 i32) (result i64)))
  (import "host" "write_u8" (func $write_u8 (param i32 i32 i32)))
  (import "host" "read_u8" (func $read_u8 (param i32 i32) (result i32)))
  (import "host" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "host" "arena_free" (func $arena_free (param i32)))
  (import "host" "log" (func $log (param i32 i32)))

  (memory (export "memory") 1)

  (data (i32.const 64) "PASS add(2,3)=5 sqrt verified\00")
  (data (i32.const 160) "FAIL add\00")
  (data (i32.const 192) "FAIL sqrt\00")
  (data (i32.const 224) "FAIL sqrt-bench\00")
  (data (i32.const 240) "FAIL add-bench\00")
  (data (i32.const 256) "FAIL str-bench\00")
  (data (i32.const 272) "FAIL async\00")
  (data (i32.const 288) "PASS async heavy_reduce=2016\00")

  (func (export "run") (result i32)
    (local $r i64) (local $r2 i64)
    (local $status i32) (local $n i32) (local $in_cap i32) (local $i i32)
    (local $out_cap i32) (local $v f64) (local $err f64)
    (local $j i32)

    ;; ── add(2, 3) must return 5 ───────────────────────────────────────────
    (local.set $r (call $call_add (i32.const 2) (i32.const 3)))
    (local.set $status
      (i32.wrap_i64 (i64.shr_u (local.get $r) (i64.const 32))))
    (if (i32.ne (local.get $status) (i32.const 0))
      (then (call $log (i32.const 160) (i32.const 8)) (return (i32.const 1))))
    (local.set $status
      (i32.wrap_i64 (i64.and (local.get $r) (i64.const 0xffffffff))))
    (if (i32.ne (local.get $status) (i32.const 5))
      (then (call $log (i32.const 160) (i32.const 8)) (return (i32.const 2))))

    ;; ── latency benchmark: 1000 add() round trips into Lisp ──────────────
    ;; each call crosses wasm -> host import -> rings/TCP -> kerneld -> Lisp
    ;; -> back; the host import times every call with rdtsc and the sidecar
    ;; reports median/p99 after run() returns.
    (local.set $i (i32.const 0))
    (block $bench_done
      (loop $bench
        (br_if $bench_done (i32.ge_u (local.get $i) (i32.const 1000)))

        ;; ── add bench-workload guard ─────────────────────────────────────
        ;; Vary the operands per iteration and verify the reply: the check
        ;; expects add(i, i) == 2i, distinct for every i, so a cached or
        ;; echoed reply (or a bench that regressed to constant inputs)
        ;; fails on the first mismatching iteration. Proves each timed call
        ;; is genuinely recomputed by Lisp. Runs OUTSIDE the timed host
        ;; import, so the measured latency is unaffected.
        (local.set $r (call $call_add (local.get $i) (local.get $i)))
        (local.set $status
          (i32.wrap_i64 (i64.shr_u (local.get $r) (i64.const 32))))
        (if (i32.ne (local.get $status) (i32.const 0))
          (then (call $log (i32.const 240) (i32.const 14)) (return (i32.const 8))))
        (local.set $status
          (i32.wrap_i64 (i64.and (local.get $r) (i64.const 0xffffffff))))
        (if (i32.ne (local.get $status) (i32.add (local.get $i) (local.get $i)))
          (then (call $log (i32.const 240) (i32.const 14)) (return (i32.const 8))))

        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $bench)))

    ;; ── sqrt_batch latency bench: 100 calls, 4096-f64 arena array each ──
    ;; fresh input buffer per iteration (the host times call_sqrt_batch),
    ;; results are owned caps we release immediately; correctness is covered
    ;; by the single verified call below. NB: $n must be set BEFORE the loop —
    ;; it was previously 0 here (locals are zero-initialized), so the bench
    ;; silently measured count=0 calls; the arena-ring change exposed it.
    (local.set $i (i32.const 0))
    (local.set $n (i32.const 4096))
    (block $sqrt_bench_done
      (loop $sqrt_bench
        (br_if $sqrt_bench_done (i32.ge_u (local.get $i) (i32.const 100)))
        (local.set $in_cap (call $arena_alloc (i32.mul (local.get $n) (i32.const 8))))
        (local.set $j (i32.const 0))
        (block $sqrt_fill_done
          (loop $sqrt_fill
            (br_if $sqrt_fill_done (i32.ge_u (local.get $j) (local.get $n)))
            (call $write_f64
              (local.get $in_cap)
              (local.get $j)
              (i64.reinterpret_f64 (f64.convert_i32_u (local.get $j))))
            (local.set $j (i32.add (local.get $j) (i32.const 1)))
            (br $sqrt_fill)))
        (local.set $r2 (call $call_sqrt_batch (local.get $n) (local.get $in_cap)))
        (local.set $out_cap
          (i32.wrap_i64 (i64.and (local.get $r2) (i64.const 0xffffffff))))

        ;; ── bench-workload guard ──────────────────────────────────────────
        ;; The timed call must have done REAL 4096-element work. Sample two
        ;; outputs (mid + last): if count <= index the element was never
        ;; written (fresh arena pages are zeroed) and sqrt(i) != 0, so the
        ;; |v*v - i| < 1e-6 check trips. This catches a silent count=0
        ;; regression in the bench (the $n-before-loop bug) while the full
        ;; 4096-element verification below stays the exhaustive check.
        ;; These reads are OUTSIDE the timed host import, so they never
        ;; inflate the measured round trip.
        (local.set $j (i32.const 2048))
        (local.set $v (f64.reinterpret_i64 (call $read_f64 (local.get $out_cap) (local.get $j))))
        (local.set $err
          (f64.sub (f64.mul (local.get $v) (local.get $v))
                   (f64.convert_i32_u (local.get $j))))
        (if (f64.gt (f64.abs (local.get $err)) (f64.const 1e-6))
          (then (call $log (i32.const 224) (i32.const 15)) (return (i32.const 7))))
        (local.set $j (i32.const 4095))
        (local.set $v (f64.reinterpret_i64 (call $read_f64 (local.get $out_cap) (local.get $j))))
        (local.set $err
          (f64.sub (f64.mul (local.get $v) (local.get $v))
                   (f64.convert_i32_u (local.get $j))))
        (if (f64.gt (f64.abs (local.get $err)) (f64.const 1e-6))
          (then (call $log (i32.const 224) (i32.const 15)) (return (i32.const 7))))

        (call $arena_free (local.get $out_cap))
        (call $arena_free (local.get $in_cap))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $sqrt_bench)))

    ;; ── reverse bench: 100 calls, variable-length string (32..63 B) ─────
    ;; The string/bytes IDL path: a bytes MEM cap in, an arena MEM cap out.
    ;; Byte j of the sent string is (j*7 + i*11) & 0xff; the Lisp side must
    ;; return the REVERSED bytes, verified at 3 sampled offsets (outside the
    ;; timed import). Varying length + content defeats input-keyed caching.
    (local.set $i (i32.const 0))
    (block $str_bench_done
      (loop $str_bench
        (br_if $str_bench_done (i32.ge_u (local.get $i) (i32.const 100)))
        (local.set $n
          (i32.add (i32.const 32) (i32.and (local.get $i) (i32.const 31))))
        (local.set $in_cap (call $arena_alloc (local.get $n)))
        (local.set $j (i32.const 0))
        (block $str_fill_done
          (loop $str_fill
            (br_if $str_fill_done (i32.ge_u (local.get $j) (local.get $n)))
            (call $write_u8
              (local.get $in_cap)
              (local.get $j)
              (i32.and (i32.add (i32.mul (local.get $j) (i32.const 7))
                                (i32.mul (local.get $i) (i32.const 11)))
                       (i32.const 255)))
            (local.set $j (i32.add (local.get $j) (i32.const 1)))
            (br $str_fill)))
        (local.set $r2 (call $call_reverse (local.get $in_cap) (local.get $n)))
        (local.set $out_cap
          (i32.wrap_i64 (i64.and (local.get $r2) (i64.const 0xffffffff))))

        ;; ── reverse bench-workload guard ─────────────────────────────────
        ;; reply[j] must equal sent[n-1-j] = ((n-1-j)*7 + i*11) & 255.
        ;; Sample j = 0, n>>1, n-1. A forward-copy callee, a cached reply,
        ;; or a constant-input bench fails on the first mismatch.
        (local.set $j (i32.const 0))
        (local.set $status
          (i32.and (i32.add (i32.mul (i32.sub (local.get $n) (i32.const 1)) (i32.const 7))
                            (i32.mul (local.get $i) (i32.const 11)))
                   (i32.const 255)))
        (if (i32.ne (call $read_u8 (local.get $out_cap) (local.get $j)) (local.get $status))
          (then (call $log (i32.const 256) (i32.const 14)) (return (i32.const 9))))
        (local.set $j (i32.shr_u (local.get $n) (i32.const 1)))
        (local.set $status
          (i32.and (i32.add (i32.mul (i32.sub (i32.sub (local.get $n) (i32.const 1)) (local.get $j)) (i32.const 7))
                            (i32.mul (local.get $i) (i32.const 11)))
                   (i32.const 255)))
        (if (i32.ne (call $read_u8 (local.get $out_cap) (local.get $j)) (local.get $status))
          (then (call $log (i32.const 256) (i32.const 14)) (return (i32.const 9))))
        (local.set $j (i32.sub (local.get $n) (i32.const 1)))
        (local.set $status
          (i32.and (i32.add (i32.mul (i32.sub (i32.sub (local.get $n) (i32.const 1)) (local.get $j)) (i32.const 7))
                            (i32.mul (local.get $i) (i32.const 11)))
                   (i32.const 255)))
        (if (i32.ne (call $read_u8 (local.get $out_cap) (local.get $j)) (local.get $status))
          (then (call $log (i32.const 256) (i32.const 14)) (return (i32.const 9))))

        (call $arena_free (local.get $out_cap))
        (call $arena_free (local.get $in_cap))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $str_bench)))

    ;; ── sqrt_batch: N = 4096 f64s through the shared arena ───────────────
    (local.set $n (i32.const 4096))

    ;; allocate the input buffer from the arena
    (local.set $in_cap (call $arena_alloc (i32.mul (local.get $n) (i32.const 8))))
    (if (i32.eqz (local.get $in_cap))
      (then (call $log (i32.const 192) (i32.const 9)) (return (i32.const 3))))

    ;; fill: for i in 0..N: write_f64(in_cap, i, f64(i))
    (local.set $i (i32.const 0))
    (block $fill_done
      (loop $fill
        (br_if $fill_done (i32.ge_u (local.get $i) (local.get $n)))
        (call $write_f64
          (local.get $in_cap)
          (local.get $i)
          (i64.reinterpret_f64 (f64.convert_i32_u (local.get $i))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $fill)))

    ;; call into Lisp: sqrt_batch(count, input_cap) -> ArenaSlice<f64>
    (local.set $r2 (call $call_sqrt_batch (local.get $n) (local.get $in_cap)))
    (local.set $out_cap
      (i32.wrap_i64 (i64.shr_u (local.get $r2) (i64.const 32))))
    (if (i32.ne (local.get $out_cap) (i32.const 0))
      (then (call $log (i32.const 192) (i32.const 9)) (return (i32.const 4))))
    (local.set $out_cap
      (i32.wrap_i64 (i64.and (local.get $r2) (i64.const 0xffffffff))))
    (if (i32.eqz (local.get $out_cap))
      (then (call $log (i32.const 192) (i32.const 9)) (return (i32.const 5))))

    ;; verify: for i in 0..N: |v*v - i| < 1e-6  (v = read_f64(out_cap, i))
    (local.set $i (i32.const 0))
    (block $verify_done
      (loop $verify
        (br_if $verify_done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $v (f64.reinterpret_i64 (call $read_f64 (local.get $out_cap) (local.get $i))))
        (local.set $err
          (f64.sub (f64.mul (local.get $v) (local.get $v))
                   (f64.convert_i32_u (local.get $i))))
        (if (f64.gt (f64.abs (local.get $err)) (f64.const 1e-6))
          (then (call $log (i32.const 192) (i32.const 9)) (return (i32.const 6))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $verify)))

    ;; done: release both arena buffers (ownership accounting is in the kernel)
    (call $arena_free (local.get $out_cap))
    (call $arena_free (local.get $in_cap))

    ;; ── async heavy_reduce: T13/T14 ───────────────────────────────────────
    ;; Fire-and-forget send (NO_REPLY), immediate ACK, then the real result
    ;; arrives later on the dedicated result channel. Only the shared-ring
    ;; transport has a dedicated result ring; on tcp the ring is not
    ;; provisioned, so skip (the shm leg exercises the full path). Fill N=64
    ;; f64s with i; the worker sums them -> sum(0..63) = 2016 (u32).
    ;; (in_cap was already released above with out_cap — do NOT free again)
    (if (i32.eqz (call $is_shm))
      (then (return (i32.const 0))))
    (local.set $n (i32.const 64))
    (local.set $in_cap (call $arena_alloc (i32.mul (local.get $n) (i32.const 8))))
    (if (i32.eqz (local.get $in_cap))
      (then (call $log (i32.const 272) (i32.const 10)) (return (i32.const 10))))
    (local.set $i (i32.const 0))
    (block $async_fill_done
      (loop $async_fill
        (br_if $async_fill_done (i32.ge_u (local.get $i) (local.get $n)))
        (call $write_f64
          (local.get $in_cap)
          (local.get $i)
          (i64.reinterpret_f64 (f64.convert_i32_u (local.get $i))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $async_fill)))

    ;; 1. send + immediate ACK (status 0 = request accepted)
    (local.set $r2 (call $call_heavy_reduce (local.get $in_cap) (local.get $n)))
    (local.set $status
      (i32.wrap_i64 (i64.shr_u (local.get $r2) (i64.const 32))))
    (if (i32.ne (local.get $status) (i32.const 0))
      (then (call $log (i32.const 272) (i32.const 10)) (return (i32.const 11))))

    ;; 2. block on the result channel until the async result arrives
    (local.set $r2 (call $await_async_result))
    (local.set $status
      (i32.wrap_i64 (i64.shr_u (local.get $r2) (i64.const 32))))
    (if (i32.ne (local.get $status) (i32.const 0))
      (then (call $log (i32.const 272) (i32.const 10)) (return (i32.const 12))))
    (local.set $v (f64.convert_i32_u (i32.wrap_i64 (i64.and (local.get $r2) (i64.const 0xffffffff)))))
    (if (f64.ne (local.get $v) (f64.const 2016))
      (then (call $log (i32.const 272) (i32.const 10)) (return (i32.const 13))))

    (call $arena_free (local.get $in_cap))

    (call $log (i32.const 288) (i32.const 27))
    (call $log (i32.const 64) (i32.const 34))
    (i32.const 0))
)
