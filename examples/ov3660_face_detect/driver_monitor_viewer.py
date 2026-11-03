"""Display OV3660 video and the eye/mouth observations computed on ESP32-S3."""

import argparse
import json
import struct
import time
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np
import serial

PREFIX = b"\xaa\x55\xaa"
JPEG_KIND = 0x55
STATUS_KIND = 0x56
STREAM_BAUD = 1_500_000


def open_stream_port(name, baud=STREAM_BAUD):
    # CH343 boards may use DTR/RTS for GPIO0/EN. Keep both inactive so a
    # physical RST boots the application rather than the ROM downloader.
    port = serial.Serial(port=None, baudrate=baud, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = name
    port.open()
    # Windows USB-UART defaults can overflow during short GUI/scheduler stalls.
    # Only keep the latest preview after parsing; a larger RX ring is not a video queue.
    try:
        port.set_buffer_size(rx_size=65536, tx_size=4096)
    except (AttributeError, serial.SerialException):
        pass
    port.reset_input_buffer()
    return port


def packet_checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def packets(buffer, stats=None):
    while True:
        marker = buffer.find(PREFIX)
        if marker < 0:
            if len(buffer) > 2:
                del buffer[:-2]
            return
        if marker:
            del buffer[:marker]
        if len(buffer) < 8:
            return
        kind = buffer[3]
        length = struct.unpack_from("<I", buffer, 4)[0]
        limit = 100_000 if kind == JPEG_KIND else 2048
        if kind not in (JPEG_KIND, STATUS_KIND) or length == 0 or length > limit:
            del buffer[0]
            continue
        packet_end = 8 + length + 4
        if len(buffer) < packet_end:
            return
        payload = bytes(buffer[8 : 8 + length])
        expected = struct.unpack_from("<I", buffer, 8 + length)[0]
        if packet_checksum(buffer[3 : 8 + length]) != expected:
            # A dropped UART byte can make the declared JPEG length consume
            # the next packet. Search again without discarding that header.
            if stats is not None:
                stats["bad_packets"] += 1
            del buffer[0]
            continue
        del buffer[:packet_end]
        if kind == JPEG_KIND and (
            not payload.startswith(b"\xff\xd8") or not payload.endswith(b"\xff\xd9")
        ):
            if stats is not None:
                stats["bad_packets"] += 1
            continue
        yield kind, payload


def label(value, valid, yes, no):
    return yes if valid and value else no if valid else "UNKNOWN"


def alarm_label(reasons):
    names = [name for bit, name in ((1, "EYES"), (4, "YAWNS"), (8, "PERCLOS"), (16, "NO FACE"))
             if reasons & bit]
    return "+".join(names) if names else "OFF"


def buzzer_label(pattern):
    return {0: "OFF", 1: "EYES", 2: "YAWNS x2", 3: "PERCLOS x3", 4: "NO FACE slow"}.get(pattern, "?")


def draw(frame, state, fps, bad_packets=0):
    shown = cv2.resize(frame, (480, 480), interpolation=cv2.INTER_LINEAR)
    canvas = np.zeros((506, 800, 3), dtype=np.uint8)
    canvas[:480, :480] = shown
    lines = [
        "ESP32-S3 driver monitor",
        f"Video: {fps:.1f} FPS  AI: {state.get('analysis_hz', 0):.1f} FPS",
        f"Face: {'HELD 1.2s' if state.get('face_holdover') else label(state.get('driver_present'), True, 'PRESENT', 'ABSENT')}  Light: {state.get('face_luma') if state.get('face_luma_valid') else '--'}/255",
        f"Eyes: {label(state.get('eyes_closed'), state.get('eyes_valid'), 'CLOSED', 'OPEN')}",
        (f"Eye closed P: {state.get('eye_closed_probability', 0):.2f}"
         if state.get('eyes_valid') else "Eye closed P: N/A"),
        f"Light: { {0:'N/A',1:'ADJUST',2:'OK',3:'DARK',4:'BRIGHT'}.get(state.get('lighting_state', 0), '?')}  AE:{state.get('ae_level', -2)}  Scene:{state.get('scene_luma', 0)}",
        f"Age: {state.get('frame_age_ms', 0):.0f}ms  Encode: {state.get('encode_ms', 0):.0f}ms",
        f"Closed: {state.get('eye_closed_duration', 0):.1f} s  Alarm: {alarm_label(state.get('alert_reasons', 0))}",
        f"Buzzer: {buzzer_label(state.get('buzzer_pattern', 0))}",
        (f"PERCLOS {state.get('perclos', 0) * 100:.0f}% | "
         f"{state.get('perclos_observed_s', 0):.0f}s eyes | "
         f"{'READY' if state.get('perclos_window_ready') else 'COLLECT'}")
        if state.get("perclos_valid") else "PERCLOS: collecting",
        f"Blinks (<1.2 s)*: {state.get('blink_count', 0)}  Window: {state.get('blink_window_count', 0)}",
        f"Mouth: {label(state.get('mouth_open'), state.get('mouth_valid'), 'OPEN', 'CLOSED')}",
        f"Mouth open P: {state.get('mouth_open_probability', 0):.2f}",
        f"Mouth contrast: {state.get('mouth_contrast', 0):.3f}",
        f"Yawns (>=0.8 s)*: {state.get('yawn_count', 0)}  Window: {state.get('yawn_window_count', 0)}",
        (f"WiFi: CONNECTED  Time: {'OK' if state.get('time_synced') else 'WAIT'}"
         if state.get('wifi_connected') else f"WiFi: OFFLINE  reason: {state.get('wifi_disconnect_reason', 0)}"),
        (f"Guard: {'READY' if state.get('guard_ready') else 'WAIT'}  TB seq:{state.get('guard_forwarded_sequence', 0)}"
         if state.get('guard_enabled') else f"MQTTS: {'CONNECTED' if state.get('mqtt_connected') else 'OFFLINE'}  ACK: {state.get('mqtt_acked', 0)}"),
        f"Seq:{state.get('mqtt_sequence', 0)} Skip:{state.get('stream_dropped', 0)} UARTbad:{bad_packets}",
        "Q to quit",
    ]
    for index, line in enumerate(lines):
        y = 20 + index * 26
        color = (255, 255, 255)
        if line.startswith(("Eyes: CLOSED", "Mouth: OPEN")) or ("Alarm:" in line and "Alarm: OFF" not in line) or (line.startswith("Buzzer:") and "OFF" not in line):
            color = (0, 180, 255)
        if line.startswith("Face: ABSENT") or "UNKNOWN" in line:
            color = (100, 200, 255)
        cv2.putText(canvas, line, (492, y), cv2.FONT_HERSHEY_SIMPLEX, 0.52, color, 1, cv2.LINE_AA)
    return canvas


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--baud", type=int, default=STREAM_BAUD)
    parser.add_argument("--out", type=Path, help="Save JPEG frames and board status while displaying")
    parser.add_argument("--seconds", type=float, default=0,
                        help="Stop after this many seconds; 0 keeps streaming until Q")
    args = parser.parse_args()
    if args.out:
        if (args.out / "frames.jsonl").exists():
            parser.error(f"Existing capture: {args.out}")
        (args.out / "frames").mkdir(parents=True, exist_ok=True)
    buffer = bytearray()
    state = {}
    stats = {"bad_packets": 0}
    frame_count = 0
    total_count = 0
    start = time.monotonic()
    session_start = start
    fps = 0.0

    metadata = (args.out / "frames.jsonl").open("w", encoding="utf-8") if args.out else None
    with open_stream_port(args.port, args.baud) as port:
        print(f"Receiving video and ESP32-S3 status on {args.port}. Press Q to exit.")
        if args.out:
            print(f"Saving frames and board status to {args.out}")
        try:
            last_frame_at = 0.0
            waiting_frame = np.zeros((240, 240, 3), dtype=np.uint8)
            while True:
                buffer.extend(port.read(port.in_waiting or 1))
                got_frame = False
                latest = None
                for kind, payload in packets(buffer, stats):
                    if kind == STATUS_KIND:
                        try:
                            state = json.loads(payload)
                        except (UnicodeDecodeError, json.JSONDecodeError):
                            continue
                    else:
                        got_frame = True
                        last_frame_at = time.monotonic()
                        if args.out:
                            (args.out / "frames" / f"{total_count:06d}.jpg").write_bytes(payload)
                            metadata.write(json.dumps({
                                "frame": total_count,
                                "utc": datetime.now(timezone.utc).isoformat(),
                                "board_status": state,
                            }, separators=(",", ":")) + "\n")
                            metadata.flush()
                        frame_count += 1
                        total_count += 1
                        # Decode/render only the newest frame in a read batch.
                        # All packets are still checked and recordings keep every frame.
                        latest = (payload, state.copy())
                if latest is not None:
                    payload, frame_state = latest
                    frame = cv2.imdecode(np.frombuffer(payload, np.uint8), cv2.IMREAD_COLOR)
                    if frame is not None:
                        elapsed = time.monotonic() - start
                        if elapsed >= 2:
                            fps = frame_count / elapsed
                            frame_count = 0
                            start = time.monotonic()
                        cv2.imshow("ESP32-S3 driver monitoring", draw(frame, frame_state, fps, stats["bad_packets"]))
                if not got_frame and time.monotonic() - last_frame_at > 1.0:
                    waiting = draw(waiting_frame, {}, 0.0, stats["bad_packets"])
                    cv2.putText(waiting, "Waiting for board stream / restart",
                                (12, 450), cv2.FONT_HERSHEY_SIMPLEX, 0.47,
                                (0, 210, 255), 1, cv2.LINE_AA)
                    cv2.imshow("ESP32-S3 driver monitoring", waiting)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    return
                if args.seconds and time.monotonic() - session_start >= args.seconds:
                    return
        finally:
            cv2.destroyAllWindows()
            if metadata:
                metadata.close()
                print(f"Saved {total_count} frames to {args.out}")


if __name__ == "__main__":
    main()
