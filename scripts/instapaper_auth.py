#!/usr/bin/env python3
"""
Generate CrossPoint's Instapaper tokens file.

Exchanges your Instapaper username and password for an OAuth 1.0a access
token pair using the xAuth flow, then writes the four tokens to a file
that CrossPoint reads from the SD card at startup.

Usage:
    python3 scripts/instapaper_auth.py \\
        --consumer-key  CK \\
        --consumer-secret CS \\
        --username USER \\
        --password PASS \\
        --output instapaper_tokens.txt

You need an Instapaper OAuth consumer key/secret. Apply for one at:
    https://www.instapaper.com/developers/applications/create

Copy the resulting file to your SD card at:
    /.crosspoint/instapaper_tokens.txt
"""
import argparse
import base64
import getpass
import hashlib
import hmac
import random
import string
import sys
import time
import urllib.parse
import urllib.request


def percent_encode(s: str) -> str:
    return urllib.parse.quote(s, safe="-_.~")


def sign(method: str, url: str, params: dict, consumer_secret: str, token_secret: str = "") -> str:
    encoded = sorted(
        (percent_encode(k), percent_encode(str(v))) for k, v in params.items()
    )
    param_str = "&".join(f"{k}={v}" for k, v in encoded)
    base = f"{method}&{percent_encode(url)}&{percent_encode(param_str)}"
    key = f"{percent_encode(consumer_secret)}&{percent_encode(token_secret)}"
    digest = hmac.new(key.encode(), base.encode(), hashlib.sha1).digest()
    return base64.b64encode(digest).decode()


def build_auth_header(params: dict) -> str:
    parts = [f'{k}="{percent_encode(str(v))}"' for k, v in sorted(params.items())]
    return "OAuth " + ", ".join(parts)


def xauth_exchange(consumer_key: str, consumer_secret: str, username: str, password: str) -> tuple[str, str]:
    url = "https://www.instapaper.com/api/1/oauth/access_token"
    oauth_params = {
        "oauth_consumer_key": consumer_key,
        "oauth_nonce": "".join(random.choices(string.ascii_letters + string.digits, k=32)),
        "oauth_signature_method": "HMAC-SHA1",
        "oauth_timestamp": str(int(time.time())),
        "oauth_version": "1.0",
    }
    body_params = {
        "x_auth_username": username,
        "x_auth_password": password,
        "x_auth_mode": "client_auth",
    }
    all_params = {**oauth_params, **body_params}
    signature = sign("POST", url, all_params, consumer_secret)
    auth_header_params = {**oauth_params, "oauth_signature": signature}

    body = urllib.parse.urlencode(body_params).encode()
    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Authorization": build_auth_header(auth_header_params),
            "Content-Type": "application/x-www-form-urlencoded",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        response_body = resp.read().decode()

    parsed = urllib.parse.parse_qs(response_body)
    token = parsed.get("oauth_token", [None])[0]
    token_secret = parsed.get("oauth_token_secret", [None])[0]
    if not token or not token_secret:
        raise RuntimeError(f"Unexpected response: {response_body!r}")
    return token, token_secret


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--consumer-key", required=True)
    parser.add_argument("--consumer-secret", required=True)
    parser.add_argument("--username", required=True)
    parser.add_argument("--password", help="Prompted if omitted")
    parser.add_argument("--output", default="instapaper_tokens.txt")
    args = parser.parse_args()

    password = args.password or getpass.getpass("Instapaper password: ")

    try:
        token, token_secret = xauth_exchange(
            args.consumer_key, args.consumer_secret, args.username, password
        )
    except Exception as e:
        print(f"Token exchange failed: {e}", file=sys.stderr)
        return 1

    with open(args.output, "w", encoding="utf-8") as f:
        f.write(f"consumer_key={args.consumer_key}\n")
        f.write(f"consumer_secret={args.consumer_secret}\n")
        f.write(f"oauth_token={token}\n")
        f.write(f"oauth_token_secret={token_secret}\n")

    print(f"Wrote {args.output}.")
    print("Copy it to your SD card at /.crosspoint/instapaper_tokens.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
