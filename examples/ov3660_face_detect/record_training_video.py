"""Record a local OV3660 training session from COM4 with a live preview.

Press R once to start or pause recording, and Q to save and quit. Board status
is saved for diagnostics only; it is not treated as a training label.
"""

import argparse
import json
import time
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np
from driver_monitor_viewer import JPEG_KIND, STATUS_KIND, open_stream_port, packets


def preview(frame, recording, elapsed, count, status):
    canvas = cv2.resize(frame, (720, 720), interpolation=cv2.INTER_LINEAR)
    cv2.rectangle(canvas, (0, 0), (720, 95), (0, 0, 0), -1)
    title = f"REC {elapsed:.0f}s  {count} frames" if recording else "READY / PAUSED"
    color = (0, 0, 255) if recording else (0, 220, 220)
    cv2.putText(canvas, title, (16, 34), cv2.FONT_HERSHEY_SIMPLEX, 0.9, color, 2, cv2.LINE_AA)
    face = "face: detected" if status.get("driver_present") else "face: not detected"
    cv2.putText(canvas, f"R: record/pause   Q: save/quit   {face}",
                (16, 75), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                (255, 255, 255), 1, cv2.LINE_AA)
    return canvas


def export_video(folder, timestamps):
    if len(timestamps) < 2:
        return None
    duration = max(timestamps[-1] - timestamps[0], 0.001)
    fps = max(1.0, min(30.0, (len(timestamps) - 1) / duration))
    path = folder / "training_video.avi"
    writer = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*"MJPG"),
                             fps, (240, 240))
    if not writer.isOpened():
        raise RuntimeError("OpenCV could not create MJPEG AVI")
    try:
        for index, recorded_s in enumerate(timestamps):
            frame = cv2.imread(str(folder / "frames" / f"{index:06d}.jpg"))
            if frame is None:
                frame = np.zeros((240, 240, 3), dtype=np.uint8)
            # AVI playback time is approximate if camera FPS changes. Burn in
            # the exact recorded_s used by labels.json and the trainer.
            cv2.rectangle(frame, (0, 0), (104, 24), (0, 0, 0), -1)
            cv2.putText(frame, f"{recorded_s:.1f}s", (5, 18),
                        cv2.FONT_HERSHEY_SIMPLEX, .5, (255, 255, 255), 1,
                        cv2.LINE_AA)
            writer.write(frame)
    finally:
        writer.release()
    return {"path": str(path), "fps": fps, "duration_s": duration}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--max-seconds", type=float, default=180)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    folder = args.out or Path(__file__).with_name("recordings") / datetime.now().strftime("%Y%m%d_%H%M%S")
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "frames").mkdir(exist_ok=True)
    buffer = bytearray()
    state = {}
    recording = False
    recorded_s = 0.0
    segment_start = 0.0
    timestamps = []
    window = "ESP32-S3 training recorder"
    cv2.namedWindow(window, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window, 720, 720)
    print(f"Preview on {args.port}. Press R to record/pause, Q to save: {folder}", flush=True)

    with open_stream_port(args.port) as port, \
            (folder / "frames.jsonl").open("w", encoding="utf-8") as metadata:
        try:
            while True:
                buffer.extend(port.read(port.in_waiting or 1))
                for kind, payload in packets(buffer):
                    if kind == STATUS_KIND:
                        try:
                            state = json.loads(payload)
                        except (UnicodeDecodeError, json.JSONDecodeError):
                            continue
                        continue
                    if kind != JPEG_KIND:
                        continue
                    frame = cv2.imdecode(np.frombuffer(payload, np.uint8), cv2.IMREAD_COLOR)
                    if frame is None:
                        continue
                    now = time.monotonic()
                    elapsed = recorded_s + (now - segment_start if recording else 0.0)
                    if recording:
                        index = len(timestamps)
                        (folder / "frames" / f"{index:06d}.jpg").write_bytes(payload)
                        metadata.write(json.dumps({
                            "frame": index,
                            "utc": datetime.now(timezone.utc).isoformat(),
                            "recorded_s": elapsed,
                            "board_status": state,
                        }, separators=(",", ":")) + "\n")
                        metadata.flush()
                        timestamps.append(elapsed)
                    cv2.imshow(window, preview(frame, recording, elapsed, len(timestamps), state))
                    key = cv2.waitKey(1) & 0xFF
                    if key in (ord("q"), 27):
                        return folder, timestamps, elapsed
                    if key == ord("r"):
                        if recording:
                            recorded_s = elapsed
                            recording = False
                            print(f"Paused at {recorded_s:.1f}s", flush=True)
                        else:
                            segment_start = time.monotonic()
                            recording = True
                            print("Recording", flush=True)
                    if recording and elapsed >= args.max_seconds:
                        return folder, timestamps, elapsed
                if cv2.getWindowProperty(window, cv2.WND_PROP_VISIBLE) < 1:
                    return folder, timestamps, recorded_s
        finally:
            cv2.destroyAllWindows()


if __name__ == "__main__":
    folder, timestamps, recorded_s = main()
    video = export_video(folder, timestamps)
    summary = {"frames": len(timestamps), "recorded_s": recorded_s,
               "video": video, "labels_verified": False}
    (folder / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Saved {len(timestamps)} frames; video: {video}", flush=True)
