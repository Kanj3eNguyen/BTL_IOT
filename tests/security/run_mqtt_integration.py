"""Test TLS/auth/ACL/tamper/replay through a real isolated MQTT gateway.

Run from repo root. Creates its own device, port 8886 and private database;
does not reset the real ESP32 session. --thingsboard also checks persistence.
"""
import argparse
import base64
import json
import os
import queue
import secrets
import socket
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt

from server.mqtt_guard.protocol import authenticate, sign
from server.thingsboard_local.setup import ThingsBoard, load_account

ROOT = Path(__file__).resolve().parents[2]


def wait_for(test, timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = test()
        if value:
            return value
        time.sleep(.1)
    raise AssertionError("Timed out waiting for test condition")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--thingsboard", action="store_true")
    parser.add_argument("--demo", action="store_true", help="Explain each check and pause for ThingsBoard inspection in an interactive terminal")
    parser.add_argument("--no-pause", action="store_true", help="Do not wait for Enter in demo mode")
    args = parser.parse_args()
    if args.demo and not args.thingsboard:
        parser.error("Use --demo with --thingsboard so the demonstration verifies actual persistence")
    folder = ROOT / ".private" / ("mqtt_test_" + time.strftime("%Y%m%d_%H%M%S"))
    folder.mkdir()
    config = json.loads((ROOT / ".private/mqtt_guard/config.json").read_text())
    device = "security-self-test"
    key = secrets.token_bytes(32)
    password = secrets.token_urlsafe(24)
    config.update(bind="127.0.0.1:8886", ntp_port=0, database=str(folder / "state.db"),
                  devices={device: {"key_id": "test-key", "hmac_key_hex": key.hex(), "mqtt_password": password}})
    # Keep the test's upstream MQTT identity separate from the real gateway.
    config["thingsboard"]["client_id"] = "guard-self-test-" + secrets.token_hex(8)
    tb = None
    tb_device = None
    test_name = None
    if args.thingsboard:
        tb = ThingsBoard()
        tb.login("tenant@thingsboard.org", load_account())
        test_name = "MQTT security test " + secrets.token_hex(4)
        tb_device = tb.call("POST", "/api/device", {"name": test_name, "type": "Test"})["id"]["id"]
        config["thingsboard"]["token"] = tb.call("GET", f"/api/device/{tb_device}/credentials")["credentialsId"]
    config_path = folder / "config.json"
    config_path.write_text(json.dumps(config))
    env = {**os.environ, "TEMP": str(ROOT / ".private/tmp"), "TMP": str(ROOT / ".private/tmp"),
           "PYTHONPATH": str(ROOT) + os.pathsep + str(ROOT / ".venv/Lib/site-packages"), "PYTHONDONTWRITEBYTECODE": "1"}
    log = (folder / "gateway.log").open("w", encoding="utf-8")
    command = [sys.executable, "-u", "-m", "server.mqtt_guard.gateway", "--config", str(config_path)]
    if not args.thingsboard:
        command.append("--verify-only")
    process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    messages = queue.Queue()
    connected = queue.Queue()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="guard-" + device)
    client.username_pw_set(device, password)
    client.tls_set(ca_certs=str(ROOT / ".private/mqtt_guard/ca.pem"))
    prefix = "dm/v1/" + device + "/"
    client.on_connect = lambda c, u, f, r, p: (connected.put(r), c.subscribe(prefix + "down", 1) if r == 0 else None)
    client.on_message = lambda c, u, m: messages.put(m.payload)
    boot = secrets.token_hex(16)
    checks = []

    def record(name, explanation):
        checks.append(name)
        if args.demo:
            print(f"[PASS {len(checks):02d}] {name}: {explanation}", flush=True)

    def base(kind, **fields):
        return {"v": 1, "type": kind, "device_id": device, "key_id": "test-key", "boot_id": boot,
                "sent_at_ms": int(time.time() * 1000), **fields}

    def counts():
        try:
            with sqlite3.connect(config["database"]) as db:
                return dict(db.execute("SELECT reason,count FROM counters"))
        except sqlite3.OperationalError:
            return {}

    def expect(reason, before, message, topic="telemetry", explanation=""):
        client.publish(prefix + topic, message, qos=1).wait_for_publish(timeout=5)
        wait_for(lambda: counts().get(reason, 0) > before)
        record(reason, explanation or "Gateway trả đúng quyết định mong đợi.")

    def can_connect():
        try:
            with socket.create_connection(("127.0.0.1", 8886), timeout=.3):
                return True
        except OSError:
            if process.poll() is not None:
                raise AssertionError("Gateway exited; inspect private gateway log")
            return False

    try:
        wait_for(can_connect, 20)
        if args.demo:
            print("\nDEMO BẢO MẬT MQTT — gateway thử riêng 127.0.0.1:8886", flush=True)
            print("Device thử trên ThingsBoard: " + test_name, flush=True)
            print("Mở http://localhost:8080 → Devices → tìm tên trên → Latest telemetry.", flush=True)
            print("Chỉ dùng khóa/mật khẩu sinh riêng cho bài thử; không tấn công device thật.", flush=True)
            if sys.stdin.isatty() and not args.no_pause:
                input("Nhấn Enter để chạy các tình huống (chương trình giữ device thử để xem kết quả cuối)... ")
        client.connect("localhost", 8886)
        client.loop_start()
        assert connected.get(timeout=5) == 0
        time.sleep(.25)
        hello = base("hello", request_id=secrets.token_hex(16))
        client.publish(prefix + "hello", sign(hello, key), 1)
        grant, _ = authenticate(messages.get(timeout=5), config["devices"])
        assert grant["type"] == "session"
        record("TLS_AND_AUTHENTICATED_SESSION", "Kết nối TLS, xác thực MQTT và nhận phiên có HMAC hợp lệ.")
        sid = grant["session_id"]
        data = {"boot_id": 17, "sequence": 1, "uptime_ms": 20000, "driver_present": True,
                "eyes_valid": True, "eyes_closed": False, "eye_closed_probability": .1,
                "mouth_valid": True, "mouth_open": False, "mouth_open_probability": .1,
                "perclos_valid": True, "perclos_window_ready": False, "perclos": .1,
                "blink_count": 2, "yawn_count": 0, "blink_window_count": 2, "yawn_window_count": 0}
        sample = base("sample", session_id=sid, sequence=1, sample_age_ms=20, data=data)
        wire = sign(sample, key)
        expect("ACCEPTED", 0, wire, explanation="Bản tin chuẩn: sequence=1, blink=2, yawn=0, PERCLOS=0.1 (10%).")
        if tb:
            wait_for(lambda: counts().get("FORWARDED", 0) == 1, 20)
            def persisted():
                values = tb.call("GET", f"/api/plugins/telemetry/DEVICE/{tb_device}/values/timeseries?keys=auth_sequence,integrity_status")
                return values if (values.get("integrity_status") and values.get("auth_sequence") and
                                  values["integrity_status"][0]["value"] == "VERIFIED" and
                                  values["auth_sequence"][0]["value"] == "1") else None
            values = wait_for(persisted, 15)
            assert values["integrity_status"][0]["value"] == "VERIFIED", repr(values)
            assert values["auth_sequence"][0]["value"] == "1"
            record("REAL_THINGSBOARD_PERSISTED", "REST xác nhận ThingsBoard đã lưu sequence=1 và VERIFIED.")
        expect("DUPLICATE", 0, wire, explanation="Phát lại đúng byte gói vừa gửi; không tạo mẫu chuyển tiếp mới.")
        envelope = json.loads(wire)
        changed = json.loads(base64.b64decode(envelope["payload"]))
        changed["data"]["perclos"] = .99
        envelope["payload"] = base64.b64encode(json.dumps(changed).encode()).decode()
        expect("BAD_MAC", 0, json.dumps(envelope).encode(), explanation="Đổi PERCLOS từ 0.1 thành 0.99 nhưng giữ MAC cũ: bị loại.")
        expect("BAD_MAC", 1, sign(sample, b"x" * 32), explanation="Dùng khóa HMAC sai để ký: bị loại.")
        sample["data"]["blink_count"] = 999
        expect("REPLAY_OR_SEQUENCE_CONFLICT", 0, sign(sample, key), explanation="Ký hợp lệ nhưng tái dùng sequence=1 với blink=999: bị loại.")
        stale = {**sample, "sent_at_ms": int(time.time() * 1000) - 60000}
        expect("STALE_OR_FUTURE", 0, sign(stale, key), explanation="Gói ký đúng nhưng timestamp cũ 60 giây: bị loại.")
        # Renew session, then replay the earlier correctly signed sample.
        client.publish(prefix + "hello", sign(base("hello", request_id=secrets.token_hex(16)), key), 1)
        wait_for(lambda: counts().get("SESSION", 0) >= 2)
        expect("SESSION_INVALID", 0, wire, explanation="Xin phiên mới rồi gửi lại gói của phiên cũ: bị loại.")
        # Device cannot publish the server's downlink or bypass to TB's topic.
        before = counts().get("ACCEPTED", 0)
        client.publish(prefix + "down", wire, 1).wait_for_publish(5)
        client.publish("v1/devices/me/telemetry", b'{"integrity_status":"VERIFIED"}', 1).wait_for_publish(5)
        time.sleep(.3)
        assert counts().get("ACCEPTED", 0) == before
        record("PUBLISH_ACL", "Gửi xuống topic downlink hoặc topic ThingsBoard trực tiếp không được chấp nhận.")
        rejected = queue.Queue()
        bad_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="guard-" + device)
        bad_client.username_pw_set(device, "incorrect-password")
        bad_client.tls_set(ca_certs=str(ROOT / ".private/mqtt_guard/ca.pem"))
        bad_client.on_connect = lambda c, u, f, r, p: rejected.put(("CONNACK", r != 0))
        bad_client.on_disconnect = lambda c, u, f, r, p: rejected.put(("CLOSED", True))
        bad_client.connect("localhost", 8886); bad_client.loop_start()
        try:
            assert rejected.get(timeout=12)[1]
        finally:
            bad_client.disconnect(); bad_client.loop_stop()
        record("WRONG_MQTT_PASSWORD_REJECTED", "Kết nối với mật khẩu MQTT sai bị từ chối.")
        with sqlite3.connect(config["database"]) as db:
            assert db.execute("SELECT count(*) FROM outbox").fetchone()[0] == 1
        if tb:
            assert counts().get("FORWARDED", 0) == 1
            values = tb.call("GET", f"/api/plugins/telemetry/DEVICE/{tb_device}/values/timeseries?keys=auth_sequence,perclos")
            assert values["auth_sequence"][0]["value"] == "1"
            assert float(values["perclos"][0]["value"]) == .1
            # Check stored history too, rather than only the latest values.
            start_ms = sample["sent_at_ms"] - 60000
            end_ms = int(time.time() * 1000) + 1000
            history = tb.call("GET", f"/api/plugins/telemetry/DEVICE/{tb_device}/values/timeseries?"
                              f"keys=auth_sequence,perclos&startTs={start_ms}&endTs={end_ms}&agg=NONE&limit=100")
            assert len(history.get("auth_sequence", [])) == 1, repr(history)
            assert len(history.get("perclos", [])) == 1, repr(history)
            assert history["auth_sequence"][0]["value"] == "1"
            assert float(history["perclos"][0]["value"]) == .1
            record("ATTACKS_NOT_PERSISTED_TO_THINGSBOARD", "Outbox chỉ 1 hàng; ThingsBoard vẫn sequence=1, PERCLOS=0.1; FORWARDED=1.")
        report = {"passed": True, "checks": checks, "counters": counts(),
                  "test_device_name": test_name, "test_device_id": tb_device,
                  "gateway_port": 8886, "report_folder": str(folder)}
        (folder / "result.json").write_text(json.dumps(report, indent=2))
        (ROOT / ".private/security_latest.json").write_text(json.dumps(report, indent=2))
        print(json.dumps(report))
        if args.demo:
            print("\nKẾT QUẢ: 12 kiểm tra PASS; ACCEPTED=1, FORWARDED=1. Gói sửa/phát lại không tạo thêm hàng outbox.", flush=True)
            print("Xem device thử: sequence=1, blink_count=2, yawn_count=0, perclos=0.1, integrity_status=VERIFIED.", flush=True)
            print("Báo cáo riêng: " + str(folder / "result.json"), flush=True)
            print("Counter tấn công nằm trong báo cáo/log gateway thử; dashboard thiết bị thật không tăng vì bài thử được tách riêng.", flush=True)
            if sys.stdin.isatty() and not args.no_pause:
                input("Giữ màn hình này để xem/chụp kết quả trên ThingsBoard. Nhấn Enter để dọn device thử và kết thúc... ")
    finally:
        client.disconnect(); client.loop_stop()
        process.terminate(); process.wait(timeout=10); log.close()
        if tb_device:
            tb.call("DELETE", f"/api/device/{tb_device}")


if __name__ == "__main__":
    main()
