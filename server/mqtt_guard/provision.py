"""Generate private per-device keys/certificates and configure the local board."""
import argparse
import ipaddress
import json
import re
import secrets
from datetime import datetime, timedelta, timezone
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

ROOT = Path(__file__).resolve().parents[2]
PRIVATE = ROOT / ".private/mqtt_guard"


def certificates(host):
    ca_path = PRIVATE / "ca.pem"
    cert_path = PRIVATE / "server.pem"
    key_path = PRIVATE / "server-key.pem"
    ca_key_path = PRIVATE / "ca-key.pem"
    existing_host = PRIVATE / "certificate-host.txt"
    if cert_path.exists() and ca_path.exists() and key_path.exists() and existing_host.exists() and existing_host.read_text() == host:
        return
    now = datetime.now(timezone.utc)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Driver Monitor Local Ingest CA")])
    ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name).public_key(ca_key.public_key())
          .serial_number(x509.random_serial_number()).not_valid_before(now - timedelta(days=1))
          .not_valid_after(now + timedelta(days=3650)).add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
          .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), True)
          .sign(ca_key, hashes.SHA256()))
    server_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    names = [x509.DNSName("localhost"), x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]
    try:
        names.append(x509.IPAddress(ipaddress.ip_address(host)))
    except ValueError:
        names.append(x509.DNSName(host))
    cert = (x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, host)]))
            .issuer_name(ca_name).public_key(server_key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(days=1)).not_valid_after(now + timedelta(days=365))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), True)
            .add_extension(x509.SubjectAlternativeName(names), False)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), False)
            .sign(ca_key, hashes.SHA256()))
    for path, key in ((key_path, server_key), (ca_key_path, ca_key)):
        path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                          serialization.NoEncryption()))
    ca_path.write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    existing_host.write_text(host)


def set_sdkconfig(path, values):
    text = path.read_text(encoding="utf-8")
    for name, value in values.items():
        line = name + "=" + ("y" if value is True else json.dumps(value))
        pattern = rf"^(?:{re.escape(name)}=.*|# {re.escape(name)} is not set)$"
        if re.search(pattern, text, re.M):
            text = re.sub(pattern, lambda _: line, text, flags=re.M)
        else:
            text += "\n" + line + "\n"
    path.write_text(text, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="LAN IPv4 address of this PC, reachable by the ESP32")
    parser.add_argument("--device", default="esp32s3-driver-01")
    parser.add_argument("--port", type=int, default=8885)
    parser.add_argument("--configure-board", action="store_true")
    args = parser.parse_args()
    ipaddress.IPv4Address(args.host)
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,48}", args.device):
        parser.error("Device ID must contain 1-48 letters, digits, '-' or '_'")
    PRIVATE.mkdir(parents=True, exist_ok=True)
    config_path = PRIVATE / "config.json"
    config = json.loads(config_path.read_text(encoding="utf-8")) if config_path.exists() else {
        "devices": {}, "thingsboard": {"host": "127.0.0.1", "port": 18884, "tls": False,
                                    "token": secrets.token_urlsafe(32)}}
    device = config["devices"].setdefault(args.device, {"key_id": "key-1", "hmac_key_hex": secrets.token_hex(32),
                                                        "mqtt_password": secrets.token_urlsafe(32)})
    config.update({"host": args.host, "bind": f"0.0.0.0:{args.port}",
                   "database": str(PRIVATE / "state.sqlite3"), "ntp_bind": "0.0.0.0", "ntp_port": 123,
                   "cert_file": str(PRIVATE / "server.pem"), "key_file": str(PRIVATE / "server-key.pem")})
    certificates(args.host)
    config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
    if args.configure_board:
        project = ROOT / "examples/ov3660_face_detect"
        sdkconfig = project / "sdkconfig"
        if not sdkconfig.exists():
            parser.error("Create sdkconfig with idf.py/menuconfig first, then rerun --configure-board")
        backup = PRIVATE / "sdkconfig.before-guard"
        if not backup.exists():
            backup.write_bytes(sdkconfig.read_bytes())
        (project / "main/certs/ingest-root-ca.pem").write_bytes((PRIVATE / "ca.pem").read_bytes())
        set_sdkconfig(sdkconfig, {"CONFIG_DRIVER_MQTT_GUARD_ENABLED": True,
                                 "CONFIG_DRIVER_GUARD_URI": f"mqtts://{args.host}:{args.port}",
                                 "CONFIG_DRIVER_GUARD_DEVICE_ID": args.device,
                                 "CONFIG_DRIVER_GUARD_KEY_ID": device["key_id"],
                                 "CONFIG_DRIVER_GUARD_HMAC_KEY_HEX": device["hmac_key_hex"],
                                 "CONFIG_DRIVER_GUARD_MQTT_PASSWORD": device["mqtt_password"],
                                 "CONFIG_DRIVER_SNTP_SERVER": args.host})
    print(f"Private configuration ready for {args.device}; gateway mqtts://{args.host}:{args.port}")
    print("Keys/tokens were stored under .private and were not printed. Do not commit .private or sdkconfig.")


if __name__ == "__main__":
    main()
