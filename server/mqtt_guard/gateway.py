"""TLS MQTT ingress -> authenticate/replay check -> ThingsBoard local (or an optional TLS upstream)."""
import argparse
import asyncio
import hmac
import json
import logging
import os
import time
from logging.handlers import RotatingFileHandler
import ssl
from dataclasses import dataclass
from pathlib import Path

from amqtt.broker import Action, Broker
from amqtt.plugins.base import BaseAuthPlugin, BasePlugin, BaseTopicPlugin
import paho.mqtt.client as mqtt

from .verifier import Verifier
from .ntp import start_ntp

LOG = logging.getLogger("mqtt_guard")
RUNTIME = None


class GuardAuth(BaseAuthPlugin):
    @dataclass
    class Config:
        pass

    async def authenticate(self, *, session):
        device = RUNTIME.config["devices"].get(session.username)
        return bool(device and isinstance(session.password, str)
                    and hmac.compare_digest(session.password.encode(), device["mqtt_password"].encode())
                    and session.client_id == "guard-" + session.username)


class GuardACL(BaseTopicPlugin):
    @dataclass
    class Config:
        pass

    async def topic_filtering(self, *, session=None, topic=None, action=None):
        if session is None or session.username not in RUNTIME.config["devices"]:
            return False
        prefix = "dm/v1/" + session.username + "/"
        if action == Action.PUBLISH:
            return topic in (prefix + "hello", prefix + "telemetry")
        return topic == prefix + "down"


class GuardMessages(BasePlugin):
    @dataclass
    class Config:
        pass

    async def on_broker_message_broadcast(self, *, client_id, message):
        # Unlike MESSAGE_RECEIVED this event is emitted after publish ACL checks.
        session = self.context.get_session(client_id)
        if not session or not isinstance(message.data, (bytes, bytearray)):
            return
        device = session.username
        prefix = "dm/v1/" + device + "/"
        expected = {prefix + "hello": "hello", prefix + "telemetry": "sample"}.get(message.topic)
        if not expected:
            return
        try:
            result = RUNTIME.verifier.handle(message.data, device, expected)
            RUNTIME.last_decision = {"device": device, "reason": result["status"], "at_ms": int(time.time() * 1000)}
            if result["status"] not in ("SESSION", "ACCEPTED", "DUPLICATE"):
                RUNTIME.last_rejected = RUNTIME.last_decision.copy()
            LOG.info("device=%s decision=%s", device, result["status"])
            if response := result.get("response"):
                await self.context.broadcast_message(prefix + "down", response, qos=1)
        except Exception:
            # A storage/programming failure must never bypass validation.
            LOG.exception("Verifier failed closed")


class Runtime:
    def __init__(self, config, *, verify_only=False):
        self.config = config
        self.verifier = Verifier(config["database"], config["devices"])
        self.verify_only = verify_only
        self.health_file = Path(config["_health_file"]) if config.get("_health_file") else None
        self.started_at_ms = int(time.time() * 1000)
        self.started_monotonic = time.monotonic()
        self.last_decision = self.last_rejected = self.last_forwarded = None
        self.last_health_warning = 0.0
        upstream = config["thingsboard"]
        self.upstream = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                    client_id=upstream.get("client_id", "driver-guard-forwarder"))
        self.upstream.username_pw_set(upstream["token"])
        # Plain MQTT is allowed only on loopback for the ThingsBoard container
        # on this same PC. Every ESP32/LAN connection still requires TLS.
        if upstream.get("tls", True):
            self.upstream.tls_set(ca_certs=upstream["ca_file"], cert_reqs=ssl.CERT_REQUIRED, tls_version=ssl.PROTOCOL_TLS_CLIENT)
            self.upstream.tls_insecure_set(False)
        elif upstream["host"] not in ("127.0.0.1", "localhost", "::1"):
            raise ValueError("Unencrypted upstream is restricted to loopback")
        self.upstream.reconnect_delay_set(1, 10)
        self.broker = Broker({
            "listeners": {"default": {"type": "tcp", "bind": config["bind"], "ssl": True,
                                        "certfile": config["cert_file"], "keyfile": config["key_file"],
                                        "max_connections": 16}},
            "plugins": {
                "server.mqtt_guard.gateway.GuardAuth": {},
                "server.mqtt_guard.gateway.GuardACL": {},
                "server.mqtt_guard.gateway.GuardMessages": {},
            },
        })

    def publish_upstream(self, row, counters):
        if not self.upstream.is_connected():
            return False
        values = json.loads(row["telemetry"])
        values.update({
            "guard_rejected_count": sum(value for name, value in counters.items()
                                        if name not in ("SESSION", "ACCEPTED", "FORWARDED", "DUPLICATE")),
            "guard_bad_mac_count": counters.get("BAD_MAC", 0),
            "guard_duplicate_count": counters.get("DUPLICATE", 0),
            "guard_replay_rejected_count": sum(counters.get(name, 0) for name in
                ("REPLAY_OR_SEQUENCE_CONFLICT", "SESSION_INVALID", "OLD_HELLO")),
        })
        payload = json.dumps({"ts": row["measured_at"], "values": values},
                             ensure_ascii=False, separators=(",", ":"))
        info = self.upstream.publish("v1/devices/me/telemetry", payload, qos=1, retain=False)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            return False
        try:
            info.wait_for_publish(timeout=10)
            return info.is_published()
        except (RuntimeError, ValueError):
            return False

    async def forward_loop(self):
        while True:
            row = self.verifier.next_pending()
            if row and not self.verify_only and await asyncio.to_thread(self.publish_upstream, row, self.verifier.counts()):
                ack = self.verifier.mark_forwarded(row["id"])
                self.last_forwarded = {"device": row["device"], "sequence": row["sequence"], "at_ms": int(time.time() * 1000)}
                LOG.info("device=%s decision=FORWARDED sequence=%s", row["device"], row["sequence"])
                if ack:
                    await self.broker.internal_message_broadcast("dm/v1/" + ack[0] + "/down", ack[1], qos=1)
            await asyncio.sleep(0.25)

    def write_health(self, running=True):
        """Local diagnostic metadata only: never credentials or message payloads."""
        if not self.health_file:
            return
        pending = self.verifier.db.execute("SELECT count(*),min(measured_at) FROM outbox WHERE status='pending'").fetchone()
        now_ms = int(time.time() * 1000)
        snapshot = {
            "running": running, "pid": os.getpid(), "updated_at_ms": now_ms,
            "started_at_ms": self.started_at_ms, "uptime_s": round(time.monotonic() - self.started_monotonic, 1),
            "mqtt_bind": self.config["bind"], "ntp_port": self.config.get("ntp_port", 0),
            "upstream_connected": not self.verify_only and self.upstream.is_connected(),
            "upstream_host": self.config["thingsboard"]["host"], "upstream_port": self.config["thingsboard"].get("port", 18884),
            "counts": self.verifier.counts(), "pending_count": pending[0],
            "oldest_pending_age_ms": max(0, now_ms - pending[1]) if pending[1] else 0,
            "last_decision": self.last_decision, "last_rejected": self.last_rejected,
            "last_forwarded": self.last_forwarded,
        }
        try:
            self.health_file.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.health_file.with_suffix(".tmp")
            temporary.write_text(json.dumps(snapshot), encoding="utf-8")
            temporary.replace(self.health_file)
        except OSError as exc:
            if time.monotonic() - self.last_health_warning >= 30:
                LOG.warning("Local monitor snapshot unavailable: %s", type(exc).__name__)
                self.last_health_warning = time.monotonic()

    async def health_loop(self):
        # Independent of forwarding: a slow/disconnected upstream must not make
        # the monitor mistake a live gateway for a frozen process.
        while True:
            self.write_health()
            await asyncio.sleep(1)

    async def run(self, seconds=0):
        ntp_transport = None
        if self.config.get("ntp_port"):
            ntp_transport, _ = await start_ntp(self.config.get("ntp_bind", "0.0.0.0"), self.config["ntp_port"])
            LOG.info("Local bootstrap NTP listening on UDP %s", self.config["ntp_port"])
        if not self.verify_only:
            upstream = self.config["thingsboard"]
            self.upstream.connect_async(upstream["host"], upstream.get("port", 8883), 30)
            self.upstream.loop_start()
        await self.broker.start()
        LOG.info("TLS MQTT guard listening on %s; ThingsBoard forwarding %s", self.config["bind"],
                 "DISABLED (test only)" if self.verify_only else "enabled")
        task = asyncio.create_task(self.forward_loop())
        health_task = asyncio.create_task(self.health_loop()) if self.health_file else None
        try:
            if seconds:
                await asyncio.sleep(seconds)
            else:
                await asyncio.Event().wait()
        finally:
            task.cancel()
            tasks = [task]
            if health_task:
                health_task.cancel()
                tasks.append(health_task)
            await asyncio.gather(*tasks, return_exceptions=True)
            await self.broker.shutdown()
            self.upstream.disconnect()
            self.upstream.loop_stop()
            LOG.info("Guard counters: %s", self.verifier.counts())
            self.write_health(running=False)
            self.verifier.close()
            if ntp_transport:
                ntp_transport.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path(".private/mqtt_guard/config.json"))
    parser.add_argument("--verify-only", "--no-cloud", dest="verify_only", action="store_true", help="Tests only: verify but do not forward")
    parser.add_argument("--seconds", type=float, default=0)
    args = parser.parse_args()
    logging.basicConfig(level=logging.WARNING, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    LOG.setLevel(logging.INFO)
    config = json.loads(args.config.read_text(encoding="utf-8"))
    root = Path(__file__).resolve().parents[2]
    if args.config.resolve() == (root / ".private/mqtt_guard/config.json").resolve():
        # Test gateways on other config paths do not overwrite the real monitor.
        config["_health_file"] = str(root / ".private/runtime/gateway-health.json")
    audit = RotatingFileHandler(Path(config["database"]).with_suffix(".audit.log"),
                               maxBytes=5_000_000, backupCount=3, encoding="utf-8")
    audit.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
    LOG.addHandler(audit)
    global RUNTIME
    # -m executes as __main__; plugins must resolve the same runtime object.
    import sys
    sys.modules["server.mqtt_guard.gateway"] = sys.modules[__name__]
    async def run():
        global RUNTIME
        RUNTIME = Runtime(config, verify_only=args.verify_only)
        await RUNTIME.run(args.seconds)
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
