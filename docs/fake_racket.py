"""Fake Perceptio 2.0 racket -- speaks docs/protocol-v2.md exactly as the firmware must.

Use it to exercise the backend/dashboard without hardware, and as a reference for the firmware:
    ENROL_PW=... /opt/perceptio2/venv/bin/python fake_racket.py --id 0a0b0c0d0e0f [--state-dir DIR] [--run SECONDS]
First run: enrols (shows as "Waiting for approval" in /p2/), waits for approval, stores the
credential in <state-dir>/<id>.json (the firmware's encrypted NVS). Later runs connect directly.
"""
import argparse
import json
import os
import secrets
import ssl
import sys
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt
from paho.mqtt.packettypes import PacketTypes
from paho.mqtt.properties import Properties

HOST, PORT, PATH = "mqtt.padelsense.net", 443, "/mqtt"


def ms():
    return int(time.time() * 1000)


def new_client(client_id, user, pw, will=None, clean_start=False):
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id, protocol=mqtt.MQTTv5, transport="websockets")
    c.ws_set_options(path=PATH)
    c.tls_set(cert_reqs=ssl.CERT_REQUIRED)            # verify the server: never "insecure" (v1 lesson)
    c.username_pw_set(user, pw)
    if will:
        c.will_set(will[0], json.dumps(will[1]), qos=1, retain=True)
    return c


def connect(c, clean_start=False, session_expiry=3600):
    props = Properties(PacketTypes.CONNECT)
    # Racket login: persistent session, so QoS1 commands survive short drops (they still expire).
    # Enrolment login: session_expiry=0 -- it must leave nothing behind for the racket's real session.
    props.SessionExpiryInterval = session_expiry
    c.connect(HOST, PORT, keepalive=30, clean_start=clean_start, properties=props)


def enrol(dev, enrol_pw, info):
    nonce = secrets.token_hex(16)
    got = {}
    done = threading.Event()
    c = new_client(dev, "enrol", enrol_pw)

    def on_connect(cl, u, flags, rc, props=None):
        print("[enrol] connected:", rc, flush=True)
        cl.subscribe("perceptio/enrol/%s/response" % dev, qos=1)
        cl.publish("perceptio/enrol/%s/request" % dev, json.dumps(dict(info, device_id=dev, nonce=nonce)), qos=1)
        print("[enrol] request sent, waiting for a person to approve in /p2/ ...", flush=True)

    def on_message(cl, u, msg):
        p = json.loads(msg.payload)
        if p.get("nonce") != nonce:
            return print("[enrol] ignoring a response with someone else's nonce", flush=True)
        got.update(p)
        done.set()

    c.on_connect, c.on_message = on_connect, on_message
    connect(c, clean_start=True, session_expiry=0)
    c.loop_start()
    done.wait()
    c.loop_stop(); c.disconnect()
    print("[enrol] response:", got.get("status"), flush=True)
    return got


def run(dev, cred, state_path, seconds):
    st = json.loads(state_path.read_text())
    st["boot_id"] = st.get("boot_id", 0) + 1            # persisted, +1 every boot
    state_path.write_text(json.dumps(st))
    boot_id, seq = st["boot_id"], [0]
    base = "perceptio/2/%s/" % dev
    seen_cmds = {}
    t0 = time.monotonic()

    def event(cl, typ, data=None):
        e = {"boot_id": boot_id, "seq": seq[0], "utc_ms": ms(), "mono_ms": int((time.monotonic() - t0) * 1000),
             "type": typ, "data": data}
        seq[0] += 1
        cl.publish(base + "event", json.dumps(e), qos=1)

    hello = {
        "proto": 2, "device_id": dev, "model": "perceptio-2", "hw_rev": "SIM", "fw": "0.0.1-sim", "build": "fake",
        "boot_id": boot_id, "reset_reason": "poweron", "uptime_s": 0,
        "clock": {"utc_ms": ms(), "synced": True, "offset_ms": 0, "last_sync_utc_ms": ms()},
        "state": "idle", "recordings": [],
        "storage": {"total_mb": 1024, "free_mb": 1000}, "power": {"battery_pct": 91, "charging": False, "plugged": False},
        "net": {"ip": "10.0.0.99", "rssi": -55},
        "caps": {"gyro": 1, "accel": [{"fsr_g": 16}, {"fsr_g": 320}], "rate_hz": [1000]},
        "config_hash": "sim000",
    }
    c = new_client(dev, cred["username"], cred["password"], will=(base + "state", {"online": False}))

    def on_connect(cl, u, flags, rc, props=None):
        print("[run] connected:", rc, "session present:", flags.session_present, flush=True)
        cl.subscribe(base + "cmd", qos=1)
        hello["uptime_s"] = int(time.monotonic() - t0)
        hello["clock"]["utc_ms"] = ms()
        cl.publish(base + "hello", json.dumps(hello), qos=1, retain=True)
        cl.publish(base + "state", json.dumps({"online": True, "state": "idle", "since_utc_ms": ms(),
                                                "boot_id": boot_id, "seq": seq[0]}), qos=1, retain=True)
        event(cl, "boot", {"reset_reason": "poweron"})

    def ack(cl, cid, status, **extra):
        cl.publish(base + "ack", json.dumps(dict({"id": cid, "status": status, "utc_ms": ms()}, **extra)), qos=1)

    def on_message(cl, u, msg):
        p = json.loads(msg.payload)
        cid = p.get("id")
        if cid in seen_cmds:                            # duplicate: re-ack the original outcome
            return ack(cl, cid, *seen_cmds[cid])
        if p.get("expires_utc_ms") and ms() > p["expires_utc_ms"]:
            seen_cmds[cid] = ("rejected",); return ack(cl, cid, "rejected", reason="expired")
        ack(cl, cid, "received")
        cmd = p.get("cmd")
        if cmd == "identify":
            print("[run] *** IDENTIFY: blinking for %s s ***" % (p.get("args") or {}).get("seconds", 5), flush=True)
            seen_cmds[cid] = ("applied",); ack(cl, cid, "applied")
        elif cmd == "sync_time":
            seen_cmds[cid] = ("applied",); ack(cl, cid, "applied", result={"offset_ms": 0}); event(cl, "time_synced", {"offset_ms": 0})
        else:
            seen_cmds[cid] = ("rejected",); ack(cl, cid, "rejected", reason="unsupported")

    c.on_connect, c.on_message = on_connect, on_message
    connect(c)
    c.loop_start()
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        time.sleep(10)
        c.publish(base + "telemetry", json.dumps({"utc_ms": ms(), "battery_pct": 91, "charging": False, "rssi": -55,
                                                  "free_mb": 1000, "rec_samples": 0, "dropped_samples": 0}), qos=0)
    print("[run] done (exiting WITHOUT a clean disconnect so the last will fires)", flush=True)
    os._exit(0)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--id", required=True)
    ap.add_argument("--state-dir", default=str(Path(__file__).parent / "fake_state"))
    ap.add_argument("--run", type=int, default=3600)
    a = ap.parse_args()
    sd = Path(a.state_dir); sd.mkdir(parents=True, exist_ok=True)
    sp = sd / ("%s.json" % a.id)
    if not sp.exists() or "password" not in json.loads(sp.read_text()):
        info = {"proto": 2, "model": "perceptio-2", "hw_rev": "SIM", "fw": "0.0.1-sim",
                "caps": {"gyro": 1, "accel": [{"fsr_g": 16}, {"fsr_g": 320}], "rate_hz": [1000]}}
        r = enrol(a.id, os.environ["ENROL_PW"], info)
        if r.get("status") != "approved":
            sys.exit("not approved")
        sp.write_text(json.dumps({"username": r["username"], "password": r["password"], "ws_url": r["ws_url"]}))
        os.chmod(sp, 0o600)
    run(a.id, json.loads(sp.read_text()), sp, a.run)
