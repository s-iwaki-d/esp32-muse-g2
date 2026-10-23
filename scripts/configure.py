#!/usr/bin/env python3
"""Create local secrets without putting tokens in shell history or source files."""
import argparse
import getpass
import json
import os
from pathlib import Path
import secrets
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]


def private_write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as stream:
        stream.write(text)
    path.chmod(0o600)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker-url", required=True, help="https://WORKER.ACCOUNT.workers.dev")
    args = parser.parse_args()
    url = urlsplit(args.worker_url)
    if (url.scheme != "https" or not url.hostname or url.username or url.password
            or url.query or url.fragment or url.path not in ("", "/")):
        parser.error("worker-url must be an HTTPS origin without a path or credentials")
    origin = f"https://{url.netloc}"
    secret_path = ROOT / ".private/cloudflare-secrets.json"
    if secret_path.exists():
        keys = json.loads(secret_path.read_text())
    else:
        keys = {"EVEN_API_KEY": secrets.token_urlsafe(32), "DEVICE_TOKEN": secrets.token_urlsafe(32)}
    for key in ("EVEN_API_KEY", "DEVICE_TOKEN"):
        value = keys.get(key)
        if not isinstance(value, str) or len(value) < 32 or any(ord(c) < 33 or ord(c) > 126 for c in value):
            parser.error("invalid existing local bridge credential")
    sdk_token = getpass.getpass("Muse SDK token (mgst_..., blank to configure it later): ").strip()
    if sdk_token and (not sdk_token.startswith("mgst_") or not all(c.isalnum() or c in "_-" for c in sdk_token)):
        parser.error("invalid SDK token format")
    private_write(secret_path, json.dumps(keys, indent=2) + "\n")
    values = {
        "CONFIG_MUSE_BRIDGE_URL": f"wss://{url.netloc}/device/ws",
        "CONFIG_MUSE_BRIDGE_TOKEN": keys["DEVICE_TOKEN"],
    }
    # Preserve an existing token when rerunning only to change the Worker URL.
    local_path = ROOT / ".private/sdkconfig.local"
    if not sdk_token and local_path.exists():
        for line in local_path.read_text().splitlines():
            if line.startswith("CONFIG_GADGET_SDK_TOKEN="):
                sdk_token = json.loads(line.split("=", 1)[1])
    if sdk_token:
        values["CONFIG_GADGET_SDK_TOKEN"] = sdk_token
    private_write(local_path, "".join(f"{k}={json.dumps(v)}\n" for k, v in values.items()))
    private_write(ROOT / ".private/even-settings.txt",
                  f"Endpoint: {origin}/v1/chat/completions\nAPI Key: {keys['EVEN_API_KEY']}\n")
    private_write(ROOT / "cloudflare/.dev.vars",
                  "".join(f"{k}={json.dumps(v)}\n" for k, v in keys.items()))
    # ESP-IDF's generated sdkconfig overrides defaults. Update the build config
    # too, if present, so a new URL/token actually takes effect on the next build.
    generated = ROOT / ".build/stopwatch/sdkconfig"
    if generated.exists():
        lines = [line for line in generated.read_text().splitlines()
                 if line.split("=", 1)[0] not in values]
        lines.extend(f"{k}={json.dumps(v)}" for k, v in values.items())
        private_write(generated, "\n".join(lines) + "\n")
    print("Saved local settings in .private/ and cloudflare/.dev.vars (mode 0600).")
    print("Copy the Even settings from .private/even-settings.txt; tokens were not printed.")
    print("Next: build; upload the Cloudflare secrets; pair the flashed StopWatch in Muse.")


if __name__ == "__main__":
    main()
