#!/usr/bin/env python3
"""
Parse a Perceptio 2.0 recording (.p2rec = raw NAND pages from GET /api/v1/recordings/<rec_id>).

    python tools/p2rec.py REC.p2rec            # summary
    python tools/p2rec.py REC.p2rec -o out.csv # samples as CSV, scaled to g / dps / g
    python tools/p2rec.py REC.p2rec -o out.npz # numpy: t_us, gyro_dps, accel_g, hg_g, temp_c

Format (include/store.h): 2048 B pages, each = 16 B page header + payload.
  page header : <H magic 0x3250 'P2'> <B type 1 hdr/2 data/3 trailer> <B n samples> <I boot_id> <H rec_n> <H page> <I first_sample>
  HEADER page : store_header_t   (identity, tag, sensor config, start anchors)
  DATA page   : n × 20 B samples: int16 LE  TEMP GX GY GZ AX AY AZ HGX HGY HGZ
  TRAILER page: store_trailer_t  (counts, first/last sample monotonic time, stop anchors)
Time base: t(k) = first_sample_mono_us + k / odr_hz  (anchors in the trailer; the header's start_utc_ms maps it to UTC).
"""
import argparse, struct, sys

PAGE = 2048
HDR = struct.Struct("<HBBIHHI")                                   # 16 B
HEADER = struct.Struct("<HHHBB16s16s65s15sqqHHHHfff48s")          # store_header_t
TRAILER = struct.Struct("<qqqqIIII15sB")                          # store_trailer_t
SAMPLE = struct.Struct("<10h")

def cstr(b): return b.split(b"\0", 1)[0].decode("utf-8", "replace")

def parse(path):
    data = open(path, "rb").read()
    if len(data) % PAGE: print(f"warning: {len(data)} bytes is not a whole number of pages", file=sys.stderr)
    header = trailer = None
    samples, gaps, pages = [], 0, 0
    expect = 0
    for off in range(0, len(data) - PAGE + 1, PAGE):
        pg = data[off:off + PAGE]
        magic, typ, n, boot_id, rec_n, page, first = HDR.unpack_from(pg, 0)
        if magic != 0x3250:
            gaps += 1; continue                                   # blank page = lost block
        pages += 1
        if typ == 1:
            f = HEADER.unpack_from(pg, 16)
            header = dict(version=f[0], sample_bytes=f[1], samples_per_page=f[2], hw_rev=f[3], device_id=cstr(f[5]),
                          fw=cstr(f[6]), rec_tag=cstr(f[7]), by=cstr(f[8]), start_mono_us=f[9], start_utc_ms=f[10],
                          odr_hz=f[11], xl_fs_g=f[12], gy_fs_dps=f[13], hg_fs_g=f[14], xl_mg_lsb=f[15],
                          gy_mdps_lsb=f[16], hg_mg_lsb=f[17], channels=cstr(f[18]), boot_id=boot_id, rec_n=rec_n)
        elif typ == 2:
            if first != expect: gaps += 1                          # missing samples (P-FAIL / full / dropped)
            for i in range(n):
                samples.append(SAMPLE.unpack_from(pg, 16 + i * 20))
            expect = first + n
        elif typ == 3:
            f = TRAILER.unpack_from(pg, 16)
            trailer = dict(first_sample_mono_us=f[0], last_sample_mono_us=f[1], stop_mono_us=f[2], stop_utc_ms=f[3],
                           samples=f[4], dropped=f[5], pages=f[6], saturated=f[7], stop_by=cstr(f[8]))
    return header, trailer, samples, pages, gaps

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file"); ap.add_argument("-o", "--out", help=".csv or .npz")
    a = ap.parse_args()
    h, t, s, pages, gaps = parse(a.file)
    if not h: sys.exit("no header page found")
    odr = h["odr_hz"]
    print(f"recording {h['device_id']}-{h['boot_id']}-{h['rec_n']}  tag '{h['rec_tag']}'  by {h['by']}  fw {h['fw']}")
    print(f"  {odr} Hz, accel ±{h['xl_fs_g']} g ({h['xl_mg_lsb']} mg/LSB), gyro ±{h['gy_fs_dps']} dps ({h['gy_mdps_lsb']} mdps/LSB), high-g ±{h['hg_fs_g']} g ({h['hg_mg_lsb']} mg/LSB)")
    print(f"  start utc_ms {h['start_utc_ms'] or 'unknown'}  pages {pages}  samples {len(s)} = {len(s)/odr:.1f} s  gaps {gaps}")
    if t:
        span = (t["last_sample_mono_us"] - t["first_sample_mono_us"]) / 1e6
        rate = (t["samples"] - 1) / span if span > 0 and t["samples"] > 1 else 0
        print(f"  trailer: {t['samples']} samples, {t['dropped']} dropped, {t['saturated']} saturated, stopped by {t['stop_by']}, "
              f"measured rate {rate:.3f} Hz ({(rate/odr-1)*1e6:+.0f} ppm vs nominal)")
    else:
        print("  no trailer: power was lost while recording (data up to the last page is valid)")
    if not a.out: return
    xl, gy, hg = h["xl_mg_lsb"] / 1000, h["gy_mdps_lsb"] / 1000, h["hg_mg_lsb"] / 1000
    t0 = t["first_sample_mono_us"] if t else h["start_mono_us"]
    if a.out.endswith(".npz"):
        import numpy as np
        arr = np.array(s, dtype=np.int16)
        np.savez(a.out, t_us=t0 + np.arange(len(s)) * 1e6 / odr, temp_c=25 + arr[:, 0] / 256.0,
                 gyro_dps=arr[:, 1:4] * gy, accel_g=arr[:, 4:7] * xl, hg_g=arr[:, 7:10] * hg, odr_hz=odr,
                 start_utc_ms=h["start_utc_ms"], rec_tag=h["rec_tag"])
    else:
        with open(a.out, "w") as f:
            f.write("t_us,temp_c,gx_dps,gy_dps,gz_dps,ax_g,ay_g,az_g,hx_g,hy_g,hz_g\n")
            for k, v in enumerate(s):
                f.write(f"{t0 + k*1e6/odr:.0f},{25+v[0]/256:.2f},{v[1]*gy:.3f},{v[2]*gy:.3f},{v[3]*gy:.3f},"
                        f"{v[4]*xl:.4f},{v[5]*xl:.4f},{v[6]*xl:.4f},{v[7]*hg:.3f},{v[8]*hg:.3f},{v[9]*hg:.3f}\n")
    print(f"  wrote {a.out}")

if __name__ == "__main__":
    main()
