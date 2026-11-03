#!/usr/bin/env python3
"""Prints the crashed thread of a macOS crash report (.ips) with function names.

The binary is taken from APP/symbols/<UUID>/GGTG (see tools/save_symbols.sh) or, when its UUID
matches, from the last local build. Usage: tools/symbolicate_crash.py ~/Library/Logs/DiagnosticReports/GGTG-*.ips
"""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "desktop/nicegram-desktop/out/Release/GGTG.app/Contents/MacOS/GGTG")


def uuid_of(path):
    try:
        out = subprocess.run(["dwarfdump", "--uuid", path], capture_output=True, text=True).stdout
        return out.split()[1].upper() if out else ""
    except Exception:
        return ""


def find_binary(uuid):
    saved = os.path.join(ROOT, "APP/symbols", uuid, "GGTG")
    if os.path.exists(saved):
        return saved
    if os.path.exists(BUILD) and uuid_of(BUILD) == uuid:
        return BUILD
    return None


def main(path):
    text = open(path, encoding="utf-8", errors="ignore").read()
    head, _, body = text.partition("\n")
    report = json.loads(body)
    images = report["usedImages"]
    ggtg = next(i for i in images if i.get("name") == "GGTG")
    uuid = ggtg["uuid"].upper()
    print(report.get("exception", {}), "UUID", uuid)
    binary = find_binary(uuid)
    if not binary:
        print("no binary with this UUID: run tools/save_symbols.sh for every release build")
        return 1
    thread = next((t for t in report["threads"] if t.get("triggered")), report["threads"][0])
    for i, frame in enumerate(thread["frames"][:40]):
        image = images[frame["imageIndex"]]
        if image.get("name") != "GGTG":
            print(f"{i:2} {image.get('name')} {frame.get('symbol', '')}")
            continue
        address = hex(ggtg["base"] + frame["imageOffset"])
        out = subprocess.run(
            ["atos", "-o", binary, "-arch", "arm64", "-l", hex(ggtg["base"]), address],
            capture_output=True, text=True).stdout.strip()
        print(f"{i:2} {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
