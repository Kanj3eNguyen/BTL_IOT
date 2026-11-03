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
    args = parser.parse_args()
    folder = ROOT / ".private" / ("mqtt_test_" + time.strftime("%Y%m%d_%H%M%S"))
    folder.mkdir()
    config = json.loads((ROOT / ".private/mqtt_guard/config.json").read_text())
    device = "security-self-test"
    key = secrets.token_bytes(32)
    password = secrets.token_urlsafe(24)
    config.update(bind="127.0.0.1:8886", ntp_port=0, database=str(folder / "state.db"),
                  devices={device: {"key_id": "test-key", "hmac_key_hex": key.hex(), "mqtt_password": password}})
    tb = None
    tb_device = None
    if args.thingsboard:
        tb = ThingsBoard()
        tb.login("tenant@thingsboard.org", load_account())
        tb_device = tb.call("POST", "/api/device", {"name": "MQTT security test " + secrets.token_hex(4), "type": "Test"})["id"]["id"]
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

    def base(kind, **fields):
        return {"v": 1, "type": kind, "device_id": device, "key_id": "test-key", "boot_id": boot,
                "sent_at_ms": int(time.time() * 1000), **fields}

    def counts():
        try:
            with sqlite3.connect(config["database"]) as db:
                return dict(db.execute("SELECT reason,count FROM counters"))
        except sqlite3.OperationalError:
            return {}

    def expect(reason, before, message, topic="telemetry"):
        client.publish(prefix + topic, message, qos=1).wait_for_publish(timeout=5)
        wait_for(lambda: counts().get(reason, 0) > before)
        checks.append(reason)

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
        client.connect("localhost", 8886)
        client.loop_start()
        assert connected.get(timeout=5) == 0
        time.sleep(.25)
        hello = base("hello", request_id=secrets.token_hex(16))
        client.publish(prefix + "hello", sign(hello, key), 1)
        grant, _ = authenticate(messages.get(timeout=5), config["devices"])
        assert grant["type"] == "session"
        checks.append("TLS_AND_AUTHENTICATED_SESSION")
        sid = grant["session_id"]
        data = {"boot_id": 17, "sequence": 1, "uptime_ms": 20000, "driver_present": True,
                "eyes_valid": True, "eyes_closed": False, "eye_closed_probability": .1,
                "mouth_valid": True, "mouth_open": False, "mouth_open_probability": .1,
                "perclos_valid": True, "perclos_window_ready": False, "perclos": .1,
                "blink_count": 2, "yawn_count": 0, "blink_window_count": 2, "yawn_window_count": 0}
        sample = base("sample", session_id=sid, sequence=1, sample_age_ms=20, data=data)
        wire = sign(sample, key)
        expect("ACCEPTED", 0, wire)
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
            checks.append("REAL_THINGSBOARD_PERSISTED")
        expect("DUPLICATE", 0, wire)
        envelope = json.loads(wire)
        changed = json.loads(base64.b64decode(envelope["payload"]))
        changed["data"]["perclos"] = .99
        envelope["payload"] = base64.b64encode(json.dumps(changed).encode()).decode()
        expect("BAD_MAC", 0, json.dumps(envelope).encode())
        expect("BAD_MAC", 1, sign(sample, b"x" * 32))
        sample["data"]["blink_count"] = 999
        expect("REPLAY_OR_SEQUENCE_CONFLICT", 0, sign(sample, key))
        stale = {**sample, "sent_at_ms": int(time.time() * 1000) - 60000}
        expect("STALE_OR_FUTURE", 0, sign(stale, key))
        # Renew session, then replay the earlier correctly signed sample.
        client.publish(prefix + "hello", sign(base("hello", request_id=secrets.token_hex(16)), key), 1)
        wait_for(lambda: counts().get("SESSION", 0) >= 2)
        expect("SESSION_INVALID", 0, wire)
        # Device cannot publish the server's downlink or bypass to TB's topic.
        before = counts().get("ACCEPTED", 0)
        client.publish(prefix + "down", wire, 1).wait_for_publish(5)
        client.publish("v1/devices/me/telemetry", b'{"integrity_status":"VERIFIED"}', 1).wait_for_publish(5)
        time.sleep(.3)
        assert counts().get("ACCEPTED", 0) == before
        checks.append("PUBLISH_ACL")
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
        checks.append("WRONG_MQTT_PASSWORD_REJECTED")
        with sqlite3.connect(config["database"]) as db:
            assert db.execute("SELECT count(*) FROM outbox").fetchone()[0] == 1
        if tb:
            assert counts().get("FORWARDED", 0) == 1
            values = tb.call("GET", f"/api/plugins/telemetry/DEVICE/{tb_device}/values/timeseries?keys=auth_sequence,perclos")
            assert values["auth_sequence"][0]["value"] == "1"
            assert float(values["perclos"][0]["value"]) == .1
            checks.append("ATTACKS_NOT_PERSISTED_TO_THINGSBOARD")
        report = {"passed": True, "checks": checks, "counters": counts()}
        (folder / "result.json").write_text(json.dumps(report, indent=2))
        print(json.dumps(report))
    finally:
        client.disconnect(); client.loop_stop()
        process.terminate(); process.wait(timeout=10); log.close()
        if tb_device:
            tb.call("DELETE", f"/api/device/{tb_device}")


if __name__ == "__main__":
    main()
