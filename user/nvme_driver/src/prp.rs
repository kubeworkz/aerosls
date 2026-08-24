//! Pure PRP-address arithmetic, ported 1:1 from `drivers/nvme_io.c`'s
//! `nvme_build_prp()` and `nvme_build_prp_gather()`.
//!
//! Why this is separated out and host-tested while the rest of the NVMe path
//! is not (the same argument as the C file's own header comment): the submit
//! path writes MMIO doorbells and polls a hardware completion queue, so it
//! cannot run on a host at all. PRP construction is the part where a mistake
//! is genuinely dangerous rather than merely broken — the controller DMAs to
//! whatever addresses the list names, so a wrong entry silently scribbles
//! over unrelated memory instead of failing loudly. It is pure arithmetic
//! over caller-supplied addresses, so it can be checked exhaustively here.
//!
//! The tests below are the Rust mirror of `tests/nvme_prp_host_test.c`
//! (which exercises the real C functions); keeping both suites aligned means
//! the two ports cannot drift.
//!
//! NVMe PRP rules encoded here:
//!   * every PRP entry after the first must have a zero page offset, so the
//!     whole buffer has to start page-aligned for a list to describe it;
//!   * one page  -> prp1 only, prp2 unused;
//!   * two pages -> prp2 is the SECOND PAGE ITSELF, never a pointer to a
//!     list (pointing prp2 at a list for a 2-page transfer makes the
//!     controller read the list page's first 8 bytes as data);
//!   * more      -> prp2 points at a list of the remaining (page_count - 1)
//!     page addresses, up to NVME_MAX_PAGES_PER_XFER (32; a 4 KiB list page
//!     holds 512 entries, so multi-page-list chaining cannot arise).

/// 4 KiB NVMe page size (drivers/nvme_io.h `NVME_PAGE_SIZE`).
pub const NVME_PAGE_SIZE: u32 = 4096;
/// 512-byte sectors per 4 KiB page (drivers/nvme_io.h `NVME_SECTORS_PER_PAGE`).
pub const NVME_SECTORS_PER_PAGE: u32 = 8;
/// Per-command page cap (drivers/nvme_io.h `NVME_MAX_PAGES_PER_XFER`,
/// 128 KiB; rationale there: below the smallest MDTS in practical use and
/// far under the 512-entry limit of a single PRP list page).
pub const NVME_MAX_PAGES_PER_XFER: u32 = 32;

/// PRP construction failures. All correspond to a C `nvme_build_prp` return
/// of 1; none of them can happen through the server's validation, so the
/// caller treats them as internal-invariant violations.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PrpErr {
    /// `page_count` is 0 or above the cap.
    BadCount,
    /// The buffer is not 4 KiB-aligned (a PRP list cannot describe it).
    Unaligned,
    /// A needed PRP list page was not supplied (too short for the transfer).
    MissingList,
    /// A gather page entry is NULL or not 4 KiB-aligned.
    BadPage,
}

/// Build PRPs for one physically contiguous, page-aligned buffer. Fills
/// `prp_list` with `(page_count - 1)` entries when `page_count > 2`; the
/// slice must hold at least that many. Returns `(prp1, prp2)`.
pub fn build_prp(
    buf_phys: u64,
    page_count: u32,
    prp_list: &mut [u64],
) -> Result<(u64, u64), PrpErr> {
    if page_count == 0 || page_count > NVME_MAX_PAGES_PER_XFER {
        return Err(PrpErr::BadCount);
    }
    if buf_phys & (NVME_PAGE_SIZE as u64 - 1) != 0 {
        return Err(PrpErr::Unaligned);
    }

    let prp1 = buf_phys;
    if page_count == 1 {
        return Ok((prp1, 0)); // no second entry for a single page
    }
    if page_count == 2 {
        return Ok((prp1, buf_phys + NVME_PAGE_SIZE as u64)); // second page directly
    }

    if prp_list.len() < (page_count - 1) as usize {
        return Err(PrpErr::MissingList);
    }
    for (i, e) in prp_list
        .iter_mut()
        .enumerate()
        .take((page_count - 1) as usize)
    {
        *e = buf_phys + (i as u64 + 1) * NVME_PAGE_SIZE as u64;
    }
    // In the shared-address-space MVP the list page's address is its
    // physical address (identity mapping) — exactly what the controller's
    // prp2 wants, and what the C driver put there (`(uintptr_t)prp_list_page`).
    Ok((prp1, prp_list.as_ptr() as u64))
}

/// Build PRPs for a scatter list: `pages[i]` is the physical address of page
/// `i`. A PRP list is natively a scatter list — every entry is an independent
/// page address and nothing requires them to be consecutive — so no copying
/// is needed to describe non-contiguous buffers (this is how the C driver's
/// `nvme_read_pages_gather_sync` worked). Fills `prp_list` with
/// `(page_count - 1)` entries when `page_count > 2`.
pub fn build_prp_gather(pages: &[u64], prp_list: &mut [u64]) -> Result<(u64, u64), PrpErr> {
    let page_count = pages.len() as u32;
    if page_count == 0 || page_count > NVME_MAX_PAGES_PER_XFER {
        return Err(PrpErr::BadCount);
    }
    // Every entry must be a real, page-aligned address: an unaligned or NULL
    // page would make the controller DMA somewhere it was never told to.
    for &p in pages {
        if p == 0 || p & (NVME_PAGE_SIZE as u64 - 1) != 0 {
            return Err(PrpErr::BadPage);
        }
    }

    let prp1 = pages[0];
    if page_count == 1 {
        return Ok((prp1, 0));
    }
    if page_count == 2 {
        return Ok((prp1, pages[1])); // second page directly
    }

    if prp_list.len() < pages.len() - 1 {
        return Err(PrpErr::MissingList);
    }
    for (i, e) in prp_list.iter_mut().enumerate().take(pages.len() - 1) {
        *e = pages[i + 1];
    }
    Ok((prp1, prp_list.as_ptr() as u64))
}

#[cfg(test)]
mod tests {
    use super::*;

    const PAGE: u64 = NVME_PAGE_SIZE as u64;

    #[test]
    fn single_page() {
        let mut list = [0xEEEE_EEEE_EEEE_EEEEu64; 8];
        let (prp1, prp2) = build_prp(0x2000_00, 1, &mut list).unwrap();
        assert_eq!(prp1, 0x2000_00);
        assert_eq!(prp2, 0, "a single page needs no second entry");
        assert_eq!(
            list[0], 0xEEEE_EEEE_EEEE_EEEE,
            "the PRP list page is left completely untouched"
        );
    }

    #[test]
    fn two_pages_second_page_directly() {
        // The case most easily got wrong: pointing prp2 at a list for a
        // 2-page transfer makes the controller read the list page's first
        // 8 bytes as data.
        let mut list = [0xEEEE_EEEE_EEEE_EEEEu64; 8];
        let (prp1, prp2) = build_prp(0x2000_00, 2, &mut list).unwrap();
        assert_eq!(prp1, 0x2000_00);
        assert_eq!(prp2, 0x2000_00 + PAGE, "prp2 is the second page itself");
        assert_eq!(list[0], 0xEEEE_EEEE_EEEE_EEEE, "still no list page used");
    }

    #[test]
    fn three_pages_smallest_list() {
        let mut list = [0u64; 8];
        let (prp1, prp2) = build_prp(0x2000_00, 3, &mut list).unwrap();
        assert_eq!(prp1, 0x2000_00);
        assert_eq!(prp2, list.as_ptr() as u64, "prp2 points at the list page");
        assert_eq!(list[0], 0x2000_00 + PAGE);
        assert_eq!(list[1], 0x2000_00 + 2 * PAGE);
        assert_eq!(list[2], 0, "exactly page_count-1 entries written");
    }

    #[test]
    fn max_batch() {
        let mut list = [0u64; 512];
        let (prp1, prp2) = build_prp(0x2000_00, NVME_MAX_PAGES_PER_XFER, &mut list).unwrap();
        assert_eq!(prp1, 0x2000_00);
        assert_eq!(prp2, list.as_ptr() as u64);
        for i in 0..NVME_MAX_PAGES_PER_XFER as usize - 1 {
            assert_eq!(list[i], 0x2000_00 + (i as u64 + 1) * PAGE);
            assert_eq!(list[i] & (PAGE - 1), 0, "every entry page-aligned");
        }
        assert_eq!(
            list[NVME_MAX_PAGES_PER_XFER as usize - 1],
            0,
            "nothing written past entry 30 — no off-by-one overrun"
        );
    }

    #[test]
    fn rejections() {
        let mut list = [0u64; 8];
        // Each of these would be a silent memory-corruption bug if accepted.
        assert_eq!(build_prp(0x2000_00, 0, &mut list), Err(PrpErr::BadCount));
        assert_eq!(
            build_prp(0x2000_00, NVME_MAX_PAGES_PER_XFER + 1, &mut list),
            Err(PrpErr::BadCount),
            "rejects above the cap rather than truncating"
        );
        assert_eq!(
            build_prp(0x2000_01, 4, &mut list),
            Err(PrpErr::Unaligned)
        );
        assert_eq!(
            build_prp(0x2000_00 + 2048, 2, &mut list),
            Err(PrpErr::Unaligned),
            "rejects a half-page-offset buffer even in the 2-page no-list case"
        );
        assert_eq!(
            build_prp(0x2000_00, 4, &mut []),
            Err(PrpErr::MissingList),
            "rejects >2 pages with no list page supplied"
        );
        // ...but 1 and 2 pages need no list.
        assert!(build_prp(0x2000_00, 1, &mut []).is_ok());
        assert!(build_prp(0x2000_00, 2, &mut []).is_ok());
        // A too-short list (e.g. 2 entries for 4 pages) is also refused.
        let mut short = [0u64; 2];
        assert_eq!(build_prp(0x2000_00, 4, &mut short), Err(PrpErr::MissingList));
    }

    #[test]
    fn gather() {
        let pages = [0x3000_00, 0x4000_00, 0x5000_00];
        let mut list = [0u64; 8];

        // 1 page: prp1 only.
        let (p1, p2) = build_prp_gather(&pages[..1], &mut list).unwrap();
        assert_eq!((p1, p2), (0x3000_00, 0));

        // 2 pages: prp2 is the second page directly.
        let (p1, p2) = build_prp_gather(&pages[..2], &mut list).unwrap();
        assert_eq!((p1, p2), (0x3000_00, 0x4000_00));
        assert_eq!(list[0], 0, "still no list used");

        // 3 pages: list carries the tail.
        let (p1, p2) = build_prp_gather(&pages, &mut list).unwrap();
        assert_eq!(p1, 0x3000_00);
        assert_eq!(p2, list.as_ptr() as u64);
        assert_eq!(&list[..2], &[0x4000_00, 0x5000_00]);
        assert_eq!(list[2], 0);

        // Gather accepts NON-contiguous pages — that is its whole point.
        let scattered = [0x3000_00, 0x9000_00, 0x7000_00];
        let (_, p2) = build_prp_gather(&scattered, &mut list).unwrap();
        assert_eq!(p2, list.as_ptr() as u64);
        assert_eq!(&list[..2], &[0x9000_00, 0x7000_00]);
    }

    #[test]
    fn gather_rejections() {
        let mut list = [0u64; 8];
        assert_eq!(build_prp_gather(&[], &mut list), Err(PrpErr::BadCount));
        assert_eq!(build_prp_gather(&[0x2000_00], &mut []).unwrap(), (0x2000_00, 0));
        // NULL page entry.
        assert_eq!(build_prp_gather(&[0x2000_00, 0], &mut list), Err(PrpErr::BadPage));
        // Unaligned page entry.
        assert_eq!(
            build_prp_gather(&[0x2000_00, 0x2000_01], &mut list),
            Err(PrpErr::BadPage)
        );
        // >32 pages.
        let big: Vec<u64> = (0..NVME_MAX_PAGES_PER_XFER + 1)
            .map(|i| 0x1_0000_0000 + i as u64 * PAGE)
            .collect();
        assert_eq!(build_prp_gather(&big, &mut list), Err(PrpErr::BadCount));
        // Too-short list for 3 pages.
        let mut short = [0u64; 1];
        assert_eq!(
            build_prp_gather(&[0x3000_00, 0x4000_00, 0x5000_00], &mut short),
            Err(PrpErr::MissingList)
        );
    }
}
