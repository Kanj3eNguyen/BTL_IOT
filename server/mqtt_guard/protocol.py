"""DM-MQTT-1: authenticate exact UTF-8 bytes, never re-serialize before HMAC."""
import base64
import hashlib
import hmac
import json
import re

DOMAIN = b"DM-MQTT-1\n"
MAX_WIRE = 8192
IDENTIFIER = re.compile(r"[A-Za-z0-9_-]{1,48}\Z")
NONCE = re.compile(r"[0-9a-f]{32}\Z")


class Rejected(ValueError):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise Rejected("DUPLICATE_JSON_KEY")
        result[key] = value
    return result


def _constant(value):
    raise Rejected("INVALID_NUMBER")


def parse_json(value):
    try:
        result = json.loads(value, object_pairs_hook=_object, parse_constant=_constant)
    except (ValueError, UnicodeError) as exc:
        if isinstance(exc, Rejected):
            raise
        raise Rejected("MALFORMED_JSON") from exc
    if not isinstance(result, dict):
        raise Rejected("OBJECT_REQUIRED")
    return result


def sign(message, key):
    raw = json.dumps(message, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")
    return wrap(raw, key)


def wrap(raw, key):
    return json.dumps({"v": 1, "payload": base64.b64encode(raw).decode("ascii"),
                       "mac": hmac.new(key, DOMAIN + raw, hashlib.sha256).hexdigest()},
                      separators=(",", ":")).encode("ascii")


def authenticate(wire, devices):
    if len(wire) > MAX_WIRE:
        raise Rejected("TOO_LARGE")
    envelope = parse_json(wire)
    if set(envelope) != {"v", "payload", "mac"} or type(envelope["v"]) is not int or envelope["v"] != 1:
        raise Rejected("SCHEMA")
    if not isinstance(envelope["payload"], str) or not isinstance(envelope["mac"], str):
        raise Rejected("SCHEMA")
    if not re.fullmatch(r"[0-9a-f]{64}", envelope["mac"]):
        raise Rejected("BAD_MAC")
    try:
        raw = base64.b64decode(envelope["payload"], validate=True)
    except ValueError as exc:
        raise Rejected("BASE64") from exc
    body = parse_json(raw)
    device = devices.get(body.get("device_id")) if isinstance(body.get("device_id"), str) else None
    if not device or body.get("key_id") != device["key_id"]:
        raise Rejected("UNKNOWN_KEY")
    expected = hmac.new(bytes.fromhex(device["hmac_key_hex"]), DOMAIN + raw, hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expected, envelope["mac"]):
        raise Rejected("BAD_MAC")
    return body, hashlib.sha256(raw).hexdigest()


def integer(body, name, low=0, high=(1 << 53) - 1):
    value = body.get(name)
    if type(value) is not int or not low <= value <= high:
        raise Rejected("SCHEMA_" + name.upper())
    return value


def nonce(body, name):
    value = body.get(name)
    if not isinstance(value, str) or not NONCE.fullmatch(value):
        raise Rejected("SCHEMA_" + name.upper())
    return value
