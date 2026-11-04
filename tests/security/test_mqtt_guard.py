"""Security properties: tamper/replay, reset/restart, QoS dedupe and freshness."""
import base64
import copy
import json
import tempfile
import unittest
from pathlib import Path

from server.mqtt_guard.protocol import DOMAIN, Rejected, authenticate, parse_json, sign, wrap
from server.mqtt_guard.verifier import SESSION_MS, Verifier

KEY = bytes(range(32))
DEVICES = {"test-board": {"key_id": "key-1", "hmac_key_hex": KEY.hex()}}


class GuardTests(unittest.TestCase):
    def setUp(self):
        scratch = Path(__file__).resolve().parents[2] / ".private/tmp"
        scratch.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=scratch)
        self.path = str(Path(self.tmp.name) / "state.db")
        self.now = 1800000000000
        self.verifier = Verifier(self.path, DEVICES, lambda: self.now)
        self.boot = "01" * 16
        self.request = "02" * 16
        self.hello = self.base("hello", request_id=self.request)
        self.sid = self.issue(self.hello)

    def tearDown(self):
        self.verifier.close()
        self.tmp.cleanup()

    def base(self, kind, **fields):
        return {"v": 1, "type": kind, "device_id": "test-board", "key_id": "key-1", "boot_id": self.boot,
                "sent_at_ms": self.now, **fields}

    def issue(self, hello):
        result = self.verifier.handle(sign(hello, KEY), "test-board", "hello")
        self.assertEqual(result["status"], "SESSION")
        body, _ = authenticate(result["response"], DEVICES)
        return body["session_id"]

    def sample(self, sequence=1, **fields):
        data = {"boot_id": 123, "uptime_ms": 10000, "sequence": sequence, "driver_present": True,
                "eyes_valid": True, "eyes_closed": False, "eye_closed_probability": .2,
                "mouth_valid": True, "mouth_open": False, "mouth_open_probability": .1,
                "perclos_valid": True, "perclos_window_ready": False, "perclos": .12,
                "perclos_observed_s": 20, "blink_count": 3, "yawn_count": 0,
                "blink_window_count": 3, "yawn_window_count": 0, "alert_reasons": "Bình thường"}
        return self.base("sample", session_id=self.sid, sequence=sequence, sample_age_ms=100, data=data, **fields)

    def send(self, message):
        return self.verifier.handle(sign(message, KEY), "test-board", "sample")

    def test_valid_data_goes_to_transactional_outbox(self):
        self.assertEqual(self.send(self.sample())["status"], "ACCEPTED")
        row = self.verifier.next_pending()
        values = json.loads(row["telemetry"])
        self.assertEqual(values["integrity_status"], "VERIFIED")
        self.assertEqual(values["auth_sequence"], 1)
        self.assertEqual(row["measured_at"], self.now - 100)

    def test_tamper_payload_without_key_is_rejected(self):
        envelope = json.loads(sign(self.sample(), KEY))
        body = json.loads(base64.b64decode(envelope["payload"]))
        body["data"]["perclos"] = .99
        envelope["payload"] = base64.b64encode(json.dumps(body).encode()).decode()
        result = self.verifier.handle(json.dumps(envelope).encode(), "test-board", "sample")
        self.assertEqual(result["status"], "BAD_MAC")
        self.assertIsNone(self.verifier.next_pending())

    def test_wrong_key_and_non_ascii_mac_fail_closed(self):
        result = self.verifier.handle(sign(self.sample(), b"x" * 32), "test-board", "sample")
        self.assertEqual(result["status"], "BAD_MAC")
        envelope = json.loads(sign(self.sample(), KEY)); envelope["mac"] = "á" * 64
        result = self.verifier.handle(json.dumps(envelope).encode(), "test-board", "sample")
        self.assertEqual(result["status"], "BAD_MAC")

    def test_qos_duplicate_does_not_forward_twice(self):
        body = self.sample()
        self.assertEqual(self.send(body)["status"], "ACCEPTED")
        self.assertEqual(self.send(body)["status"], "DUPLICATE")
        self.assertEqual(self.verifier.db.execute("SELECT count(*) FROM outbox").fetchone()[0], 1)

    def test_duplicate_after_forward_returns_signed_ack(self):
        body = self.sample(); result = self.send(body)
        self.verifier.mark_forwarded(result["outbox_id"])
        retry = self.send(body)
        self.assertEqual(retry["status"], "DUPLICATE")
        ack, _ = authenticate(retry["response"], DEVICES)
        self.assertEqual(ack["status"], "FORWARDED")

    def test_sequence_conflict_and_out_of_order_are_rejected(self):
        self.send(self.sample(5))
        self.assertEqual(self.send(self.sample(4))["status"], "REPLAY_OR_SEQUENCE_CONFLICT")
        body = self.sample(5); body["data"]["blink_count"] += 1
        self.assertEqual(self.send(body)["status"], "REPLAY_OR_SEQUENCE_CONFLICT")

    def test_sequence_gap_is_accepted(self):
        self.send(self.sample(1))
        self.assertEqual(self.send(self.sample(20))["status"], "ACCEPTED")

    def test_restart_keeps_replay_state(self):
        body = self.sample(); self.send(body)
        self.verifier.close()
        self.verifier = Verifier(self.path, DEVICES, lambda: self.now)
        self.assertEqual(self.send(body)["status"], "DUPLICATE")

    def test_reset_board_creates_new_session_and_rejects_old(self):
        old = self.sample(); self.send(old)
        self.boot = "03" * 16
        self.sid = self.issue(self.base("hello", request_id="04" * 16))
        self.assertEqual(self.send(old)["status"], "SESSION_INVALID")
        self.assertEqual(self.send(self.sample(1))["status"], "ACCEPTED")

    def test_hello_retry_does_not_reset_sequence(self):
        self.send(self.sample(10))
        self.assertEqual(self.issue(self.hello), self.sid)
        self.assertEqual(self.send(self.sample(1))["status"], "REPLAY_OR_SEQUENCE_CONFLICT")

    def test_replayed_older_hello_cannot_restore_retired_session(self):
        self.sid = self.issue(self.base("hello", request_id="05" * 16))
        result = self.verifier.handle(sign(self.hello, KEY), "test-board", "hello")
        self.assertEqual(result["status"], "OLD_HELLO")
        self.assertEqual(self.send(self.sample())["status"], "ACCEPTED")

    def test_expired_session_requires_new_challenge(self):
        self.now += SESSION_MS + 1
        self.assertEqual(self.send(self.sample())["status"], "SESSION_EXPIRED")

    def test_stale_and_future_envelopes(self):
        for delta in (-30001, 5001):
            body = self.sample(); body["sent_at_ms"] += delta
            self.assertEqual(self.send(body)["status"], "STALE_OR_FUTURE")

    def test_fresh_envelope_cannot_hide_old_measurement(self):
        body = self.sample(); body["sample_age_ms"] = 15001
        self.assertEqual(self.send(body)["status"], "SCHEMA_SAMPLE_AGE_MS")

    def test_invalid_perclos_and_valid_flags(self):
        for value in (-.1, 1.1, "0.2", True):
            body = self.sample(); body["data"]["perclos"] = value
            self.assertEqual(self.send(body)["status"], "DATA_RANGE")
        body = self.sample(); body["data"]["eyes_valid"] = False
        self.assertEqual(self.send(body)["status"], "DATA_VALIDITY")

    def test_missing_observation_remains_null(self):
        body = self.sample()
        for state, probability, valid in (("eyes_closed", "eye_closed_probability", "eyes_valid"),
                                          ("mouth_open", "mouth_open_probability", "mouth_valid")):
            body["data"][valid] = False
            body["data"][state] = body["data"][probability] = None
        self.assertEqual(self.send(body)["status"], "ACCEPTED")

    def test_sender_cannot_set_verified_status(self):
        body = self.sample(); body["data"]["integrity_status"] = "VERIFIED"
        self.assertEqual(self.send(body)["status"], "RESERVED_FIELD")

    def test_cross_device_and_message_type_reflection(self):
        self.assertEqual(self.verifier.handle(sign(self.sample(), KEY), "another", "sample")["status"], "CONTEXT_MISMATCH")
        self.assertEqual(self.verifier.handle(sign(self.hello, KEY), "test-board", "sample")["status"], "CONTEXT_MISMATCH")

    def test_duplicate_json_keys_and_bad_syntax(self):
        for wire in (b'null', b'[]', b'{', b'{"v":1,"v":1}', b'{"x":NaN}'):
            with self.subTest(wire=wire):
                result = self.verifier.handle(wire, "test-board", "sample")
                self.assertNotEqual(result["status"], "ACCEPTED")
        raw = json.dumps(self.sample(), separators=(",", ":"))[:-1] + ',"sequence":2}'
        self.assertEqual(self.verifier.handle(wrap(raw.encode(), KEY), "test-board", "sample")["status"], "DUPLICATE_JSON_KEY")

    def test_bool_sequence_is_not_integer(self):
        body = self.sample(); body["sequence"] = True
        self.assertEqual(self.send(body)["status"], "SCHEMA_SEQUENCE")

    def test_pending_data_expires_during_outage(self):
        self.send(self.sample())
        self.now += 30001
        self.assertIsNone(self.verifier.next_pending())

    def test_protocol_vector(self):
        # UTF-8 signing/authentication round trip; the real-board test separately
        # verifies firmware/server interoperability.
        wire = sign({"v": 1, "type": "vector", "device_id": "test-board", "key_id": "key-1", "text": "Bình thường"}, KEY)
        body, _ = authenticate(wire, DEVICES)
        self.assertEqual(body["text"], "Bình thường")

    def test_demo_identity_is_quarantined_and_duplicate_is_still_detected(self):
        self.verifier.devices = copy.deepcopy(DEVICES)
        self.verifier.devices["test-board"]["demo_only"] = True
        body = self.sample()
        self.assertEqual(self.send(body)["status"], "ACCEPTED")
        self.assertIsNone(self.verifier.next_pending())
        row = self.verifier.db.execute("SELECT id,status FROM outbox").fetchone()
        self.assertEqual(row["status"], "demo")
        self.assertIsNone(self.verifier.mark_forwarded(row["id"]))
        self.assertEqual(self.send(body)["status"], "DUPLICATE")

    def test_sender_cannot_select_demo_quarantine_policy(self):
        body = self.sample()
        body["data"]["demo_only"] = True
        self.assertEqual(self.send(body)["status"], "ACCEPTED")
        self.assertIsNotNone(self.verifier.next_pending())


if __name__ == "__main__":
    unittest.main()
