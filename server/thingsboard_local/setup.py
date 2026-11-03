"""Provision the verified device on a NEW local ThingsBoard instance."""
import argparse
import json
import secrets
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PRIVATE = ROOT / ".private/thingsboard"


class ThingsBoard:
    def __init__(self, url="http://127.0.0.1:8080"):
        if urllib.parse.urlparse(url).hostname not in ("127.0.0.1", "localhost"):
            raise ValueError("This setup tool only manages local ThingsBoard")
        self.url = url.rstrip("/")
        self.token = None

    def call(self, method, path, data=None):
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["X-Authorization"] = "Bearer " + self.token
        request = urllib.request.Request(self.url + path, method=method, headers=headers,
                                         data=None if data is None else json.dumps(data).encode())
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                value = response.read()
                return json.loads(value) if value else None
        except urllib.error.HTTPError as exc:
            # Do not include request bodies/passwords/tokens in diagnostics.
            raise RuntimeError(f"ThingsBoard {method} {path.split('?')[0]} returned HTTP {exc.code}") from None

    def login(self, username, password):
        self.token = None
        result = self.call("POST", "/api/auth/login", {"username": username, "password": password})
        self.token = result["token"]


def load_account(username="tenant@thingsboard.org"):
    return json.loads((PRIVATE / "accounts.json").read_text(encoding="utf-8"))[username]["password"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    args = parser.parse_args()
    PRIVATE.mkdir(parents=True, exist_ok=True)
    account_path = PRIVATE / "accounts.json"
    accounts = json.loads(account_path.read_text(encoding="utf-8")) if account_path.exists() else {
        name + "@thingsboard.org": {"password": name} for name in ("tenant", "sysadmin", "customer")}
    tb = ThingsBoard(args.url)
    for username, record in accounts.items():
        try:
            tb.login(username, record["password"])
        except RuntimeError:
            if "pending" not in record:
                raise
            tb.login(username, record["pending"])
            record["password"] = record.pop("pending")
            record["changed"] = True
        if not record.get("changed"):
            record["pending"] = secrets.token_urlsafe(24)
            account_path.write_text(json.dumps(accounts, indent=2), encoding="utf-8")
            tb.call("POST", "/api/auth/changePassword", {"currentPassword": record["password"],
                                                        "newPassword": record["pending"]})
            record["password"] = record.pop("pending")
            record["changed"] = True
        account_path.write_text(json.dumps(accounts, indent=2), encoding="utf-8")
    tb.login("tenant@thingsboard.org", accounts["tenant@thingsboard.org"]["password"])
    device_name = "ESP32S3 Driver Monitor - Verified"
    try:
        device = tb.call("GET", "/api/tenant/devices?deviceName=" + urllib.parse.quote(device_name))
    except RuntimeError as exc:
        if "HTTP 404" not in str(exc):
            raise
        device = tb.call("POST", "/api/device", {"name": device_name, "type": "Driver Monitor"})
    if not device:
        device = tb.call("POST", "/api/device", {"name": device_name, "type": "Driver Monitor"})
    device_id = device["id"]["id"]
    config_path = ROOT / ".private/mqtt_guard/config.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    credentials = tb.call("GET", f"/api/device/{device_id}/credentials")
    credentials["credentialsType"] = "ACCESS_TOKEN"
    credentials["credentialsId"] = config["thingsboard"]["token"]
    credentials["credentialsValue"] = None
    tb.call("POST", "/api/device/credentials", credentials)
    tb.call("POST", f"/api/plugins/telemetry/DEVICE/{device_id}/SERVER_SCOPE", {
        "verified_ingress_only": True, "guard_device_id": next(iter(config["devices"])),
        "description": "Only the local HMAC verifier holds this device access token."})
    (PRIVATE / "device.json").write_text(json.dumps({"id": device_id, "name": device_name, "url": args.url}, indent=2), encoding="utf-8")
    (PRIVATE / "DANG_NHAP.txt").write_text(
        "ThingsBoard local: " + args.url + "\nTài khoản: tenant@thingsboard.org\nMật khẩu: " +
        accounts["tenant@thingsboard.org"]["password"] + "\n\nKhông đưa thư mục .private lên Git.\n", encoding="utf-8")
    print("Local ThingsBoard provisioned:", device_name)
    print("Login details: .private/thingsboard/DANG_NHAP.txt (password not printed)")


if __name__ == "__main__":
    main()
