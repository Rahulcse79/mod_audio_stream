"""Minimal push-back audio test for mod_audio_stream.

What it does
------------
- Starts a WebSocket server (default: ws://0.0.0.0:8765)
- When FreeSWITCH/mod_audio_stream connects, sends 20 audio frames (1..20)
  as raw binary PCM16 mono @ 8kHz, 20ms per frame (320 bytes)
- Each frame is a short tone burst; frequency changes with the frame number.

This is intentionally hardcoded to match your current FreeSWITCH command:
  uuid_audio_stream <uuid> start ws://127.0.0.1:8765 mono 8000

Dependencies:
	pip install websockets

Optional WSS (TLS) server:
	Set PB_USE_WSS=1 and provide PEM files in the same folder:
		- PB_TLS_CERT: path to certificate PEM (default: ./server.crt.pem)
		- PB_TLS_KEY:  path to private key PEM (default: ./server.key.pem)

Note:
	The provided ./wss.pem in this repo is NOT a standard PEM file (it appears to
	be an OpenSSL text dump + embedded PEM blocks). Python's ssl module can't load
	that directly. If you want WSS, export proper cert/key PEMs.
"""

from __future__ import annotations

import asyncio
import math
import os
import struct
import time
from typing import Optional

import ssl

import websockets


HOST = os.getenv("PB_HOST", "0.0.0.0")
PORT = int(os.getenv("PB_PORT", "8765"))

# WSS options (off by default)
USE_WSS = os.getenv("PB_USE_WSS", "0") == "1"
TLS_CERT = os.getenv("PB_TLS_CERT", os.path.join(os.path.dirname(__file__), "server.crt.pem"))
TLS_KEY = os.getenv("PB_TLS_KEY", os.path.join(os.path.dirname(__file__), "server.key.pem"))

# Hardcoded audio contract
SAMPLE_RATE = 8000
CHANNELS = 1
FRAME_MS = 20
SAMPLES_PER_FRAME = int(SAMPLE_RATE * FRAME_MS / 1000)  # 160 samples
BYTES_PER_SAMPLE = 2  # PCM16
FRAME_BYTES = SAMPLES_PER_FRAME * CHANNELS * BYTES_PER_SAMPLE  # 320 bytes


def log(msg: str) -> None:

    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def pcm16_tone_frame(freq_hz: float, amp: float = 0.25) -> bytes:

    """Generate exactly one 20ms mono PCM16 frame at 8kHz."""

    out = bytearray()
    # prevent clipping
    amp = max(0.0, min(0.95, amp))
    for i in range(SAMPLES_PER_FRAME):
        v = amp * math.sin(2.0 * math.pi * freq_hz * (i / SAMPLE_RATE))
        s = int(max(-1.0, min(1.0, v)) * 32767)
        out += struct.pack("<h", s)
    return bytes(out)


async def handle_client(ws: websockets.WebSocketServerProtocol) -> None:

    peer = getattr(ws, "remote_address", None)
    log(f"FreeSWITCH connected: {peer}")

    # Some mod_audio_stream builds send an initial text "metadata" message.
    # We'll read a couple of incoming messages (non-blocking) to keep the socket happy,
    # but we don't require anything specific.

    async def _drain_initial(max_messages: int = 3, timeout_s: float = 0.15) -> None:
        for _ in range(max_messages):
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=timeout_s)
            except asyncio.TimeoutError:
                return
            except Exception:
                return
            if isinstance(msg, (bytes, bytearray)):
                log(f"WS IN (binary): {len(msg)} bytes")
            else:
                log(f"WS IN (text): {msg}")

    await _drain_initial()

    # Send 20 frames labeled 1..20.
    # Change tone frequency per frame so you can clearly hear it.

    log(f"Sending 20 push-back frames: pcm16/1ch/8000Hz/20ms ({FRAME_BYTES} bytes each)")
    total_sent = 0
    for n in range(1, 21):
        freq = 300.0 + (n * 30.0)  # 330Hz..900Hz-ish
        frame = pcm16_tone_frame(freq_hz=freq)
        if len(frame) != FRAME_BYTES:
            raise RuntimeError(f"BUG: frame_bytes={len(frame)} expected={FRAME_BYTES}")

        await ws.send(frame)
        total_sent += len(frame)
        log(
            f"WS OUT (binary): push_back frame {n}/20 sent ({FRAME_BYTES} bytes, {freq:.1f}Hz) "
            f"total_sent={total_sent}"
        )
        await asyncio.sleep(FRAME_MS / 1000.0)

    log(f"Push-back completed: frames=20 bytes={total_sent}. Keeping socket open.")

    # Keep connection alive so FreeSWITCH doesn't immediately tear down.

    try:
        async for msg in ws:
            if isinstance(msg, (bytes, bytearray)):
                log(f"WS IN (binary): {len(msg)} bytes")
            else:
                log(f"WS IN (text): {msg}")
    except websockets.ConnectionClosed:
        log("FreeSWITCH disconnected")


async def main() -> None:

    log("====================================")
    log("mod_audio_stream push-back test")
    scheme = "wss" if USE_WSS else "ws"
    log(f"Listening on {scheme}://{HOST}:{PORT}")
    log(
        f"Hardcoded audio: pcm16/{CHANNELS}ch/{SAMPLE_RATE}Hz/{FRAME_MS}ms "
        f"frame_bytes={FRAME_BYTES}"
    )
    if USE_WSS:
        log(f"TLS: cert={TLS_CERT}")
        log(f"TLS: key ={TLS_KEY}")
    log("====================================")

    ssl_ctx = None
    if USE_WSS:
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(certfile=TLS_CERT, keyfile=TLS_KEY)

    async with websockets.serve(handle_client, HOST, PORT, max_size=None, ssl=ssl_ctx):
        await asyncio.Future()


if __name__ == "__main__":

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("Shutting down (Ctrl+C)")

