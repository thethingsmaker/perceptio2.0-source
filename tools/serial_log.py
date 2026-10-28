"""
Perceptio serial monitor that never misses a boot.

The board's USB serial port disappears and comes back on every chip restart; most monitors (PlatformIO in Cursor
included) do not reopen it, so everything printed until you restart them is lost. This one reopens the port as soon
as it is back (polling every 50 ms), timestamps every line, decodes backtraces, and appends everything to
logs/serial-YYYYMMDD.log in the project folder.

Run it with PlatformIO's Python (it already has pyserial), from the project folder:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools\\serial_log.py COM8
Stop with Ctrl+C. Do not run it together with another monitor on the same port.
"""
import datetime, glob, os, re, subprocess, sys, time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run this with PlatformIO's python (see the header of this file)")

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, ".pio", "build", "perceptio2", "firmware.elf")
LOGDIR = os.path.join(ROOT, "logs")
os.makedirs(LOGDIR, exist_ok=True)


def find_addr2line():
    home = os.path.expanduser("~")
    hits = glob.glob(os.path.join(home, ".platformio", "packages", "toolchain-xtensa-esp*", "bin", "xtensa-esp32s3-elf-addr2line*"))
    return hits[0] if hits else None


ADDR2LINE = find_addr2line()


def decode(line):
    """'Backtrace: 0x4200CE89:0x3C179430 ...' -> list of 'func at file:line' (needs the ELF of the running build)."""
    if not ADDR2LINE or not os.path.exists(ELF):
        return []
    pcs = re.findall(r"0x(4[0-9a-fA-F]{7}):0x[0-9a-fA-F]{8}", line)
    if not pcs:
        return []
    try:
        out = subprocess.run([ADDR2LINE, "-pfiaC", "-e", ELF] + ["0x" + p for p in pcs],
                             capture_output=True, text=True, timeout=10).stdout
        return ["    " + l for l in out.splitlines() if l.strip()]
    except Exception as e:  # decoding is a convenience, never a reason to lose log lines
        return ["    (backtrace decode failed: %s)" % e]


def emit(text, logf):
    stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    s = "%s > %s" % (stamp, text)
    print(s, flush=True)
    logf.write(s + "\n")
    logf.flush()


def main():
    logpath = os.path.join(LOGDIR, "serial-%s.log" % datetime.date.today().strftime("%Y%m%d"))
    print("logging %s to %s  (backtrace decode: %s)" % (PORT, logpath, "on" if ADDR2LINE and os.path.exists(ELF) else "off"))
    with open(logpath, "a", encoding="utf-8") as logf:
        waiting_said = False
        while True:
            try:
                ser = serial.Serial(PORT, 115200, timeout=0.2)
            except (serial.SerialException, OSError):
                if not waiting_said:
                    emit("--- %s not available, waiting for it ---" % PORT, logf)
                    waiting_said = True
                time.sleep(0.05)
                continue
            emit("--- %s open ---" % PORT, logf)
            waiting_said = False
            buf = b""
            try:
                while True:
                    chunk = ser.read(4096)
                    if not chunk:
                        continue
                    buf += chunk
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode("utf-8", "replace").rstrip("\r")
                        line = re.sub(r"\x1b\[[0-9;]*m", "", line)      # colour codes
                        emit(line, logf)
                        if line.startswith("Backtrace:"):
                            for d in decode(line):
                                emit(d, logf)
            except (serial.SerialException, OSError):
                if buf:
                    emit(buf.decode("utf-8", "replace"), logf)
                emit("--- %s lost (board restarted or unplugged) ---" % PORT, logf)
                try:
                    ser.close()
                except Exception:
                    pass


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped")
