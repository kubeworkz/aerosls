//! Catalog operations trait — the interface the proc manager uses for the
//! §9.1 mediated catalog without depending on a concrete `CatalogClient`.
//!
//! The sidecar crate provides `CatalogClient<K, A>` which implements this
//! trait; procmgr stores `Box<dyn CatalogOps>` so an applet can read the
//! partition's namespace from `ctx.catalog()` the same way it reaches the
//! network driver from `ctx.net()` — and for the same reason: procmgr must
//! not depend on the crate that implements the wire.
//!
//! The error is not a bare status. §9.1's contract is that a refusal is
//! *visible in the caller's transcript* — "CAT_STAT refused by
//! kernel.env.catalog — SYSTEM_METADATA is not tenant data" — so the error
//! carries the service's rendered text alongside the status, and only falls
//! back to a synthesized line where no service answered to render one.

use crate::{
    CatInfo, CatListEntry, CatListHead, CatStat, CatValue, CAT_LIST_PAGE_MAX,
    CAT_TEXT_CAP,
};

/// Client-side statuses: conditions where no service rendered a refusal.
/// Deliberately outside the service's own range (0..=9), so a synthesized
/// line can never be mistaken for an answer kernel.env.catalog gave.
pub const CAT_EPROTO: u16 = 0xFFFE; // malformed or foreign reply
pub const CAT_EKERNEL: u16 = 0xFFFD; // the channel send/recv itself failed
pub const CAT_ECLOSED: u16 = 0xFFFC; // no reply within the park budget
pub const CAT_ENOCHAN: u16 = 0xFFFB; // the manifest declared no catalog cap

/// Why a catalog call did not return data: the service's status plus the
/// refusal it rendered, NUL-free and truncated to the kernel's own cap.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CatRefusal {
    pub status: u16,
    pub text: [u8; CAT_TEXT_CAP],
    pub text_len: u8,
}

impl CatRefusal {
    /// Build a refusal from a status and a printable reason. Used both for
    /// the service's rendered text (copied verbatim) and for the three
    /// client-side statuses, whose reason is this file's own wording.
    pub fn new(status: u16, reason: &str) -> CatRefusal {
        let mut text = [0u8; CAT_TEXT_CAP];
        let n = reason.as_bytes().len().min(CAT_TEXT_CAP - 1);
        text[..n].copy_from_slice(&reason.as_bytes()[..n]);
        CatRefusal {
            status,
            text,
            text_len: n as u8,
        }
    }

    /// The refusal as text, for a transcript. Never empty: a status with no
    /// rendered reason still prints its name, which is the whole point of
    /// surfacing a refusal rather than swallowing it.
    pub fn as_str(&self) -> &str {
        let s = core::str::from_utf8(&self.text[..self.text_len as usize]).unwrap_or("");
        if s.is_empty() {
            crate::cat_status_name(self.status)
        } else {
            s
        }
    }
}

pub type CatResult<T> = Result<T, CatRefusal>;

/// A page of CAT_LIST entries: the head says how many of these slots are
/// real (and how many the namespace holds in total), so a fixed array can
/// carry the reply with no allocation and no length negotiation.
pub type CatPage = (CatListHead, [CatListEntry; CAT_LIST_PAGE_MAX as usize]);

/// The four verbs of §9.1's mediated surface, in the order the section
/// names them. There is deliberately no write method: the verb does not
/// exist in the kernel's table, and a trait that offered one would be
/// promising something the service answers CAT_ERR_UNKNOWN.
pub trait CatalogOps {
    /// CAT_INFO — the handshake: the real ceilings and the partition the
    /// kernel computed this caller's boundary from.
    fn cat_info(&mut self) -> CatResult<CatInfo>;

    /// CAT_LIST — one page of this partition's namespace, plus the totals
    /// the page was cut from.
    fn cat_list(&mut self, page: u16, page_size: u16) -> CatResult<CatPage>;

    /// CAT_STAT — one name's metadata. A foreign name and a name that never
    /// existed both come back as the same NOENT refusal; that indistinguishability
    /// is the boundary, not a loss of information.
    fn cat_stat(&mut self, name: &[u8]) -> CatResult<CatStat>;

    /// CAT_READ — one (object, key) pair from the legacy record path.
    fn cat_read(&mut self, name: &[u8], key: &[u8]) -> CatResult<CatValue>;
}
