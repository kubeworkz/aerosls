//! ramfs — the in-memory filesystem for `/tmp` (Phase 2 design §3.2,
//! respawn decision §6).
//!
//! Sidecar-internal and **never stale**: it is core memory, not a device.
//! All files live in a `BTreeMap` keyed by inode; directories are trees of
//! child links. The VFS layer maps this onto the same syscall surface as
//! aerofs-lite (`lookup`/`read`/`write`/`stat`/`read_dir`/...).

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::string::ToString;
use alloc::vec::Vec;

use crate::aerofs::{FileType, S_IFDIR, S_IFREG};
use crate::errno::{DirEnt, Errno, Stat};

/// Inode 1 is the ramfs root (a separate namespace from aerofs-lite).
pub const ROOT_INO: u64 = 1;

pub struct RamFs {
    nodes: BTreeMap<u64, RamNode>,
    next_ino: u64,
}

struct RamNode {
    mode: u16,
    uid: u16,
    gid: u16,
    mtime: u32,
    /// File contents. For directories: unused (children hold the tree).
    data: Vec<u8>,
    children: BTreeMap<String, u64>,
    parent: u64,
}

impl RamFs {
    pub fn new() -> RamFs {
        let mut nodes = BTreeMap::new();
        nodes.insert(
            ROOT_INO,
            RamNode {
                mode: S_IFDIR | 0o755,
                uid: 0,
                gid: 0,
                mtime: 1,
                data: Vec::new(),
                children: BTreeMap::new(),
                parent: ROOT_INO,
            },
        );
        RamFs {
            nodes,
            next_ino: ROOT_INO + 1,
        }
    }

    /// Look up a path (already normalized, absolute) component by component
    /// from the root. Returns the inode and type.
    pub fn lookup(&self, comps: &[&str]) -> Result<(u64, FileType), Errno> {
        let mut cur = ROOT_INO;
        for (i, c) in comps.iter().enumerate() {
            let node = self.nodes.get(&cur).ok_or(Errno::EInval)?;
            match *c {
                "." => continue,
                ".." => {
                    cur = node.parent;
                    continue;
                }
                c => {
                    let child = node.children.get(c).ok_or(Errno::ENoent)?;
                    let last = i + 1 == comps.len();
                    let ty = self.nodes.get(child).ok_or(Errno::EInval)?.ty()?;
                    if !last && ty != FileType::Dir {
                        return Err(Errno::ENotdir);
                    }
                    cur = *child;
                }
            }
        }
        Ok((cur, self.nodes.get(&cur).ok_or(Errno::EInval)?.ty()?))
    }

    pub fn read(&self, ino: u64, offset: u64, buf: &mut [u8]) -> Result<usize, Errno> {
        let node = self.nodes.get(&ino).ok_or(Errno::ENoent)?;
        if node.ty()? != FileType::File {
            return Err(Errno::EIsdir);
        }
        let size = node.data.len() as u64;
        if offset >= size {
            return Ok(0);
        }
        let n = core::cmp::min(buf.len() as u64, size - offset) as usize;
        buf[..n].copy_from_slice(&node.data[offset as usize..offset as usize + n]);
        Ok(n)
    }

    /// Write at an offset; grows the file with zero-filled holes when
    /// writing past the end (POSIX semantics).
    pub fn write(&mut self, ino: u64, offset: u64, buf: &[u8]) -> Result<usize, Errno> {
        let node = self.nodes.get_mut(&ino).ok_or(Errno::ENoent)?;
        if node.ty()? != FileType::File {
            return Err(Errno::EIsdir);
        }
        let end = offset
            .checked_add(buf.len() as u64)
            .ok_or(Errno::ENospc)?;
        if end > node.data.len() as u64 {
            node.data.resize(end as usize, 0);
        }
        node.data[offset as usize..end as usize].copy_from_slice(buf);
        node.mtime += 1;
        Ok(buf.len())
    }

    pub fn truncate(&mut self, ino: u64, len: u64) -> Result<(), Errno> {
        let node = self.nodes.get_mut(&ino).ok_or(Errno::ENoent)?;
        if node.ty()? != FileType::File {
            return Err(Errno::EIsdir);
        }
        node.data.resize(len as usize, 0);
        node.mtime += 1;
        Ok(())
    }

    pub fn stat(&self, ino: u64) -> Result<Stat, Errno> {
        let node = self.nodes.get(&ino).ok_or(Errno::ENoent)?;
        Ok(Stat {
            mode: node.mode,
            uid: node.uid,
            gid: node.gid,
            size: node.data.len() as u64,
            mtime: node.mtime,
            ty: node.ty()?,
        })
    }

    pub fn read_dir(&self, ino: u64) -> Result<Vec<DirEnt>, Errno> {
        let node = self.nodes.get(&ino).ok_or(Errno::ENoent)?;
        if node.ty()? != FileType::Dir {
            return Err(Errno::ENotdir);
        }
        let mut out = Vec::new();
        out.push(DirEnt {
            name: String::from("."),
            ino,
            ty: FileType::Dir,
        });
        out.push(DirEnt {
            name: String::from(".."),
            ino: node.parent,
            ty: FileType::Dir,
        });
        for (name, cino) in &node.children {
            let cnode = self.nodes.get(cino).ok_or(Errno::EInval)?;
            out.push(DirEnt {
                name: name.clone(),
                ino: *cino,
                ty: cnode.ty()?,
            });
        }
        Ok(out)
    }

    pub fn create_file(
        &mut self,
        parent: u64,
        name: &str,
        mode: u16,
        uid: u16,
        gid: u16,
    ) -> Result<u64, Errno> {
        self.check_name(name)?;
        let pnode = self.nodes.get_mut(&parent).ok_or(Errno::ENoent)?;
        if pnode.ty()? != FileType::Dir {
            return Err(Errno::ENotdir);
        }
        if pnode.children.contains_key(name) {
            return Err(Errno::EExist);
        }
        let ino = self.next_ino;
        self.next_ino += 1;
        self.nodes.insert(
            ino,
            RamNode {
                mode: S_IFREG | (mode & 0o7777),
                uid,
                gid,
                mtime: 1,
                data: Vec::new(),
                children: BTreeMap::new(),
                parent,
            },
        );
        self.nodes
            .get_mut(&parent)
            .unwrap()
            .children
            .insert(name.to_string(), ino);
        Ok(ino)
    }

    pub fn create_dir(
        &mut self,
        parent: u64,
        name: &str,
        mode: u16,
        uid: u16,
        gid: u16,
    ) -> Result<u64, Errno> {
        self.check_name(name)?;
        let pnode = self.nodes.get_mut(&parent).ok_or(Errno::ENoent)?;
        if pnode.ty()? != FileType::Dir {
            return Err(Errno::ENotdir);
        }
        if pnode.children.contains_key(name) {
            return Err(Errno::EExist);
        }
        let ino = self.next_ino;
        self.next_ino += 1;
        self.nodes.insert(
            ino,
            RamNode {
                mode: S_IFDIR | (mode & 0o7777),
                uid,
                gid,
                mtime: 1,
                data: Vec::new(),
                children: BTreeMap::new(),
                parent,
            },
        );
        self.nodes
            .get_mut(&parent)
            .unwrap()
            .children
            .insert(name.to_string(), ino);
        Ok(ino)
    }

    /// Remove a regular file.
    pub fn unlink(&mut self, parent: u64, name: &str) -> Result<(), Errno> {
        let ino = *self
            .nodes
            .get(&parent)
            .ok_or(Errno::ENoent)?
            .children
            .get(name)
            .ok_or(Errno::ENoent)?;
        let ty = self.nodes.get(&ino).ok_or(Errno::EInval)?.ty()?;
        if ty == FileType::Dir {
            return Err(Errno::EIsdir);
        }
        self.nodes.remove(&ino);
        self.nodes
            .get_mut(&parent)
            .unwrap()
            .children
            .remove(name);
        Ok(())
    }

    /// Remove an empty directory.
    pub fn rmdir(&mut self, parent: u64, name: &str) -> Result<(), Errno> {
        let ino = *self
            .nodes
            .get(&parent)
            .ok_or(Errno::ENoent)?
            .children
            .get(name)
            .ok_or(Errno::ENoent)?;
        let node = self.nodes.get(&ino).ok_or(Errno::EInval)?;
        if node.ty()? != FileType::Dir {
            return Err(Errno::ENotdir);
        }
        if !node.children.is_empty() {
            return Err(Errno::ENotempty);
        }
        self.nodes.remove(&ino);
        self.nodes
            .get_mut(&parent)
            .unwrap()
            .children
            .remove(name);
        Ok(())
    }

    /// Rename: move `old_name` from `old_parent` to `new_parent` as
    /// `new_name`.  If `new_name` already exists it is overwritten
    /// (POSIX `rename(2)` semantics).  Cross-directory moves are
    /// supported within the same RamFs.
    pub fn rename(&mut self, old_parent: u64, old_name: &str, new_parent: u64, new_name: &str) -> Result<(), Errno> {
        // Look up the source entry.
        let ino = *self
            .nodes
            .get(&old_parent)
            .ok_or(Errno::ENoent)?
            .children
            .get(old_name)
            .ok_or(Errno::ENoent)?;
        // Remove from old parent.
        self.nodes.get_mut(&old_parent).unwrap().children.remove(old_name);
        // If the target already exists, remove it first (POSIX overwrite).
        if let Some(&target_ino) = self.nodes.get(&new_parent).and_then(|n| n.children.get(new_name)) {
            self.nodes.remove(&target_ino);
        }
        // Insert into new parent.
        self.nodes.get_mut(&new_parent).ok_or(Errno::ENoent)?
            .children.insert(new_name.to_string(), ino);
        Ok(())
    }

    pub fn stale(&self) -> bool {
        false
    }

    fn check_name(&self, name: &str) -> Result<(), Errno> {
        if name.is_empty()
            || name == "."
            || name == ".."
            || name.contains('/')
            || name.as_bytes().len() > crate::aerofs::NAME_MAX
        {
            return Err(Errno::ENametoolong);
        }
        Ok(())
    }
}

impl Default for RamFs {
    fn default() -> Self {
        Self::new()
    }
}

impl RamNode {
    fn ty(&self) -> Result<FileType, Errno> {
        match self.mode & crate::aerofs::S_IFMT {
            S_IFREG => Ok(FileType::File),
            S_IFDIR => Ok(FileType::Dir),
            _ => Err(Errno::EInval),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::aerofs::FileType;

    #[test]
    fn create_write_read_roundtrip() {
        let mut fs = RamFs::new();
        let parent = ROOT_INO;
        let ino = fs.create_file(parent, "hello", 0o644, 0, 0).unwrap();
        let n = fs.write(ino, 0, b"hello world").unwrap();
        assert_eq!(n, 11);
        let mut buf = [0u8; 32];
        let n = fs.read(ino, 0, &mut buf).unwrap();
        assert_eq!(&buf[..n], b"hello world");
        assert_eq!(fs.stat(ino).unwrap().size, 11);
    }

    #[test]
    fn write_past_eof_extends_with_zeros() {
        let mut fs = RamFs::new();
        let ino = fs.create_file(ROOT_INO, "sparse", 0o644, 0, 0).unwrap();
        fs.write(ino, 10, b"xy").unwrap();
        assert_eq!(fs.stat(ino).unwrap().size, 12);
        let mut buf = [0u8; 12];
        let n = fs.read(ino, 0, &mut buf).unwrap();
        assert_eq!(n, 12);
        assert_eq!(&buf[..10], &[0u8; 10]);
        assert_eq!(&buf[10..], b"xy");
    }

    #[test]
    fn lookup_and_parents() {
        let mut fs = RamFs::new();
        let d = fs.create_dir(ROOT_INO, "etc", 0o755, 0, 0).unwrap();
        let f = fs.create_file(d, "passwd", 0o644, 0, 0).unwrap();
        fs.write(f, 0, b"root:x").unwrap();
        let (ino, ty) = fs.lookup(&["etc", "passwd"]).unwrap();
        assert_eq!(ino, f);
        assert_eq!(ty, FileType::File);
        // Walking THROUGH a regular file is ENOTDIR, even for ".." — the
        // same resolution rule Linux applies to open("file/..").
        assert_eq!(fs.lookup(&["etc", "passwd", ".."]), Err(Errno::ENotdir));
        let (pino, _) = fs.lookup(&["etc", ".."]).unwrap();
        assert_eq!(pino, ROOT_INO);
        let (root, _) = fs.lookup(&[".."]).unwrap();
        assert_eq!(root, ROOT_INO);
        assert_eq!(fs.lookup(&["nope"]), Err(Errno::ENoent));
        assert_eq!(fs.lookup(&["etc", "passwd", "x"]), Err(Errno::ENotdir));
    }

    #[test]
    fn unlink_and_rmdir_rules() {
        let mut fs = RamFs::new();
        let f = fs.create_file(ROOT_INO, "f", 0o644, 0, 0).unwrap();
        let _ = f;
        let d = fs.create_dir(ROOT_INO, "d", 0o755, 0, 0).unwrap();
        fs.create_file(d, "x", 0o644, 0, 0).unwrap();
        // Non-empty dir cannot be removed.
        assert_eq!(fs.rmdir(ROOT_INO, "d"), Err(Errno::ENotempty));
        fs.unlink(d, "x").unwrap();
        fs.rmdir(ROOT_INO, "d").unwrap();
        assert_eq!(fs.lookup(&["d"]), Err(Errno::ENoent));
        // unlink of a dir is rejected.
        assert_eq!(fs.unlink(ROOT_INO, "d"), Err(Errno::ENoent)); // already gone
        let d2 = fs.create_dir(ROOT_INO, "d2", 0o755, 0, 0).unwrap();
        let _ = d2;
        assert_eq!(fs.unlink(ROOT_INO, "d2"), Err(Errno::EIsdir));
    }

    #[test]
    fn duplicate_names_rejected() {
        let mut fs = RamFs::new();
        fs.create_file(ROOT_INO, "a", 0o644, 0, 0).unwrap();
        assert_eq!(fs.create_file(ROOT_INO, "a", 0o644, 0, 0), Err(Errno::EExist));
        assert_eq!(
            fs.create_file(ROOT_INO, "a/b", 0o644, 0, 0),
            Err(Errno::ENametoolong)
        );
    }
}
