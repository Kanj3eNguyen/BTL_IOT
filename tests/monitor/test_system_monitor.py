"""Monitor must distinguish stale data and keep credentials out of visible logs."""
import sys
import json
import struct
import tempfile
import threading
import unittest
from contextlib import nullcontext
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "examples/ov3660_face_detect"))
from driver_system_monitor import freshness, security_counts, gateway_log, Store, SerialReader
from driver_monitor_viewer import PREFIX, STATUS_KIND, JPEG_KIND, packet_checksum


class MonitorTests(unittest.TestCase):
    def test_jpeg_keeps_its_status_when_next_status_arrives_in_same_read(self):
        def wire(kind, payload):
            body = bytes([kind]) + struct.pack('<I', len(payload)) + payload
            return PREFIX + body + struct.pack('<I', packet_checksum(body))

        closed = wire(STATUS_KIND, json.dumps({'uptime_ms': 100, 'eyes_closed': True}).encode())
        opened = wire(STATUS_KIND, json.dumps({'uptime_ms': 200, 'eyes_closed': False}).encode())
        image_a = wire(JPEG_KIND, b'\xff\xd8A\xff\xd9')
        image_b = wire(JPEG_KIND, b'\xff\xd8B\xff\xd9')
        stop = threading.Event()
        chunks = [closed + image_a + opened, image_b]

        class Port:
            @property
            def in_waiting(self):
                return len(chunks[0]) if chunks else 1

            def read(self, size):
                if chunks:
                    return chunks.pop(0)
                stop.set()
                return b''

        store = Store()
        observed = []
        update = store.update

        def record(**values):
            update(**values)
            if 'frame' in values:
                snapshot = store.snapshot()
                observed.append((int(snapshot['frame'][0, 0, 0]), snapshot['state']['eyes_closed']))

        store.update = record
        args = SimpleNamespace(port='TEST', baud=1500000, out=None)
        with patch('driver_system_monitor.open_stream_port', return_value=nullcontext(Port())), \
             patch('driver_system_monitor.cv2.imdecode', side_effect=lambda raw, flag: np.full((1, 1, 3), raw[2], dtype=np.uint8)):
            SerialReader(args, store, stop).run()
        self.assertEqual(observed, [(ord('A'), True), (ord('B'), False)])

    def test_missing_stale_and_future_health_are_not_live(self):
        now = 1800000000000
        self.assertEqual(freshness(None, now), "missing")
        self.assertEqual(freshness(True, now), "missing")
        self.assertEqual(freshness(now - 5999, now), "fresh")
        self.assertEqual(freshness(now - 6001, now), "stale")
        self.assertEqual(freshness(now + 5001, now), "future")

    def test_security_groups_do_not_double_count_subcategories(self):
        counts = {"SESSION": 2, "ACCEPTED": 1, "FORWARDED": 1, "DUPLICATE": 15,
                  "BAD_MAC": 2, "REPLAY_OR_SEQUENCE_CONFLICT": 1,
                  "STALE_OR_FUTURE": 1, "SESSION_INVALID": 1}
        self.assertEqual(security_counts(counts), {"rejected": 5, "bad_mac": 2, "replay": 2, "duplicate": 15})

    def test_invalid_counter_values_are_not_displayed_as_events(self):
        self.assertEqual(security_counts({"BAD_MAC": -1, "x": True, "y": "secret"}),
                         {"rejected": 0, "bad_mac": 0, "replay": 0, "duplicate": 0})

    def test_only_allowlisted_log_metadata_reaches_the_monitor(self):
        scratch = ROOT / ".private/tmp"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as folder:
            log = Path(folder) / "audit.log"
            log.write_text(
                '2026-11-05 00:00:00 INFO device=board-1 decision=BAD_MAC payload=TOP_SECRET\n'
                '2026-11-05 00:00:01 INFO TLS MQTT guard listening on 0.0.0.0:8885 password=TOP_SECRET\n'
                '2026-11-05 00:00:02 INFO Local bootstrap NTP listening on UDP 123 token=TOP_SECRET\n'
                '2026-11-05 00:00:03 ERROR password=TOP_SECRET\n', encoding="utf-8")
            shown = gateway_log(log)
            self.assertNotIn("TOP_SECRET", shown)
            self.assertNotIn("password", shown)
            self.assertIn("BAD_MAC", shown)
            self.assertIn("8885", shown)

    def test_event_log_is_bounded_and_snapshots_are_independent(self):
        store = Store()
        for i in range(200):
            store.event(str(i))
        first = store.snapshot()
        self.assertEqual(len(first["events"]), 160)
        store.event("next")
        self.assertNotEqual(first["events"], store.snapshot()["events"])


if __name__ == "__main__":
    unittest.main()
