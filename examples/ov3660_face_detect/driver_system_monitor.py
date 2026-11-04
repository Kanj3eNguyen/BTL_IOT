"""Unified local monitor. Serial and server I/O never run in the Tk UI thread."""
import base64
import collections
import json
import threading
import time
import tkinter as tk
from tkinter import ttk
import urllib.error
import urllib.request
import urllib.parse
import webbrowser
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np
import serial

from driver_monitor_viewer import open_stream_port, packets, STATUS_KIND, JPEG_KIND

ROOT = Path(__file__).resolve().parents[2]
PRIVATE = ROOT / ".private"


def freshness(timestamp_ms, now_ms=None, limit_ms=6000):
    if not isinstance(timestamp_ms, (int, float)) or isinstance(timestamp_ms, bool):
        return "missing"
    now_ms = time.time() * 1000 if now_ms is None else now_ms
    if timestamp_ms > now_ms + 5000:
        return "future"
    return "fresh" if now_ms - timestamp_ms <= limit_ms else "stale"


def security_counts(counts):
    counts = {k: v for k, v in (counts or {}).items() if type(v) is int and v >= 0}
    return {
        "rejected": sum(v for k, v in counts.items() if k not in ("SESSION", "ACCEPTED", "FORWARDED", "DUPLICATE")),
        "bad_mac": counts.get("BAD_MAC", 0),
        "replay": sum(counts.get(k, 0) for k in ("REPLAY_OR_SEQUENCE_CONFLICT", "SESSION_INVALID", "OLD_HELLO")),
        "duplicate": counts.get("DUPLICATE", 0),
    }


class Store:
    def __init__(self):
        self.lock = threading.Lock()
        self.values = {}
        self.events = collections.deque(maxlen=160)

    def update(self, **values):
        with self.lock:
            self.values.update(values)

    def event(self, message):
        with self.lock:
            self.events.append(time.strftime("%H:%M:%S") + "  " + message)

    def snapshot(self):
        with self.lock:
            return {**self.values, "events": tuple(self.events)}


class SerialReader(threading.Thread):
    def __init__(self, args, store, stop):
        super().__init__(name="esp-monitor-serial", daemon=True)
        self.args, self.store, self.stop = args, store, stop
        self.port = None

    def run(self):
        previous = {}
        frame_id = 0
        stats = {"bad_packets": 0}
        metadata = None
        try:
            if self.args.out:
                metadata = (self.args.out / "frames.jsonl").open("w", encoding="utf-8")
            while not self.stop.is_set():
                try:
                    with open_stream_port(self.args.port, self.args.baud) as port:
                        self.port = port
                        self.store.update(serial_open=True, serial_error="")
                        self.store.event(f"Đã mở {self.args.port} · {self.args.baud} baud")
                        buffer = bytearray()
                        state = {}
                        fps_start, fps_frames, fps = time.monotonic(), 0, 0.0
                        while not self.stop.is_set():
                            buffer.extend(port.read(min(port.in_waiting or 1, 65536)))
                            newest = None
                            for kind, payload in packets(buffer, stats):
                                if kind == STATUS_KIND:
                                    try:
                                        parsed = json.loads(payload)
                                    except (ValueError, UnicodeError):
                                        continue
                                    if not isinstance(parsed, dict):
                                        continue
                                    state = parsed
                                    if state.get("uptime_ms", 0) < previous.get("uptime_ms", 0):
                                        self.store.event("ESP32 vừa khởi động lại; bộ đếm RAM có thể về 0")
                                    for field, name in (("wifi_connected", "Wi-Fi"), ("time_synced", "Đồng bộ giờ"),
                                                        ("mqtt_connected", "MQTT"), ("guard_ready", "Phiên Guard")):
                                        if state.get(field) != previous.get(field):
                                            self.store.event(name + (": OK" if state.get(field) else ": chưa sẵn sàng"))
                                    for field, name in (("blink_count", "Blink"), ("yawn_count", "Ngáp"),
                                                        ("alert_reasons", "Mã cảnh báo"), ("tls_error", "Mã lỗi TLS")):
                                        if field in state and state.get(field) != previous.get(field):
                                            self.store.event(f"{name}: {state[field]}")
                                    previous = state
                                    self.store.update(state=state, state_received=time.monotonic())
                                elif kind == JPEG_KIND:
                                    if metadata:
                                        (self.args.out / "frames" / f"{frame_id:06d}.jpg").write_bytes(payload)
                                        metadata.write(json.dumps({"frame": frame_id, "utc": datetime.now(timezone.utc).isoformat(),
                                                                   "board_status": state}, separators=(",", ":")) + "\n")
                                        metadata.flush()
                                    frame_id += 1
                                    fps_frames += 1
                                    newest = payload
                            if newest is not None:
                                frame = cv2.imdecode(np.frombuffer(newest, np.uint8), cv2.IMREAD_COLOR)
                                if frame is not None:
                                    now = time.monotonic()
                                    if now - fps_start >= 2:
                                        fps = fps_frames / (now - fps_start)
                                        fps_start, fps_frames = now, 0
                                    self.store.update(frame=frame, frame_id=frame_id, frame_received=now, fps=fps)
                            self.store.update(bad_packets=stats["bad_packets"], frames=frame_id)
                except (serial.SerialException, OSError) as exc:
                    message = f"{self.args.port}: không mở/đọc được cổng ({type(exc).__name__}); đang thử lại"
                    if self.store.snapshot().get("serial_error") != message:
                        self.store.event(message)
                    self.store.update(serial_open=False, serial_error=message)
                finally:
                    self.port = None
                self.stop.wait(1.5)
        finally:
            if metadata:
                metadata.close()


class LocalThingsBoard:
    """Read-only monitoring calls, with bounded timeouts and no credential logging."""
    def __init__(self):
        self.token = None

    def call(self, path, data=None):
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["X-Authorization"] = "Bearer " + self.token
        request = urllib.request.Request("http://127.0.0.1:8080" + path, headers=headers,
                                         data=json.dumps(data).encode() if data is not None else None)
        with urllib.request.urlopen(request, timeout=3) as response:
            return json.loads(response.read())

    def latest(self):
        device = json.loads((PRIVATE / "thingsboard/device.json").read_text(encoding="utf-8"))["id"]
        path = f"/api/plugins/telemetry/DEVICE/{device}/values/timeseries?keys=" + ",".join((
            "auth_device_id", "auth_sequence", "integrity_status", "last_verified_at",
            "guard_rejected_count", "guard_bad_mac_count", "guard_replay_rejected_count", "guard_duplicate_count"))
        for attempt in range(2):
            if not self.token:
                accounts = json.loads((PRIVATE / "thingsboard/accounts.json").read_text(encoding="utf-8"))
                result = self.call("/api/auth/login", {"username": "tenant@thingsboard.org",
                                                     "password": accounts["tenant@thingsboard.org"]["password"]})
                self.token = result["token"]
            try:
                values = self.call(path)
                return {key: rows[0]["value"] for key, rows in values.items() if rows}
            except urllib.error.HTTPError as exc:
                if exc.code != 401 or attempt:
                    raise
                self.token = None


def gateway_log(path):
    """Only allowlisted audit metadata, never raw payloads/config/tracebacks."""
    import re
    with path.open("rb") as stream:
        stream.seek(0, 2)
        stream.seek(max(0, stream.tell() - 32768))
        lines = stream.read().decode("utf-8", errors="replace").splitlines()
    result = []
    for line in lines:
        match = re.search(r"device=([A-Za-z0-9_-]+) decision=([A-Z_]+)(?: sequence=(\d+))?", line)
        if match:
            timestamp = line[11:19] if re.match(r"\d{4}-\d\d-\d\d ", line) else ""
            result.append(timestamp + "  " + match.group(1) + "  " + match.group(2) +
                          ("  seq=" + match.group(3) if match.group(3) else ""))
        elif match := re.search(r"Local bootstrap NTP listening on UDP (\d+)", line):
            result.append("NTP local · UDP " + match.group(1))
        elif match := re.search(r"TLS MQTT guard listening on ([A-Za-z0-9.:\[\]_-]+)", line):
            result.append("Gateway MQTTS · " + match.group(1))
    return "\n".join(result[-65:])


class ServerReader(threading.Thread):
    def __init__(self, store, stop):
        super().__init__(name="gateway-monitor-reader", daemon=True)
        self.store, self.stop = store, stop

    def run(self):
        while not self.stop.is_set():
            try:
                health = json.loads((PRIVATE / "runtime/gateway-health.json").read_text(encoding="utf-8"))
                if isinstance(health, dict) and isinstance(health.get("counts", {}), dict):
                    self.store.update(health=health)
            except (OSError, ValueError):
                pass  # Last snapshot will become visibly stale rather than stay green.
            try:
                self.store.update(gateway_log=gateway_log(PRIVATE / "mqtt_guard/state.audit.log"))
            except OSError:
                self.store.update(gateway_log="Chưa đọc được audit log gateway.")
            self.stop.wait(1)


class ApiReader(threading.Thread):
    def __init__(self, store, stop):
        super().__init__(name="thingsboard-monitor-api", daemon=True)
        self.store, self.stop = store, stop

    def run(self):
        tb = LocalThingsBoard()
        while not self.stop.is_set():
            try:
                values = tb.latest()
                self.store.update(tb=values, api_online=True, api_error="", api_received=time.monotonic())
            except (OSError, ValueError, KeyError) as exc:
                code = f"HTTP {exc.code}" if isinstance(exc, urllib.error.HTTPError) else type(exc).__name__
                self.store.update(api_online=False, api_error=code)
            self.stop.wait(3)


def yes_no(value, yes="OK", no="CHƯA NỐI"):
    return "—" if value is None else yes if value else no


def number(value, decimals=0, suffix=""):
    try:
        return f"{float(value):.{decimals}f}" + suffix
    except (ValueError, TypeError):
        return "—"


class Monitor:
    def __init__(self, args):
        self.args = args
        self.store, self.stop = Store(), threading.Event()
        self.root = tk.Tk()
        self.root.title("ESP32 + Gateway · Giám sát tổng hợp")
        self.root.geometry(f"{min(1280, self.root.winfo_screenwidth()-60)}x{min(700, self.root.winfo_screenheight()-140)}")
        self.root.minsize(980, 620)
        self.root.configure(bg="#eef3f6")
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("TFrame", background="#eef3f6")
        style.configure("TLabel", background="#eef3f6", font=("Segoe UI", 10))
        style.configure("Title.TLabel", font=("Segoe UI", 16, "bold"), foreground="#163a48")
        style.configure("Heading.TLabel", font=("Segoe UI", 11, "bold"), foreground="#126b61")
        style.configure("Value.TLabel", font=("Segoe UI", 10, "bold"), foreground="#163a48")
        style.configure("Count.TLabel", font=("Segoe UI", 20, "bold"), foreground="#ad6500")
        style.configure("TLabelframe", background="#eef3f6")
        style.configure("TLabelframe.Label", font=("Segoe UI", 11, "bold"), foreground="#163a48")
        self.variables, self.last_text = {}, {}
        header = ttk.Frame(self.root, padding=(16, 10))
        header.pack(fill="x")
        ttk.Label(header, text="GIÁM SÁT ESP32 · GATEWAY · THINGSBOARD", style="Title.TLabel").pack(side="left")
        ttk.Button(header, text="Mở dashboard", command=self.open_dashboard).pack(side="right")
        self.banner = ttk.Label(self.root, padding=(16, 2), style="Heading.TLabel")
        self.banner.pack(fill="x")
        ttk.Label(self.root, text="Q / Esc: đóng viewer · Tự nối lại COM · Không gửi lệnh điều khiển bo/gateway", padding=(14, 3)).pack(side="bottom", fill="x")
        body = ttk.Frame(self.root, padding=(12, 8))
        body.pack(fill="both", expand=True)
        body.columnconfigure(0, weight=4, minsize=330)
        body.columnconfigure(1, weight=3, minsize=285)
        body.columnconfigure(2, weight=3, minsize=295)
        body.rowconfigure(0, weight=1)
        video = ttk.LabelFrame(body, text="Camera trên ESP32", padding=8)
        video.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        self.camera = ttk.Label(video, text="Đang chờ ảnh từ bo…", anchor="center")
        self.camera.pack(fill="both", expand=True)
        self.video_info = ttk.Label(video, wraplength=400, justify="left")
        self.video_info.pack(fill="x", pady=(6, 0))
        esp = ttk.LabelFrame(body, text="ESP32 · trạng thái trực tiếp qua COM", padding=10)
        esp.grid(row=0, column=1, sticky="nsew", padx=(0, 8))
        for key, caption in (("serial", "Cổng serial"), ("wifi", "Wi-Fi / lỗi"), ("clock", "Đồng bộ giờ"),
                             ("mqtt", "MQTT / phiên Guard"), ("send", "Sequence gửi / PUBACK"),
                             ("ack", "TB seq ACK / số ACK"), ("uptime", "Thời gian chạy"),
                             ("heap", "RAM nội / khối lớn"), ("tls", "Lỗi TLS / stack"),
                             ("face", "Khuôn mặt / mắt"), ("closed", "Nhắm mắt liên tục"),
                             ("mouth", "Miệng"), ("blinks", "Blink tổng / cửa sổ"),
                             ("yawns", "Ngáp tổng / cửa sổ"), ("perclos", "PERCLOS / cửa sổ"),
                             ("alarm", "Cảnh báo / buzzer"), ("light", "Ánh sáng / AE")):
            self.field(esp, "esp_" + key, caption)
        gateway = ttk.LabelFrame(body, text="Gateway Windows · ThingsBoard local", padding=10)
        gateway.grid(row=0, column=2, sticky="nsew")
        for key, caption in (("process", "Gateway / health"), ("pid", "PID / thời gian chạy"),
                             ("bind", "MQTTS / NTP"), ("upstream", "MQTT tới ThingsBoard"),
                             ("api", "API ThingsBoard"), ("persisted", "TB đã lưu sequence"),
                             ("integrity", "Toàn vẹn trên REST"),
                             ("pending", "Outbox chờ / tuổi lớn nhất")):
            self.field(gateway, "gw_" + key, caption)
        counters = ttk.Frame(gateway)
        counters.pack(fill="x", pady=(10, 4))
        for i, (key, caption) in enumerate((("rejected", "Gói bị từ chối"), ("bad_mac", "HMAC lỗi"),
                                            ("replay", "Phiên / sequence"), ("duplicate", "Gói trùng bỏ qua"))):
            box = ttk.Frame(counters, padding=4)
            box.grid(row=i//2, column=i%2, sticky="ew")
            counters.columnconfigure(i%2, weight=1)
            ttk.Label(box, text=caption).pack(anchor="w")
            variable = tk.StringVar(value="—")
            self.variables["count_" + key] = variable
            ttk.Label(box, textvariable=variable, style="Count.TLabel").pack(anchor="w")
        self.decision = ttk.Label(gateway, wraplength=290, justify="left")
        self.decision.pack(fill="x", pady=(8, 0))
        logs = ttk.Notebook(self.root)
        logs.pack(fill="x", padx=12, pady=(0, 6))
        self.esp_log = self.log_tab(logs, "Sự kiện ESP32 (từ JSON trạng thái)")
        self.gateway_log = self.log_tab(logs, "Audit gateway · quyết định thật")
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.bind("<Key-q>", self.close)
        self.root.bind("<Key-Q>", self.close)
        self.root.bind("<Escape>", self.close)
        self.started = time.monotonic()
        self.last_frame_id, self.last_fields = -1, 0
        self.threads = [SerialReader(args, self.store, self.stop), ServerReader(self.store, self.stop), ApiReader(self.store, self.stop)]
        for thread in self.threads:
            thread.start()
        self.root.after(50, self.tick)

    def field(self, parent, key, caption):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=1)
        ttk.Label(row, text=caption + ":", font=("Segoe UI", 9)).pack(side="left", anchor="w")
        variable = tk.StringVar(value="—")
        self.variables[key] = variable
        ttk.Label(row, textvariable=variable, style="Value.TLabel").pack(side="right", anchor="e")

    @staticmethod
    def log_tab(notebook, title):
        frame = ttk.Frame(notebook)
        text = tk.Text(frame, height=4, font=("Consolas", 9), wrap="none", bg="#142631", fg="#d9e8ef", state="disabled")
        scroll = ttk.Scrollbar(frame, command=text.yview)
        text.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        text.pack(fill="both", expand=True)
        notebook.add(frame, text=title)
        return text

    def text(self, key, value):
        value = str(value)
        if self.last_text.get(key) != value:
            self.variables[key].set(value)
            self.last_text[key] = value

    def log_text(self, widget, value):
        key = str(widget)
        if self.last_text.get(key) != value:
            widget.configure(state="normal")
            widget.delete("1.0", "end")
            widget.insert("end", value)
            widget.see("end")
            widget.configure(state="disabled")
            self.last_text[key] = value

    def fields(self, snapshot, now):
        fresh = snapshot.get("serial_open") and now - snapshot.get("state_received", -1e9) < 2
        state = snapshot.get("state", {}) if fresh else {}
        health = snapshot.get("health", {})
        healthy = health.get("running") and freshness(health.get("updated_at_ms")) == "fresh"
        self.banner.configure(text=f"ESP32: {'ĐANG NHẬN' if fresh else 'CHỜ / MẤT DỮ LIỆU'}     |     Gateway: {'ĐANG CHẠY' if healthy else 'CHƯA CÓ HEALTH MỚI'}     |     Viewer đọc trực tiếp, không làm suy luận")
        self.text("esp_serial", self.args.port + (" · có gói mới" if fresh else " · đang chờ"))
        self.text("esp_wifi", yes_no(state.get("wifi_connected")) + " / " + str(state.get("wifi_disconnect_reason", "—")))
        self.text("esp_clock", yes_no(state.get("time_synced"), no="ĐANG CHỜ"))
        self.text("esp_mqtt", yes_no(state.get("mqtt_connected")) + " / " + yes_no(state.get("guard_ready"), "READY", "WAIT"))
        self.text("esp_send", f"{state.get('mqtt_sequence', '—')} / {state.get('mqtt_acked', '—')}")
        self.text("esp_ack", f"{state.get('guard_forwarded_sequence', '—')} / {state.get('guard_forwarded_count', '—')}")
        self.text("esp_uptime", number(state.get("uptime_ms", 0)/1000, 1, " s") if state else "—")
        self.text("esp_heap", f"{number(state.get('heap_internal', 0)/1024, 0)} / {number(state.get('heap_largest', 0)/1024, 0)} KB" if state else "—")
        self.text("esp_tls", f"{state.get('tls_error', '—')} / {state.get('tls_stack_error', '—')}")
        eye = yes_no(state.get("eyes_closed"), "NHẮM", "MỞ") if state.get("eyes_valid") else "KHÔNG RÕ"
        self.text("esp_face", yes_no(state.get("driver_present"), "CÓ MẶT", "VẮNG") + " / " + eye)
        self.text("esp_closed", number(state.get("eye_closed_duration"), 1, " s"))
        self.text("esp_mouth", yes_no(state.get("mouth_open"), "MỞ", "ĐÓNG") if state.get("mouth_valid") else "KHÔNG RÕ")
        self.text("esp_blinks", f"{state.get('blink_count', '—')} / {state.get('blink_window_count', '—')}")
        self.text("esp_yawns", f"{state.get('yawn_count', '—')} / {state.get('yawn_window_count', '—')}")
        perclos = number(state.get("perclos", 0)*100, 1, "%") if state.get("perclos_valid") else "CHƯA ĐỦ"
        self.text("esp_perclos", perclos + (" / READY" if state.get("perclos_window_ready") else " / COLLECT"))
        self.text("esp_alarm", f"mask {state.get('alert_reasons', '—')} / nhịp {state.get('buzzer_pattern', '—')}")
        light = {0:"N/A",1:"ĐIỀU CHỈNH",2:"ỔN",3:"TỐI",4:"SÁNG"}.get(state.get("lighting_state"), "—")
        self.text("esp_light", light + " / AE " + str(state.get("ae_level", "—")))
        self.text("gw_process", "ĐANG CHẠY" if healthy else "DỪNG / DỮ LIỆU CŨ" if health else "CHƯA CÓ SNAPSHOT")
        self.text("gw_pid", f"{health.get('pid', '—')} / {number(health.get('uptime_s'), 0, ' s')}")
        self.text("gw_bind", f"{health.get('mqtt_bind', '—')} / UDP {health.get('ntp_port', '—')}")
        self.text("gw_upstream", yes_no(health.get("upstream_connected")) if healthy else "KHÔNG CÓ TRẠNG THÁI MỚI")
        api_recent = snapshot.get("api_online") and now - snapshot.get("api_received", -1e9) < 10
        tb = snapshot.get("tb", {})
        try:
            age = max(0, (time.time()*1000 - int(tb["last_verified_at"]))/1000)
        except (KeyError, ValueError, TypeError):
            age = None
        self.text("gw_api", "ĐỌC ĐƯỢC REST" if api_recent else "CHƯA ĐỌC ĐƯỢC · " + snapshot.get("api_error", "đang chờ"))
        suffix = " · CŨ" if (age is not None and age > 20) or not api_recent else ""
        self.text("gw_persisted", f"{tb.get('auth_sequence', '—')} · {number(age, 1, ' s trước')}{suffix}")
        self.text("gw_integrity", str(tb.get("integrity_status", "—")) + suffix)
        self.text("gw_pending", f"{health.get('pending_count', '—')} / {number(health.get('oldest_pending_age_ms', 0)/1000, 1, ' s')}")
        for key, value in security_counts(health.get("counts")).items():
            self.text("count_"+key, str(value) + (" (cũ)" if not healthy else "") if health else "—")
        last = health.get("last_rejected") or health.get("last_decision") or {}
        caption = "Từ chối gần nhất: " if health.get("last_rejected") else "Quyết định gần nhất: "
        self.decision.configure(text=caption + last.get("reason", "—") + "\nThiết bị: " + last.get("device", "—") + "\nCounter là tổng gateway; lỗi demo live cũng được tính.")
        self.video_info.configure(text=f"Video {snapshot.get('fps', 0):.1f} FPS · AI {state.get('analysis_hz', 0):.1f} FPS\nTuổi ảnh {state.get('frame_age_ms', 0):.0f} ms · Nén {state.get('encode_ms', 0):.0f} ms\nUART lỗi {snapshot.get('bad_packets', 0)} · Bỏ preview {state.get('stream_dropped', '—')} · Ảnh nguồn lỗi {state.get('camera_bad_frames', '—')}")
        self.log_text(self.esp_log, "\n".join(snapshot["events"]))
        self.log_text(self.gateway_log, snapshot.get("gateway_log", "Đang đọc audit gateway…"))

    def tick(self):
        if self.stop.is_set():
            return
        now = time.monotonic()
        snapshot = self.store.snapshot()
        if now - self.last_fields >= .25:
            self.fields(snapshot, now)
            self.last_fields = now
        fresh_frame = now - snapshot.get("frame_received", -1e9) < 2
        if fresh_frame and snapshot.get("frame_id") != self.last_frame_id:
            frame = snapshot["frame"]
            size = max(160, min(500, self.camera.winfo_width()-8, self.camera.winfo_height()-8))
            image = cv2.resize(frame, (size, size), interpolation=cv2.INTER_LINEAR)
            ok, encoded = cv2.imencode(".png", image)
            if ok:
                self.photo = tk.PhotoImage(data=base64.b64encode(encoded).decode("ascii"))
                self.camera.configure(image=self.photo, text="")
                self.last_frame_id = snapshot["frame_id"]
        elif not fresh_frame:
            self.camera.configure(image="", text="Đang chờ ảnh từ ESP32…\nTự nối lại khi COM khả dụng")
            self.last_frame_id = -1
        if self.args.seconds and now - self.started >= self.args.seconds:
            self.close()
        else:
            self.root.after(50, self.tick)

    @staticmethod
    def open_dashboard():
        url = "http://127.0.0.1:8080"
        try:
            candidate = json.loads((PRIVATE / "thingsboard/dashboard.json").read_text(encoding="utf-8"))["url"]
            if urllib.parse.urlsplit(candidate).hostname in ("localhost", "127.0.0.1"):
                url = candidate
        except (OSError, ValueError, KeyError):
            pass
        webbrowser.open(url)

    def close(self, event=None):
        if self.stop.is_set():
            return
        self.stop.set()
        self.root.destroy()

    def run(self):
        try:
            self.root.mainloop()
        finally:
            self.stop.set()
            self.threads[0].join(timeout=2)


def run(args):
    if args.out:
        if (args.out / "frames.jsonl").exists():
            raise ValueError(f"Existing capture: {args.out}")
        (args.out / "frames").mkdir(parents=True, exist_ok=True)
    Monitor(args).run()
