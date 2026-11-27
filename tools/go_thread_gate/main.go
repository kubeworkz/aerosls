// Command go_thread_gate is the E7 thread-group gate fixture.
//
// It is deliberately small: two locked OS threads, one real futex wait/wake
// handshake over shared memory, and a deterministic file copy. It is built
// static with the pinned toolchain by tools/go_thread_gate_census.sh and run
// under strace to measure the syscall surface Go actually asks for.
//
// This is a census fixture, not a kernel test. The futex handshake can return
// EAGAIN if the wake wins the race before the waiter enters the kernel; the
// fixture reports which happened rather than hiding it. The host copy is the
// real work: input and output are checked by the census script after the run.
package main

import (
	"fmt"
	"io"
	"os"
	"runtime"
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	sysFutex           = 202
	futexWaitPrivate   = 128
	futexWakePrivate   = 129
	futexExpectedValue = 0
)

var (
	ready       uint32
	waiting     uint32
	futexWord   uint32
	done        uint32
	mainTID     int
	workerTID   int
	waitRet     uintptr
	waitErrno   syscall.Errno
	wakeRet     uintptr
	wakeErrno   syscall.Errno
)

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: go_thread_gate <input> <output>")
		os.Exit(2)
	}
	runtime.GOMAXPROCS(2)
	runtime.LockOSThread()
	mainTID = syscall.Gettid()

	go worker()

	// Wait until the worker is locked and has reached the pre-wait marker.
	for atomic.LoadUint32(&ready) == 0 {
		runtime.Gosched()
	}
	for atomic.LoadUint32(&waiting) == 0 {
		runtime.Gosched()
	}

	// Give the worker a chance to enter FUTEX_WAIT before the value changes.
	// A lost race is observable (EAGAIN), not a deadlock: the futex word is
	// checked by the kernel at entry.
	runtime.Gosched()
	atomic.StoreUint32(&futexWord, 1)
	wakeRet, _, wakeErrno = syscall.Syscall6(
		sysFutex,
		uintptr(unsafe.Pointer(&futexWord)),
		futexWakePrivate,
		1,
		0, 0, 0,
	)

	for atomic.LoadUint32(&done) == 0 {
		runtime.Gosched()
	}

	fmt.Printf("main_tid=%d worker_tid=%d\n", mainTID, workerTID)
	fmt.Printf("futex_wait_ret=%d futex_wait_errno=%d futex_wake_ret=%d futex_wake_errno=%d\n",
		waitRet, uintptr(waitErrno), wakeRet, uintptr(wakeErrno))

	if err := copyFile(os.Args[1], os.Args[2]); err != nil {
		fmt.Fprintf(os.Stderr, "copy: %v\n", err)
		os.Exit(1)
	}
	os.Exit(0)
}

func worker() {
	runtime.LockOSThread()
	workerTID = syscall.Gettid()
	atomic.StoreUint32(&ready, 1)
	atomic.StoreUint32(&waiting, 1)

	waitRet, _, waitErrno = syscall.Syscall6(
		sysFutex,
		uintptr(unsafe.Pointer(&futexWord)),
		futexWaitPrivate,
		futexExpectedValue,
		0, 0, 0,
	)
	atomic.StoreUint32(&done, 1)
}

func copyFile(srcPath, dstPath string) error {
	src, err := os.Open(srcPath)
	if err != nil {
		return err
	}
	defer src.Close()

	dst, err := os.Create(dstPath)
	if err != nil {
		return err
	}

	n, err := io.Copy(dst, src)
	if err != nil {
		_ = dst.Close()
		return err
	}
	if err := dst.Sync(); err != nil {
		_ = dst.Close()
		return err
	}
	if err := dst.Close(); err != nil {
		return err
	}
	fmt.Printf("copy_bytes=%d\n", n)
	return nil
}
