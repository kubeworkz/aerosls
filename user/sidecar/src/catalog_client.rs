//! CAT_* protocol client — the POSIX sidecar's §9.1 catalog RPC layer.
//!
//! The client half of `kernel/env_catalog.c`, and the counterpart of
//! `net_client.rs`: the same KWrap kernel handle, the same tag-and-park
//! round trip, the same rule that the FIRST thing read off a reply is its
//! status word. What differs is what the channel carries — four read verbs
//! against a partition-scoped view of the object catalog, and no handshake
//! gate (the service answers any well-formed CAT frame; `CAT_INFO` is the
//! handshake a client *chooses* to make first, so it can state the
//! partition it was registered under and compare it with its own).
//!
//! A refusal is data here, not just a number: §9.1 exists so that
//! "CAT_STAT refused by kernel.env.catalog — SYSTEM_METADATA is not tenant
//! data" is visible in the CALLER's transcript, so `CatRefusal` carries the
//! service's rendered text and the applet prints it verbatim.

extern crate alloc;

use alloc::vec::Vec;

use aerosls_proto::catalops::{
    CatPage, CatRefusal, CatResult, CatalogOps, CAT_ECLOSED, CAT_EKERNEL, CAT_EPROTO,
};
use aerosls_proto::kabi::{GrantedCap, Kernel, TIMEOUT_NONE};
use aerosls_proto::kwrap::KWrap;
use aerosls_proto::*;

/// The client's end of one catalog channel: the tenant's CHAN_W / CHAN_R
/// pair to `kernel.env.catalog`, minted from the manifest's `catalog` cap.
///
/// Endpoint-agnostic, like `NetClient`: any handle adopted from the BIB can
/// drive it. There is no per-object state to mirror — the kernel owns the
/// catalog and every answer is computed there, so this type is only the
/// wire.
pub struct CatalogClient<K: Kernel> {
    k: KWrap<K>,
    chan_w: u32,
    chan_r: u32,
    next_tag: u32,
}

impl<K: Kernel> CatalogClient<K> {
    pub fn new(k: KWrap<K>, chan_w: u32, chan_r: u32) -> Self {
        CatalogClient {
            k,
            chan_w,
            chan_r,
            next_tag: 1,
        }
    }

    /// The channel endpoints the catalog service answers on.
    pub fn chan_w(&self) -> u32 {
        self.chan_w
    }
    pub fn chan_r(&self) -> u32 {
        self.chan_r
    }

    fn next_tag(&mut self) -> u32 {
        let t = self.next_tag;
        self.next_tag = self.next_tag.wrapping_add(1);
        t
    }

    /// Send one verb and wait for its reply. Every public verb funnels
    /// through here so the status discipline is written once: parse the
    /// frame (magic + version, so a foreign frame is PROTOCOL, never a
    /// status), match the verb back against the tag's reply, then read the
    /// status at CAT_STATUS — clean answers and refusals alike.
    fn call(&mut self, ty: u16, body: &[u8]) -> CatResult<Vec<u8>> {
        const PARK_NS: u64 = 200_000_000; // 200ms
        const MAX_PARKS: u32 = 15; // 3s worst case: the service answers on
                                    // the next microkernel_service_poll()

        let tag = self.next_tag();
        let mut payload = [0u8; 256];
        if CatFrame::SIZE + body.len() > payload.len() {
            return Err(CatRefusal::new(
                CAT_EPROTO,
                "catalog: request body does not fit a frame",
            ));
        }
        payload[..CatFrame::SIZE].copy_from_slice(&CatFrame::new(ty).encode());
        payload[CatFrame::SIZE..CatFrame::SIZE + body.len()].copy_from_slice(body);
        self.k
            .send(
                self.chan_w,
                tag,
                0,
                &payload[..CatFrame::SIZE + body.len()],
                &[],
                TIMEOUT_NONE,
            )
            .map_err(|_| CatRefusal::new(CAT_EKERNEL, "catalog: channel send failed"))?;

        for _ in 0..MAX_PARKS {
            let mut buf = [0u8; ChanHeader::MAX_PAYLOAD];
            let mut caps = [GrantedCap::default(); ChanHeader::MAX_CAPS];
            match self.k.recv(self.chan_r, &mut buf, &mut caps) {
                Ok(rr) => {
                    if rr.kind != CH_KIND_MSG || rr.tag != tag {
                        continue; // someone else's message; leave it be
                    }
                    let reply = &buf[..rr.len];
                    // Type-check before trusting any field: a stale reply
                    // with this tag but another verb would otherwise be
                    // parsed as this one's body.
                    let frame = CatFrame::parse(reply).ok_or_else(|| {
                        CatRefusal::new(CAT_EPROTO, "catalog: reply is not a CAT frame")
                    })?;
                    if frame.ty != ty {
                        continue;
                    }
                    let status = cat_status(reply).ok_or_else(|| {
                        CatRefusal::new(CAT_EPROTO, "catalog: reply carries no status word")
                    })?;
                    if status != CAT_OK {
                        let text = cat_text(reply).unwrap_or(b"");
                        let s = core::str::from_utf8(text).unwrap_or("");
                        return Err(CatRefusal::new(status, s));
                    }
                    return Ok(Vec::from(reply));
                }
                Err(_) => {}
            }
            // Park until the service's next tick drains the request — a bare
            // yield loop gives the kernel's poll zero wall time, which is
            // the failure net_client.rs documents and this must not repeat.
            let _ = self.k.wait(&[self.chan_r], PARK_NS);
        }
        Err(CatRefusal::new(
            CAT_ECLOSED,
            "no reply from kernel.env.catalog within the park budget",
        ))
    }

    /// CAT_INFO: the handshake. Returns the service's ceilings and the
    /// partition the KERNEL registered this channel under — the identity
    /// every verb's boundary is computed from, offered so a tenant can
    /// verify it rather than assume it.
    pub fn info(&mut self) -> CatResult<CatInfo> {
        let reply = self.call(CAT_INFO, &[])?;
        parse_cat_info_body(&reply[CAT_BODY..])
            .ok_or_else(|| CatRefusal::new(CAT_EPROTO, "catalog: CAT_INFO body is short"))
    }

    /// CAT_LIST: one page of this partition's namespace. `page_size` must be
    /// 1..=CAT_LIST_PAGE_MAX — the service answers 0 or larger with
    /// CAT_ERR_SHAPE, and this client refuses the same values without a
    /// round trip.
    pub fn list(&mut self, page: u16, page_size: u16) -> CatResult<CatPage> {
        if page_size == 0 || page_size > CAT_LIST_PAGE_MAX {
            return Err(CatRefusal::new(
                CAT_ERR_SHAPE,
                "CAT_LIST page_size must be 1..=8",
            ));
        }
        let reply = self.call(CAT_LIST, &encode_cat_list_body(page, page_size))?;
        let body = &reply[CAT_BODY..];
        let head = parse_cat_list_head(body)
            .ok_or_else(|| CatRefusal::new(CAT_EPROTO, "catalog: CAT_LIST head is short"))?;
        if head.count > CAT_LIST_PAGE_MAX {
            return Err(CatRefusal::new(
                CAT_EPROTO,
                "catalog: CAT_LIST count exceeds its own page bound",
            ));
        }
        // The head says how many slots are real; entries past that are not
        // read, so a reply shorter than the head promises is a parse failure
        // rather than a partial name.
        let mut entries = [CatListEntry {
            obj_type: 0,
            tier: 0,
            name: [0u8; CAT_MAX_NAME],
            name_len: 0,
        }; CAT_LIST_PAGE_MAX as usize];
        for i in 0..head.count {
            entries[i as usize] = parse_cat_list_entry(body, i).ok_or_else(|| {
                CatRefusal::new(CAT_EPROTO, "catalog: CAT_LIST entry past the reply")
            })?;
        }
        Ok((head, entries))
    }

    /// CAT_STAT: one name's metadata. A foreign name answers the same
    /// NOENT a name that never had — the boundary, returned as the service
    /// states it.
    pub fn stat(&mut self, name: &[u8]) -> CatResult<CatStat> {
        let mut body = [0u8; 1 + CAT_MAX_NAME];
        let n = encode_cat_stat_body(name, &mut body).ok_or_else(|| {
            CatRefusal::new(CAT_ERR_SHAPE, "CAT_STAT name must be 1..=63 bytes")
        })?;
        let reply = self.call(CAT_STAT, &body[..n])?;
        parse_cat_stat_body(&reply[CAT_BODY..])
            .ok_or_else(|| CatRefusal::new(CAT_EPROTO, "catalog: CAT_STAT body is short"))
    }

    /// CAT_READ: one (object, key) from the legacy record path — the value
    /// only, which is what a shell prints.
    pub fn read(&mut self, name: &[u8], key: &[u8]) -> CatResult<Vec<u8>> {
        let value = self.read_value(name, key)?;
        Ok(value.value)
    }

    /// CAT_READ with the echoed key as well.
    pub fn read_value(&mut self, name: &[u8], key: &[u8]) -> CatResult<CatValue> {
        let mut body = [0u8; 2 + CAT_MAX_NAME + CAT_MAX_KEY];
        let n = encode_cat_read_body(name, key, &mut body).ok_or_else(|| {
            CatRefusal::new(
                CAT_ERR_SHAPE,
                "CAT_READ name must be 1..=63 bytes and key < 64",
            )
        })?;
        let reply = self.call(CAT_READ, &body[..n])?;
        parse_cat_read_body(&reply[CAT_BODY..])
            .ok_or_else(|| CatRefusal::new(CAT_EPROTO, "catalog: CAT_READ body is short"))
    }
}

/// The trait procmgr and the applets see. Kept as inherent methods above so
/// a caller holding the concrete client gets the same API; this impl is
/// what `Box<dyn CatalogOps>` dispatches through.
impl<K: Kernel> CatalogOps for CatalogClient<K> {
    fn cat_info(&mut self) -> CatResult<CatInfo> {
        self.info()
    }
    fn cat_list(&mut self, page: u16, page_size: u16) -> CatResult<CatPage> {
        self.list(page, page_size)
    }
    fn cat_stat(&mut self, name: &[u8]) -> CatResult<CatStat> {
        self.stat(name)
    }
    fn cat_read(&mut self, name: &[u8], key: &[u8]) -> CatResult<CatValue> {
        self.read_value(name, key)
    }
}

// ── tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::sync::Arc;
    use alloc::vec::Vec;
    use aerosls_proto::kabi::{CapInfo, ERR_STATE, RecvResult, SendCap};
    use aerosls_proto::Mutex;

    /// A kernel with one scripted conversation: `send` records the request
    /// verbatim — so a test can pin the bytes `env_catalog.c` would parse —
    /// and `recv` hands back the next scripted reply, echoing the tag the
    /// client sent it under. When the script runs out it answers ERR_STATE:
    /// the client then parks, which is the no-reply path, not a second
    /// answer.
    struct Scripted {
        sent: Mutex<Vec<u8>>,
        tag: Mutex<u32>,
        replies: Mutex<Vec<Vec<u8>>>,
    }

    impl Kernel for Scripted {
        fn wait(&self, _: &[u32], _: u64) -> Result<(usize, u16), i32> {
            Err(ERR_STATE)
        }
        fn recv(
            &self,
            _: u32,
            buf: &mut [u8],
            slots: &mut [GrantedCap],
        ) -> Result<RecvResult, i32> {
            let mut queue = self.replies.lock();
            if queue.is_empty() {
                return Err(ERR_STATE);
            }
            let reply = queue.remove(0);
            let tag = *self.tag.lock();
            buf[..reply.len()].copy_from_slice(&reply);
            for s in slots.iter_mut() {
                *s = GrantedCap::default();
            }
            Ok(RecvResult {
                kind: CH_KIND_MSG,
                flags: 0,
                tag,
                len: reply.len(),
                n_caps: 0,
            })
        }
        fn send(
            &self,
            _: u32,
            tag: u32,
            _: u16,
            payload: &[u8],
            _: &[SendCap],
            _: u64,
        ) -> Result<(), i32> {
            *self.sent.lock() = Vec::from(payload);
            *self.tag.lock() = tag;
            Ok(())
        }
        fn close(&self, _: u32, _: u16, _: u32) -> Result<(), i32> {
            Ok(())
        }
        fn cap_info(&self, _: u32) -> Result<CapInfo, i32> {
            Err(ERR_STATE)
        }
    }

    /// Little-endian u16 read, for pinning request bytes the service
    /// parses with `cat_get_u16()`.
    fn le16(b: &[u8], i: usize) -> u16 {
        (b[i] as u16) | ((b[i + 1] as u16) << 8)
    }

    /// A reply as the service builds one: header, optional error flag,
    /// status word at CAT_STATUS, body at CAT_BODY.
    fn reply(ty: u16, flags: u16, status: u16, body: &[u8]) -> Vec<u8> {
        let mut r = CatFrame::new(ty).encode().to_vec();
        r[12..14].copy_from_slice(&flags.to_le_bytes());
        r.extend_from_slice(&status.to_le_bytes());
        r.extend_from_slice(body);
        r
    }

    fn client_with(
        replies: Vec<Vec<u8>>,
    ) -> (Arc<Scripted>, CatalogClient<Scripted>) {
        let k = Arc::new(Scripted {
            sent: Mutex::new(Vec::new()),
            tag: Mutex::new(0),
            replies: Mutex::new(replies),
        });
        let client = CatalogClient::new(KWrap(k.clone()), 7, 8);
        (k, client)
    }

    /// CAT_INFO: the handshake answers the partition the KERNEL registered
    /// this channel under, and the request that asked is the bare frame the
    /// service's `cat_is_catalog_frame()` parses — magic, version, verb.
    #[test]
    fn info_carries_the_registered_partition_and_sends_a_bare_frame() {
        let mut body = [0u8; 16];
        body[0..4].copy_from_slice(&128u32.to_le_bytes()); // max_objects
        body[4..8].copy_from_slice(&64u32.to_le_bytes()); // max_name
        body[8..12].copy_from_slice(&256u32.to_le_bytes()); // max_value
        body[12..16].copy_from_slice(&5u32.to_le_bytes()); // partition
        let (k, mut client) = client_with(vec![reply(CAT_INFO, 0, CAT_OK, &body)]);

        let i = client.info().expect("INFO is answered");
        assert_eq!(
            (i.max_objects, i.max_name, i.max_value, i.partition),
            (128, 64, 256, 5)
        );

        let sent = k.sent.lock();
        assert_eq!(sent.len(), CatFrame::SIZE, "INFO carries no body");
        assert_eq!(&sent[..8], &CAT_MAGIC);
        assert_eq!(le16(&sent, 10), CAT_INFO);
    }

    /// THE status discipline, from the client's side of the wire: a
    /// two-object CAT_LIST whose FIRST body word is 2 must read as OK, not
    /// as CAT_ERR_PERM — the offset bug the kernel header records, which
    /// would otherwise be re-creatable here.
    #[test]
    fn a_two_object_list_reads_ok_not_perm() {
        let mut body = Vec::new();
        body.extend_from_slice(&2u16.to_le_bytes()); // count == 2
        body.extend_from_slice(&7u16.to_le_bytes()); // total
        body.extend_from_slice(&5u32.to_le_bytes()); // partition
        for (obj_type, name) in [(3u8, "app.tab"), (1u8, "sys.cfg")] {
            let mut e = [0u8; CAT_LIST_ENTRY];
            e[0] = obj_type;
            e[1] = 2; // tier
            e[4..4 + name.len()].copy_from_slice(name.as_bytes());
            body.extend_from_slice(&e);
        }
        let (k, mut client) = client_with(vec![reply(CAT_LIST, 0, CAT_OK, &body)]);

        let (head, entries) = client.list(0, CAT_LIST_PAGE_MAX).expect("served");
        assert_eq!((head.count, head.total, head.partition), (2, 7, 5));
        assert_eq!(entries[0].name_str(), "app.tab");
        assert_eq!(entries[1].name_str(), "sys.cfg");
        assert_eq!((entries[0].obj_type, entries[0].tier), (3, 2));

        // The request is {page u16, page_size u16} at CAT_HDR.
        let sent = k.sent.lock();
        assert_eq!(sent.len(), CatFrame::SIZE + 4);
        assert_eq!(sent[CAT_HDR], 0);
        assert_eq!(le16(&sent, CAT_HDR + 2), 8);
    }

    /// A refusal arrives with the service's rendered text, and §9.1's whole
    /// point is that the caller can print it verbatim.
    #[test]
    fn a_refusal_carries_the_services_own_rendered_text() {
        let text = b"CAT_STAT refused by kernel.env.catalog: SYSTEM_METADATA is not tenant data";
        let (k, mut client) =
            client_with(vec![reply(CAT_STAT, CAT_FLAG_ERROR, CAT_ERR_PERM, text)]);

        let e = client.stat(b"sys.meta").expect_err("PERM");
        assert_eq!(e.status, CAT_ERR_PERM);
        assert_eq!(e.as_str(), core::str::from_utf8(text).unwrap());

        // And the name it refused with is the name that was asked for:
        // {name_len u8, name[..]} at CAT_HDR.
        let sent = k.sent.lock();
        assert_eq!(sent[CAT_HDR], 8);
        assert_eq!(&sent[CAT_HDR + 1..CAT_HDR + 9], b"sys.meta");
    }

    /// A foreign frame is PROTOCOL, never a status: the client must not
    /// report another service's bytes as though the catalog had answered.
    #[test]
    fn a_foreign_frame_is_a_protocol_error_not_a_status() {
        let mut bytes = NetFrame::new(NET_INFO, false).encode().to_vec();
        bytes.extend_from_slice(&[0u8; 4]);
        let (_k, mut client) = client_with(vec![bytes]);

        let e = client.info().expect_err("not a CAT frame");
        assert_eq!(e.status, CAT_EPROTO);
    }

    /// Values the service would answer CAT_ERR_SHAPE are refused here
    /// without buying a round trip to learn them — and nothing is put on
    /// the wire while refusing, which is the whole point of refusing early.
    #[test]
    fn an_unservable_argument_never_reaches_the_wire() {
        let (k, mut client) = client_with(Vec::new());
        assert_eq!(client.list(0, 0).expect_err("0").status, CAT_ERR_SHAPE);
        assert_eq!(
            client.list(0, CAT_LIST_PAGE_MAX + 1).expect_err("too big").status,
            CAT_ERR_SHAPE
        );
        assert_eq!(client.stat(b"").expect_err("empty").status, CAT_ERR_SHAPE);
        assert_eq!(
            client.stat(&[b'x'; 64]).expect_err("too long").status,
            CAT_ERR_SHAPE
        );
        assert!(k.sent.lock().is_empty(), "nothing was put on the wire");
    }

    /// The trait object procmgr stores — the path `ctx.catalog()` takes and
    /// the `catalog` applet calls through — dispatches all four verbs, each
    /// answered by its own scripted reply.
    #[test]
    fn the_trait_object_the_applet_reaches_dispatches_all_four() {
        let mut info = [0u8; 16];
        info[12..16].copy_from_slice(&3u32.to_le_bytes());
        let mut list = Vec::new();
        list.extend_from_slice(&1u16.to_le_bytes());
        list.extend_from_slice(&1u16.to_le_bytes());
        list.extend_from_slice(&3u32.to_le_bytes());
        list.extend_from_slice(&[0u8; CAT_LIST_ENTRY]);
        let (k, mut client) = client_with(vec![
            reply(CAT_INFO, 0, CAT_OK, &info),
            reply(CAT_LIST, 0, CAT_OK, &list),
            reply(
                CAT_STAT,
                CAT_FLAG_ERROR,
                CAT_ERR_NOENT,
                b"CAT_STAT refused by kernel.env.catalog",
            ),
            reply(
                CAT_READ,
                CAT_FLAG_ERROR,
                CAT_ERR_TYPE,
                b"CAT_READ refused by kernel.env.catalog: rowstore-backed table",
            ),
        ]);

        {
            let ops: &mut dyn CatalogOps = &mut client;
            assert_eq!(ops.cat_info().expect("info").partition, 3);
            assert_eq!(ops.cat_list(0, 8).expect("list").0.count, 1);
            assert_eq!(
                ops.cat_stat(b"nope").expect_err("NOENT").status,
                CAT_ERR_NOENT
            );
            assert_eq!(
                ops.cat_read(b"t", b"k").expect_err("TYPE").status,
                CAT_ERR_TYPE
            );
        }
        let sent = k.sent.lock();
        assert_eq!(sent.len(), CatFrame::SIZE + 4, "CAT_READ's body is name+key");
        assert_eq!(le16(&sent, 10), CAT_READ, "the last verb on the wire");
    }
}