"""Validate a PCR2 file exactly as the backend will:   python3 pcr2_check.py FILE.pcr2
Needs numpy and pcr2.py next to it. Prints the measured facts and every problem found."""
import json, sys, zlib
sys.path.insert(0, __import__("os").path.dirname(__file__))
import pcr2
assert zlib.crc32(b"123456789") == 0xCBF43926       # the CRC the blocks must use (IEEE / zlib / esp_rom_crc32_le(0,..))
h, e, b, s, p = pcr2.decode(sys.argv[1])
m = pcr2.summarize(h, e, b, s)
m["problems"] = p
print(json.dumps(m, indent=2, default=float))
sys.exit(1 if p else 0)
