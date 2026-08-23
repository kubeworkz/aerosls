;;; lisp_sidecar.lisp — the real Lisp sidecar of the two-process e2e.
;;;
;;; A SBCL process that:
;;;   1. connects to sls-kerneld over TCP (the 302-304 wire ABI) or, in
;;;      TRANSPORT=shm mode, talks entirely through the shared rings (ring0
;;;      messages, ring4/ring5 arena alloc/free) with no TCP at all,
;;;   2. mmaps the shared arena file (the zero-copy path — the same pages
;;;      the Wasm sidecar writes),
;;;   3. loads the generated tools/aeroidl/gen/calculator.lisp dispatch table,
;;;   4. runs the channel event loop: recv → parse the IDL header →
;;;      calculator-dispatch → reply with caps.
;;;
;;; The `aerosls` package is the sidecar runtime the generated code expects
;;; (chan-send/chan-recv/arena-alloc/arena-free/arena-mem), implemented here
;;; over the transport. `cffi` and `bt` are shimmed with the two symbols the
;;; generated file actually uses, so it loads on a stock SBCL (no Quicklisp).
;;;
;;; Environment: KERNEL_PORT, ARENA_PATH, CALC_LISP_PATH

(require :sb-bsd-sockets)
(require :sb-posix)

;; ── packages + minimal shims for the generated code ────────────────────────

(defpackage #:aerosls
  (:use #:cl)
  (:export #:chan-send #:chan-recv #:arena-alloc #:arena-free #:arena-mem
           #:next-request-id #:cap-transfer
           #:generate-result-tag #:chan-send-result))

(defpackage #:cffi (:use #:cl) (:export #:mem-aref))
(defpackage #:bt (:use #:cl) (:export #:make-thread))

(in-package #:cffi)
(defun mem-aref (sap type &optional (offset 0))
  "Minimal cffi shim — the generated code only dereferences :double."
  (ecase type
    (:double
     (let ((bits (sb-sys:sap-ref-64 sap (* offset 8))))
       (sb-kernel:make-double-float
        (ldb (byte 32 32) bits)     ; high word first
        (ldb (byte 32 0) bits))))))
(defun (setf mem-aref) (val sap type &optional (offset 0))
  "Minimal cffi shim setf — writes a :double into the SAP."
  (ecase type
    (:double (setf (sb-sys:sap-ref-64 sap (* offset 8))
                   (sb-kernel:double-float-bits val)))))

(in-package #:bt)
(defun make-thread (fn &key name)
  (declare (ignore name))
  (sb-thread:make-thread fn))

;; ── transport state ────────────────────────────────────────────────────────

(in-package #:cl)

(format t "[lisp] boot: env KERNEL_PORT=~A ARENA=~A CALC=~A TRANSPORT=~A~%"
        (sb-ext:posix-getenv "KERNEL_PORT")
        (sb-ext:posix-getenv "ARENA_PATH")
        (sb-ext:posix-getenv "CALC_LISP_PATH")
        (or (sb-ext:posix-getenv "TRANSPORT") "tcp"))
(finish-output t)
(defvar *kernel-port* (parse-integer (sb-ext:posix-getenv "KERNEL_PORT")))
(defvar *arena-path* (sb-ext:posix-getenv "ARENA_PATH"))
(defvar *calc-lisp-path* (sb-ext:posix-getenv "CALC_LISP_PATH"))
(defvar *transport* (or (sb-ext:posix-getenv "TRANSPORT") "tcp"))
(defvar *chan-path* (sb-ext:posix-getenv "CHAN_PATH"))
(defvar *ring-mode* (string= *transport* "shm"))
(defvar *sock* nil)
(defvar *arena-fd* -1)
(defvar *arena-size* 0)
(defvar *arena-base* nil)          ; SAP into the shared arena mapping
(defvar *chan-base* nil)           ; SAP into the shared channel mapping
(defvar *chan-ring0* nil)          ; ring0 header: wasm->lisp (we consume)
(defvar *chan-ring1* nil)          ; ring1 header: lisp->wasm (we produce)
(defvar *chan-ring4* nil)          ; ring4 header: lisp->kerneld arena req (we produce)
(defvar *chan-ring5* nil)          ; ring5 header: kerneld->lisp arena reply (we consume)
(defvar *chan-ring7* nil)          ; ring7 header: lisp->wasm async results (we produce)
(defvar *cap-table* '())           ; list of (cap offset len)
(defvar *next-req-id* 0)
(defvar *service-dispatch* nil)     ; bound after loading the generated table

;; ── shared ring layout (must match polyglot::ring in ring.rs) ─────────────
;; Six rings, each 32-byte header (magic u32 @0, version u32 @4, capacity
;; u32 @8, pad u32 @12, write u64 @16, read u64 @24) + a 4 MiB data region,
;; laid out consecutively in the channel file: ring0 wasm->lisp, ring1
;; lisp->wasm, ring2 wasm->kerneld arena req, ring3 kerneld->wasm arena
;; reply, ring4 lisp->kerneld arena req (we produce), ring5 kerneld->lisp
;; arena reply (we consume). Frames are [body_len u32][body]. Synchronization
;; is the SPSC release/acquire cursor protocol; on x86 (the only platform
;; this e2e runs on) plain 64-bit loads/stores under TSO give the same
;; ordering as the Rust atomics.
(defparameter *ring-header-len* 32)
(defparameter *ring-capacity* 4194304)   ; 4 MiB data per direction
(defparameter *ring-slot-size* (+ *ring-header-len* *ring-capacity*))
(defparameter *ring1-offset* *ring-slot-size*)
(defparameter *ring4-offset* (* 4 *ring-slot-size*))
(defparameter *ring5-offset* (* 5 *ring-slot-size*))
(defparameter *ring7-offset* (* 7 *ring-slot-size*))

;; ── little-endian helpers ──────────────────────────────────────────────────

(defun le16 (b off)
  (+ (aref b off) (* (aref b (+ off 1)) 256)))

(defun le32 (b off)
  (+ (le16 b off) (* (le16 b (+ off 2)) 65536)))

(defun put-le16 (b off v)
  (setf (aref b off) (ldb (byte 8 0) v)
        (aref b (+ off 1)) (ldb (byte 8 8) v)))

(defun put-le32 (b off v)
  (put-le16 b off (ldb (byte 16 0) v))
  (put-le16 b (+ off 2) (ldb (byte 16 16) v)))

;; ── framing ────────────────────────────────────────────────────────────────

(defun recv-exact (stream n)
  (let ((buf (make-array n :element-type '(unsigned-byte 8))))
    (loop with got = 0
          while (< got n)
          for chunk = (read-sequence buf stream :start got)
          do (when (zerop chunk) (error "EOF on transport"))
             (incf got chunk))
    buf))

(defun read-frame ()
  (let ((hdr (recv-exact *sock* 8)))
    (values (le32 hdr 4)              ; syscall number
            (recv-exact *sock* (le32 hdr 0)))))

(defun write-frame (syscall body)
  (let ((hdr (make-array 8 :element-type '(unsigned-byte 8))))
    (put-le32 hdr 0 (length body))
    (put-le32 hdr 4 syscall)
    (write-sequence hdr *sock*)
    (write-sequence body *sock*)
    (finish-output *sock*)))

(defun rpc (syscall body)
  (write-frame syscall body)
  (multiple-value-bind (s reply) (read-frame)
    (declare (ignore s))
    reply))

;; ── shared-memory ring: SPSC producer/consumer over the channel mapping ──

(defun ring-write (ring) (sb-sys:sap-ref-64 ring 16))
(defun ring-read (ring) (sb-sys:sap-ref-64 ring 24))
(defun (setf ring-write) (v ring) (setf (sb-sys:sap-ref-64 ring 16) v))
(defun (setf ring-read) (v ring) (setf (sb-sys:sap-ref-64 ring 24) v))
(defun ring-data (ring) (sb-sys:sap+ ring *ring-header-len*))

(defun ring-copy-out (ring offset buf start n)
  "Copy n bytes from the ring at offset (mod capacity) into buf[start..]."
  (let* ((pos (mod offset *ring-capacity*))
         (d (ring-data ring))
         (first (min (- *ring-capacity* pos) n)))
    (loop for i below first
          do (setf (aref buf (+ start i)) (sb-sys:sap-ref-8 (sb-sys:sap+ d pos) i)))
    (loop for i below (- n first)
          do (setf (aref buf (+ start first i)) (sb-sys:sap-ref-8 d i)))
    n))

(defun ring-copy-in (ring offset buf start n)
  "Copy n bytes from buf[start..] into the ring at offset (mod capacity)."
  (let* ((pos (mod offset *ring-capacity*))
         (d (ring-data ring))
         (first (min (- *ring-capacity* pos) n)))
    (loop for i below first
          do (setf (sb-sys:sap-ref-8 (sb-sys:sap+ d pos) i) (aref buf (+ start i))))
    (loop for i below (- n first)
          do (setf (sb-sys:sap-ref-8 d i) (aref buf (+ start first i))))
    n))

(defun ring-len (ring offset)
  "Read the 4-byte LE frame length at offset (mod capacity), wrap-aware."
  (let* ((pos (mod offset *ring-capacity*))
         (d (ring-data ring))
         (first (min 4 (- *ring-capacity* pos)))
         (v 0))
    (loop for i below first
          do (setf v (+ v (* (sb-sys:sap-ref-8 (sb-sys:sap+ d pos) i) (expt 256 i)))))
    (loop for i from first below 4
          do (setf v (+ v (* (sb-sys:sap-ref-8 d (- i first)) (expt 256 i)))))
    v))

(defun ring-recv (ring)
  "Blocking receive from a shared ring (consumer side). Returns the body."
  (loop for w = (ring-write ring)
        while (= w (ring-read ring))
        do (sb-thread:thread-yield))
  (let* ((r (ring-read ring))
         (len (ring-len ring r))
         (buf (make-array len :element-type '(unsigned-byte 8))))
    (ring-copy-out ring (+ r 4) buf 0 len)
    ;; publish the read cursor only after the data is consumed (TSO orders
    ;; the loads above this store)
    (setf (ring-read ring) (+ r 4 len))
    buf))

(defun ring-send (ring body)
  "Push one frame [len u32][body] to a shared ring (producer side)."
  (let ((total (+ 4 (length body))))
    (loop for w = (ring-write ring)
          for r = (ring-read ring)
          while (> (- w r) (- *ring-capacity* total))
          do (sb-thread:thread-yield))
    (let ((w (ring-write ring)))
      (let ((field (make-array 4 :element-type '(unsigned-byte 8))))
        (put-le32 field 0 (length body))
        (ring-copy-in ring w field 0 4))
      (ring-copy-in ring (+ w 4) body 0 (length body))
      ;; publish the write cursor only after the bytes are in place (TSO
      ;; orders the stores above this one)
      (setf (ring-write ring) (+ w total)))))

;; ── the aerosls package: sidecar runtime over the transport ───────────────

(defun aerosls:next-request-id ()
  (incf *next-req-id*))

(defvar *result-ring* nil)
(defvar *result-tag* 0)

(defun aerosls:generate-result-tag ()
  "Monotonic tag for async results (heavy_reduce). Distinct from the
   request-id stream so the wasm side can tell an async result apart from
   a synchronous reply."
  (incf *result-tag*))

(defun aerosls:chan-send-result (payload &key (tag (aerosls:generate-result-tag)))
  "Deliver an async result on the dedicated result channel (ring7). The
   body is the same 303-reply wire format the wasm side's chan_recv parses
   (rc, plen, tag, flags, n-caps, descs, payload) — only the ring differs."
  (when (null *result-ring*)
    (error "*result-ring* is not set — async result channel unavailable"))
  (let* ((n (length payload))
         (body (make-array (+ 18 n) :element-type '(unsigned-byte 8))))
    (put-le32 body 0 0)                                  ; rc
    (put-le32 body 4 n)                                  ; payload_len
    (put-le32 body 8 tag)
    (put-le32 body 12 0)                                 ; flags
    (put-le16 body 16 0)                                 ; n-caps
    (replace body payload :start1 18)
    (ring-send *result-ring* body)
    0))

(defun arena-rpc (syscall req)
  "Arena syscall over the current transport — returns the reply body.
   TCP: rpc 290/304. Ring: push [syscall u32][req] into ring4 and block on
   ring5 (kerneld's per-sidecar arena ring thread services it)."
  (if *ring-mode*
      (let ((frame (make-array (+ 4 (length req))
                               :element-type '(unsigned-byte 8))))
        (put-le32 frame 0 syscall)
        (replace frame req :start1 4)
        (ring-send *chan-ring4* frame)
        (ring-recv *chan-ring5*))
      (rpc syscall req)))

(defun cap-table-find (cap)
  (assoc cap *cap-table*))

(defun aerosls:arena-alloc (size &key (rights '(:read :write)))
  (declare (ignore rights))
  (let* ((npages (max 1 (ceiling size 4096)))
         (req (make-array 8 :element-type '(unsigned-byte 8))))
    (put-le32 req 0 npages)
    (put-le32 req 4 3)               ; R | W
    (let ((reply (arena-rpc 290 req)))
      (if (/= (le32 reply 0) 0)
          (progn (format t "[lisp] arena-alloc failed rc=~A~%" (le32 reply 0)) nil)
          (let ((cap (le16 reply 4))
                (offset (le32 reply 6))
                (len (le32 reply 10)))
            (push (list cap offset len) *cap-table*)
            cap)))))

(defun aerosls:arena-mem (cap)
  (let ((entry (cap-table-find cap)))
    (unless entry
      (error "arena-mem: unknown cap ~A" cap))
    (sb-sys:sap+ *arena-base* (second entry))))

;; NOTE: in ring mode the cap descriptors carry the REAL arena offset+len
;; (the sender resolves them from its cap table), because there is no kernel
;; in the message path to map slot -> memory. In TCP mode kerneld ignores
;; the wire values and uses its own table, so sending the real offset is
;; harmless there too.
(defun aerosls:chan-send (chan opcode payload &key (caps '()) (flags 0))
  "Send a message. opcode 0 = reply (raw payload); any other opcode = the
   runtime prepends [opcode u16][payload_len u32]. Reply caps are sent with
   the ARENA_OWNED flag so ownership transfers to the caller."
  (let* ((n-caps (length caps))
         (descs (make-array (* n-caps 12) :element-type '(unsigned-byte 8))))
    (loop for cap in caps
          for i from 0
          for entry = (cap-table-find cap)
          do (let ((o (* i 12)))
               (put-le16 descs o cap)
               (put-le32 descs (+ o 2) (if entry (second entry) 0)) ; real offset
               (put-le32 descs (+ o 6) (if entry (third entry) 0))   ; len
               (setf (aref descs (+ o 10)) 1)          ; rights: R
               (setf (aref descs (+ o 11)) #x02)))     ; ARENA_OWNED
    (if *ring-mode*
        ;; Shared ring: push the 303-reply wire body directly — that is what
        ;; the caller-side chan_recv parses (rc, plen, tag, flags, n-caps,
        ;; descs, payload). The kernel is not in the path to bridge formats.
        (let* ((n (length payload))
               (reply (make-array (+ 18 (* n-caps 12) n)
                                  :element-type '(unsigned-byte 8))))
          (put-le32 reply 0 0)                                  ; rc
          (put-le32 reply 4 n)                                  ; payload_len
          (put-le32 reply 8 (aerosls:next-request-id))          ; tag
          (put-le32 reply 12 flags)
          (put-le16 reply 16 n-caps)
          (replace reply descs :start1 18)
          (replace reply payload :start1 (+ 18 (* n-caps 12)))
          (ring-send *chan-ring1* reply)
          0)
        ;; TCP path: rpc 302 with the SEND wire body; kerneld bridges it to
        ;; the 303-reply format on recv. NB: fill the header BEFORE
        ;; concatenating — `concatenate` copies the header's current bytes,
        ;; and bind body in its own LET (init forms run in parallel).
        (let* ((header (make-array 12 :element-type '(unsigned-byte 8))))
          (put-le16 header 0 chan)
          (put-le16 header 2 n-caps)
          (put-le32 header 4 (aerosls:next-request-id))
          (put-le32 header 8 flags)
          (let* ((body (concatenate '(vector (unsigned-byte 8)) header descs payload))
                 (reply (rpc 302 body)))
            (le32 reply 0))))))

(defun aerosls:arena-free (cap)
  (let ((req (make-array 2 :element-type '(unsigned-byte 8))))
    (put-le16 req 0 cap)
    (arena-rpc 304 req)
    (setf *cap-table* (remove cap *cap-table* :key #'first))
    nil))

(defun tcp-recv-303 (chan)
  "TCP 303-recv call — returns the reply body kerneld formats."
  (let ((body (make-array 6 :element-type '(unsigned-byte 8))))
    (put-le16 body 0 chan)
    (put-le32 body 2 4096)
    (rpc 303 body)))

(defun aerosls:chan-recv (chan)
  "Blocking receive. Returns (values payload tag flags cap-list); on EAGAIN
   returns (values nil nil nil nil)."
  (let ((reply (if *ring-mode*
                   ;; Shared ring: the frame body IS the 303-reply wire format.
                   (ring-recv *chan-ring0*)
                   (tcp-recv-303 chan))))
    (if (/= (le32 reply 0) 0)
        (values nil nil nil nil)
        (let* ((plen (le32 reply 4))
               (n-caps (le16 reply 16))
               (payload (subseq reply (+ 18 (* n-caps 12))
                                (+ 18 (* n-caps 12) plen)))
               (caps '()))
          (loop for i below n-caps
                for o = (+ 18 (* i 12))
                do (let ((slot (le16 reply o))
                         (offset (le32 reply (+ o 2)))
                         (len (le32 reply (+ o 6))))
                     (push (list slot offset len) *cap-table*)
                     (push slot caps)))
          (values payload (le32 reply 8) (le32 reply 12) (nreverse caps))))))

;; ── event loop ─────────────────────────────────────────────────────────────

(defun run-dispatch-loop ()
  (handler-case
      (loop
        (multiple-value-bind (payload tag flags caps) (aerosls:chan-recv 1)
          (declare (ignore tag))
          (unless payload
            (sleep 0.01)
            (return))            ;; Parse the IDL header the Rust runtime prepends:
            ;;   [opcode u16][payload_len u32][args...]
            (let* ((opcode (le16 payload 0))
                   (plen (le32 payload 2))
                   (args (make-array plen :element-type '(unsigned-byte 8)
                                         :initial-contents
                                         (loop for i from 6 below (+ 6 plen)
                                               collect (aref payload i))))
                   (cap-slots (make-array 8 :element-type '(unsigned-byte 16)
                                          :initial-element 0)))
            (loop for c in caps
                  for i from 0
                  do (setf (aref cap-slots i) c))
            (multiple-value-bind (reply-payload reply-caps n-reply-caps)
                (handler-case
                    (funcall *service-dispatch*
                             opcode args plen cap-slots (length caps))
                  (error (e)
                    (format t "[lisp] dispatch error: ~A~%" e)
                    (values (make-array 32 :element-type '(unsigned-byte 8)
                                              :initial-element 0)
                            (make-array 8 :element-type '(unsigned-byte 16))
                            0)))
              (finish-output t)
              ;; The received input caps were @borrowed — drop our handles.
              ;; EXCEPT on NO_REPLY (async): the callee keeps ownership (the
              ;; heavy_reduce worker thread reads the input buffer after the
              ;; dispatch returns), so freeing here would yank the cap out
              ;; from under it.
              (unless (logtest 1 flags)
                (dolist (c caps) (aerosls:arena-free c)))
              ;; Reply raw (opcode 0); the output cap transfers to the caller.
              (let ((rc (aerosls:chan-send 2 0 reply-payload
                                   :caps (loop for i below n-reply-caps
                                               collect (aref reply-caps i)))))
                (when (/= rc 0)
                  (format t "[lisp] reply send failed rc=~A~%" rc)))))))
    (error (e)
      (format t "[lisp] event loop error: ~A~%" e))))

;; ── bootstrap ──────────────────────────────────────────────────────────────

(defun main ()
  (format t "[lisp] connecting to 127.0.0.1:~A~%" *kernel-port*)
  (finish-output t)
  (let ((s (sb-bsd-sockets:make-inet-socket :stream :tcp)))
    (setf (sb-bsd-sockets:sockopt-tcp-nodelay s) t)  ; no Nagle: ping-pong frames
    (sb-bsd-sockets:socket-connect s #(127 0 0 1) *kernel-port*)
    (setf *sock*
          (sb-bsd-sockets:socket-make-stream
           s :input t :output t :element-type '(unsigned-byte 8) :buffering :full)))
  (format t "[lisp] connected, opening arena ~A~%" *arena-path*)
  (finish-output t)
  (setf *arena-fd* (sb-posix:open *arena-path* sb-posix:o-rdwr))
  (setf *arena-size* (sb-posix:stat-size (sb-posix:fstat *arena-fd*)))
  (setf *arena-base*
        (sb-posix:mmap nil *arena-size*
                       (logior sb-posix:prot-read sb-posix:prot-write)
                       sb-posix:map-shared *arena-fd* 0))
  (format t "[lisp] arena mapped ~D bytes at ~A~%" *arena-size* *arena-base*)
  (when *ring-mode*
    (unless *chan-path*
      (error "CHAN_PATH is required with TRANSPORT=shm"))
    (let ((fd (sb-posix:open *chan-path* sb-posix:o-rdwr)))
      (setf *chan-base*
            (sb-posix:mmap nil (* 8 *ring-slot-size*)
                           (logior sb-posix:prot-read sb-posix:prot-write)
                           sb-posix:map-shared fd 0))
      (setf *chan-ring0* (sb-sys:sap+ *chan-base* 0))
      (setf *chan-ring1* (sb-sys:sap+ *chan-base* *ring1-offset*))
      (setf *chan-ring4* (sb-sys:sap+ *chan-base* *ring4-offset*))
      (setf *chan-ring5* (sb-sys:sap+ *chan-base* *ring5-offset*))
      (setf *chan-ring7* (sb-sys:sap+ *chan-base* *ring7-offset*))
      (setf *result-ring* *chan-ring7*))
    (format t "[lisp] channel mapped, ring-mode ON~%")
    (finish-output t))
  (load *calc-lisp-path*)
  ;; Resolve the dispatch entry point at runtime — the generated package only
  ;; exists after (load), so the sidecar file must not name it at read time.
  (setf *service-dispatch*
        (symbol-function (intern "CALCULATOR-DISPATCH" "AEROSLS.CALCULATOR")))
  (format t "LISP READY~%")
  (finish-output t)
  (run-dispatch-loop)
  (format t "[lisp] event loop exited~%"))

(main)
