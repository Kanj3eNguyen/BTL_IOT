"""Export a saved live stream with ESP32-S3 predictions drawn beside each frame."""

import argparse
import json
from datetime import datetime
from pathlib import Path

import cv2

from driver_monitor_viewer import draw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, help='Folder produced by viewer --out')
    parser.add_argument('--out', type=Path, help='Output AVI; defaults to capture/review_stream.avi')
    args = parser.parse_args()
    rows = [json.loads(line) for line in (args.capture / 'frames.jsonl').read_text(
        encoding='utf-8').splitlines()]
    if len(rows) < 2:
        parser.error('Capture contains fewer than two frames')
    start = datetime.fromisoformat(rows[0]['utc'])
    end = datetime.fromisoformat(rows[-1]['utc'])
    duration = max((end - start).total_seconds(), .001)
    fps = min(30.0, max(1.0, (len(rows)-1) / duration))
    output = args.out or args.capture / 'review_stream.avi'
    writer = None
    written = 0
    try:
        for row in rows:
            frame = cv2.imread(str(args.capture / 'frames' / f'{row["frame"]:06d}.jpg'))
            if frame is not None:
                canvas = draw(frame, row['board_status'], fps)
                if writer is None:
                    height, width = canvas.shape[:2]
                    writer = cv2.VideoWriter(str(output), cv2.VideoWriter_fourcc(*'MJPG'),
                                             fps, (width, height))
                    if not writer.isOpened():
                        raise RuntimeError(f'Cannot create {output}')
                writer.write(canvas)
                written += 1
    finally:
        if writer is not None:
            writer.release()
    if not written:
        raise RuntimeError('Capture contains no readable JPEG frames')
    print(f'Saved {written} frames at {fps:.2f} FPS to {output}')


if __name__ == '__main__':
    main()
