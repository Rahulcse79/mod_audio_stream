#!/usr/bin/env python3
"""Lightweight log verifier for mod_audio_stream push-back.

Reads a FreeSWITCH log file and prints PASS/FAIL signals:
- Did WRITE callback execute? ("first WRITE tick")
- Did we receive inbound WS bytes? (pushback.totals and/or recv counters)
- Did we inject bytes? (inject counter)
- Are underruns excessive?

Usage:
  python3 tools/verify_pushback_logs.py --log /usr/local/freeswitch/var/log/freeswitch/freeswitch.log --since "2026-01-28 12:34:00"

Notes:
- This is heuristic. It won’t be perfect, but it makes debugging faster.
"""

import argparse
import re
from datetime import datetime


def parse_since(s: str):
    # Expected FS log prefix: YYYY-MM-DD HH:MM:SS.mmmmmm
    # We allow seconds precision in input.
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', required=True)
    ap.add_argument('--since', required=False, help='Only consider log lines >= this time (YYYY-MM-DD HH:MM:SS)')
    args = ap.parse_args()

    since_dt = parse_since(args.since) if args.since else None

    first_write = False
    ws_dropped = 0

    recv_bytes = None
    inject_bytes = None
    underruns = None

    # Example:
    # mod_audio_stream pushback: ... recv=123 inject=456 underruns=7
    re_pushback = re.compile(r"mod_audio_stream pushback: .* recv=(\d+) inject=(\d+) underruns=(\d+)")

    with open(args.log, 'r', errors='ignore') as f:
        for line in f:
            if since_dt:
                m = re.match(r"^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\.", line)
                if m:
                    dt = datetime.strptime(m.group(1) + " " + m.group(2), "%Y-%m-%d %H:%M:%S")
                    if dt < since_dt:
                        continue

            if "mod_audio_stream pushback: first WRITE tick" in line:
                first_write = True

            if "MOD_AUDIO_STREAM_V2 ws dropped payload=" in line:
                ws_dropped += 1

            m2 = re_pushback.search(line)
            if m2:
                recv_bytes = int(m2.group(1))
                inject_bytes = int(m2.group(2))
                underruns = int(m2.group(3))

    print("=== pushback verification ===")
    print(f"WRITE callback seen: {'YES' if first_write else 'NO'}")
    if ws_dropped:
        print(f"WS drops seen: {ws_dropped}")

    if recv_bytes is not None:
        print(f"recv_bytes: {recv_bytes}")
        print(f"inject_bytes: {inject_bytes}")
        print(f"underruns: {underruns}")

    # Verdict
    ok = True
    if not first_write:
        ok = False
        print("FAIL: WRITE callback not observed (no injection possible).")

    if recv_bytes is None:
        ok = False
        print("FAIL: no periodic pushback counters found (module may not be running updated binary or logging not reached).")
    else:
        if recv_bytes == 0:
            ok = False
            print("FAIL: recv_bytes=0 (WS server not sending or inbound buffering not happening).")
        if inject_bytes == 0:
            ok = False
            print("FAIL: inject_bytes=0 (WRITE callback ran but no audio injected).")

        # Heuristic threshold: underruns should be small compared to time.
        if underruns is not None and underruns > 200:
            print("WARN: underruns is high; likely WS timing/jitter buffer too small.")

    if ok:
        print("PASS: pushback path looks active (WRITE callback + recv + inject observed).")


if __name__ == '__main__':
    main()
