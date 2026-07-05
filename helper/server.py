#!/usr/bin/env python3
"""Reference helper server for the mini-assistant (Phase 8).

Protocol (v1, matches Core/Src/assistant_client.c):
  - WebSocket on ws://0.0.0.0:8765 (any path; the firmware uses /utterance)
  - client -> server: ONE binary message per utterance
                      (raw PCM, int16 little-endian, 16 kHz, mono)
  - server -> client: ONE text message = the assistant's reply (ASCII, short)

Pipeline: faster-whisper ASR -> optional OpenAI-compatible chat endpoint.
Modes:
  --echo            no ASR/LLM; replies with a canned string (transport test)
  (default)         ASR only; replies "You said: <transcript>" unless an LLM
                    endpoint is configured via environment variables
LLM environment variables (OpenAI-compatible /v1/chat/completions):
  OPENAI_BASE_URL   e.g. http://localhost:11434/v1  (Ollama) or an API base
  OPENAI_API_KEY    key if the endpoint needs one (optional for local)
  OPENAI_MODEL      model name (default: llama3.2)

SECURITY: v1 is plain ws:// with no authentication — run it ONLY on a trusted
LAN. Do not expose this port to the internet (see README.md).
"""

import argparse
import asyncio
import logging
import os
import time

import websockets

log = logging.getLogger("helper")

SAMPLE_RATE = 16000
REPLY_MAX_CHARS = 480  # firmware truncates at 511; keep replies comfortably short

SYSTEM_PROMPT = (
    "You are a compact voice assistant running through a tiny embedded display. "
    "Answer in one or two short sentences, at most 240 characters, plain ASCII, "
    "no markdown."
)


class Pipeline:
    """Owns the (lazily constructed) ASR model and the optional LLM client."""

    def __init__(self, args):
        self.echo = args.echo
        self.model = None
        if not self.echo:
            from faster_whisper import WhisperModel  # import here so --echo needs no deps

            log.info("loading faster-whisper model %r ...", args.whisper_model)
            self.model = WhisperModel(
                args.whisper_model, device=args.device, compute_type="int8"
            )
            log.info("ASR ready")

        self.chat_url = os.environ.get("OPENAI_BASE_URL", "").rstrip("/")
        self.chat_key = os.environ.get("OPENAI_API_KEY", "")
        self.chat_model = os.environ.get("OPENAI_MODEL", "llama3.2")
        if self.chat_url:
            log.info("LLM endpoint: %s (model %s)", self.chat_url, self.chat_model)
        elif not self.echo:
            log.info("no OPENAI_BASE_URL set - will reply with the transcript only")

    def transcribe(self, pcm: bytes) -> str:
        import numpy as np

        audio = np.frombuffer(pcm, dtype="<i2").astype("float32") / 32768.0
        segments, _info = self.model.transcribe(audio, language="en", beam_size=1)
        return " ".join(seg.text.strip() for seg in segments).strip()

    def chat(self, prompt: str) -> str:
        import requests

        headers = {"Content-Type": "application/json"}
        if self.chat_key:
            headers["Authorization"] = f"Bearer {self.chat_key}"
        resp = requests.post(
            f"{self.chat_url}/chat/completions",
            headers=headers,
            json={
                "model": self.chat_model,
                "messages": [
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": prompt},
                ],
                "max_tokens": 120,
            },
            timeout=15,
        )
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"].strip()

    def handle(self, pcm: bytes) -> str:
        """Blocking worker (runs in an executor thread): audio bytes -> reply."""
        if self.echo:
            secs = len(pcm) / 2 / SAMPLE_RATE
            return f"Echo: received {secs:.1f}s of audio ({len(pcm)} bytes)."

        t0 = time.time()
        transcript = self.transcribe(pcm)
        t_asr = time.time() - t0
        log.info("ASR %.2fs: %r", t_asr, transcript)
        if not transcript:
            return "I didn't catch that."

        if not self.chat_url:
            return f"You said: {transcript}"

        t0 = time.time()
        try:
            reply = self.chat(transcript)
        except Exception as exc:  # endpoint down/misconfigured: still answer
            log.warning("LLM call failed: %s", exc)
            return f"(LLM unavailable) You said: {transcript}"
        log.info("LLM %.2fs: %r", time.time() - t0, reply)
        return reply


def sanitize(reply: str) -> str:
    """The board renders ASCII 0x20..0x7E only; keep the reply well-formed."""
    reply = reply.encode("ascii", "replace").decode("ascii")
    reply = " ".join(reply.split())  # collapse newlines/whitespace runs
    return reply[:REPLY_MAX_CHARS] if reply else "(empty reply)"


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--echo", action="store_true",
                    help="transport test mode: no ASR/LLM, canned reply")
    ap.add_argument("--whisper-model", default="tiny.en",
                    help="faster-whisper model (tiny.en/base.en/small.en...)")
    ap.add_argument("--device", default="cpu", help="ASR device (cpu/cuda)")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    pipeline = Pipeline(args)
    loop = asyncio.get_running_loop()

    async def handler(ws):
        peer = ws.remote_address
        log.info("client connected: %s", peer)
        try:
            async for msg in ws:
                if not isinstance(msg, bytes):
                    continue  # firmware only sends binary; ignore stray text
                t0 = time.time()
                reply = sanitize(
                    await loop.run_in_executor(None, pipeline.handle, msg)
                )
                await ws.send(reply)
                log.info("round-trip %.2fs -> %r", time.time() - t0, reply)
        except websockets.ConnectionClosed:
            pass
        finally:
            log.info("client disconnected: %s", peer)

    # max_size: an 8 s utterance is 256 KB; 1 MiB leaves ample headroom.
    async with websockets.serve(handler, "0.0.0.0", args.port,
                                max_size=1024 * 1024):
        log.info("listening on ws://0.0.0.0:%d  (%s mode)",
                 args.port, "echo" if args.echo else "ASR")
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
