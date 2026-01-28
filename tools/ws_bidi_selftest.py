#!/usr/bin/env python3
"""Self-test for tools/pushback_ws_server.py.

Goal: validate WS bidirectional binary handling WITHOUT FreeSWITCH.

This script:
- connects to the server
- sends random PCM16 frames (320 bytes)
- optionally expects echo (server started with --echo)

Usage:
  python3 tools/ws_bidi_selftest.py --url ws://127.0.0.1:8765 --frames 50 --expect-echo
"""

import argparse
import asyncio
import os

import websockets

FRAME_BYTES = 320


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='ws://127.0.0.1:8765')
    ap.add_argument('--frames', type=int, default=50)
    ap.add_argument('--expect-echo', action='store_true')
    args = ap.parse_args()

    sent = 0
    recv = 0

    async with websockets.connect(args.url, max_size=None) as ws:
        for _ in range(args.frames):
            payload = os.urandom(FRAME_BYTES)
            await ws.send(payload)
            sent += 1

            if args.expect_echo:
                msg = await ws.recv()
                if not isinstance(msg, (bytes, bytearray)):
                    raise RuntimeError(f"expected binary echo, got: {type(msg)}")
                if len(msg) != FRAME_BYTES:
                    raise RuntimeError(f"expected {FRAME_BYTES} bytes echo, got {len(msg)}")
                recv += 1

        # small delay to allow server tx_task logs
        await asyncio.sleep(0.1)

    print("=== ws bidi selftest ===")
    print(f"sent_frames={sent}")
    if args.expect_echo:
        print(f"echo_frames={recv}")
        if recv != sent:
            raise SystemExit("FAIL: echo count mismatch")
    print("PASS")


if __name__ == '__main__':
    asyncio.run(main())
