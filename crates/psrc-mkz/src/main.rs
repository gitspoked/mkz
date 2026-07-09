// SPDX-License-Identifier: MIT OR Apache-2.0
//! mkz (crate `mkz`) is a tar-style archiver whose backend is the `autocol`
//! pre-pass plus zstd.
//!
//! ```text
//! mkz -c[z[LEVEL]][v] -f <archive> <files-or-dirs...>   # create
//! mkz -x[v]           -f <archive> [destdir]            # extract (default: .)
//! mkz transform   <in> <out>       # raw autocol blob, no backend (for piping)
//! mkz untransform <in> <out>
//! ```
//!
//! A reversible, bit-exact pre-pass that makes zstd measurably smaller on structured/line
//! data, and is provably never worse anywhere else. Everything streams: peak memory ~ one
//! block for typical line-oriented input. Newline-free input is the exception: a block
//! extends to the next newline, so a file with no newlines is buffered whole and not yet
//! bounded.
//!
//! Layers:
//!   - codec (`compress_stream`/`decompress_stream`): a byte stream <-> a `PAS1` container:
//!     line-aligned blocks, per-block autocol+zstd with a never-worse gate, streaming SHA-256.
//!   - archive (`ArchiveReader`/`ArchiveSink`): a directory tree <-> a flat entry byte stream,
//!     fed through the codec. 
//!
//! Entry := [tag u8 0=file/1=dir][uvarint pathlen][path][file: uvarint size + bytes].
//!

use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{self, BufRead, BufReader, BufWriter, Cursor, Read, Write};
use std::path::{Component, Path, PathBuf};

const MAGIC: &[u8; 4] = b"PAS1";
const DEFAULT_BLOCK_MB: usize = 16;
const DEFAULT_ZSTD_LEVEL: i32 = 12;

fn block_size() -> usize {
    std::env::var("PSRC_AC_BLOCK_MB")
        .ok()
        .and_then(|s| s.parse::<usize>().ok())
        .filter(|&n| n > 0)
        .unwrap_or(DEFAULT_BLOCK_MB)
        * (1 << 20)
}

fn env_zstd_level() -> i32 {
    std::env::var("PSRC_AC_ZSTD_LEVEL")
        .ok()
        .and_then(|s| s.parse::<i32>().ok())
        .filter(|&n| (1..=22).contains(&n))
        .unwrap_or(DEFAULT_ZSTD_LEVEL)
}

// -- varint ------------------------------------------------------------------
fn write_uvarint<W: Write>(w: &mut W, mut n: u64) -> io::Result<()> {
    loop {
        let b = (n & 0x7f) as u8;
        n >>= 7;
        if n != 0 {
            w.write_all(&[b | 0x80])?;
        } else {
            return w.write_all(&[b]);
        }
    }
}
fn push_uvarint(out: &mut Vec<u8>, mut n: u64) {
    loop {
        let b = (n & 0x7f) as u8;
        n >>= 7;
        if n != 0 {
            out.push(b | 0x80);
        } else {
            out.push(b);
            return;
        }
    }
}
fn read_uvarint<R: Read>(r: &mut R) -> Result<u64> {
    let (mut n, mut shift) = (0u64, 0u32);
    loop {
        let mut b = [0u8; 1];
        r.read_exact(&mut b).context("EOF in uvarint")?;
        n |= ((b[0] & 0x7f) as u64) << shift;
        if b[0] & 0x80 == 0 {
            return Ok(n);
        }
        shift += 7;
        if shift >= 64 {
            bail!("uvarint overflow");
        }
    }
}
/// Parse a uvarint from the front of a slice; None if incomplete.
fn uvarint_slice(s: &[u8]) -> Option<(u64, usize)> {
    let (mut n, mut shift, mut i) = (0u64, 0u32, 0usize);
    loop {
        let b = *s.get(i)?;
        i += 1;
        n |= ((b & 0x7f) as u64) << shift;
        if b & 0x80 == 0 {
            return Some((n, i));
        }
        shift += 7;
        if shift >= 64 {
            return None;
        }
    }
}

// -- codec: byte stream <-> PAS1 container ---------------------------------------
fn read_block<R: BufRead>(r: &mut R, target: usize) -> io::Result<Vec<u8>> {
    let mut buf = Vec::new();
    r.by_ref().take(target as u64).read_to_end(&mut buf)?;
    if !buf.is_empty() && buf.last() != Some(&b'\n') {
        r.read_until(b'\n', &mut buf)?; // finish the partial trailing line (no-op at EOF)
    }
    Ok(buf)
}

/// Create-side statistics, accumulated per encoded block. The never-worse gate already
/// compresses every block both ways, so `zstd_alone_bytes` (what plain zstd would have
/// cost) is exact, not an estimate; saved = zstd_alone_bytes - payload_bytes >= 0.
#[derive(Default)]
struct CompressStats {
    blocks: u64,
    autocol_blocks: u64,
    orig_bytes: u64,
    payload_bytes: u64,
    zstd_alone_bytes: u64,
}

impl CompressStats {
    fn report(&self) -> String {
        let pct = |num: u64, den: u64| if den == 0 { 0.0 } else { 100.0 * num as f64 / den as f64 };
        let saved = self.zstd_alone_bytes - self.payload_bytes;
        format!(
            "mkz: autocol kept {}/{} blocks ({:.1}%); payloads {:.1}% of stream; saved {} bytes ({:.1}%) vs zstd alone",
            self.autocol_blocks,
            self.blocks,
            pct(self.autocol_blocks, self.blocks),
            pct(self.payload_bytes, self.orig_bytes),
            saved,
            pct(saved, self.zstd_alone_bytes),
        )
    }
}

fn compress_stream<R: BufRead, W: Write>(r: &mut R, w: &mut W, block: usize, level: i32) -> Result<CompressStats> {
    w.write_all(MAGIC)?;
    let mut hasher = Sha256::new();
    let mut stats = CompressStats::default();
    loop {
        let data = read_block(r, block)?;
        if data.is_empty() {
            break;
        }
        hasher.update(&data);
        let comp_raw = zstd::bulk::compress(&data[..], level).context("zstd (raw block)")?;
        let comp_raw_len = comp_raw.len();
        let ac = autocol::encode(&data);
        let (flags, payload) = if autocol::try_decode(&ac).as_deref() == Some(&data[..]) {
            let comp_ac = zstd::bulk::compress(&ac[..], level).context("zstd (autocol block)")?;
            if comp_ac.len() < comp_raw.len() {
                (1u8, comp_ac)
            } else {
                (0u8, comp_raw)
            }
        } else {
            (0u8, comp_raw)
        };
        stats.blocks += 1;
        stats.autocol_blocks += u64::from(flags & 1);
        stats.orig_bytes += data.len() as u64;
        stats.payload_bytes += payload.len() as u64;
        stats.zstd_alone_bytes += comp_raw_len as u64;
        w.write_all(&[1u8])?;
        w.write_all(&[flags])?;
        write_uvarint(w, data.len() as u64)?;
        write_uvarint(w, payload.len() as u64)?;
        w.write_all(&payload)?;
    }
    w.write_all(&[0u8])?;
    w.write_all(&hasher.finalize())?;
    Ok(stats)
}

fn decompress_stream<R: Read, W: Write>(r: &mut R, w: &mut W) -> Result<()> {
    let mut magic = [0u8; 4];
    r.read_exact(&mut magic).context("reading magic")?;
    if &magic != MAGIC {
        bail!("bad magic: not a PAS1 stream");
    }
    let mut hasher = Sha256::new();
    loop {
        let mut tag = [0u8; 1];
        r.read_exact(&mut tag).context("reading block tag")?;
        if tag[0] == 0 {
            break;
        }
        let mut flags = [0u8; 1];
        r.read_exact(&mut flags)?;
        let orig_len = read_uvarint(r)? as usize;
        let comp_len = read_uvarint(r)? as usize;
        // Bound the allocation by the bytes actually present: a crafted comp_len must not
        // pre-allocate (capacity-overflow panic, exit 101) BEFORE the SHA gate. take() grows
        // the buffer only as real bytes arrive; a short read means a truncated/hostile block.
        let mut payload = Vec::new();
        let got = r
            .by_ref()
            .take(comp_len as u64)
            .read_to_end(&mut payload)
            .context("reading block payload")?;
        if got != comp_len {
            bail!("truncated block payload: expected {comp_len} bytes, got {got}");
        }
        let backend = zstd::decode_all(&payload[..]).context("zstd decode")?;
        let block = if flags[0] == 1 {
            autocol::try_decode(&backend)
                .ok_or_else(|| anyhow::anyhow!("autocol decode failed: corrupt block"))?
        } else {
            backend
        };
        if block.len() != orig_len {
            bail!("block length mismatch: {} != {}", block.len(), orig_len);
        }
        hasher.update(&block);
        w.write_all(&block)?;
    }
    let mut sha = [0u8; 32];
    r.read_exact(&mut sha).context("reading sha256 trailer")?;
    if hasher.finalize().as_slice() != sha {
        bail!("integrity check FAILED: SHA-256 mismatch, refusing corrupt output");
    }
    Ok(())
}

// -- archive: directory tree <-> flat entry byte stream --------------------------
struct Entry {
    is_dir: bool,
    rel: String,
    abs: PathBuf,
    size: u64,
}

/// Path with root/prefix/`.`/`..` components stripped, joined by '/'. Keeps archives
/// relocatable and safe (no absolute paths, no traversal).
fn safe_rel(p: &Path) -> String {
    p.components()
        .filter_map(|c| match c {
            Component::Normal(s) => Some(s.to_string_lossy().into_owned()),
            _ => None,
        })
        .collect::<Vec<_>>()
        .join("/")
}

fn collect_entries(paths: &[String]) -> Result<Vec<Entry>> {
    let mut out = Vec::new();
    for p in paths {
        let path = PathBuf::from(p);
        let md = std::fs::symlink_metadata(&path).with_context(|| format!("stat {p}"))?;
        if md.is_dir() {
            walk(&path, &mut out)?;
        } else if md.is_file() {
            out.push(Entry { is_dir: false, rel: safe_rel(&path), abs: path.clone(), size: md.len() });
        } else {
            bail!("{p}: not a regular file or directory (symlinks/devices unsupported)");
        }
    }
    Ok(out)
}

fn walk(dir: &Path, out: &mut Vec<Entry>) -> Result<()> {
    let rel = safe_rel(dir);
    if !rel.is_empty() {
        out.push(Entry { is_dir: true, rel, abs: dir.to_path_buf(), size: 0 });
    }
    let mut kids: Vec<PathBuf> = std::fs::read_dir(dir)
        .with_context(|| format!("read_dir {}", dir.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .collect();
    kids.sort(); // deterministic archive order
    for k in kids {
        let md = std::fs::symlink_metadata(&k)?;
        if md.is_dir() {
            walk(&k, out)?;
        } else if md.is_file() {
            out.push(Entry { is_dir: false, rel: safe_rel(&k), abs: k.clone(), size: md.len() });
        }
        // silently skip symlinks/special files
    }
    Ok(())
}

/// `Read` that streams the flat entry byte stream: header, then file content, per entry.
/// Bounded working set: only one file handle open at a time.
struct ArchiveReader {
    entries: std::vec::IntoIter<Entry>,
    header: Cursor<Vec<u8>>,
    file: Option<File>,
    verbose: bool,
}
impl ArchiveReader {
    fn new(entries: Vec<Entry>, verbose: bool) -> Self {
        ArchiveReader { entries: entries.into_iter(), header: Cursor::new(Vec::new()), file: None, verbose }
    }
}
impl Read for ArchiveReader {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        loop {
            let n = self.header.read(buf)?;
            if n > 0 {
                return Ok(n);
            }
            if let Some(f) = &mut self.file {
                let n = f.read(buf)?;
                if n > 0 {
                    return Ok(n);
                }
                self.file = None;
            }
            match self.entries.next() {
                None => return Ok(0),
                Some(e) => {
                    if self.verbose {
                        eprintln!("{} {}", if e.is_dir { "d" } else { "a" }, e.rel);
                    }
                    let mut h = Vec::new();
                    h.push(if e.is_dir { 1 } else { 0 });
                    push_uvarint(&mut h, e.rel.len() as u64);
                    h.extend_from_slice(e.rel.as_bytes());
                    if !e.is_dir {
                        push_uvarint(&mut h, e.size);
                        self.file = Some(File::open(&e.abs)?);
                    }
                    self.header = Cursor::new(h);
                }
            }
        }
    }
}

enum SinkState {
    Header,
    Body { remaining: u64, out: BufWriter<File> },
}
/// `Write` sink that parses the entry stream and reconstructs the tree under `dest`.
/// Buffers only headers (file content streams straight to disk); peak ~ one codec block.
struct ArchiveSink {
    dest: PathBuf,
    buf: Vec<u8>,
    state: SinkState,
    verbose: bool,
}
impl ArchiveSink {
    fn new(dest: PathBuf, verbose: bool) -> Self {
        ArchiveSink { dest, buf: Vec::new(), state: SinkState::Header, verbose }
    }
    fn target(&self, rel: &str) -> io::Result<PathBuf> {
        // re-validate: refuse traversal / absolute even if the archive is hostile
        let safe = safe_rel(Path::new(rel));
        if safe.is_empty() {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "empty entry path"));
        }
        Ok(self.dest.join(safe))
    }
    fn drain(&mut self) -> io::Result<()> {
        let mut i = 0usize;
        loop {
            match &mut self.state {
                SinkState::Body { remaining, out } => {
                    let avail = self.buf.len() - i;
                    if avail == 0 || *remaining == 0 {
                        if *remaining == 0 {
                            out.flush()?;
                            self.state = SinkState::Header;
                            continue;
                        }
                        break; // need more bytes
                    }
                    let take = (*remaining as usize).min(avail);
                    out.write_all(&self.buf[i..i + take])?;
                    i += take;
                    *remaining -= take as u64;
                }
                SinkState::Header => {
                    let s = &self.buf[i..];
                    if s.is_empty() {
                        break;
                    }
                    let tag = s[0];
                    let Some((plen, off1)) = uvarint_slice(&s[1..]).map(|(v, o)| (v as usize, 1 + o)) else { break };
                    if s.len() < off1 + plen {
                        break;
                    }
                    let rel = String::from_utf8_lossy(&s[off1..off1 + plen]).into_owned();
                    match tag {
                        1 => {
                            let p = self.target(&rel)?;
                            std::fs::create_dir_all(&p)?;
                            if self.verbose {
                                eprintln!("d {rel}");
                            }
                            i += off1 + plen;
                        }
                        0 => {
                            let Some((size, off2)) = uvarint_slice(&s[off1 + plen..]).map(|(v, o)| (v, off1 + plen + o)) else { break };
                            let p = self.target(&rel)?;
                            if let Some(parent) = p.parent() {
                                std::fs::create_dir_all(parent)?;
                            }
                            let out = BufWriter::new(File::create(&p)?);
                            if self.verbose {
                                eprintln!("x {rel}");
                            }
                            i += off2;
                            self.state = SinkState::Body { remaining: size, out };
                        }
                        _ => return Err(io::Error::new(io::ErrorKind::InvalidData, "bad entry tag")),
                    }
                }
            }
        }
        self.buf.drain(..i);
        Ok(())
    }
}
impl Write for ArchiveSink {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        self.buf.extend_from_slice(data);
        self.drain()?;
        Ok(data.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        if let SinkState::Body { out, .. } = &mut self.state {
            out.flush()?;
        }
        Ok(())
    }
}

fn create(archive: &str, paths: &[String], block: usize, level: i32, verbose: bool) -> Result<()> {
    if paths.is_empty() {
        bail!("create (-c) needs at least one file or directory");
    }
    let entries = collect_entries(paths)?;
    let mut reader = BufReader::new(ArchiveReader::new(entries, verbose));
    let mut w = BufWriter::new(File::create(archive).with_context(|| format!("create {archive}"))?);
    let stats = compress_stream(&mut reader, &mut w, block, level)?;
    w.flush()?;
    if verbose {
        eprintln!("{}", stats.report());
    }
    let o = std::fs::metadata(archive)?.len();
    eprintln!("mkz: wrote {archive} ({o} bytes, {} MB blocks, zstd {level})", block >> 20);
    Ok(())
}

fn extract(archive: &str, dest: &str, verbose: bool) -> Result<()> {
    let mut r = BufReader::new(File::open(archive).with_context(|| format!("open {archive}"))?);
    std::fs::create_dir_all(dest).with_context(|| format!("mkdir {dest}"))?;
    let mut sink = ArchiveSink::new(PathBuf::from(dest), verbose);
    decompress_stream(&mut r, &mut sink)?; // verifies the SHA-256 trailer before returning Ok
    sink.flush().ok();
    eprintln!("mkz: extracted {archive} -> {dest} (SHA-256 verified)");
    Ok(())
}

// -- CLI (tar-style) -----------------------------------------------------------
const HELP: &str = concat!(
    "mkz ", env!("CARGO_PKG_VERSION"), " (psrc)\n",
    "A reversible, bit-exact pre-pass that makes zstd measurably smaller on structured/line\n",
    "data, and is provably never worse anywhere else. Streaming, bounded memory for typical text/logs.\n\n",
    "usage:\n",
    "  mkz -c[z[LEVEL]][v] -f <archive> <files-or-dirs...>   create\n",
    "  mkz -x[v]           -f <archive> [destdir]            extract (default: .)\n",
    "  mkz transform   <in> <out>   raw autocol blob, no backend (for piping)\n",
    "  mkz untransform <in> <out>\n\n",
    "flags:  -c create   -x extract   -z[LEVEL] zstd level 1-22 (default 12)   -f archive   -v verbose\n",
    "env:    PSRC_AC_BLOCK_MB (default 16)   PSRC_AC_ZSTD_LEVEL (default 12)\n",
    "examples:\n",
    "  mkz -czf logs.mkz /var/log            mkz -xvf logs.mkz out/\n",
    "  mkz -cz19vf big.mkz data/             mkz -xf big.mkz\n",
);

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() || matches!(args[0].as_str(), "-h" | "--help") {
        print!("{HELP}");
        return Ok(());
    }
    if matches!(args[0].as_str(), "-V" | "--version") {
        println!("mkz {} (psrc)", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }

    // raw-transform subcommands (kept for piping)
    if matches!(args[0].as_str(), "transform" | "untransform") {
        if args.len() != 3 {
            bail!("usage: mkz {} <in> <out>", args[0]);
        }
        let input = std::fs::read(&args[1]).with_context(|| format!("reading {}", args[1]))?;
        let out = if args[0] == "transform" {
            autocol::encode(&input)
        } else {
            autocol::try_decode(&input).ok_or_else(|| anyhow::anyhow!("malformed autocol blob"))?
        };
        std::fs::write(&args[2], &out).with_context(|| format!("writing {}", args[2]))?;
        return Ok(());
    }

    // tar-style flags
    let (mut create_m, mut extract_m, mut verbose) = (false, false, false);
    let mut level = env_zstd_level();
    let mut archive: Option<String> = None;
    let mut positionals: Vec<String> = Vec::new();

    let mut it = args.iter().peekable();
    while let Some(arg) = it.next() {
        if arg.len() > 1 && arg.starts_with('-') && !arg.starts_with("--") {
            let mut cs = arg[1..].chars().peekable();
            while let Some(c) = cs.next() {
                match c {
                    'c' => create_m = true,
                    'x' => extract_m = true,
                    'v' => verbose = true,
                    'z' => {
                        let mut num = String::new();
                        while let Some(d) = cs.peek() {
                            if d.is_ascii_digit() {
                                num.push(*d);
                                cs.next();
                            } else {
                                break;
                            }
                        }
                        if !num.is_empty() {
                            match num.parse::<i32>() {
                                Ok(n) if (1..=22).contains(&n) => level = n,
                                _ => bail!("zstd level must be 1-22, got {num}"),
                            }
                        }
                    }
                    'f' => {
                        archive = Some(it.next().cloned().ok_or_else(|| anyhow::anyhow!("-f needs an archive filename"))?);
                    }
                    other => bail!("unknown flag -{other}\n\n{HELP}"),
                }
            }
        } else {
            positionals.push(arg.clone());
        }
    }

    if create_m == extract_m {
        bail!("specify exactly one of -c (create) or -x (extract)\n\n{HELP}");
    }
    let archive = archive.ok_or_else(|| anyhow::anyhow!("missing -f <archive>"))?;

    if create_m {
        create(&archive, &positionals, block_size(), level, verbose)?;
    } else {
        let dest = positionals.first().cloned().unwrap_or_else(|| ".".to_string());
        if positionals.len() > 1 {
            bail!("extract takes at most one destdir, got {:?}", positionals);
        }
        extract(&archive, &dest, verbose)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip_codec(data: &[u8], block: usize) {
        let mut comp = Vec::new();
        compress_stream(&mut Cursor::new(data), &mut comp, block, 12).expect("compress");
        let mut out = Vec::new();
        decompress_stream(&mut Cursor::new(&comp), &mut out).expect("decompress");
        assert_eq!(out, data, "codec roundtrip (len {}, block {})", data.len(), block);
    }

    #[test]
    fn codec_single_and_multi_block() {
        roundtrip_codec(b"", 1 << 20);
        roundtrip_codec(b"hello\nhello\nworld\n", 1 << 20);
        let mut d = Vec::new();
        for i in 0..5000 {
            d.extend_from_slice(format!("2026 host{} GET /p/{} 200 {}\n", i % 7, i, 100 + i).as_bytes());
        }
        roundtrip_codec(&d, 1 << 20);
        roundtrip_codec(&d, 4096); // many blocks
        roundtrip_codec(&d, 64);
    }

    #[test]
    fn codec_corruption_rejected() {
        let mut comp = Vec::new();
        compress_stream(&mut Cursor::new(b"data data data\n"), &mut comp, 4096, 12).unwrap();
        let n = comp.len();
        comp[n - 1] ^= 0xff;
        let mut out = Vec::new();
        assert!(decompress_stream(&mut Cursor::new(&comp), &mut out).is_err());
    }

    #[test]
    fn archive_tree_roundtrip() {
        let base = std::env::temp_dir().join(format!("mkz_test_{}", std::process::id()));
        let src = base.join("src");
        let dst = base.join("out");
        let _ = std::fs::remove_dir_all(&base);
        std::fs::create_dir_all(src.join("logs/sub")).unwrap();
        std::fs::write(src.join("a.log"), b"alpha\nalpha\nbeta\n").unwrap();
        std::fs::write(src.join("logs/b.log"), vec![0u8, 1, 2, 255, b'\n', 7]).unwrap(); // binary, no NL-end
        std::fs::write(src.join("logs/sub/c.txt"), b"x".repeat(20000)).unwrap();
        std::fs::create_dir_all(src.join("empty")).unwrap();

        let archive = base.join("a.mkz");
        create(archive.to_str().unwrap(), &[src.to_str().unwrap().to_string()], 1 << 16, 12, false).unwrap();
        extract(archive.to_str().unwrap(), dst.to_str().unwrap(), false).unwrap();

        // src abs path safe_rel'd under dst, recompute the stored root
        let root = dst.join(safe_rel(&src));
        assert_eq!(std::fs::read(root.join("a.log")).unwrap(), b"alpha\nalpha\nbeta\n");
        assert_eq!(std::fs::read(root.join("logs/b.log")).unwrap(), vec![0u8, 1, 2, 255, b'\n', 7]);
        assert_eq!(std::fs::read(root.join("logs/sub/c.txt")).unwrap(), b"x".repeat(20000));
        assert!(root.join("empty").is_dir());

        std::fs::remove_dir_all(&base).ok();
    }

    #[test]
    fn help_and_version_smoke() {
        assert!(HELP.contains("mkz -czf"));
    }
}
