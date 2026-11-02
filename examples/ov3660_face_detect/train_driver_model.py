"""Train on-board eye/mouth models from one locally reviewed camera recording.

The board's predictions are never used as labels. Training runs on the PC;
only the generated C++ weight arrays run on the ESP32-S3 after build/flash.
"""

import argparse
import bisect
import json
from collections import defaultdict
from functools import lru_cache
from pathlib import Path

import cv2
import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import balanced_accuracy_score, confusion_matrix
from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler


ROOT = Path(__file__).resolve().parent
CURRENT_WEIGHTS = ROOT / "main" / "driver_ml_weights.hpp"
CANDIDATE_WEIGHTS = ROOT / "main" / "driver_ml_weights.candidate.hpp"


def sample(gray, cx, cy, half_w, half_h, n):
    height, width = gray.shape
    values = []
    for iy in range(n):
        for ix in range(n):
            x = min(max(round(cx + half_w * (2 * ix / (n - 1) - 1)), 0), width - 1)
            y = min(max(round(cy + half_h * (2 * iy / (n - 1) - 1)), 0), height - 1)
            values.append(gray[y, x])
    return np.asarray(values, dtype=np.float32)


def eye_features(gray, status):
    box, points = status.get("face_box", []), status.get("face_keypoints", [])
    if len(box) != 4 or len(points) != 10:
        return []
    face_w, face_h = box[2] - box[0], box[3] - box[1]
    if face_w < 68 or face_h < 75:
        return []
    eye_gap = points[6] - points[0]
    if eye_gap <= 0:
        return []
    pose = abs(points[4] - (points[0] + points[6]) / 2) / eye_gap
    # Both eyes must be visible for the firmware to use the result.
    if pose > .18:
        return []
    result = []
    for x, y in ((points[0], points[1]), (points[6], points[7])):
        skin = sample(gray, x, y + face_h * .10, face_w * .05, face_h * .02, 3).mean()
        if skin < 25:
            continue
        patch = sample(gray, x, y, face_w * .105, face_h * .070, 12)
        result.append(np.clip(patch / skin, 0, 2))
    return result


def mouth_features(gray, status):
    box, points = status.get("face_box", []), status.get("face_keypoints", [])
    if len(box) != 4 or len(points) != 10:
        return []
    face_w, face_h = box[2] - box[0], box[3] - box[1]
    if face_w < 68 or face_h < 75:
        return []
    x, y = (points[2] + points[8]) / 2, (points[3] + points[9]) / 2
    skin = (sample(gray, x - face_w * .33, y, face_w * .03, face_h * .03, 3).mean() +
            sample(gray, x + face_w * .33, y, face_w * .03, face_h * .03, 3).mean()) / 2
    if skin < 25:
        return []
    patch = sample(gray, x, y, face_w * .18, face_h * .15, 12)
    return [np.clip(patch / skin, 0, 2)]


def label_intervals(labels, task, state):
    intervals = []
    for value in labels.get(f"{task}_{state}_seconds", []):
        if not isinstance(value, int) or value < 0:
            raise ValueError(f"{task}_{state}_seconds must contain nonnegative integers")
        intervals.append((float(value), float(value + 1), True))
    for pair in labels.get(f"{task}_{state}_ranges", []):
        if not isinstance(pair, list) or len(pair) != 2:
            raise ValueError(f"{task}_{state}_ranges must contain [start, end] pairs")
        start, end = pair
        if not all(isinstance(v, (int, float)) for v in pair) or start < 0 or end <= start:
            raise ValueError(f"Invalid {task}_{state} range: {pair}")
        intervals.append((float(start), float(end), False))
    return sorted(intervals)


def labeled_targets(labels, task):
    closed = label_intervals(labels, task, "closed" if task == "eye" else "open")
    opened = label_intervals(labels, task, "open" if task == "eye" else "closed")
    if not closed or not opened:
        raise ValueError(f"Label both classes for {task} in labels.json")
    for a_start, a_end, _ in closed:
        for b_start, b_end, _ in opened:
            if a_start < b_end and b_start < a_end:
                raise ValueError(f"Overlapping {task} labels at {max(a_start, b_start):.1f}s")
    targets = []
    for label, intervals in ((1, closed), (0, opened)):
        for start, end, one_second in intervals:
            if one_second:
                targets.append((start + .2, label))
            else:
                # Keep away from transitions; use about three frames per second.
                time = start + .35
                while time < end - .35:
                    targets.append((time, label))
                    time += .35
    return sorted(targets)


def c_float(value):
    number = f"{float(value):.9g}"
    if "." not in number and "e" not in number:
        number += ".0"
    return number + "f"


def fmt_array(name, values):
    flat = np.asarray(values).ravel()
    lines = [f"inline constexpr float {name}[{len(flat)}] = {{"]
    for start in range(0, len(flat), 8):
        lines.append("    " + ", ".join(c_float(x) for x in flat[start:start + 8]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recording", type=Path, required=True,
                        help="Folder made by record_training_video.py with labels.json")
    parser.add_argument("--output", type=Path, default=CANDIDATE_WEIGHTS)
    parser.add_argument("--overwrite-current", action="store_true",
                        help="Allow direct replacement of the model compiled by firmware")
    args = parser.parse_args()
    recording = args.recording.resolve()
    if not (recording / "frames.jsonl").is_file() or not (recording / "labels.json").is_file():
        parser.error("Recording needs frames.jsonl and labels.json")
    if args.output.resolve() == CURRENT_WEIGHTS.resolve() and not args.overwrite_current:
        parser.error("Use --overwrite-current to replace driver_ml_weights.hpp directly")

    labels = json.loads((recording / "labels.json").read_text(encoding="utf-8"))
    split = labels.get("validation_from_second")
    if not isinstance(split, (int, float)) or split <= 2:
        parser.error("labels.json needs validation_from_second greater than 2")
    rows = [json.loads(line) for line in (recording / "frames.jsonl").read_text(
        encoding="utf-8").splitlines() if line.strip()]
    if not rows or any("recorded_s" not in row for row in rows):
        parser.error("No recorded_s found: record with record_training_video.py")
    rows.sort(key=lambda row: row["recorded_s"])
    times = [row["recorded_s"] for row in rows]

    @lru_cache(maxsize=128)
    def gray_frame(frame_index):
        frame = cv2.imread(str(recording / "frames" / f"{frame_index:06d}.jpg"))
        return cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY) if frame is not None else None

    trained = {}
    for task, extract in (("eye", eye_features), ("mouth", mouth_features)):
        samples = {"train": [], "validation": []}
        seen_frames = set()
        for target_time, label in labeled_targets(labels, task):
            if target_time < split - .5:
                split_name = "train"
            elif target_time > split + .5:
                split_name = "validation"
            else:
                continue
            index = bisect.bisect_left(times, target_time)
            choices = [i for i in (index - 1, index) if 0 <= i < len(rows)]
            if not choices:
                continue
            row_index = min(choices, key=lambda i: abs(times[i] - target_time))
            if abs(times[row_index] - target_time) > .25 or row_index in seen_frames:
                continue
            row = rows[row_index]
            status = row.get("board_status", {})
            if not status.get("driver_present") or status.get("face_holdover"):
                continue
            gray = gray_frame(row["frame"])
            if gray is None:
                continue
            features = extract(gray, status)
            if features:
                seen_frames.add(row_index)
                for feature in features:
                    samples[split_name].append((feature, label, row_index, times[row_index]))

        train, validation = samples["train"], samples["validation"]
        train_counts = np.bincount([s[1] for s in train], minlength=2)
        val_frames = defaultdict(list)
        for feature, label, frame_index, time in validation:
            val_frames[frame_index].append((label, time))
        val_counts = np.bincount([items[0][0] for items in val_frames.values()], minlength=2)
        print(f"{task}: train samples class 0/1={train_counts.tolist()}, "
              f"validation frames class 0/1={val_counts.tolist()}")
        if min(train_counts) < 8 or min(val_counts) < 3:
            raise ValueError(f"Too few {task} samples in one class. Record and label "
                             "both states before and after validation_from_second.")

        x_train = np.stack([s[0] for s in train])
        y_train = np.asarray([s[1] for s in train])
        x_val = np.stack([s[0] for s in validation])
        scaler = StandardScaler().fit(x_train)
        if task == "eye":
            # Balance the eye MLP's training classes without touching validation.
            rng = np.random.default_rng(12)
            indices = np.concatenate([rng.choice(np.flatnonzero(y_train == value),
                                                 size=max(train_counts), replace=True)
                                      for value in (0, 1)])
            model = MLPClassifier(hidden_layer_sizes=(24,), alpha=5, max_iter=600,
                                  random_state=12)
            model.fit(scaler.transform(x_train[indices]), y_train[indices])
        else:
            model = LogisticRegression(max_iter=1000, class_weight="balanced", C=.2)
            model.fit(scaler.transform(x_train), y_train)

        probabilities = model.predict_proba(scaler.transform(x_val))[:, 1]
        by_frame = defaultdict(list)
        for (_, label, frame_index, time), probability in zip(validation, probabilities):
            by_frame[frame_index].append((label, time, probability))
        truth = [items[0][0] for items in by_frame.values()]
        predicted = [int(np.mean([item[2] for item in items]) >= .5)
                     for items in by_frame.values()]
        score = balanced_accuracy_score(truth, predicted)
        matrix = confusion_matrix(truth, predicted, labels=[0, 1]).tolist()
        errors = [round(items[0][1], 1) for items, actual, guess in
                  zip(by_frame.values(), truth, predicted) if actual != guess]
        print(f"{task}: validation balanced accuracy={score:.3f}, "
              f"confusion [[TN,FP],[FN,TP]]={matrix}, "
              f"error times (first 20)={errors[:20]}")
        trained[task] = (scaler, model)

    eye_scale, eye_model = trained["eye"]
    mouth_scale, mouth_model = trained["mouth"]
    parts = ["// Generated by train_driver_model.py from manually labeled OV3660 video.",
             "#pragma once", "namespace driver_ml_weights {",
             fmt_array("eye_mean", eye_scale.mean_),
             fmt_array("eye_inv_scale", 1 / eye_scale.scale_),
             fmt_array("eye_w1", eye_model.coefs_[0]),
             fmt_array("eye_b1", eye_model.intercepts_[0]),
             fmt_array("eye_w2", eye_model.coefs_[1]),
             f"inline constexpr float eye_b2 = {c_float(eye_model.intercepts_[1][0])};",
             fmt_array("mouth_mean", mouth_scale.mean_),
             fmt_array("mouth_inv_scale", 1 / mouth_scale.scale_),
             fmt_array("mouth_w", mouth_model.coef_[0]),
             f"inline constexpr float mouth_b = {c_float(mouth_model.intercept_[0])};",
             "} // namespace driver_ml_weights", ""]
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n\n".join(parts), encoding="utf-8")
    print("Generated candidate:", output)
    print("Review validation results before replacing main/driver_ml_weights.hpp.")


if __name__ == "__main__":
    main()
