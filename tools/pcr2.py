"""PCR2 recording decoder (docs/data-v2.md). Pure function of the file: never modifies it.

decode(path) -> (header, end, blocks, samples, problems)
  header   : the file header JSON
  end      : the END2 JSON, or None (racket died mid-recording)
  blocks   : list of dicts (seq, first_mono_us, first_sample_index, n, flags, ok, offset)
  samples  : numpy structured array: t_us (int64, racket monotonic µs) + one column per header field (raw ints)
  problems : list of human-readable strings (bad CRC, gaps in block_seq, truncation...)
summarize(header, end, blocks, samples) -> dict of measured facts for the DB/dashboard.
"""
import json
import struct
import zlib

import numpy as np

TYPES = {"u8": "<u1", "i8": "<i1", "u16": "<u2", "i16": "<i2", "u32": "<u4", "i32": "<i4", "f32": "<f4"}
SIZES = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4, "f32": 4}
BLK = struct.Struct("<4sIQIHBBII")          # 32 bytes
assert BLK.size == 32


class FormatError(Exception):
    pass


# ---- codec 1: per-field delta + zigzag + bit-packing in frames of 32 (docs/data-v2.md §1.4) ----
FRAME = 32
UINT = {1: np.uint8, 2: np.uint16, 4: np.uint32}


def _c1_layout(payload, n, fields):
    """Walk a codec-1 payload: returns per field (first_value_offset, [(bit_offset, width, count)]) or raises."""
    pos, out = 0, []
    for f in fields:
        size = SIZES[f["type"]]
        if f["type"] == "f32":
            raise FormatError("codec 1 cannot carry f32 field %s" % f["name"])
        if n == 0:
            out.append((None, []))
            continue
        first = pos
        pos += size
        frames, left = [], n - 1
        while left > 0:
            cnt = min(FRAME, left)
            if pos >= len(payload):
                raise FormatError("payload too short")
            w = payload[pos]
            if w > 8 * size:
                raise FormatError("frame width %d > %d bits for %s" % (w, 8 * size, f["name"]))
            frames.append((pos + 1, w, cnt))
            pos += 1 + (w * cnt + 7) // 8
            left -= cnt
        out.append((first, frames))
    if pos != len(payload):
        raise FormatError("payload length %d, codec 1 content ends at %d" % (len(payload), pos))
    return out


def _c1_decode_many(buf, items, fields, dt):
    """Bulk-decode codec-1 blocks. buf: numpy uint8 of the whole file (+8 pad). items: [(payload_offset, n, layout)].
    Returns a structured array (dt) with all their samples, in order."""
    total = sum(n for _o, n, _l in items)
    res = np.empty(total, dtype=dt)
    starts = np.cumsum([0] + [n for _o, n, _l in items])[:-1]
    for fi, f in enumerate(fields):
        size = SIZES[f["type"]]
        ut = UINT[size]
        firsts, fpos, bitpos, width, cnt = [], [], [], [], []
        for (off, n, lay), st in zip(items, starts):
            if n == 0:
                continue
            first, frames = lay[fi]
            firsts.append(off + first)
            fpos.append(st)
            d = st + 1
            for o, w, c in frames:
                bitpos.append((off + o) * 8)
                width.append(w)
                cnt.append(c)
                d += c
        col = np.zeros(total, dtype=np.uint64)
        if firsts:
            fo = np.asarray(firsts, dtype=np.int64)
            fv = np.zeros(len(fo), dtype=np.uint64)
            for k in range(size):
                fv |= buf[fo + k].astype(np.uint64) << np.uint64(8 * k)
            col[np.asarray(fpos)] = fv
        if cnt:
            cnt_a = np.asarray(cnt, dtype=np.int64)
            w_a = np.repeat(np.asarray(width, dtype=np.int64), cnt_a)
            # bit offset of every value: frame start + index-in-frame * width
            idx_in = np.arange(cnt_a.sum()) - np.repeat(np.cumsum(cnt_a) - cnt_a, cnt_a)
            bp = np.repeat(np.asarray(bitpos, dtype=np.int64), cnt_a) + idx_in * w_a
            by, sh = bp >> 3, (bp & 7).astype(np.uint64)
            v = np.zeros(len(bp), dtype=np.uint64)
            for k in range(5):                      # widths <= 32 + shift 7 -> 39 bits -> 5 bytes
                v |= buf[by + k].astype(np.uint64) << np.uint64(8 * k)
            v = (v >> sh) & ((np.uint64(1) << w_a.astype(np.uint64)) - np.uint64(1))
            delta = (v >> np.uint64(1)) ^ (np.uint64(0) - (v & np.uint64(1)))   # un-zigzag (mod 2^64)
            # destination: every sample but each block's first
            mask = np.ones(total, dtype=bool)
            mask[np.asarray(fpos, dtype=np.int64)] = False
            col[mask] = delta
        # segmented running sum (mod 2^64), restarted at each block's first sample, then truncate to the field width
        cs = np.cumsum(col, dtype=np.uint64)
        base = np.zeros(total, dtype=np.uint64)
        if fpos:
            fp = np.asarray(fpos, dtype=np.int64)
            prev = np.where(fp > 0, cs[np.maximum(fp - 1, 0)], np.uint64(0))
            seg = np.zeros(total, dtype=np.int64)
            seg[fp] = 1
            seg = np.cumsum(seg) - 1
            base = prev[seg]
        res[f["name"]] = (cs - base).astype(ut).view(dt[f["name"]])
    return res


def _sample_dtype(header):
    fields = header["sample"]["fields"]
    size = header["sample"]["size"]
    if sum(SIZES[f["type"]] for f in fields) != size:
        raise FormatError("sample.size %s != sum of field sizes" % size)
    names = [f["name"] for f in fields]
    if "dt_us" not in names:
        raise FormatError("sample.fields must include dt_us")
    return np.dtype([(f["name"], TYPES[f["type"]]) for f in fields])


def decode(path):
    data = open(path, "rb").read()
    problems = []
    if data[:4] != b"PCR2":
        raise FormatError("not a PCR2 file")
    version, _res, jlen = struct.unpack_from("<HHI", data, 4)
    if version != 1:
        raise FormatError("unsupported format_version %d" % version)
    header = json.loads(data[12:12 + jlen].decode("utf-8"))
    pos = 12 + jlen
    pos += (-pos) % 4
    dt = _sample_dtype(header)

    fields = header["sample"]["fields"]
    buf = None
    blocks, chunks, end = [], [], None
    expect_seq = 0
    while pos < len(data):
        magic = data[pos:pos + 4]
        if magic == b"END2":
            (elen,) = struct.unpack_from("<I", data, pos + 4)
            end = json.loads(data[pos + 8:pos + 8 + elen].decode("utf-8"))
            pos += 8 + elen
            break
        if magic != b"BLK2":
            problems.append("unexpected bytes at offset %d (%r): stopped reading" % (pos, magic))
            break
        if pos + 32 > len(data):
            problems.append("truncated block header at offset %d" % pos)
            break
        _m, seq, first_us, first_idx, n, codec, flags, plen, crc = BLK.unpack_from(data, pos)
        payload = data[pos + 32:pos + 32 + plen]
        b = {"seq": seq, "first_mono_us": first_us, "first_sample_index": first_idx, "n": n,
             "flags": flags, "offset": pos, "ok": True}
        if len(payload) < plen:
            problems.append("block %d truncated (%d of %d bytes)" % (seq, len(payload), plen))
            b["ok"] = False
            blocks.append(b)
            break
        if seq != expect_seq:
            problems.append("block_seq jumps from %d to %d" % (expect_seq, seq))
        expect_seq = seq + 1
        b["codec"] = codec
        if codec not in (0, 1):
            problems.append("block %d: unknown codec %d, skipped" % (seq, codec))
            b["ok"] = False
        elif zlib.crc32(payload) & 0xFFFFFFFF != crc:
            problems.append("block %d: CRC mismatch, skipped" % seq)
            b["ok"] = False
        elif codec == 0 and plen != n * dt.itemsize:
            problems.append("block %d: payload %d != n*size %d, skipped" % (seq, plen, n * dt.itemsize))
            b["ok"] = False
        if b["ok"] and codec == 1:
            try:
                chunks.append(("c1", first_us, n, (pos + 32, n, _c1_layout(payload, n, fields))))
            except FormatError as e:
                problems.append("block %d: %s, skipped" % (seq, e))
                b["ok"] = False
        elif b["ok"] and n:
            chunks.append(("raw", first_us, n, np.frombuffer(payload, dtype=dt, count=n)))
        blocks.append(b)
        pos += 32 + plen

    c1 = [c[3] for c in chunks if c[0] == "c1" and c[2]]
    c1_arr = None
    if c1:
        buf = np.frombuffer(data + b"\0" * 8, dtype=np.uint8)
        c1_arr = _c1_decode_many(buf, c1, fields, dt)
    out_dt = np.dtype([("t_us", "<i8")] + [(n_, dt[n_]) for n_ in dt.names])
    total = sum(c[2] for c in chunks)
    samples = np.empty(total, dtype=out_dt)
    i = j = 0
    for kind, first_us, k, a in chunks:
        if k == 0:
            continue
        if kind == "c1":
            a = c1_arr[j:j + k]
            j += k
        dts = a["dt_us"].astype(np.int64)
        samples["t_us"][i:i + k] = first_us + np.cumsum(dts) - dts[0]
        for n_ in dt.names:
            samples[n_][i:i + k] = a[n_]
        i += k
    if pos < len(data) and end is not None:
        problems.append("%d trailing bytes after END2" % (len(data) - pos))
    return header, end, blocks, samples, problems


def summarize(header, end, blocks, samples):
    fields = header["sample"]["fields"]
    n = len(samples)
    s = {"samples": int(n), "blocks": len(blocks), "bad_blocks": sum(1 for b in blocks if not b["ok"]),
         "complete": bool(end and end.get("complete", True)), "has_end": end is not None,
         "lost_flags": sum(1 for b in blocks if b["flags"] & 1)}
    if n >= 2:
        t = samples["t_us"]
        dur_us = int(t[-1] - t[0])
        d = np.diff(t)
        med = float(np.median(d))
        s.update({
            "duration_ms": dur_us / 1000.0,
            "rate_measured_hz": (n - 1) / (dur_us / 1e6) if dur_us > 0 else None,
            "dt_median_us": med, "dt_p99_us": float(np.percentile(d, 99)), "dt_max_us": int(d.max()),
            "gaps": int(np.sum(d > max(2.5 * med, med + 500))),
            "gap_ms_total": float(np.sum(d[d > max(2.5 * med, med + 500)]) / 1000.0),
        })
    sa, ea = header.get("start_anchor") or {}, (end or {}).get("stop_anchor") or {}
    if sa.get("synced") and ea.get("synced") and ea.get("mono_us") and sa.get("mono_us"):
        mono = (ea["mono_us"] - sa["mono_us"]) / 1000.0
        utc = ea["utc_ms"] - sa["utc_ms"]
        if mono > 0:
            s["clock_drift_ppm"] = (utc - mono) / mono * 1e6
    ch = {}
    for f in fields:
        if f["name"] == "dt_us" or n == 0:
            continue
        col = samples[f["name"]]
        scale = f.get("scale", 1)
        info = {"unit": f.get("unit"), "min": float(col.min()) * scale, "max": float(col.max()) * scale,
                "sensor": f.get("sensor")}
        if f["type"] in ("i16", "i8", "i32"):
            lim = {"i16": 32767, "i8": 127, "i32": 2147483647}[f["type"]]
            info["saturated"] = int(np.sum((col >= lim) | (col <= -lim)))
        ch[f["name"]] = info
    s["channels"] = ch
    sat = {}
    for name, info in ch.items():
        key = info.get("sensor") or ("gyro" if name.startswith("g") else name)
        sat[key] = sat.get(key, 0) + info.get("saturated", 0)
    s["saturated_by_sensor"] = sat
    return s
