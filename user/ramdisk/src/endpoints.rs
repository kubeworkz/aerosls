//! The set of RD_* endpoints the driver serves (implementation plan §4).
//!
//! Adoption is idempotent and comes from two sources, with one code path:
//! the initial-table scan (respawn: the POSIX control-channel end is in the
//! initial table) and `NEW_CHANNEL` events on the console channel (boot: the
//! kernel injects the POSIX endpoint after the driver started). The console
//! channel is *not* an RD_* endpoint — it is waited on only for control
//! events.
//!
//! Per-endpoint persistent state is deliberately tiny: `{handle, state}`.
//! There is no outstanding-request bookkeeping because window=1 (kernel
//! enforced) serializes each endpoint and the driver processes each request
//! to completion within one event-loop iteration; transient grants exist only
//! for the duration of a handler call and are auto-revoked at reply.

/// `limits.max_channels` (16) minus the console channel.
pub const MAX_RD_ENDPOINTS: usize = 15;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EndpointState {
    /// First message on this endpoint must be `RD_INFO` (the implicit
    /// handshake, implementation plan §5.2).
    AwaitingHandshake,
    Active,
}

#[derive(Clone, Copy, Debug)]
pub struct Endpoint {
    pub handle: u32,
    pub state: EndpointState,
}

pub struct EndpointSet {
    console: u32,
    eps: [Option<Endpoint>; MAX_RD_ENDPOINTS],
    n: usize,
}

impl EndpointSet {
    pub fn new(console: u32) -> EndpointSet {
        EndpointSet {
            console,
            eps: [None; MAX_RD_ENDPOINTS],
            n: 0,
        }
    }

    /// Adopt a channel as an RD_* endpoint. Idempotent. Returns `false` when
    /// the table is full — the caller should close the channel.
    pub fn adopt(&mut self, handle: u32) -> bool {
        if self.find(handle).is_some() {
            return true;
        }
        if self.n >= MAX_RD_ENDPOINTS {
            return false;
        }
        self.eps[self.n] = Some(Endpoint {
            handle,
            state: EndpointState::AwaitingHandshake,
        });
        self.n += 1;
        true
    }

    pub fn drop_ep(&mut self, handle: u32) {
        if let Some(i) = self.find(handle) {
            // Compact: shift the tail left.
            for j in i..self.n - 1 {
                self.eps[j] = self.eps[j + 1];
            }
            self.n -= 1;
            self.eps[self.n] = None;
        }
    }

    pub fn by_handle(&mut self, handle: u32) -> Option<&mut Endpoint> {
        let i = self.find(handle)?;
        self.eps[i].as_mut()
    }

    pub fn mark_active(&mut self, handle: u32) {
        if let Some(ep) = self.by_handle(handle) {
            ep.state = EndpointState::Active;
        }
    }

    pub fn is_console(&self, handle: u32) -> bool {
        handle == self.console
    }

    pub fn console(&self) -> u32 {
        self.console
    }

    /// Handles to wait on: console first, then adopted endpoints. Owned
    /// array so the caller can mutate the set without borrow conflicts.
    pub fn wait_list(&self) -> [u32; MAX_RD_ENDPOINTS + 1] {
        let mut list = [0u32; MAX_RD_ENDPOINTS + 1];
        list[0] = self.console;
        for (i, ep) in self.eps[..self.n].iter().enumerate() {
            if let Some(ep) = ep {
                list[i + 1] = ep.handle;
            }
        }
        list
    }

    pub fn wait_len(&self) -> usize {
        self.n + 1
    }

    fn find(&self, handle: u32) -> Option<usize> {
        (0..self.n).find(|&i| self.eps[i].map_or(false, |e| e.handle == handle))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn adopt_drop_roundtrip() {
        let mut s = EndpointSet::new(2);
        assert!(s.is_console(2));
        assert_eq!(s.wait_len(), 1);
        assert_eq!(s.wait_list()[0], 2);

        assert!(s.adopt(4));
        assert!(s.adopt(5));
        assert_eq!(s.wait_len(), 3);
        assert_eq!(s.wait_list(), [2, 4, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);

        // idempotent
        assert!(s.adopt(4));
        assert_eq!(s.wait_len(), 3);

        s.mark_active(5);
        assert_eq!(s.by_handle(5).unwrap().state, EndpointState::Active);
        assert_eq!(s.by_handle(4).unwrap().state, EndpointState::AwaitingHandshake);

        s.drop_ep(4);
        assert_eq!(s.wait_len(), 2);
        assert_eq!(s.wait_list(), [2, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        assert!(s.by_handle(4).is_none());
    }

    #[test]
    fn table_full_rejects() {
        let mut s = EndpointSet::new(2);
        for h in 0..MAX_RD_ENDPOINTS {
            assert!(s.adopt(100 + h as u32));
        }
        assert!(!s.adopt(999));
    }
}
