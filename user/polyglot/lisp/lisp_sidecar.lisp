;;; lisp_sidecar.lisp — the real Lisp sidecar of the two-process e2e.
;;;
;;; A SBCL process that:
;;;   1. connects to sls-kerneld over TCP (the 302-304 wire ABI),
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
           #:next-request-id #:cap-transfer))

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

(format t "[lisp] boot: env KERNEL_PORT=~A ARENA=~A CALC=~A~%"
        (sb-ext:posix-getenv "KERNEL_PORT")
        (sb-ext:posix-getenv "ARENA_PATH")
        (sb-ext:posix-getenv "CALC_LISP_PATH"))
(finish-output t)
(defvar *kernel-port* (parse-integer (sb-ext:posix-getenv "KERNEL_PORT")))
(defvar *arena-path* (sb-ext:posix-getenv "ARENA_PATH"))
(defvar *calc-lisp-path* (sb-ext:posix-getenv "CALC_LISP_PATH"))
(defvar *sock* nil)
(defvar *arena-fd* -1)
(defvar *arena-size* 0)
(defvar *arena-base* nil)          ; SAP into the shared mapping
(defvar *cap-table* '())           ; list of (cap offset len)
(defvar *next-req-id* 0)
(defvar *service-dispatch* nil)     ; bound after loading the generated table

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

;; ── the aerosls package: sidecar runtime over the transport ───────────────

(defun aerosls:next-request-id ()
  (incf *next-req-id*))

(defun cap-table-find (cap)
  (assoc cap *cap-table*))

(defun aerosls:arena-alloc (size &key (rights '(:read :write)))
  (declare (ignore rights))
  (let* ((npages (max 1 (ceiling size 4096)))
         (body (make-array 8 :element-type '(unsigned-byte 8))))
    (put-le32 body 0 npages)
    (put-le32 body 4 3)               ; R | W
    (let ((reply (rpc 290 body)))
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

(defun aerosls:arena-free (cap)
  (let ((body (make-array 2 :element-type '(unsigned-byte 8))))
    (put-le16 body 0 cap)
    (rpc 304 body)
    (setf *cap-table* (remove cap *cap-table* :key #'first))
    nil))

(defun aerosls:chan-send (chan opcode payload &key (caps '()) (flags 0))
  "Send a message. opcode 0 = reply (raw payload); any other opcode = the
   runtime prepends [opcode u16][payload_len u32]. Reply caps are sent with
   the ARENA_OWNED flag so ownership transfers to the caller."
  (let* ((n-caps (length caps))
         (header (make-array 12 :element-type '(unsigned-byte 8)))
         (descs (make-array (* n-caps 12) :element-type '(unsigned-byte 8))))
    (put-le16 header 0 chan)
    (put-le16 header 2 n-caps)
    (put-le32 header 4 (aerosls:next-request-id))
    (put-le32 header 8 flags)
    (loop for cap in caps
          for i from 0
          for entry = (cap-table-find cap)
          do (let ((o (* i 12)))
               (put-le16 descs o cap)
               (put-le32 descs (+ o 2) 0)              ; offset
               (put-le32 descs (+ o 6) (if entry (third entry) 0)) ; len
               (setf (aref descs (+ o 10)) 1)          ; rights: R
               (setf (aref descs (+ o 11)) #x02)))     ; ARENA_OWNED
    (let* ((body (concatenate '(vector (unsigned-byte 8)) header descs payload))
           (reply (rpc 302 body)))
      (le32 reply 0))))

(defun aerosls:chan-recv (chan)
  "Blocking receive. Returns (values payload tag flags cap-list); on EAGAIN
   returns (values nil nil nil nil)."
  (let ((body (make-array 6 :element-type '(unsigned-byte 8))))
    (put-le16 body 0 chan)
    (put-le32 body 2 4096)
    (let ((reply (rpc 303 body)))
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
            (values payload (le32 reply 8) (le32 reply 12) (nreverse caps)))))))

;; ── event loop ─────────────────────────────────────────────────────────────

(defun run-dispatch-loop ()
  (handler-case
      (loop
        (multiple-value-bind (payload tag flags caps) (aerosls:chan-recv 1)
          (declare (ignore tag flags))
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
              (dolist (c caps) (aerosls:arena-free c))
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
