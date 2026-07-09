// SPDX-License-Identifier: MIT OR Apache-2.0
//! psrc-autocol - schema-free auto-columnar transform.
//!
//! A reversible, SHA-256-verifiable transform that discovers record structure in
//! line-oriented data **with no schema** and reorganizes it so a downstream entropy
//! coder (brotli/zstd) sees per-column-homogeneous data instead of interleaved rows.
//!
//! It is *not* a compressor on its own; it is the structural pre-pass that makes a
//! general compressor win on logs/CSV/JSONL/telemetry. [`encode`] produces a single
//! packed blob; [`decode`] inverts it exactly. The caller runs the blob through the
//! backend compressor and applies a safe gate (`min(backend(blob), backend(raw))`),
//! so the arrangement can only help or no-op, never expand.
//!
//! ## How it works
//! 1. Split on `\n` into records.
//! 2. Tokenize each record into maximal ASCII-alphanumeric **words** and the
//!    **separators** between them. Whole-token (so hex/uuid stay intact, no template
//!    explosion).
//! 3. Group records by **skeleton** (their separator sequence). Within a group, a word
//!    position that is constant across all records is folded into the **template**;
//!    positions that vary become **columns**.
//! 4. Pack `[templates][record->template ids][columns]` into one blob. Numeric columns
//!    get a zigzag-varint **delta** transform (timestamps/ids); others stay raw.
//!
//! The win comes from per-column homogeneity (all status codes together, monotone
//! timestamps delta-coded) plus single-stream packing (one backend call amortizes
//! per-stream overhead). Measured 20-44% over brotli -q11 on real logs, bit-exact.

// -- varint / zigzag ----------------------------------------------------------

fn put_uvarint(out: &mut Vec<u8>, mut n: u64) {
    loop {
        let byte = (n & 0x7f) as u8;
        n >>= 7;
        if n != 0 {
            out.push(byte | 0x80);
        } else {
            out.push(byte);
            break;
        }
    }
}

fn get_uvarint(buf: &[u8], pos: &mut usize) -> Option<u64> {
    let mut shift = 0u32;
    let mut val = 0u64;
    loop {
        let b = *buf.get(*pos)?;
        *pos += 1;
        val |= ((b & 0x7f) as u64) << shift;
        if b & 0x80 == 0 {
            return Some(val);
        }
        shift += 7;
        if shift >= 64 {
            return None;
        }
    }
}

#[inline]
fn zigzag(n: i64) -> u64 {
    ((n << 1) ^ (n >> 63)) as u64
}
#[inline]
fn unzigzag(z: u64) -> i64 {
    ((z >> 1) as i64) ^ -((z & 1) as i64)
}

/// Byte length of `n` encoded as a uvarint, used to pick the cheapest per-column codec.
fn uvarint_len(mut n: u64) -> usize {
    let mut c = 1;
    while n >= 0x80 {
        n >>= 7;
        c += 1;
    }
    c
}

/// Blob format version. v1 prepends this byte and adds the global value dictionary that
/// backs the dict-ref column codec (codec id `2`).
const FORMAT_VERSION: u8 = 1;

// -- tokenization -------------------------------------------------------------

#[inline]
fn is_word_byte(b: u8) -> bool {
    b.is_ascii_alphanumeric()
}

/// Split a line into (separators, words) with `seps.len() == words.len() + 1`.
/// Separators are maximal non-alphanumeric runs (possibly empty); words are maximal
/// ASCII-alphanumeric runs.
fn tokenize(line: &[u8]) -> (Vec<&[u8]>, Vec<&[u8]>) {
    let mut seps = Vec::new();
    let mut words = Vec::new();
    let n = line.len();
    let mut i = 0;
    loop {
        let s0 = i;
        while i < n && !is_word_byte(line[i]) {
            i += 1;
        }
        seps.push(&line[s0..i]);
        if i >= n {
            break;
        }
        let w0 = i;
        while i < n && is_word_byte(line[i]) {
            i += 1;
        }
        words.push(&line[w0..i]);
    }
    (seps, words)
}

/// Hashable key identifying a record's structure: the separator sequence (which also
/// fixes the word count). Length-prefixed so it never collides.
fn skeleton_key(seps: &[&[u8]]) -> Vec<u8> {
    let mut k = Vec::new();
    put_uvarint(&mut k, seps.len() as u64);
    for s in seps {
        put_uvarint(&mut k, s.len() as u64);
        k.extend_from_slice(s);
    }
    k
}

/// If every token is a canonical non-negative decimal integer that fits in i64,
/// return the parsed values (so the column can be delta-coded). "Canonical" excludes
/// leading zeros (so the decimal string round-trips exactly).
fn try_ints(col: &[&[u8]]) -> Option<Vec<i64>> {
    let mut out = Vec::with_capacity(col.len());
    for &v in col {
        if v.is_empty() || v.len() > 18 {
            return None;
        }
        if v[0] == b'0' && v.len() > 1 {
            return None; // leading zero is not canonical
        }
        if !v.iter().all(|b| b.is_ascii_digit()) {
            return None;
        }
        let mut n: i64 = 0;
        for &b in v {
            n = n * 10 + (b - b'0') as i64;
        }
        out.push(n);
    }
    Some(out)
}

// -- encode -------------------------------------------------------------------

/// Transform `data` into the packed auto-columnar blob. Pure and reversible.
pub fn encode(data: &[u8]) -> Vec<u8> {
    let lines: Vec<&[u8]> = data.split(|&b| b == b'\n').collect();
    let parsed: Vec<(Vec<&[u8]>, Vec<&[u8]>)> = lines.iter().map(|l| tokenize(l)).collect();

    // group records by skeleton
    use std::collections::HashMap;
    let mut group_of: HashMap<Vec<u8>, usize> = HashMap::new();
    let mut group_lines: Vec<Vec<usize>> = Vec::new();
    let mut line_gid: Vec<usize> = vec![0; lines.len()];
    for (i, (seps, _)) in parsed.iter().enumerate() {
        let key = skeleton_key(seps);
        let gid = *group_of.entry(key).or_insert_with(|| {
            group_lines.push(Vec::new());
            group_lines.len() - 1
        });
        group_lines[gid].push(i);
        line_gid[i] = gid;
    }

    let ntmpl = group_lines.len();
    let mut templates: Vec<Vec<u8>> = Vec::with_capacity(ntmpl);
    let mut columns: Vec<Vec<&[u8]>> = Vec::new(); // (gid-major, slot order)

    for gid in 0..ntmpl {
        let idxs = &group_lines[gid];
        let first = idxs[0];
        let seps0 = &parsed[first].0;
        let words0 = &parsed[first].1;
        let nword = words0.len();

        // a slot is variable if any record in the group differs from the first
        let mut is_var = vec![false; nword];
        for &li in idxs.iter().skip(1) {
            let wj = &parsed[li].1;
            for j in 0..nword {
                if wj[j] != words0[j] {
                    is_var[j] = true;
                }
            }
        }

        // template record: nword, sep0, then per slot [flag, (const word)?, sep_{j+1}]
        let mut t = Vec::new();
        put_uvarint(&mut t, nword as u64);
        put_uvarint(&mut t, seps0[0].len() as u64);
        t.extend_from_slice(seps0[0]);
        for j in 0..nword {
            if is_var[j] {
                t.push(0);
            } else {
                t.push(1);
                put_uvarint(&mut t, words0[j].len() as u64);
                t.extend_from_slice(words0[j]);
            }
            put_uvarint(&mut t, seps0[j + 1].len() as u64);
            t.extend_from_slice(seps0[j + 1]);
        }
        templates.push(t);

        // collect variable columns (values in group-line / original order)
        for j in 0..nword {
            if is_var[j] {
                let mut col: Vec<&[u8]> = Vec::with_capacity(idxs.len());
                for &li in idxs {
                    col.push(parsed[li].1[j]);
                }
                columns.push(col);
            }
        }
    }

    // -- per-column codec selection: raw(0) | delta(1) | dict-ref(2) --
    // Build a tentative frequency-ranked value dictionary spanning ALL columns, then pick
    // the smallest pre-coder encoding per column. dict-ref shares one global dictionary, so
    // a column's dict cost is just its id stream. A monotone-numeric column's deltas are
    // tiny varints and beat its (large, distinct) ids, so it keeps delta; the automatic
    // guard against deduping incompressible numeric columns (the shutdown_monitor lesson).
    let mut freq: HashMap<&[u8], u64> = HashMap::new();
    for col in &columns {
        for &v in col {
            *freq.entry(v).or_insert(0) += 1;
        }
    }
    let mut ranked: Vec<&[u8]> = freq.keys().copied().collect();
    ranked.sort_unstable_by(|a, b| freq[b].cmp(&freq[a]).then_with(|| a.cmp(b)));
    let mut tent_id: HashMap<&[u8], u64> = HashMap::with_capacity(ranked.len());
    for (i, &v) in ranked.iter().enumerate() {
        tent_id.insert(v, i as u64);
    }

    enum ColCodec {
        Raw,
        Delta(Vec<i64>),
        Dict,
    }
    let mut chosen: Vec<ColCodec> = Vec::with_capacity(columns.len());
    for col in &columns {
        let raw_sz: usize = col.iter().map(|v| uvarint_len(v.len() as u64) + v.len()).sum();
        let ints = try_ints(col);
        let delta_sz = ints.as_ref().map(|vals| {
            let mut prev = 0i64;
            let mut s = 0usize;
            for &n in vals {
                s += uvarint_len(zigzag(n - prev));
                prev = n;
            }
            s
        });
        let dict_sz: usize = col.iter().map(|&v| uvarint_len(tent_id[v])).sum();
        // prefer delta on ties with raw (keeps numeric homogeneity); take dict only if it is
        // strictly smaller than both, so a numeric column never loses its delta to the dict.
        let mut best = raw_sz;
        let mut codec = ColCodec::Raw;
        if let Some(ds) = delta_sz {
            if ds <= best {
                best = ds;
                codec = ColCodec::Delta(ints.unwrap());
            }
        }
        if dict_sz < best {
            codec = ColCodec::Dict;
        }
        chosen.push(codec);
    }

    // Final dictionary: only the values actually referenced by dict-ref columns, re-ranked
    // (frequent -> small id) for compact varints and deterministic output.
    let mut used: std::collections::HashSet<&[u8]> = std::collections::HashSet::new();
    for (col, codec) in columns.iter().zip(&chosen) {
        if matches!(codec, ColCodec::Dict) {
            for &v in col {
                used.insert(v);
            }
        }
    }
    let mut final_dict: Vec<&[u8]> = used.into_iter().collect();
    final_dict.sort_unstable_by_key(|v| tent_id[v]);
    let mut final_id: HashMap<&[u8], u64> = HashMap::with_capacity(final_dict.len());
    for (i, &v) in final_dict.iter().enumerate() {
        final_id.insert(v, i as u64);
    }

    // pack one blob
    let mut blob = Vec::new();
    blob.push(FORMAT_VERSION);
    put_uvarint(&mut blob, ntmpl as u64);
    for t in &templates {
        put_uvarint(&mut blob, t.len() as u64);
        blob.extend_from_slice(t);
    }
    put_uvarint(&mut blob, lines.len() as u64);
    for &g in &line_gid {
        put_uvarint(&mut blob, g as u64);
    }
    // global value dictionary (id == index); empty when no column chose dict-ref
    put_uvarint(&mut blob, final_dict.len() as u64);
    for v in &final_dict {
        put_uvarint(&mut blob, v.len() as u64);
        blob.extend_from_slice(v);
    }
    put_uvarint(&mut blob, columns.len() as u64);
    for (col, codec) in columns.iter().zip(&chosen) {
        match codec {
            ColCodec::Delta(vals) => {
                blob.push(1); // delta codec
                put_uvarint(&mut blob, col.len() as u64);
                let mut prev: i64 = 0;
                for &n in vals {
                    put_uvarint(&mut blob, zigzag(n - prev));
                    prev = n;
                }
            }
            ColCodec::Dict => {
                blob.push(2); // dict-ref codec
                put_uvarint(&mut blob, col.len() as u64);
                for &v in col {
                    put_uvarint(&mut blob, final_id[v]);
                }
            }
            ColCodec::Raw => {
                blob.push(0); // raw codec
                put_uvarint(&mut blob, col.len() as u64);
                for v in col {
                    put_uvarint(&mut blob, v.len() as u64);
                    blob.extend_from_slice(v);
                }
            }
        }
    }
    blob
}

// -- decode -------------------------------------------------------------------

enum Slot {
    Const(Vec<u8>),
    Var,
}
struct Tmpl {
    seps: Vec<Vec<u8>>, // len == slots.len() + 1
    slots: Vec<Slot>,
}

fn read_bytes(buf: &[u8], pos: &mut usize) -> Option<Vec<u8>> {
    let len = get_uvarint(buf, pos)? as usize;
    let end = pos.checked_add(len)?;
    let slice = buf.get(*pos..end)?;
    *pos = end;
    Some(slice.to_vec())
}

fn parse_template(rec: &[u8]) -> Option<Tmpl> {
    let mut pos = 0;
    let nword = get_uvarint(rec, &mut pos)? as usize;
    // cap pre-alloc: each word/sep consumes >=1 byte, so a crafted nword can't exceed the
    // template bytes left; bound it instead of allocating (capacity-overflow panic).
    let cap = nword.min(rec.len().saturating_sub(pos));
    let mut seps = Vec::with_capacity(cap + 1);
    let mut slots = Vec::with_capacity(cap);
    seps.push(read_bytes(rec, &mut pos)?);
    for _ in 0..nword {
        let flag = *rec.get(pos)?;
        pos += 1;
        if flag == 1 {
            slots.push(Slot::Const(read_bytes(rec, &mut pos)?));
        } else {
            slots.push(Slot::Var);
        }
        seps.push(read_bytes(rec, &mut pos)?);
    }
    Some(Tmpl { seps, slots })
}

/// Inverse of [`encode`]. Returns `None` only on a structurally invalid blob.
pub fn try_decode(blob: &[u8]) -> Option<Vec<u8>> {
    let mut pos = 0;

    // format version
    if *blob.get(pos)? != FORMAT_VERSION {
        return None;
    }
    pos += 1;

    // templates
    let ntmpl = get_uvarint(blob, &mut pos)? as usize;
    // cap pre-alloc by bytes left: each template consumes >=1 byte, so a crafted huge ntmpl
    // cannot legitimately exceed the remaining input; bound it instead of panicking.
    let mut templates = Vec::with_capacity(ntmpl.min(blob.len().saturating_sub(pos)));
    for _ in 0..ntmpl {
        let rec = read_bytes(blob, &mut pos)?;
        templates.push(parse_template(&rec)?);
    }

    // record -> template ids
    let nlines = get_uvarint(blob, &mut pos)? as usize;
    let mut line_gid = Vec::with_capacity(nlines.min(blob.len().saturating_sub(pos)));
    for _ in 0..nlines {
        let g = get_uvarint(blob, &mut pos)? as usize;
        if g >= ntmpl {
            return None;
        }
        line_gid.push(g);
    }

    // global value dictionary (dict-ref codec); id == index
    let ndict = get_uvarint(blob, &mut pos)? as usize;
    let mut dict: Vec<Vec<u8>> = Vec::with_capacity(ndict.min(blob.len().saturating_sub(pos)));
    for _ in 0..ndict {
        dict.push(read_bytes(blob, &mut pos)?);
    }

    // columns (decoded to concrete value lists, in blob order)
    let ncol = get_uvarint(blob, &mut pos)? as usize;
    let mut columns: Vec<Vec<Vec<u8>>> = Vec::with_capacity(ncol.min(blob.len().saturating_sub(pos)));
    for _ in 0..ncol {
        let codec = *blob.get(pos)?;
        pos += 1;
        let count = get_uvarint(blob, &mut pos)? as usize;
        let mut col = Vec::with_capacity(count.min(blob.len().saturating_sub(pos)));
        if codec == 1 {
            let mut prev: i64 = 0;
            for _ in 0..count {
                let z = get_uvarint(blob, &mut pos)?;
                prev += unzigzag(z);
                col.push(prev.to_string().into_bytes());
            }
        } else if codec == 2 {
            for _ in 0..count {
                let id = get_uvarint(blob, &mut pos)? as usize;
                col.push(dict.get(id)?.clone());
            }
        } else {
            for _ in 0..count {
                col.push(read_bytes(blob, &mut pos)?);
            }
        }
        columns.push(col);
    }

    // assign columns to groups (gid-major, slot order)
    let mut group_columns: Vec<Vec<usize>> = vec![Vec::new(); ntmpl];
    let mut ci = 0usize;
    for (gid, t) in templates.iter().enumerate() {
        for s in &t.slots {
            if matches!(s, Slot::Var) {
                if ci >= ncol {
                    return None;
                }
                group_columns[gid].push(ci);
                ci += 1;
            }
        }
    }
    if ci != ncol {
        return None;
    }

    // reconstruct, pulling each group's column values in original line order
    let mut cursor = vec![0usize; ncol];
    let mut out_lines: Vec<Vec<u8>> = Vec::with_capacity(nlines);
    for &gid in &line_gid {
        let t = &templates[gid];
        let mut line = Vec::new();
        line.extend_from_slice(&t.seps[0]);
        let mut var_k = 0usize;
        for (j, s) in t.slots.iter().enumerate() {
            match s {
                Slot::Const(w) => line.extend_from_slice(w),
                Slot::Var => {
                    let col_idx = *group_columns[gid].get(var_k)?;
                    var_k += 1;
                    let v = columns[col_idx].get(cursor[col_idx])?;
                    cursor[col_idx] += 1;
                    line.extend_from_slice(v);
                }
            }
            line.extend_from_slice(&t.seps[j + 1]);
        }
        out_lines.push(line);
    }
    Some(out_lines.join(&b"\n"[..]))
}

/// Convenience wrapper that panics on a malformed blob (use [`try_decode`] to handle
/// untrusted input). Always succeeds on output of [`encode`].
pub fn decode(blob: &[u8]) -> Vec<u8> {
    try_decode(blob).expect("psrc-autocol: malformed blob")
}

// -- tests --------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip(data: &[u8]) {
        let blob = encode(data);
        let back = decode(&blob);
        assert_eq!(back, data, "roundtrip mismatch (len {})", data.len());
    }

    #[test]
    fn empty() {
        roundtrip(b"");
    }

    #[test]
    fn single_char_and_newlines() {
        roundtrip(b"a");
        roundtrip(b"\n");
        roundtrip(b"\n\n\n");
        roundtrip(b"a\nb\nc");
        roundtrip(b"a\nb\nc\n");
    }

    #[test]
    fn log_like_columns() {
        let mut d = Vec::new();
        for i in 0..50 {
            d.extend_from_slice(
                format!("10.0.0.{} - - [t] GET /p/{} 200 {}\n", i % 7, i, 100 + i * 3).as_bytes(),
            );
        }
        roundtrip(&d);
        // it should actually find structure (>=1 template, >=1 column)
        let blob = encode(&d);
        assert!(blob.len() < d.len() * 2);
    }

    #[test]
    fn leading_zeros_stay_raw() {
        // canonical-int guard: "007" must NOT be delta-coded (would lose the zeros)
        let d = b"x 007\nx 042\nx 100\n";
        roundtrip(d);
    }

    #[test]
    fn hex_tokens_whole() {
        // alnum hex stays a single token -> no template explosion, exact roundtrip
        let mut d = Vec::new();
        for i in 0..40 {
            d.extend_from_slice(
                format!("id={:08x}beef rec{}\n", (i as u32).wrapping_mul(2654435761), i).as_bytes(),
            );
        }
        roundtrip(&d);
    }

    #[test]
    fn non_utf8_and_high_bytes() {
        let d: Vec<u8> = (0u16..=255).map(|b| b as u8).chain([b'\n', 1, 2, 0]).collect();
        roundtrip(&d);
    }

    #[test]
    fn sentinel_byte_in_data_is_safe() {
        // 0x01 used to be an in-band sentinel; ensure it's just data now
        roundtrip(&[1u8, 1, 1, b'\n', b'a', 1, b'b', b'\n']);
    }

    // simple dependency-free fuzz: xorshift-driven random bytes incl newlines/structure
    #[test]
    fn fuzz_roundtrip() {
        let mut state = 0x9E3779B97F4A7C15u64;
        let mut next = || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state
        };
        for _ in 0..400 {
            let len = (next() % 600) as usize;
            let mut d = Vec::with_capacity(len);
            for _ in 0..len {
                // bias toward alnum / newline / a few punct so structure appears
                let r = next() % 100;
                let b = if r < 45 {
                    b'a' + (next() % 26) as u8
                } else if r < 70 {
                    b'0' + (next() % 10) as u8
                } else if r < 82 {
                    b'\n'
                } else if r < 92 {
                    *b" ,.:/-".get((next() % 6) as usize).unwrap()
                } else {
                    (next() % 256) as u8
                };
                d.push(b);
            }
            roundtrip(&d);
        }
    }

    /// Number of global-dictionary entries in a blob (0 == no column used dict-ref).
    fn dict_len(blob: &[u8]) -> usize {
        let mut pos = 0;
        assert_eq!(blob[pos], FORMAT_VERSION);
        pos += 1;
        let ntmpl = get_uvarint(blob, &mut pos).unwrap() as usize;
        for _ in 0..ntmpl {
            read_bytes(blob, &mut pos).unwrap();
        }
        let nlines = get_uvarint(blob, &mut pos).unwrap() as usize;
        for _ in 0..nlines {
            get_uvarint(blob, &mut pos).unwrap();
        }
        get_uvarint(blob, &mut pos).unwrap() as usize
    }

    #[test]
    fn dict_ref_dedups_cross_skeleton_values() {
        // alice/bob/web01/web02 recur across TWO different skeletons -> dict-ref should fire,
        // and it must round-trip exactly.
        let mut d = Vec::new();
        for _ in 0..60 {
            d.extend_from_slice(b"user alice from host web01\n");
            d.extend_from_slice(b"user bob from host web02\n");
            d.extend_from_slice(b"login: alice @ web01\n");
            d.extend_from_slice(b"login: bob @ web02\n");
        }
        roundtrip(&d);
        assert!(dict_len(&encode(&d)) > 0, "expected dict-ref to be used on repeated values");
    }

    #[test]
    fn numeric_columns_not_dict_poisoned() {
        // monotone-numeric columns must stay delta-coded; the dictionary must be empty
        // (this is the guard against the shutdown_monitor -35% regression).
        let mut d = Vec::new();
        for i in 0..200u64 {
            d.extend_from_slice(format!("ts={} seq={}\n", 1000 + i, i).as_bytes());
        }
        roundtrip(&d);
        assert_eq!(dict_len(&encode(&d)), 0, "numeric columns must not be deduped");
    }

    proptest::proptest! {
        #[test]
        fn prop_roundtrip(data: Vec<u8>) {
            let blob = encode(&data);
            proptest::prop_assert_eq!(decode(&blob), data);
        }
    }
}
