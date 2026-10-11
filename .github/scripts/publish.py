#!/usr/bin/env python3
"""Uploads firmware to the backend using the workflow's GitHub OIDC token (no stored secrets)."""
import argparse, json, os, sys, urllib.request, urllib.error

API = os.environ["API"]
AUDIENCE = "esp-box-assistant"


def oidc_token():
    url = os.environ["ACTIONS_ID_TOKEN_REQUEST_URL"] + "&audience=" + AUDIENCE
    req = urllib.request.Request(url, headers={"Authorization": "bearer " + os.environ["ACTIONS_ID_TOKEN_REQUEST_TOKEN"]})
    return json.load(urllib.request.urlopen(req, timeout=30))["value"]


def api(path, body, token):
    req = urllib.request.Request(API + path, data=json.dumps(body).encode(), method="POST",
                                 headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    try:
        return json.load(urllib.request.urlopen(req, timeout=60))
    except urllib.error.HTTPError as e:
        sys.exit(f"{path} failed: {e.code} {e.read().decode()[:500]}")


def upload(token, version, name, path):
    url = api("/ci/upload-url", {"version": version, "file": name}, token)["url"]
    data = open(path, "rb").read()
    req = urllib.request.Request(url, data=data, method="PUT",
                                 headers={"Content-Type": "application/octet-stream", "x-upsert": "true"})
    try:
        urllib.request.urlopen(req, timeout=300).read()
    except urllib.error.HTTPError as e:
        sys.exit(f"upload {name} failed: {e.code} {e.read().decode()[:500]}")
    print(f"uploaded {name} ({len(data)} bytes)")
    return len(data)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("cmd", choices=["release", "log"])
    p.add_argument("--version", required=True)
    p.add_argument("--ota"); p.add_argument("--factory")
    p.add_argument("--sha", default=""); p.add_argument("--notes", default="")
    p.add_argument("--status", default="success"); p.add_argument("--log")
    a = p.parse_args()
    token = oidc_token()
    run_id = os.environ.get("GITHUB_RUN_ID", "")

    if a.cmd == "log":
        text = open(a.log, errors="replace").read()[-20000:] if a.log and os.path.exists(a.log) else "(no build log)"
        api("/ci/log", {"run_id": run_id, "version": a.version, "status": a.status, "log": text}, token)
        return

    size = upload(token, a.version, "ota.bin", a.ota)
    upload(token, a.version, "factory.bin", a.factory)
    api("/ci/release", {"version": a.version, "git_sha": a.sha, "size": size, "notes": a.notes}, token)
    api("/ci/log", {"run_id": run_id, "version": a.version, "status": "success", "log": "published"}, token)
    print(f"released {a.version}")


if __name__ == "__main__":
    main()
