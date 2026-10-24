#!/usr/bin/env python3
"""
Validate / summarise a Perceptio 2.0 PCR2 recording (docs/data-v2.md). Standalone (no numpy).

    python tools/pcr2_dump.py REC.pcr2              # summary, exit 1 on any problem
    python tools/pcr2_dump.py REC.pcr2 -o out.csv   # samples scaled to units, t_us absolute

The backend's own decoder (pcr2_check.py) is authoritative; this one is for a quick look on the bench.
"""
import argparse, json, struct, sys, zlib

TYPES = {"u8": "B", "i8": "b", "u16": "H", "i16": "h", "u32": "I", "i32": "i", "f32": "f"}
BLK = struct.Struct("<4sIQIHBBII")   # 32 B

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("file"); ap.add_argument("-o", "--out")
    a = ap.parse_args()
    d = open(a.file, "rb").read()
    problems = []
    if d[:4] != b"PCR2": sys.exit("not a PCR2 file")
    ver, _, jl = struct.unpack_from("<HHI", d, 4)
    hdr = json.loads(d[12:12 + jl].decode("utf-8"))
    pos = (12 + jl + 3) & ~3
    fields = hdr["sample"]["fields"]
    fmt = "<" + "".join(TYPES[f["type"]] for f in fields)
    size = struct.calcsize(fmt)
    if size != hdr["sample"]["size"]: problems.append(f"sample.size {hdr['sample']['size']} != fields {size}")
    if fields[0]["name"] != "dt_us" or fields[0]["type"] != "u16": problems.append("first field must be dt_us u16")
    print(f"PCR2 v{ver}: {hdr['rec_id']}  tag '{hdr['rec_tag']}'  by {hdr['started_by']}  fw {hdr['fw']}  rate {hdr['nominal_rate_hz']} Hz  sample {size} B")
    print("  start_anchor", hdr["start_anchor"])
    samples, blocks, bad, seq, gaps, lost_flags = 0, 0, 0, 0, 0, 0
    first_t = last_t = None; dts = []; rows = []; end = None
    scales = [f.get("scale", 1) for f in fields]
    while pos + 4 <= len(d):
        tag = d[pos:pos + 4]
        if tag == b"END2":
            el = struct.unpack_from("<I", d, pos + 4)[0]
            end = json.loads(d[pos + 8:pos + 8 + el].decode("utf-8")); pos += 8 + el; break
        if tag != b"BLK2": problems.append(f"unexpected bytes at {pos}: {tag!r}"); break
        if pos + 32 > len(d): problems.append("truncated block header"); break
        m, bseq, first_us, first_idx, n, codec, flags, plen, crc = BLK.unpack_from(d, pos)
        if pos + 32 + plen > len(d): problems.append(f"block {bseq} truncated (power cut?)"); break
        payload = d[pos + 32:pos + 32 + plen]
        if bseq != seq: problems.append(f"block seq {bseq}, expected {seq}")
        seq = bseq + 1
        if codec != 0: problems.append(f"block {bseq}: codec {codec}")
        if plen != n * size: problems.append(f"block {bseq}: payload_len {plen} != {n}×{size}")
        if first_idx != samples: problems.append(f"block {bseq}: first_sample_index {first_idx}, expected {samples}")
        if zlib.crc32(payload) != crc: bad += 1
        else:
            t = first_us
            for i in range(n):
                v = struct.unpack_from(fmt, payload, i * size)
                if i: t += v[0]; dts.append(v[0])
                if a.out: rows.append((t,) + tuple(x * s for x, s in zip(v[1:], scales[1:])))
            if first_t is None: first_t = first_us
            if last_t is not None and first_us - last_t > 65535: gaps += 1
            last_t = t
        if flags & 1: lost_flags += 1
        samples += n; blocks += 1; pos += 32 + plen
    if pos != len(d): problems.append(f"{len(d) - pos} trailing bytes")
    span = (last_t - first_t) / 1e6 if first_t is not None and last_t else 0
    rate = (samples - 1) / span if span > 0 else 0
    print(f"  blocks {blocks}  samples {samples}  bad_crc {bad}  lost_flags {lost_flags}  gaps>65ms {gaps}  span {span:.3f} s  rate_measured {rate:.2f} Hz")
    if dts:
        dts.sort(); print(f"  dt_us min {dts[0]} median {dts[len(dts)//2]} max {dts[-1]}")
    if end: print("  END2", end)
    else: print("  no END2: incomplete (power lost while recording)")
    if a.out:
        with open(a.out, "w") as f:
            f.write("t_us," + ",".join(x["name"] for x in fields[1:]) + "\n")
            for r in rows: f.write(f"{r[0]}," + ",".join(f"{x:.5g}" for x in r[1:]) + "\n")
        print("  wrote", a.out)
    for p in problems: print("  PROBLEM:", p)
    sys.exit(1 if problems or bad else 0)

if __name__ == "__main__":
    main()
