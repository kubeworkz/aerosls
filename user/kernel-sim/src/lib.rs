//! Host-side fake kernel for testing AeroSLS sidecars end to end.
//!
//! Implements the semantics sidecars depend on, faithfully enough to test
//! them (capability-layer spec §3–§4, transport spec §3–§6):
//!
//! - per-sidecar capability tables (generation-marked revocation);
//! - FIFO per-endpoint receive queues with total order of control events;
//! - window = 1 per endpoint (one outstanding request, enforced);
//! - transient grants: minted on the client's send, validated (no
//!   amplification), auto-revoked when the driver replies with the matching
//!   tag, or on close;
//! - persist grants (`RD_MAP`): lineage to the driver's storage cap, killed
//!   by deep revocation on driver death (`kill_driver`);
//! - `NEW_CHANNEL` injection on the console channel (boot path);
//! - close events delivered in-band, never lost, always waking a blocked
//!   wait/recv;
//! - blocking `wait`/`recv` with condvar wakeups (mirrors the cooperative
//!   kernel), on **both** sides: `FakeKernel` is the driver's view of the
//!   kernel, `FakeClient` (which also implements `Kernel`) is the client's.
//!
//! The sidecar under test runs in its own thread; the test drives the other
//! end synchronously. There is no unsafe in this file: regions are `Box<[u8]>`
//! allocations held in `State.arenas` (stable addresses — the `Box` pointer
//! never moves once pushed), and sidecars reach them through the raw bases
//! returned by `cap_info`, which is exactly how the real kernel works.

use aerosls_proto::kabi::{
    self, CapInfo, GrantedCap, Kernel, RecvResult, SendCap,
};
use aerosls_proto::*;
use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

/// Driver table layout (matches the manifest's initial caps in order).
pub const DRIVER_STORAGE: u32 = 1;
pub const DRIVER_CONSOLE: u32 = 2;

/// Fake-internal flag: a driver-table entry that is a transient grant
/// (minted on a client send, auto-revoked at reply).
const T_TRANSIENT: u16 = 0x8000;

const DEAD: u32 = u32::MAX;

const CHAN_RIGHTS: u16 = 0x7; // SEND | RECV | CLOSE

#[derive(Clone, Copy, Debug)]
struct Region {
    base: u64,
    len: u64,
}

#[derive(Clone, Copy, Debug)]
enum Parent {
    Driver,
    Client,
}

#[derive(Clone, Copy, Debug)]
struct CapEntry {
    ty: u16,
    rights: u16,
    flags: u16,
    region: Region,
    gen: u32,
    parent: Option<Parent>,
}

enum QEntry {
    Msg {
        tag: u32,
        flags: u16,
        payload: Vec<u8>,
        grants: Vec<GrantedCap>,
    },
    Close { reason: u16, detail: u32 },
    NewChannel { handle: u32 },
}

struct Channel {
    driver_handle: u32,
    client_handle: u32, // u32::MAX for the console channel (kernel-held peer)
    driver_rx: VecDeque<QEntry>,
    client_rx: VecDeque<QEntry>,
    outstanding_tag: Option<u32>, // window = 1
    transients: Vec<(u32, u32)>,  // (driver grant handle, tag)
    driver_closed: bool,
    client_closed: bool,
}

struct State {
    driver_table: Vec<Option<CapEntry>>,
    client_table: Vec<Option<CapEntry>>,
    channels: Vec<Channel>,
    /// Stable allocations; never dropped for the lifetime of the fake.
    arenas: Vec<Box<[u8]>>,
    shutdown: bool,
    storage_base: u64,
    storage_len: u64,
    /// MSG deliveries to the driver (assertion helper: cache-hit tests).
    requests: u64,
}

impl State {
    /// Wire a fresh channel pair: one endpoint in each table.
    fn wire_channel(&mut self) -> (u32, u32) {
        let d = mint_entry(
            &mut self.driver_table,
            kabi::CAP_CHAN,
            CHAN_RIGHTS,
            Region { base: 0, len: 0 },
            0,
            None,
        );
        let c = mint_entry(
            &mut self.client_table,
            kabi::CAP_CHAN,
            CHAN_RIGHTS,
            Region { base: 0, len: 0 },
            0,
            None,
        );
        self.channels.push(Channel {
            driver_handle: d,
            client_handle: c,
            driver_rx: VecDeque::new(),
            client_rx: VecDeque::new(),
            outstanding_tag: None,
            transients: Vec::new(),
            driver_closed: false,
            client_closed: false,
        });
        (d, c)
    }
}

/// Free-function table ops: taking the *table* (not `&mut State`) keeps the
/// borrows disjoint when a channel is borrowed at the same time (the fake
/// holds `&mut st.channels[ci]` while minting/revoking in the other tables).
fn mint_entry(
    table: &mut Vec<Option<CapEntry>>,
    ty: u16,
    rights: u16,
    region: Region,
    flags: u16,
    parent: Option<Parent>,
) -> u32 {
    let h = table.len() as u32;
    table.push(Some(CapEntry {
        ty,
        rights,
        flags,
        region,
        gen: 1,
        parent,
    }));
    h
}

fn revoke_entry(table: &mut Vec<Option<CapEntry>>, handle: u32) {
    if let Some(Some(e)) = table.get_mut(handle as usize) {
        e.gen = DEAD;
    }
}

fn add_arena(st: &mut State, data: Vec<u8>) -> (u64, u64) {
    let len = data.len() as u64;
    let boxed: Box<[u8]> = data.into_boxed_slice();
    let base = boxed.as_ptr() as u64;
    st.arenas.push(boxed);
    (base, len)
}

fn channel_by_driver(st: &State, h: u32) -> Option<usize> {
    st.channels.iter().position(|c| c.driver_handle == h)
}

fn channel_by_client(st: &State, h: u32) -> Option<usize> {
    st.channels.iter().position(|c| c.client_handle == h)
}

fn ready_kind(st: &State, driver_handle: u32) -> Option<u16> {
    let ch = st.channels.iter().find(|c| c.driver_handle == driver_handle)?;
    ch.driver_rx.front().map(|e| match e {
        QEntry::Msg { .. } => CH_KIND_MSG,
        QEntry::Close { .. } => CH_KIND_CLOSE,
        QEntry::NewChannel { .. } => CH_KIND_NEW_CHANNEL,
    })
}

fn ready_any(st: &State, chans: &[u32]) -> Option<usize> {
    chans
        .iter()
        .position(|&h| ready_kind(st, h).is_some())
}

fn client_ready_kind(st: &State, client_handle: u32) -> Option<u16> {
    let ch = st.channels.iter().find(|c| c.client_handle == client_handle)?;
    ch.client_rx.front().map(|e| match e {
        QEntry::Msg { .. } => CH_KIND_MSG,
        QEntry::Close { .. } => CH_KIND_CLOSE,
        QEntry::NewChannel { .. } => CH_KIND_NEW_CHANNEL,
    })
}

fn client_ready_any(st: &State, chans: &[u32]) -> Option<usize> {
    chans
        .iter()
        .position(|&h| client_ready_kind(st, h).is_some())
}

/// The kernel side, as seen by the driver. Clone to move into the driver
/// thread while keeping a handle for assertions.
#[derive(Clone)]
pub struct FakeKernel {
    state: Arc<Mutex<State>>,
    cond: Arc<Condvar>,
}

/// The client side, as seen by a test driving the "POSIX sidecar" end. Also
/// implements `Kernel`, so a client component (the block cache) can be
/// written generically and driven against this fake.
#[derive(Clone)]
pub struct FakeClient {
    state: Arc<Mutex<State>>,
    cond: Arc<Condvar>,
}

impl FakeKernel {
    /// Build a fake with a driver table matching the manifest and `n` POSIX
    /// endpoints already wired into the initial table (respawn style).
    /// `n == 0` is boot style: the endpoint arrives later via
    /// `client.inject_posix_endpoint()` (NEW_CHANNEL on the console). The
    /// storage cap is read-only (the manifest's `"r"`); use `new_writable`
    /// for the writable-driver variant.
    pub fn new(storage: Vec<u8>, n_initial_endpoints: usize) -> (FakeKernel, FakeClient) {
        Self::new_with_rights(storage, n_initial_endpoints, R as u16)
    }

    /// Like `new`, but the storage cap is `rw` — the writable driver is one
    /// manifest letter away from the read-only one (implementation plan §1),
    /// and the block cache's write tests need it.
    pub fn new_writable(storage: Vec<u8>, n_initial_endpoints: usize) -> (FakeKernel, FakeClient) {
        Self::new_with_rights(storage, n_initial_endpoints, (R | W) as u16)
    }

    fn new_with_rights(
        storage: Vec<u8>,
        n_initial_endpoints: usize,
        storage_rights: u16,
    ) -> (FakeKernel, FakeClient) {
        let mut st = State {
            driver_table: Vec::new(),
            client_table: Vec::new(),
            channels: Vec::new(),
            arenas: Vec::new(),
            shutdown: false,
            storage_base: 0,
            storage_len: 0,
            requests: 0,
        };

        // budget MEM (rw)
        let (bbase, blen) = add_arena(&mut st, vec![0u8; 256 * 1024]);
        st.driver_table.push(Some(CapEntry {
            ty: kabi::CAP_MEM,
            rights: 0x3,
            flags: 0,
            region: Region { base: bbase, len: blen },
            gen: 1,
            parent: None,
        }));

        // storage MEM (r, or rw for the writable-driver variant)
        let (sbase, slen) = add_arena(&mut st, storage);
        st.storage_base = sbase;
        st.storage_len = slen;
        st.driver_table.push(Some(CapEntry {
            ty: kabi::CAP_MEM,
            rights: storage_rights,
            flags: 0,
            region: Region { base: sbase, len: slen },
            gen: 1,
            parent: None,
        }));

        // console CHAN (kernel-held peer; client end is u32::MAX)
        st.driver_table.push(Some(CapEntry {
            ty: kabi::CAP_CHAN,
            rights: CHAN_RIGHTS,
            flags: 0,
            region: Region { base: 0, len: 0 },
            gen: 1,
            parent: None,
        }));
        st.channels.push(Channel {
            driver_handle: DRIVER_CONSOLE,
            client_handle: u32::MAX,
            driver_rx: VecDeque::new(),
            client_rx: VecDeque::new(),
            outstanding_tag: None,
            transients: Vec::new(),
            driver_closed: false,
            client_closed: false,
        });

        // img MEM (rx)
        let (ibase, ilen) = add_arena(&mut st, vec![0u8; 16 * 1024]);
        st.driver_table.push(Some(CapEntry {
            ty: kabi::CAP_MEM,
            rights: 0x4,
            flags: 0,
            region: Region { base: ibase, len: ilen },
            gen: 1,
            parent: None,
        }));

        for _ in 0..n_initial_endpoints {
            st.wire_channel();
        }

        let state = Arc::new(Mutex::new(st));
        let cond = Arc::new(Condvar::new());
        (
            FakeKernel {
                state: state.clone(),
                cond: cond.clone(),
            },
            FakeClient { state, cond },
        )
    }

    /// Driver-table CHAN caps except console — what the driver's initial
    /// scan adopts (respawn style).
    pub fn initial_chan_caps(&self) -> Vec<u32> {
        let st = self.state.lock().unwrap();
        st.driver_table
            .iter()
            .enumerate()
            .filter(|(_, e)| e.map_or(false, |e| e.ty == kabi::CAP_CHAN && e.gen != DEAD))
            .map(|(i, _)| i as u32)
            .filter(|&h| h != DRIVER_CONSOLE)
            .collect()
    }

    /// (base, len) of the driver's storage cap.
    pub fn storage_info(&self) -> (u64, u64) {
        let st = self.state.lock().unwrap();
        let e = st.driver_table[DRIVER_STORAGE as usize].unwrap();
        (e.region.base, e.region.len)
    }

    /// Live transient-grant entries in the driver table (assertion helper:
    /// after a reply or close the count must be zero).
    pub fn driver_grant_count(&self) -> usize {
        let st = self.state.lock().unwrap();
        st.driver_table
            .iter()
            .flatten()
            .filter(|e| e.gen != DEAD && e.flags & T_TRANSIENT != 0)
            .count()
    }

    /// Requests delivered to the driver (MSG recv's). Used by cache-hit
    /// tests: a repeat read must not reach the driver.
    pub fn driver_requests(&self) -> u64 {
        self.state.lock().unwrap().requests
    }
}

impl Kernel for FakeKernel {
    fn wait(&self, chans: &[u32], timeout_ns: u64) -> Result<(usize, u16), i32> {
        let mut st = self.state.lock().unwrap();
        loop {
            if st.shutdown {
                return Err(kabi::ERR_SHUTDOWN);
            }
            for (i, &h) in chans.iter().enumerate() {
                if let Some(kind) = ready_kind(&st, h) {
                    return Ok((i, kind));
                }
            }
            if timeout_ns == kabi::TIMEOUT_NONE {
                st = self.cond.wait(st).unwrap();
            } else {
                let (guard, _) = self
                    .cond
                    .wait_timeout(st, Duration::from_nanos(timeout_ns))
                    .unwrap();
                st = guard;
                if !st.shutdown && ready_any(&st, chans).is_none() {
                    return Err(kabi::ERR_TIMEOUT);
                }
            }
        }
    }

    fn recv(
        &self,
        chan: u32,
        buf: &mut [u8],
        slots: &mut [GrantedCap],
    ) -> Result<RecvResult, i32> {
        let mut st = self.state.lock().unwrap();
        let ci = channel_by_driver(&st, chan).ok_or(kabi::ERR_NOTFOUND)?;
        let ch = &mut st.channels[ci];
        let Some(entry) = ch.driver_rx.pop_front() else {
            return Err(kabi::ERR_STATE);
        };
        match entry {
            QEntry::Msg {
                tag,
                flags,
                payload,
                grants,
            } => {
                if payload.len() > buf.len() || grants.len() > slots.len() {
                    // No-loss rule (transport spec §3.4): put it back.
                    ch.driver_rx.push_front(QEntry::Msg {
                        tag,
                        flags,
                        payload,
                        grants,
                    });
                    return Err(kabi::ERR_BUFSZ);
                }
                buf[..payload.len()].copy_from_slice(&payload);
                for (i, g) in grants.iter().enumerate() {
                    slots[i] = *g;
                }
                st.requests += 1;
                Ok(RecvResult {
                    kind: CH_KIND_MSG,
                    flags,
                    tag,
                    len: payload.len(),
                    n_caps: grants.len(),
                })
            }
            QEntry::Close { reason, detail } => {
                let p = encode_close_body(reason, detail);
                if p.len() > buf.len() {
                    ch.driver_rx.push_front(QEntry::Close { reason, detail });
                    return Err(kabi::ERR_BUFSZ);
                }
                buf[..p.len()].copy_from_slice(&p);
                Ok(RecvResult {
                    kind: CH_KIND_CLOSE,
                    flags: 0,
                    tag: 0,
                    len: p.len(),
                    n_caps: 0,
                })
            }
            QEntry::NewChannel { handle } => {
                let p = encode_new_channel(handle, CHAN_RIGHTS, 0, 0);
                if p.len() > buf.len() {
                    ch.driver_rx.push_front(QEntry::NewChannel { handle });
                    return Err(kabi::ERR_BUFSZ);
                }
                buf[..p.len()].copy_from_slice(&p);
                Ok(RecvResult {
                    kind: CH_KIND_NEW_CHANNEL,
                    flags: 0,
                    tag: 0,
                    len: p.len(),
                    n_caps: 0,
                })
            }
        }
    }

    fn send(
        &self,
        chan: u32,
        tag: u32,
        flags: u16,
        payload: &[u8],
        caps: &[SendCap],
        _timeout_ns: u64,
    ) -> Result<(), i32> {
        let mut st = self.state.lock().unwrap();
        let ci = channel_by_driver(&st, chan).ok_or(kabi::ERR_NOTFOUND)?;
        // All channel access is short-lived (no `&mut Channel` held across
        // the table operations below — the borrow checker does not split
        // fields through the MutexGuard deref).
        if st.channels[ci].driver_closed {
            return Err(kabi::ERR_STATE);
        }

        if flags & F_REPLY != 0 {
            // Reply: window reopens, transient grants bound to the tag die.
            match st.channels[ci].outstanding_tag {
                Some(t) if t == tag => {}
                _ => return Err(kabi::ERR_STATE),
            }
            let revoke: Vec<u32> = st.channels[ci]
                .transients
                .iter()
                .filter(|(_, t)| *t == tag)
                .map(|(h, _)| *h)
                .collect();
            for h in revoke {
                revoke_entry(&mut st.driver_table, h);
            }
            st.channels[ci].transients.retain(|(_, t)| *t != tag);
            st.channels[ci].outstanding_tag = None;
        }

        // Mint the driver's caps into the client table (no amplification).
        let mut grants = Vec::new();
        for c in caps {
            let info = st
                .driver_table
                .get(c.slot as usize)
                .and_then(|e| *e)
                .ok_or(kabi::ERR_NOTFOUND)?;
            if info.gen == DEAD {
                return Err(kabi::ERR_REVOKED);
            }
            if info.ty != kabi::CAP_MEM {
                return Err(kabi::ERR_TYPE);
            }
            if (c.rights as u16) & !info.rights != 0 {
                return Err(kabi::ERR_RIGHTS);
            }
            let off = c.offset as u64;
            let len = c.len as u64;
            if off + len > info.region.len {
                return Err(kabi::ERR_RANGE);
            }
            let base = info.region.base + off;
            let handle = mint_entry(
                &mut st.client_table,
                kabi::CAP_MEM,
                c.rights as u16,
                Region { base, len },
                0,
                Some(Parent::Driver),
            );
            grants.push(GrantedCap {
                handle,
                rights: c.rights,
                base,
                len,
            });
        }

        if st.channels[ci].client_closed {
            return Err(kabi::ERR_STATE);
        }
        st.channels[ci].client_rx.push_back(QEntry::Msg {
            tag,
            flags,
            payload: payload.to_vec(),
            grants,
        });
        self.cond.notify_all();
        Ok(())
    }

    fn close(&self, chan: u32, reason: u16, detail: u32) -> Result<(), i32> {
        let mut st = self.state.lock().unwrap();
        let ci = channel_by_driver(&st, chan).ok_or(kabi::ERR_NOTFOUND)?;
        if st.channels[ci].driver_closed {
            return Ok(()); // idempotent
        }
        let revoke: Vec<u32> = st.channels[ci]
            .transients
            .iter()
            .map(|(h, _)| *h)
            .collect();
        st.channels[ci].transients.clear();
        st.channels[ci].outstanding_tag = None;
        st.channels[ci].driver_closed = true;
        for h in revoke {
            revoke_entry(&mut st.driver_table, h);
        }
        let client_handle = st.channels[ci].client_handle;
        if client_handle != u32::MAX {
            st.channels[ci]
                .client_rx
                .push_back(QEntry::Close { reason, detail });
        }
        self.cond.notify_all();
        Ok(())
    }

    fn cap_info(&self, handle: u32) -> Result<CapInfo, i32> {
        let st = self.state.lock().unwrap();
        match st.driver_table.get(handle as usize).and_then(|e| *e) {
            Some(e) if e.gen != DEAD => Ok(CapInfo {
                ty: e.ty,
                rights: e.rights,
                flags: e.flags,
                base: e.region.base,
                len: e.region.len,
            }),
            _ => Err(kabi::ERR_REVOKED),
        }
    }
}

impl FakeClient {
    /// Allocate a fresh MEM region in the client's table (the memory is
    /// stable for the fake's lifetime). Client handles are always the next
    /// free client-table index.
    pub fn new_region(&self, data: Vec<u8>, rights: u8) -> u32 {
        let mut st = self.state.lock().unwrap();
        let (base, len) = add_arena(&mut st, data);
        mint_entry(
            &mut st.client_table,
            kabi::CAP_MEM,
            rights as u16,
            Region { base, len },
            0,
            None,
        )
    }

    /// Backdoor: a client cap over arbitrary memory (used by the aliasing
    /// test to point a grant at the storage region).
    pub fn new_region_at(&self, base: u64, len: u64, rights: u8) -> u32 {
        let mut st = self.state.lock().unwrap();
        mint_entry(
            &mut st.client_table,
            kabi::CAP_MEM,
            rights as u16,
            Region { base, len },
            0,
            None,
        )
    }

    /// Copy the client-side memory behind a cap (assertions).
    pub fn region_bytes(&self, handle: u32) -> Vec<u8> {
        let st = self.state.lock().unwrap();
        let e = st
            .client_table
            .get(handle as usize)
            .and_then(|e| *e)
            .expect("no such client cap");
        assert!(e.gen != DEAD, "cap revoked");
        let arena = st
            .arenas
            .iter()
            .find(|a| {
                let b = a.as_ptr() as u64;
                b <= e.region.base && e.region.base + e.region.len <= b + a.len() as u64
            })
            .expect("region not backed by an arena");
        let off = (e.region.base - arena.as_ptr() as u64) as usize;
        arena[off..off + e.region.len as usize].to_vec()
    }

    pub fn storage_bytes(&self) -> Vec<u8> {
        let st = self.state.lock().unwrap();
        let arena = st
            .arenas
            .iter()
            .find(|a| a.as_ptr() as u64 == st.storage_base)
            .expect("storage arena missing");
        arena[..st.storage_len as usize].to_vec()
    }

    pub fn storage_len(&self) -> u64 {
        self.state.lock().unwrap().storage_len
    }

    pub fn storage_base(&self) -> u64 {
        self.state.lock().unwrap().storage_base
    }

    /// Send a request (window=1 enforced). Caps are validated against the
    /// client's table and minted into the driver's table as transient
    /// grants — no amplification (capability-layer spec §4.1).
    pub fn send(
        &self,
        chan: u32,
        tag: u32,
        flags: u16,
        payload: &[u8],
        caps: &[SendCap],
    ) -> Result<(), i32> {
        let mut st = self.state.lock().unwrap();
        if st.shutdown {
            return Err(kabi::ERR_SHUTDOWN);
        }
        let ci = channel_by_client(&st, chan).ok_or(kabi::ERR_NOTFOUND)?;
        if st.channels[ci].client_closed || st.channels[ci].driver_closed {
            return Err(kabi::ERR_STATE);
        }
        if st.channels[ci].outstanding_tag.is_some() {
            return Err(kabi::ERR_STATE); // window = 1
        }

        let mut grants = Vec::new();
        let mut handles = Vec::new();
        for c in caps {
            let info = st
                .client_table
                .get(c.slot as usize)
                .and_then(|e| *e)
                .ok_or(kabi::ERR_NOTFOUND)?;
            if info.gen == DEAD {
                return Err(kabi::ERR_REVOKED);
            }
            if info.ty != kabi::CAP_MEM {
                return Err(kabi::ERR_TYPE);
            }
            if (c.rights as u16) & !info.rights != 0 {
                return Err(kabi::ERR_RIGHTS);
            }
            let off = c.offset as u64;
            let len = c.len as u64;
            if off + len > info.region.len {
                return Err(kabi::ERR_RANGE);
            }
            let base = info.region.base + off;
            let handle = mint_entry(
                &mut st.driver_table,
                kabi::CAP_MEM,
                c.rights as u16,
                Region { base, len },
                T_TRANSIENT,
                Some(Parent::Client),
            );
            grants.push(GrantedCap {
                handle,
                rights: c.rights,
                base,
                len,
            });
            handles.push(handle);
        }

        st.channels[ci].outstanding_tag = Some(tag);
        for h in handles {
            st.channels[ci].transients.push((h, tag));
        }
        st.channels[ci].driver_rx.push_back(QEntry::Msg {
            tag,
            flags,
            payload: payload.to_vec(),
            grants,
        });
        self.cond.notify_all();
        Ok(())
    }

    /// Receive the next message/event from the client end (blocks until one
    /// arrives, mirroring the kernel wakeup).
    pub fn recv(
        &self,
        chan: u32,
        buf: &mut [u8],
        slots: &mut [GrantedCap],
    ) -> Result<RecvResult, i32> {
        let mut st = self.state.lock().unwrap();
        let ci = channel_by_client(&st, chan).ok_or(kabi::ERR_NOTFOUND)?;
        loop {
            let ready = st.channels[ci].client_rx.front().is_some();
            if ready || st.shutdown {
                break;
            }
            st = self.cond.wait(st).unwrap();
        }
        let ch = &mut st.channels[ci];
        let Some(entry) = ch.client_rx.pop_front() else {
            return Err(kabi::ERR_SHUTDOWN);
        };
        match entry {
            QEntry::Msg {
                tag,
                flags,
                payload,
                grants,
            } => {
                if payload.len() > buf.len() || grants.len() > slots.len() {
                    ch.client_rx.push_front(QEntry::Msg {
                        tag,
                        flags,
                        payload,
                        grants,
                    });
                    return Err(kabi::ERR_BUFSZ);
                }
                buf[..payload.len()].copy_from_slice(&payload);
                for (i, g) in grants.iter().enumerate() {
                    slots[i] = *g;
                }
                Ok(RecvResult {
                    kind: CH_KIND_MSG,
                    flags,
                    tag,
                    len: payload.len(),
                    n_caps: grants.len(),
                })
            }
            QEntry::Close { reason, detail } => {
                let p = encode_close_body(reason, detail);
                buf[..p.len()].copy_from_slice(&p);
                Ok(RecvResult {
                    kind: CH_KIND_CLOSE,
                    flags: 0,
                    tag: 0,
                    len: p.len(),
                    n_caps: 0,
                })
            }
            QEntry::NewChannel { .. } => Err(kabi::ERR_STATE), // never delivered to clients
        }
    }

    /// Close the client end (client death / timeout): delivers CLOSE_PEER to
    /// the driver and revokes the endpoint's transient grants.
    pub fn close(&self, chan: u32, reason: u16, detail: u32) -> Result<(), i32> {
        let mut st = self.state.lock().unwrap();
        let ci = channel_by_client(&st, chan).ok_or(kabi::ERR_NOTFOUND)?;
        if st.channels[ci].client_closed {
            return Ok(());
        }
        let revoke: Vec<u32> = st.channels[ci]
            .transients
            .iter()
            .map(|(h, _)| *h)
            .collect();
        st.channels[ci].transients.clear();
        st.channels[ci].outstanding_tag = None;
        st.channels[ci].client_closed = true;
        for h in revoke {
            revoke_entry(&mut st.driver_table, h);
        }
        st.channels[ci]
            .driver_rx
            .push_back(QEntry::Close { reason, detail });
        self.cond.notify_all();
        Ok(())
    }

    pub fn cap_info(&self, handle: u32) -> Result<CapInfo, i32> {
        let st = self.state.lock().unwrap();
        match st.client_table.get(handle as usize).and_then(|e| *e) {
            Some(e) if e.gen != DEAD => Ok(CapInfo {
                ty: e.ty,
                rights: e.rights,
                flags: e.flags,
                base: e.region.base,
                len: e.region.len,
            }),
            _ => Err(kabi::ERR_REVOKED),
        }
    }

    /// Simulate the POSIX sidecar's boot wiring: the kernel injects a fresh
    /// endpoint into the driver's table and delivers NEW_CHANNEL on the
    /// console channel (transport spec §2.2, §5). Returns the client handle.
    pub fn inject_posix_endpoint(&self) -> Result<u32, i32> {
        let mut st = self.state.lock().unwrap();
        if st.shutdown {
            return Err(kabi::ERR_SHUTDOWN);
        }
        let (d, c) = st.wire_channel();
        let ci = channel_by_driver(&st, DRIVER_CONSOLE).ok_or(kabi::ERR_NOTFOUND)?;
        st.channels[ci]
            .driver_rx
            .push_back(QEntry::NewChannel { handle: d });
        self.cond.notify_all();
        Ok(c)
    }

    /// Tear the driver down the way the kernel does on sidecar death
    /// (capability-layer spec §6.3): shutdown, revoke the driver's whole
    /// table, deep-revoke client caps derived from it, deliver
    /// CLOSE_PEER_DEAD to every client, wake everyone.
    pub fn kill_driver(&self, detail: u32) {
        let mut st = self.state.lock().unwrap();
        st.shutdown = true;
        for e in st.driver_table.iter_mut().flatten() {
            e.gen = DEAD;
        }
        for e in st.client_table.iter_mut().flatten() {
            if matches!(e.parent, Some(Parent::Driver)) {
                e.gen = DEAD;
            }
        }
        for ch in st.channels.iter_mut() {
            if ch.client_handle != u32::MAX {
                ch.client_rx
                    .push_back(QEntry::Close { reason: CLOSE_PEER_DEAD, detail });
            }
        }
        self.cond.notify_all();
    }
}

/// The client's view of the kernel: `wait` on the client queues, blocking
/// `recv` (a reply or a close event — a close always wakes a blocked recv),
/// sends with window=1, idempotent close, and `cap_info` against the client
/// table. The inherent `send` (no timeout) is kept for the driver tests; the
/// trait method adds the transport-spec timeout, which the fake honors by
/// ignoring (nothing in the sim ever blocks a send).
impl Kernel for FakeClient {
    fn wait(&self, chans: &[u32], timeout_ns: u64) -> Result<(usize, u16), i32> {
        let mut st = self.state.lock().unwrap();
        loop {
            if st.shutdown {
                return Err(kabi::ERR_SHUTDOWN);
            }
            for (i, &h) in chans.iter().enumerate() {
                if let Some(kind) = client_ready_kind(&st, h) {
                    return Ok((i, kind));
                }
            }
            if timeout_ns == kabi::TIMEOUT_NONE {
                st = self.cond.wait(st).unwrap();
            } else {
                let (guard, _) = self
                    .cond
                    .wait_timeout(st, Duration::from_nanos(timeout_ns))
                    .unwrap();
                st = guard;
                if !st.shutdown && client_ready_any(&st, chans).is_none() {
                    return Err(kabi::ERR_TIMEOUT);
                }
            }
        }
    }

    fn recv(
        &self,
        chan: u32,
        buf: &mut [u8],
        slots: &mut [GrantedCap],
    ) -> Result<RecvResult, i32> {
        self.recv(chan, buf, slots)
    }

    fn send(
        &self,
        chan: u32,
        tag: u32,
        flags: u16,
        payload: &[u8],
        caps: &[SendCap],
        _timeout_ns: u64,
    ) -> Result<(), i32> {
        self.send(chan, tag, flags, payload, caps)
    }

    fn close(&self, chan: u32, reason: u16, detail: u32) -> Result<(), i32> {
        self.close(chan, reason, detail)
    }

    fn cap_info(&self, handle: u32) -> Result<CapInfo, i32> {
        self.cap_info(handle)
    }

    fn poll(&self, chan: u32) -> Result<u16, i32> {
        let st = self.state.lock().unwrap();
        if st.shutdown {
            return Err(kabi::ERR_SHUTDOWN);
        }
        Ok(client_ready_kind(&st, chan).unwrap_or(CH_KIND_NONE))
    }
}
