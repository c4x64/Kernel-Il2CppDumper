use std::fs;
use std::path::Path;

/// Physical-address map captured from a live process (lxgr_dump --pamap).
///
/// The file is a list of 4 KiB page mappings:
///     # VA PA
///     0x7934777000 0x553601000
///     ...
/// Both addresses are page-aligned. PA is the host-side physical address of
/// the page frame, so physical address = page_pa + (va & 0xFFF).
///
/// `base` is the in-process base VA of libil2cpp.so (ASLR-dependent); method
/// RVAs in the dump are image-relative, so in-process VA = base + rva.
#[derive(Clone)]
pub struct PhysMap {
    /// page-aligned VA -> page-aligned PA, sorted by VA
    entries: Vec<(u64, u64)>,
    base: u64,
}

impl PhysMap {
    /// Load a pamap file produced by `lxgr_dump --pamap`. Returns None if the
    /// file cannot be read or `base` is 0.
    pub fn load(path: &Path, base: u64) -> Option<Self> {
        if base == 0 {
            return None;
        }
        let data = fs::read(path).ok()?;
        let mut entries = Vec::new();
        for line in data.split(|&b| b == b'\n') {
            let s = match std::str::from_utf8(line) {
                Ok(s) => s.trim(),
                Err(_) => continue,
            };
            if s.is_empty() || s.starts_with('#') {
                continue;
            }
            let mut it = s.split_whitespace();
            let va = it.next().and_then(|x| parse_hex(x))?;
            let pa = it.next().and_then(parse_hex)?;
            entries.push((va, pa));
        }
        if entries.is_empty() {
            return None;
        }
        // keep sorted (they usually are) for binary search
        entries.sort_unstable_by_key(|&(v, _)| v);
        Some(PhysMap {
            entries,
            base,
        })
    }

    /// Number of page mappings loaded.
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Convert an image-relative RVA to a host physical address.
    /// Returns None if the page isn't covered by the pamap.
    pub fn pa_from_rva(&self, rva: u64) -> Option<u64> {
        let va = self.base.wrapping_add(rva);
        let off = va & 0xFFF;
        let page = va & !0xFFF;
        let idx = self.entries.binary_search_by_key(&page, |&(v, _)| v).ok()?;
        Some(self.entries[idx].1 | off)
    }
}

fn parse_hex(s: &str) -> Option<u64> {
    let t = s.trim().trim_start_matches("0x").trim_start_matches("0X");
    if t.is_empty() {
        return None;
    }
    u64::from_str_radix(t, 16).ok()
}
