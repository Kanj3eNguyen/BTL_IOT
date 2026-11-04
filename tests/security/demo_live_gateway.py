"""Exercise the running production MQTT gateway and its real dashboard counters.

Uses an administrator-provisioned, temporary demo-only identity. Its baseline
sample is quarantined atomically; no synthetic measurements reach ThingsBoard.
"""
import argparse
import base64
import json
import queue
import secrets
import sqlite3
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt

from server.mqtt_guard.protocol import authenticate, sign
from server.thingsboard_local.setup import ThingsBoard, load_account

ROOT = Path(__file__).resolve().parents[2]
PRIVATE = ROOT / ".private"
CONFIG = PRIVATE / "mqtt_guard/config.json"


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def save_config(config):
    # Both the temporary and final file are in the known private directory on D.
    temporary = CONFIG.with_name("config.live-tmp.json")
    temporary.write_text(json.dumps(config, indent=2), encoding="utf-8")
    temporary.replace(CONFIG)


def wait(test, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        result = test()
        if result:
            return result
        time.sleep(.15)
    raise AssertionError("Timed out waiting for the expected gateway/dashboard result")


def prepare():
    config = read(CONFIG)
    if any(d.startswith("live-demo-") for d in config["devices"]):
        raise RuntimeError("A previous live-demo identity exists. Clean up that run before starting another.")
    folder = PRIVATE / ("live_security_" + time.strftime("%Y%m%d_%H%M%S") + "_" + secrets.token_hex(2))
    folder.mkdir()
    (folder / "before-config.json").write_bytes(CONFIG.read_bytes())
    identity = {"device": "live-demo-" + secrets.token_hex(4), "key_id": "demo-key",
                "hmac_key_hex": secrets.token_hex(32), "mqtt_password": secrets.token_urlsafe(24),
                "demo_only": True}
    (folder / "identity.json").write_text(json.dumps(identity), encoding="utf-8")
    config["devices"][identity["device"]] = {k: v for k, v in identity.items() if k != "device"}
    save_config(config)
    print(folder)


def cleanup(folder):
    identity = read(folder / "identity.json")
    config = read(CONFIG)
    current = config["devices"].get(identity["device"])
    if current is not None:
        if current.get("hmac_key_hex") != identity["hmac_key_hex"] or current.get("demo_only") is not True:
            raise RuntimeError("Demo identity changed unexpectedly; no configuration was removed")
        del config["devices"][identity["device"]]
        save_config(config)
    (folder / "revoked.txt").write_text("Temporary identity revoked from gateway configuration.\n")
    print("Đã thu hồi danh tính demo; giữ báo cáo và counter audit.")


def run(folder, no_pause):
    identity, config = read(folder / "identity.json"), read(CONFIG)
    device = identity["device"]
    assert config["devices"][device]["demo_only"] is True
    port = int(config["bind"].rsplit(":", 1)[1])
    assert port == 8885, "This demonstration targets the running gateway on 8885"
    database = Path(config["database"])
    assert database.is_file()

    def query(sql, parameters=()):
        with sqlite3.connect(database, timeout=5) as db:
            db.row_factory = sqlite3.Row
            return [dict(row) for row in db.execute(sql, parameters)]

    def counters():
        return {r["reason"]: r["count"] for r in query("SELECT reason,count FROM counters")}

    def cards(values):
        return {
            "guard_rejected_count": sum(v for k, v in values.items() if k not in ("SESSION", "ACCEPTED", "FORWARDED", "DUPLICATE")),
            "guard_bad_mac_count": values.get("BAD_MAC", 0),
            "guard_replay_rejected_count": sum(values.get(k, 0) for k in ("REPLAY_OR_SEQUENCE_CONFLICT", "SESSION_INVALID", "OLD_HELLO")),
            "guard_duplicate_count": values.get("DUPLICATE", 0),
        }

    tb = ThingsBoard()
    tb.login("tenant@thingsboard.org", load_account())
    production = read(PRIVATE / "thingsboard/device.json")["id"]
    real_device = tb.call("GET", f"/api/plugins/telemetry/DEVICE/{production}/values/timeseries?keys=auth_device_id")
    real_identity = real_device["auth_device_id"][0]["value"]
    prefix, key = "dm/v1/" + device + "/", bytes.fromhex(identity["hmac_key_hex"])
    connected, subscribed, messages = queue.Queue(), queue.Queue(), queue.Queue()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="guard-" + device)
    client.username_pw_set(device, identity["mqtt_password"])
    client.tls_set(ca_certs=str(PRIVATE / "mqtt_guard/ca.pem"))
    client.on_connect = lambda c, u, f, r, p: (connected.put(r), c.subscribe(prefix + "down", 1) if r == 0 else None)
    client.on_subscribe = lambda c, u, m, r, p: subscribed.put(r)
    client.on_message = lambda c, u, m: messages.put(m.payload)
    boot = secrets.token_hex(16)
    checks = []

    def body(kind, **fields):
        return {"v": 1, "type": kind, "device_id": device, "key_id": identity["key_id"],
                "boot_id": boot, "sent_at_ms": int(time.time() * 1000), **fields}

    def publish(topic, wire):
        info = client.publish(prefix + topic, wire, qos=1)
        info.wait_for_publish(5)
        assert info.is_published()

    def session():
        request = secrets.token_hex(16)
        publish("hello", sign(body("hello", request_id=request), key))
        while True:
            grant, _ = authenticate(messages.get(timeout=5), config["devices"])
            if grant.get("type") == "session" and grant.get("request_id") == request:
                assert grant["device_id"] == device and grant["boot_id"] == boot
                return grant["session_id"]

    def expect(reason, wire, description):
        previous = counters().get(reason, 0)
        publish("telemetry", wire)
        wait(lambda: counters().get(reason, 0) > previous)
        checks.append(reason)
        print(f"[PASS] {reason}: {description}", flush=True)

    before = counters()
    print("\nDEMO TRÊN GATEWAY THẬT 8885 → DASHBOARD ĐANG DÙNG", flush=True)
    print("Mở dashboard local, xem bốn ô bảo mật. Không dùng gateway thử 8886.", flush=True)
    print("Counter trước demo: " + json.dumps(cards(before)), flush=True)
    print("Mẫu chuẩn của danh tính demo chỉ lưu cách ly, không chuyển số đo giả vào ThingsBoard.", flush=True)
    if sys.stdin.isatty() and not no_pause:
        input("Nhấn Enter để gửi các gói demo vào gateway thật... ")
    started_ms = int(time.time() * 1000)
    try:
        client.connect("localhost", port)
        client.loop_start()
        assert connected.get(timeout=8) == 0
        assert all(not r.is_failure for r in subscribed.get(timeout=5))
        sid = session()
        data = {"boot_id": 1, "sequence": 1, "uptime_ms": 10000, "driver_present": False,
                "eyes_valid": False, "eyes_closed": None, "eye_closed_probability": None,
                "mouth_valid": False, "mouth_open": None, "mouth_open_probability": None,
                "perclos_valid": True, "perclos_window_ready": False, "perclos": .1,
                "blink_count": 2, "yawn_count": 0, "blink_window_count": 2, "yawn_window_count": 0}
        sample = body("sample", session_id=sid, sequence=1, sample_age_ms=0, data=data)
        wire = sign(sample, key)
        expect("ACCEPTED", wire, "Mẫu chuẩn được kiểm tra và cách ly (demo_only).")
        rows = query("SELECT status FROM outbox WHERE device=?", (device,))
        assert len(rows) == 1 and rows[0]["status"] == "demo", "Demo data must never be pending/sent"
        expect("DUPLICATE", wire, "Phát lại nguyên gói: ô gói trùng tăng.")
        envelope = json.loads(wire)
        changed = json.loads(base64.b64decode(envelope["payload"]))
        changed["data"]["perclos"] = .99
        envelope["payload"] = base64.b64encode(json.dumps(changed).encode()).decode()
        expect("BAD_MAC", json.dumps(envelope).encode(), "Sửa PERCLOS nhưng giữ MAC cũ.")
        expect("BAD_MAC", sign(sample, b"x" * 32), "Ký bằng khóa sai.")
        altered = {**sample, "data": {**data, "blink_count": 999}}
        expect("REPLAY_OR_SEQUENCE_CONFLICT", sign(altered, key), "Tái dùng sequence với nội dung khác.")
        expect("STALE_OR_FUTURE", sign({**sample, "sent_at_ms": int(time.time()*1000)-60000}, key), "Timestamp cũ 60 giây.")
        assert session() != sid
        expect("SESSION_INVALID", wire, "Gửi mẫu thuộc phiên cũ.")
        after, minimum = counters(), cards(before)
        for name, increment in (("guard_rejected_count", 5), ("guard_bad_mac_count", 2),
                                ("guard_replay_rejected_count", 2), ("guard_duplicate_count", 1)):
            minimum[name] += increment
            assert cards(after)[name] >= minimum[name]

        def dashboard_updated():
            names = [*minimum, "auth_device_id", "last_verified_at", "auth_sequence"]
            raw = tb.call("GET", f"/api/plugins/telemetry/DEVICE/{production}/values/timeseries?keys=" + ",".join(names))
            values = {k: v[0]["value"] for k, v in raw.items() if v}
            if (values.get("auth_device_id") == real_identity and
                int(values.get("last_verified_at", 0)) >= started_ms and
                all(int(values.get(k, -1)) >= n for k, n in minimum.items())):
                return values
            return None

        print("Đợi bản tin thật kế tiếp của ESP32 đưa các counter lên dashboard...", flush=True)
        values = wait(dashboard_updated, 75)
        history = tb.call("GET", f"/api/plugins/telemetry/DEVICE/{production}/values/timeseries?"
                          f"keys=auth_device_id&startTs={started_ms-15000}&endTs={int(time.time()*1000)+1000}&agg=NONE&limit=1000")
        assert history.get("auth_device_id") and all(r["value"] == real_identity for r in history["auth_device_id"])
        rows = query("SELECT status FROM outbox WHERE device=?", (device,))
        assert len(rows) == 1 and rows[0]["status"] == "demo"
        report = {"passed": True, "mode": "LIVE_GATEWAY", "gateway_port": port, "identity": device,
                  "checks": checks, "before": cards(before), "after": cards(after),
                  "dashboard": values, "quarantined_rows": 1, "synthetic_measurements_forwarded": 0,
                  "report_folder": str(folder)}
        (folder / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        (PRIVATE / "live_security_latest.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("[PASS] Dashboard thật đã cập nhật: " + json.dumps({k: values[k] for k in minimum}), flush=True)
        print("[PASS] Lịch sử ThingsBoard trong lượt thử chỉ chứa danh tính bo thật.", flush=True)
        print("Báo cáo: " + str(folder / "result.json"), flush=True)
        if sys.stdin.isatty() and not no_pause:
            input("Bạn xem/chụp dashboard ngay bây giờ. Nhấn Enter để thu hồi danh tính demo... ")
    finally:
        client.disconnect()
        client.loop_stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "run", "cleanup"))
    parser.add_argument("--folder", type=Path)
    parser.add_argument("--no-pause", action="store_true")
    args = parser.parse_args()
    if args.action == "prepare":
        prepare()
        return
    if args.folder is None:
        parser.error("--folder is required")
    folder = args.folder.resolve()
    if folder.parent != PRIVATE.resolve() or not folder.name.startswith("live_security_"):
        parser.error("Expected a private live-demo run directory inside this project")
    if args.action == "cleanup":
        cleanup(folder)
    else:
        run(folder, args.no_pause)


if __name__ == "__main__":
    main()
