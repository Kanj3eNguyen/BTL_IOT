"""TLS MQTT ingress -> authenticate/replay check -> ThingsBoard local (or an optional TLS upstream)."""
import argparse
import asyncio
import hmac
import json
import logging
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
        self.upstream = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="driver-guard-forwarder")
        upstream = config["thingsboard"]
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
                LOG.info("device=%s decision=FORWARDED sequence=%s", row["device"], row["sequence"])
                if ack:
                    await self.broker.internal_message_broadcast("dm/v1/" + ack[0] + "/down", ack[1], qos=1)
            await asyncio.sleep(0.25)

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
        try:
            if seconds:
                await asyncio.sleep(seconds)
            else:
                await asyncio.Event().wait()
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await self.broker.shutdown()
            self.upstream.disconnect()
            self.upstream.loop_stop()
            LOG.info("Guard counters: %s", self.verifier.counts())
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
