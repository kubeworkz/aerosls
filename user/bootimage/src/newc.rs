//! Minimal `newc` CPIO archive writer + parser.
//!
//! `newc` is the initrd format GRUB (Multiboot2 module), U-Boot (`-initrd` /
//! `bootm`), and QEMU (`-initrd`) all accept. The writer emits exactly the
//! subset the loader needs: regular files + the `TRAILER!!!` terminator,
//! 4-byte alignment, `c_check = 0` (newc has no checksum), UID/GID 0.
//! The parser is the test-side ground truth and the reference for the
//! kernel loader's walk (kernel/boot_image.h).
//!
//! Header layout (110 bytes, ASCII hex fields):
//! ```text
//! offset size  field
//! 0      6     magic "070701"
//! 6      8     c_ino
//! 14     8     c_mode        (0100644 for a regular file)
//! 22     8     c_uid
//! 30     8     c_gid
//! 38     8     c_nlink
//! 46     8     c_mtime
//! 54     8     c_filesize
//! 62     8     c_devmajor
//! 70     8     c_devminor
//! 78     8     c_rdevmajor
//! 86     8     c_rdevminor
//! 94     8     c_namesize    (name length INCLUDING the trailing NUL)
//! 102    8     c_check       (0)
//! ```
//! Followed by the NUL-terminated name padded to a 4-byte boundary, then
//! the file data padded to a 4-byte boundary.

/// Magic for a `newc` entry.
pub const NEWC_MAGIC: &[u8; 6] = b"070701";
/// The terminator entry's name.
pub const TRAILER: &str = "TRAILER!!!";
/// Regular-file mode (S_IFREG | 0644).
const MODE_FILE: u32 = 0o100644;

fn hex_field(out: &mut Vec<u8>, v: u64) {
    out.extend_from_slice(format!("{v:08x}").as_bytes());
}

fn align4(n: usize) -> usize {
    (n + 3) & !3
}

/// Append one file entry to the archive. `name` must not contain a NUL.
pub fn write_entry(out: &mut Vec<u8>, name: &str, data: &[u8]) {
    debug_assert!(!name.contains('\0'));
    let name_len = name.len() + 1; // including the terminator
    out.extend_from_slice(NEWC_MAGIC);
    hex_field(out, 0); // c_ino
    hex_field(out, MODE_FILE as u64); // c_mode
    hex_field(out, 0); // c_uid
    hex_field(out, 0); // c_gid
    hex_field(out, 1); // c_nlink
    hex_field(out, 0); // c_mtime
    hex_field(out, data.len() as u64); // c_filesize
    hex_field(out, 0); // c_devmajor
    hex_field(out, 0); // c_devminor
    hex_field(out, 0); // c_rdevmajor
    hex_field(out, 0); // c_rdevminor
    hex_field(out, name_len as u64); // c_namesize
    hex_field(out, 0); // c_check
    out.extend_from_slice(name.as_bytes());
    out.push(0);
    let name_pad = align4(name_len) - name_len;
    out.extend(std::iter::repeat(0).take(name_pad));
    out.extend_from_slice(data);
    let data_pad = align4(data.len()) - data.len();
    out.extend(std::iter::repeat(0).take(data_pad));
}

/// Append the `TRAILER!!!` terminator. The archive is complete once this
/// is written.
pub fn finish(out: &mut Vec<u8>) {
    write_entry(out, TRAILER, &[]);
}

/// One parsed archive entry.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub name: String,
    pub data: Vec<u8>,
}

/// Parse failures — all are "this is not a valid newc archive".
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NewcErr {
    /// Shorter than a header, or a header that doesn't start with `070701`.
    BadHeader,
    /// A field walk ran past the end of the archive.
    Truncated,
}

impl core::fmt::Display for NewcErr {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            NewcErr::BadHeader => write!(f, "bad newc header"),
            NewcErr::Truncated => write!(f, "truncated newc archive"),
        }
    }
}

/// Parse the archive into entries (in archive order), stopping at the
/// `TRAILER!!!` terminator. The archive must start at offset 0.
pub fn parse_archive(archive: &[u8]) -> Result<Vec<Entry>, NewcErr> {
    let mut entries = Vec::new();
    let mut off = 0usize;
    loop {
        if off + 110 > archive.len() {
            return Err(NewcErr::Truncated);
        }
        if &archive[off..off + 6] != NEWC_MAGIC {
            return Err(NewcErr::BadHeader);
        }
        let rd = |o: usize| -> Result<u64, NewcErr> {
            let s = core::str::from_utf8(&archive[off + o..off + o + 8])
                .map_err(|_| NewcErr::BadHeader)?;
            u64::from_str_radix(s, 16).map_err(|_| NewcErr::BadHeader)
        };
        let filesize = rd(54)? as usize;
        let namesize = rd(94)? as usize; // includes the NUL
        if namesize < 1 {
            return Err(NewcErr::BadHeader);
        }
        let name_pad = align4(namesize) - namesize;
        let data_pad = align4(filesize) - filesize;
        let name_end = off + 110 + namesize + name_pad;
        if name_end > archive.len() {
            return Err(NewcErr::Truncated);
        }
        let name = core::str::from_utf8(&archive[off + 110..off + 110 + namesize - 1])
            .map_err(|_| NewcErr::BadHeader)?
            .to_string();
        let data_start = name_end;
        let data_end = data_start + filesize + data_pad;
        if data_end > archive.len() {
            return Err(NewcErr::Truncated);
        }
        let data = archive[data_start..data_start + filesize].to_vec();
        if name == TRAILER {
            return Ok(entries);
        }
        entries.push(Entry { name, data });
        off = data_end;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_files_and_trailer() {
        let mut a = Vec::new();
        write_entry(&mut a, "boot/init.bin", &[1, 2, 3]);
        write_entry(&mut a, "boot/layout", b"a=b\n");
        finish(&mut a);
        let es = parse_archive(&a).unwrap();
        assert_eq!(es.len(), 2);
        assert_eq!(es[0].name, "boot/init.bin");
        assert_eq!(es[0].data, vec![1, 2, 3]);
        assert_eq!(es[1].name, "boot/layout");
        assert_eq!(es[1].data, b"a=b\n");
    }

    #[test]
    fn handles_padding_for_alignment() {
        // names "x" (2 bytes w/ NUL) and "abcdefgh" (9 bytes w/ NUL) force
        // both 4-byte name padding and 4-byte data padding.
        let mut a = Vec::new();
        write_entry(&mut a, "x", &[0u8; 5]);
        write_entry(&mut a, "abcdefgh", &[0u8; 2]);
        finish(&mut a);
        let es = parse_archive(&a).unwrap();
        assert_eq!(es[0].name, "x");
        assert_eq!(es[0].data.len(), 5);
        assert_eq!(es[1].name, "abcdefgh");
        assert_eq!(es[1].data.len(), 2);
    }

    #[test]
    fn rejects_garbage() {
        // Short of a full header -> truncated.
        assert!(matches!(
            parse_archive(b"not a cpio archive"),
            Err(NewcErr::Truncated)
        ));
        // A full-length header with a wrong magic -> bad header.
        let mut a = Vec::new();
        a.extend_from_slice(b"070700");
        a.extend_from_slice(&[0u8; 104]);
        assert!(matches!(parse_archive(&a), Err(NewcErr::BadHeader)));
        // Valid magic but truncated mid-header.
        let mut b = Vec::new();
        b.extend_from_slice(NEWC_MAGIC);
        b.extend_from_slice(&[0u8; 20]);
        assert!(matches!(parse_archive(&b), Err(NewcErr::Truncated)));
    }
}
