"""Capture board-computed status and matching JPEGs for camera calibration."""

import argparse
import json
import time
from pathlib import Path

from driver_monitor_viewer import JPEG_KIND, STATUS_KIND, open_stream_port, packets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--seconds", type=float, default=8)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    buffer = bytearray()
    last_status = None
    samples = []
    stats = {"bad_packets": 0}
    deadline = time.monotonic() + args.seconds
    with open_stream_port(args.port) as port:
        while time.monotonic() < deadline:
            buffer.extend(port.read(port.in_waiting or 1))
            for kind, payload in packets(buffer, stats):
                if kind == STATUS_KIND:
                    try:
                        last_status = json.loads(payload)
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        last_status = None
                elif kind == JPEG_KIND and last_status is not None:
                    index = len(samples)
                    (args.out / f"frame_{index:03d}.jpg").write_bytes(payload)
                    samples.append(last_status)
                    last_status = None
    (args.out / "status.json").write_text(json.dumps(samples, indent=2), encoding="utf-8")
    print(f"Captured {len(samples)} frames in {args.out}; bad UART packets: {stats['bad_packets']}")


if __name__ == "__main__":
    main()
