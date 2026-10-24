"""Write a synthetic PCR2 recording (docs/data-v2.md) -- the reference ENCODER for the firmware.

    python make_pcr2.py OUT.pcr2 --rec-id ID --device ID --seconds 120 [--rate 927] [--no-end] [--corrupt-block N] [--codec 0|1]

Layout written = the example in data-v2.md (dt_us + gyro xyz + accel16 xyz + accel320 xyz, 20 bytes).
Content: slow swing motion + an impact every ~3 s (spikes that saturate the 16 g sensor but not the 320 g one),
1 ms nominal period with jitter, and one 40 ms gap, so every measurement in the decoder has something to find.
"""
import argparse
import json
import math
import random
import struct
import time
import zlib

BLK = struct.Struct("<4sIQIHBBII")
GS, A1, A2 = 2000 / 32768, 16 / 32768, 320 / 32768        # dps/LSB, g/LSB, g/LSB


def header_json(rec_id, device, mono0_us, utc0_ms, rate):
    return {
        "rec_id": rec_id, "device_id": device, "boot_id": int(rec_id.split("-")[1]), "rec_tag": "sim",
        "fw": "0.0.1-sim", "hw_rev": "SIM", "started_by": "cmd",
        "start_anchor": {"mono_us": mono0_us, "utc_ms": utc0_ms, "synced": True},
        "nominal_rate_hz": 927,          # design rate (user, 2026-10-22)
        "sample": {"size": 20, "fields": [
            {"name": "dt_us", "type": "u16", "unit": "us"},
            {"name": "gx", "type": "i16", "scale": GS, "unit": "dps"},
            {"name": "gy", "type": "i16", "scale": GS, "unit": "dps"},
            {"name": "gz", "type": "i16", "scale": GS, "unit": "dps"},
            {"name": "a1x", "type": "i16", "scale": A1, "unit": "g", "sensor": "accel_16g"},
            {"name": "a1y", "type": "i16", "scale": A1, "unit": "g", "sensor": "accel_16g"},
            {"name": "a1z", "type": "i16", "scale": A1, "unit": "g", "sensor": "accel_16g"},
            {"name": "a2x", "type": "i16", "scale": A2, "unit": "g", "sensor": "accel_320g"},
            {"name": "a2y", "type": "i16", "scale": A2, "unit": "g", "sensor": "accel_320g"},
            {"name": "a2z", "type": "i16", "scale": A2, "unit": "g", "sensor": "accel_320g"}]},
        "sensors": {"gyro": {"part": "SIM", "fsr_dps": 2000, "odr_hz": 1000},
                    "accel_16g": {"part": "SIM", "fsr_g": 16, "odr_hz": 1000},
                    "accel_320g": {"part": "SIM", "fsr_g": 320, "odr_hz": 1000}},
        "sim_rate_hz": rate,
    }


SIZES = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4}
FMT = {"u8": "<B", "i8": "<b", "u16": "<H", "i16": "<h", "u32": "<I", "i32": "<i"}


def encode_c1(rows, fields):
    """Codec 1 (data-v2 §1.4) -- the reference the firmware's C encoder must match byte for byte.
    rows: list of samples, each a list of ints in header field order. Returns the payload bytes.
    Per field, in header order:  first value (field's own size, LE)  then the n-1 deltas in frames of 32:
      u8 width w, then the frame's zigzag deltas packed LSB-first at w bits each, padded to a whole byte.
    Delta and zigzag are computed modulo 2^bits of the field (wrap-around), so w <= field bits."""
    out = bytearray()
    n = len(rows)
    for fi, f in enumerate(fields):
        bits = 8 * SIZES[f["type"]]
        mask = (1 << bits) - 1
        if n == 0:
            continue
        out += struct.pack(FMT[f["type"]], rows[0][fi])
        zz = []
        for k in range(1, n):
            d = (rows[k][fi] - rows[k - 1][fi]) & mask                 # wrap to the field width
            d = d - (1 << bits) if d >> (bits - 1) else d              # as signed
            zz.append(((d << 1) ^ (d >> (bits - 1))) & mask)           # zigzag
        for a in range(0, len(zz), 32):
            frame = zz[a:a + 32]
            w = max(frame).bit_length()
            out.append(w)
            acc, nb = 0, 0
            for v in frame:
                acc |= v << nb
                nb += w
                while nb >= 8:
                    out.append(acc & 0xFF)
                    acc >>= 8
                    nb -= 8
            if nb:
                out.append(acc & 0xFF)
    return bytes(out)


def transcode(src, dst, codec=1):
    """Rewrite a codec-0 PCR2 file with every block in `codec` (header and END2 kept)."""
    data = open(src, "rb").read()
    jlen = struct.unpack_from("<I", data, 8)[0]
    h = json.loads(data[12:12 + jlen])
    fields = h["sample"]["fields"]
    rowfmt = "<" + "".join(FMT[f["type"]][1] for f in fields)
    size = struct.calcsize(rowfmt)
    pos = 12 + jlen + ((-(12 + jlen)) % 4)
    out = bytearray(data[:pos])
    while pos < len(data) and data[pos:pos + 4] == b"BLK2":
        m, seq, t0, i0, n, c, flags, plen, crc = BLK.unpack_from(data, pos)
        payload = data[pos + 32:pos + 32 + plen]
        assert c == 0
        rows = [list(struct.unpack_from(rowfmt, payload, k * size)) for k in range(n)]
        p = encode_c1(rows, fields) if codec == 1 else payload
        out += BLK.pack(b"BLK2", seq, t0, i0, n, codec, flags, len(p), zlib.crc32(p) & 0xFFFFFFFF) + p
        pos += 32 + plen
    out += data[pos:]
    open(dst, "wb").write(out)
    return len(data), len(out)


def clip(v):
    return max(-32768, min(32767, int(round(v))))


def write(path, rec_id, device, seconds, rate=927, end=True, corrupt_block=None, block_samples=400, codec=1):
    rnd = random.Random(7)
    mono0 = 4_000_000_000
    utc0 = int(time.time() * 1000) - int(seconds * 1000)
    hj = json.dumps(header_json(rec_id, device, mono0, utc0, rate)).encode()
    out = bytearray(b"PCR2" + struct.pack("<HHI", 1, 0, len(hj)) + hj)
    out += b"\0" * ((-len(out)) % 4)
    period = 1e6 / rate
    n_total = int(seconds * rate)
    t = mono0
    idx, seq = 0, 0
    gap_at = n_total // 2
    while idx < n_total:
        n = min(block_samples, n_total - idx)
        payload = bytearray()
        first_t = None
        prev = None
        flags = 0
        for k in range(n):
            i = idx + k
            step = period + rnd.uniform(-40, 40)
            if i == gap_at:
                step += 40_000                      # one 40 ms hole
                if k != 0:
                    break
                flags |= 1
            t += step
            tt = int(t)
            if first_t is None:
                first_t = tt
            dt = 0 if prev is None else tt - prev
            prev = tt
            sec = (tt - mono0) / 1e6
            swing = math.sin(2 * math.pi * 0.7 * sec)
            gx, gy, gz = 900 * swing, 300 * math.cos(2 * math.pi * 0.7 * sec), 150 * math.sin(sec)
            ax, ay, az = 2.5 * swing, 1.2, 0.9 * math.cos(sec)
            phase = sec % 3.0
            if phase < 0.004:                       # impact: ~4 ms spike
                peak = 45 + 25 * math.sin(i)
                ax += peak * (1 - phase / 0.004)
                gz += 1400 * (1 - phase / 0.004)
            noise = lambda s: rnd.gauss(0, s)
            payload += struct.pack("<H9h", dt,
                                   clip((gx + noise(3)) / GS), clip((gy + noise(3)) / GS), clip((gz + noise(3)) / GS),
                                   clip((ax + noise(0.02)) / A1), clip((ay + noise(0.02)) / A1), clip((az + noise(0.02)) / A1),
                                   clip((ax + noise(0.3)) / A2), clip((ay + noise(0.3)) / A2), clip((az + noise(0.3)) / A2))
        n_written = len(payload) // 20
        if codec == 1:
            payload = bytearray(encode_c1([list(struct.unpack_from("<H9h", payload, k * 20)) for k in range(n_written)],
                                          header_json(rec_id, device, 0, 0, rate)["sample"]["fields"]))
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        if corrupt_block is not None and seq == corrupt_block:
            payload[10] ^= 0xFF                    # CRC no longer matches
        out += BLK.pack(b"BLK2", seq, first_t, idx, n_written, codec, flags, len(payload), crc) + payload
        idx += n_written
        seq += 1
    if end:
        ej = json.dumps({"stop_anchor": {"mono_us": int(t), "utc_ms": utc0 + int((t - mono0) / 1000) + 1, "synced": True},
                         "samples": idx, "blocks": seq, "dropped": 0, "stopped_by": "cmd", "complete": True}).encode()
        out += b"END2" + struct.pack("<I", len(ej)) + ej
    open(path, "wb").write(out)
    return {"samples": idx, "blocks": seq, "bytes": len(out)}


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--rec-id", required=True)
    ap.add_argument("--device", required=True)
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--rate", type=float, default=927)
    ap.add_argument("--no-end", action="store_true")
    ap.add_argument("--corrupt-block", type=int)
    ap.add_argument("--codec", type=int, default=1, choices=(0, 1))
    a = ap.parse_args()
    print(write(a.out, a.rec_id, a.device, a.seconds, a.rate, not a.no_end, a.corrupt_block, codec=a.codec))
