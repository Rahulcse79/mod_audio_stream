#!/usr/bin/env python3
"""WebSocket test peer for mod_audio_stream bidirectional audio.

What it does:
- Accepts a WS connection from mod_audio_stream.
- Logs inbound binary audio from FreeSWITCH (outbound capture path).
- Sends 20ms PCM16 @ 8kHz mono frames back to FreeSWITCH (push-back path).
    - mode=sine: deterministic tone
    - mode=random: random PCM16 bytes (good for plumbing validation)
- Optional echo mode: echoes inbound binary frames back to the client (helps validate WS full duplex).

This does NOT prove FreeSWITCH injected the audio (you must check FS logs for
"first WRITE tick" and pushback counters), but it ensures we consistently send
properly-sized PCM frames.

Requires:
  pip install websockets

Run:
    python3 tools/pushback_ws_server.py --host 127.0.0.1 --port 8765 --seconds 0 --mode sine
    python3 tools/pushback_ws_server.py --host 127.0.0.1 --port 8765 --seconds 0 --mode random
    python3 tools/pushback_ws_server.py --host 127.0.0.1 --port 8765 --seconds 0 --mode sine --echo
"""

import argparse
import asyncio
import math
import os
import struct
import sys
import time
from typing import Optional

import websockets


FRAME_BYTES_8K_MONO_20MS = 320  # 160 samples * 2 bytes (PCM16LE)


def make_sine_pcm16_8k_20ms(freq_hz: float, amp: float, phase: float = 0.0):
    """Return (bytes, new_phase) for 20ms of PCM16LE @ 8kHz mono."""
    rate = 8000
    n = rate // 50  # 160 samples
    out = bytearray(n * 2)
    for i in range(n):
        t = (phase + i) / rate
        s = math.sin(2.0 * math.pi * freq_hz * t)
        v = int(max(-1.0, min(1.0, s * amp)) * 32767)
        struct.pack_into('<h', out, i * 2, v)
    return bytes(out), phase + n


def make_random_pcm16_8k_20ms() -> bytes:
    return os.urandom(FRAME_BYTES_8K_MONO_20MS)


async def handler(ws):
    peer = getattr(ws, 'remote_address', None)
    print(f"client connected: {peer}", flush=True)

    start = time.time()
    last_rx_log = start
    rx_bytes = 0
    rx_msgs = 0

    # Push-back tone parameters
    phase = 0.0
    hz = 440.0
    amp = 0.25

    # We'll run two tasks:
    # 1) receive and count inbound audio
    # 2) send PCM frames every 20ms

    async def rx_task():
        nonlocal rx_bytes, rx_msgs, last_rx_log
        try:
            async for msg in ws:
                if isinstance(msg, (bytes, bytearray)):
                    rx_msgs += 1
                    rx_bytes += len(msg)
                    now = time.time()
                    if now - last_rx_log >= 1.0:
                        print(f"rx: msgs={rx_msgs} bytes={rx_bytes}")
                        last_rx_log = now

                    if handler.echo:
                        await ws.send(bytes(msg))
                else:
                    # text
                    print(f"rx text: {msg}")
        except Exception as e:
            print(f"rx ended: {type(e).__name__}: {e}")

    async def tx_task(seconds: Optional[float]):
        nonlocal phase
        tx_msgs = 0
        tx_bytes = 0
        last_tx_log = start
        try:
            while True:
                if seconds is not None and time.time() - start >= seconds:
                    print("tx: done, closing")
                    await ws.close()
                    return

                if handler.mode == 'sine':
                    frame, phase = make_sine_pcm16_8k_20ms(hz, amp, phase)
                else:
                    frame = make_random_pcm16_8k_20ms()

                # Contract: 20ms @ 8k mono => 320 bytes
                if len(frame) != FRAME_BYTES_8K_MONO_20MS:
                    raise RuntimeError(f"bad frame size: {len(frame)}")
                await ws.send(frame)
                tx_msgs += 1
                tx_bytes += len(frame)

                now = time.time()
                if now - last_tx_log >= 1.0:
                    print(f"tx: msgs={tx_msgs} bytes={tx_bytes} mode={handler.mode} echo={handler.echo}")
                    last_tx_log = now
                await asyncio.sleep(0.02)
        except Exception as e:
            print(f"tx ended: {type(e).__name__}: {e}")

    await asyncio.gather(rx_task(), tx_task(handler.seconds))


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--seconds', type=float, default=10.0,
                        help='How long to push audio before closing. Use 0 to never close.')
    parser.add_argument('--mode', choices=['sine', 'random'], default='sine',
                        help='What to push back: sine tone or random PCM bytes')
    parser.add_argument('--echo', action='store_true',
                        help='Echo inbound binary back to the client (WS bidi sanity check)')
    args = parser.parse_args()

    handler.seconds = None if args.seconds == 0 else float(args.seconds)
    handler.mode = args.mode
    handler.echo = bool(args.echo)

    # Force line-buffered output so logs show up even when redirected to a file.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass

    print(f"listening on ws://{args.host}:{args.port} mode={handler.mode} echo={handler.echo}", flush=True)
    async with websockets.serve(handler, args.host, args.port, max_size=None):
        await asyncio.Future()  # run forever


if __name__ == '__main__':
    asyncio.run(main())
