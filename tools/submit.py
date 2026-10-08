"""Submits a .crd replay to crimson.land's leaderboard, signed the way the game's client signs uploads
(upstream src/crimson/leaderboard/client.py). The key is CRIMSON_LAND_KEY in .env (a 32-byte Ed25519 seed, hex),
made on first use; the server ties the account to it, so keep it to submit under the same account again.

Run with upstream's environment: upstream/crimson/.venv/bin/python tools/submit.py <run.crd> <name> [--dry-run]
`tools/submit.py login` prints a one-time sign-in link for the key's account (the game's Open profile), from which
the site links X, Discord or GitHub, or joins the account into one that already has them. Links last a minute, so
`tools/submit.py login <host:port>` instead serves them: each visit there mints one and redirects to it.
"""
import base64
import json
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

from nacl.signing import SigningKey

from crimson.leaderboard.client import LEADERBOARD_URL
from crimson.leaderboard.identity import Identity
from crimson.replay.codec import inflate_replay_payload

ROOT = Path(__file__).resolve().parents[1]


def identity():
    env = ROOT / ".env"
    lines = env.read_text().splitlines() if env.exists() else []
    for line in lines:
        if line.startswith("CRIMSON_LAND_KEY="):
            return Identity(SigningKey(bytes.fromhex(line.split("=", 1)[1].strip())))
    key = SigningKey.generate()
    env.write_text("\n".join([*lines, f"CRIMSON_LAND_KEY={bytes(key).hex()}"]) + "\n")
    env.chmod(0o600)
    return Identity(key)


def call(endpoint, body):
    request = urllib.request.Request(f"{LEADERBOARD_URL}/{endpoint}", json.dumps(body).encode(), method="POST",
                                     headers={"content-type": "application/json", "user-agent": "crimson-rl"})
    with urllib.request.urlopen(request, timeout=300) as response:
        return response.status, json.loads(response.read())


def login(serve=None):
    if serve:
        host, port = serve.rsplit(":", 1)
        return HTTPServer((host, int(port)), Redirect).serve_forever()
    print(login_url())


class Redirect(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(303)
        self.send_header("Location", login_url())
        self.end_headers()


def login_url():
    me = identity()
    _, got = call("auth/challenge", {"public_key": me.public_key.hex()})
    _, got = call("auth/login", {"public_key": me.public_key.hex(), "challenge": got["challenge"],
                                 "signature": me.sign_login(got["challenge"]).hex()})
    return got["url"]


def main(path, name=None, *flags):
    if path == "login":
        return login(name)
    data = Path(path).read_bytes()
    payload = inflate_replay_payload(data)
    me = identity()
    envelope = {"replay": base64.b64encode(data).decode("ascii"), "name": name,
                "public_key": me.public_key.hex(), "signature": me.sign_run(payload, name).hex()}
    print(f"key {me.public_key.hex()} (fingerprint {me.fingerprint}), {len(data)} bytes as {name!r}")
    if "--dry-run" in flags:
        return
    try:
        print(*call("runs", envelope))
    except urllib.error.HTTPError as e:
        sys.exit(f"{e.code} {e.read().decode()}")


if __name__ == "__main__":
    main(*sys.argv[1:])
