#!/usr/bin/env python3
"""Reference helper server for the mini-assistant (Phase 8 transport, Phase 9 brains).

Protocol (v1, matches Core/Src/assistant_client.c):
  - WebSocket on ws://0.0.0.0:8765 (any path; the firmware uses /utterance)
  - client -> server: ONE binary message per utterance
                      (raw PCM, int16 little-endian, 16 kHz, mono)
  - server -> client: ONE text message = the assistant's reply (ASCII, short)

Pipeline: faster-whisper ASR (CUDA with CPU fallback) -> local LLM via any
OpenAI-compatible /v1/chat/completions endpoint (default: Ollama on localhost).

Modes:
  --echo            no ASR/LLM; replies with a canned string (transport test)
  --no-llm          ASR only; replies "You said: <transcript>"
  (default)         ASR -> LLM with short conversation memory

LLM configuration (CLI flags override the environment variables):
  --llm-url    / OPENAI_BASE_URL   default http://localhost:11434/v1  (Ollama)
  --llm-model  / OPENAI_MODEL      default llama3.2:3b
  --llm-key    / OPENAI_API_KEY    optional (local endpoints don't need one)

SECURITY: v1 is plain ws:// with no authentication — run it ONLY on a trusted
LAN. Do not expose this port to the internet (see README.md).
"""

import argparse
import asyncio
import logging
import os
import sys
import time
from collections import deque

import websockets

log = logging.getLogger("helper")

SAMPLE_RATE = 16000
REPLY_MAX_CHARS = 480  # firmware truncates at 511; keep replies comfortably short

SYSTEM_PROMPT = (
    "You are a compact voice assistant running through a tiny embedded display. "
    "Answer in one or two short sentences, at most 240 characters, plain ASCII, "
    "no markdown."
)

# Conversation memory: last N user/assistant exchanges, dropped after an idle
# gap so a stale context can't bleed into an unrelated question hours later.
MEMORY_MAX_EXCHANGES = 6
MEMORY_IDLE_RESET_S = 300.0

DEFAULT_LLM_URL = "http://localhost:11434/v1"   # Ollama's OpenAI-compatible API
DEFAULT_LLM_MODEL = "llama3.2:3b"

# Keep the LLM resident. Ollama unloads an idle model after ~5 min, and the
# reload measured 44 s on this machine — far beyond the board's 20 s response
# timeout, so the first query after a break would fail with "Helper err s8".
# A tiny generation every WARM_INTERVAL_S (< the unload window) prevents it.
WARM_INTERVAL_S = 240.0


def add_cuda_dll_dirs():
    """Windows: ctranslate2's CUDA build loads cublas64_12 / cudnn64_9 at
    runtime. The pip packages nvidia-cublas-cu12 / nvidia-cudnn-cu12 ship them
    under site-packages/nvidia/<lib>/bin but do NOT put them on PATH. (The
    system CUDA 13 toolkit doesn't help: ctranslate2 4.8 links the
    CUDA-12-series DLL names.)

    Both registrations below are load-bearing: os.add_dll_directory() covers
    import-time dependency resolution, but ctranslate2 resolves cuBLAS lazily
    via a plain LoadLibrary(), which searches PATH — verified on this machine
    that add_dll_directory alone still fails with "cublas64_12.dll is not
    found or cannot be loaded". cuda_nvrtc rides along as a cuBLAS dependency."""
    if sys.platform != "win32":
        return
    import importlib.util
    import pathlib

    spec = importlib.util.find_spec("nvidia")
    if spec is None or not spec.submodule_search_locations:
        return
    for base in spec.submodule_search_locations:
        for sub in ("cublas", "cudnn", "cuda_nvrtc"):
            d = pathlib.Path(base) / sub / "bin"
            if d.is_dir():
                os.add_dll_directory(str(d))
                os.environ["PATH"] = str(d) + os.pathsep + os.environ.get("PATH", "")
                log.debug("registered CUDA DLL dir: %s", d)


class Pipeline:
    """Owns the ASR model, the LLM client config, and the conversation memory."""

    def __init__(self, args):
        self.echo = args.echo
        self.model = None
        self.asr_device = None
        if not self.echo:
            self._init_asr(args)

        self.chat_url = (args.llm_url or
                         os.environ.get("OPENAI_BASE_URL", DEFAULT_LLM_URL)).rstrip("/")
        self.chat_key = args.llm_key or os.environ.get("OPENAI_API_KEY", "")
        self.chat_model = args.llm_model or os.environ.get("OPENAI_MODEL", DEFAULT_LLM_MODEL)
        self.max_tokens = args.max_tokens
        if args.no_llm:
            self.chat_url = ""

        self.memory_enabled = not args.no_memory
        # Alternating user/assistant messages; even maxlen keeps pairs aligned.
        self.history = deque(maxlen=2 * MEMORY_MAX_EXCHANGES)
        self.last_used = 0.0

        if self.echo:
            log.info("echo mode - no ASR/LLM")
        elif self.chat_url:
            log.info("LLM endpoint: %s (model %s, max_tokens %d, memory %s)",
                     self.chat_url, self.chat_model, self.max_tokens,
                     "on" if self.memory_enabled else "off")
        else:
            log.info("no LLM (--no-llm) - replying with the transcript only")

    def _init_asr(self, args):
        """Load faster-whisper. --device auto tries CUDA (float16) and falls
        back to CPU (int8); an explicit --device cuda/cpu is honoured strictly.
        A short warm-up transcribe validates that CUDA kernels actually run —
        model construction alone can succeed and still die at first use."""
        add_cuda_dll_dirs()
        import numpy as np
        from faster_whisper import WhisperModel

        if args.device == "auto":
            attempts = [("cuda", "float16"), ("cpu", "int8")]
        else:
            attempts = [(args.device,
                         "float16" if args.device == "cuda" else "int8")]

        last_exc = None
        for device, ctype in attempts:
            try:
                log.info("loading faster-whisper %r on %s (%s) ...",
                         args.whisper_model, device, ctype)
                model = WhisperModel(args.whisper_model, device=device,
                                     compute_type=ctype)
                segments, _ = model.transcribe(
                    np.zeros(SAMPLE_RATE // 2, dtype="float32"),
                    language="en", beam_size=1)
                list(segments)  # force execution = real kernel validation
                self.model = model
                self.asr_device = device
                log.info("ASR ready (%s, %s)", device, args.whisper_model)
                return
            except Exception as exc:
                last_exc = exc
                log.warning("ASR init on %s failed: %s", device, exc)
        raise RuntimeError(f"could not initialise ASR: {last_exc}")

    def transcribe(self, pcm: bytes) -> str:
        import numpy as np

        audio = np.frombuffer(pcm, dtype="<i2").astype("float32") / 32768.0
        segments, _info = self.model.transcribe(audio, language="en", beam_size=1)
        return " ".join(seg.text.strip() for seg in segments).strip()

    def chat(self, prompt: str) -> str:
        import requests

        now = time.time()
        if (self.memory_enabled and self.history and
                (now - self.last_used) > MEMORY_IDLE_RESET_S):
            log.info("conversation memory reset (idle %.0f s)", now - self.last_used)
            self.history.clear()

        messages = [{"role": "system", "content": SYSTEM_PROMPT}]
        messages.extend(self.history)
        messages.append({"role": "user", "content": prompt})

        headers = {"Content-Type": "application/json"}
        if self.chat_key:
            headers["Authorization"] = f"Bearer {self.chat_key}"
        resp = requests.post(
            f"{self.chat_url}/chat/completions",
            headers=headers,
            json={
                "model": self.chat_model,
                "messages": messages,
                "max_tokens": self.max_tokens,
            },
            timeout=15,
        )
        resp.raise_for_status()
        reply = resp.json()["choices"][0]["message"]["content"].strip()

        if self.memory_enabled:
            self.history.append({"role": "user", "content": prompt})
            self.history.append({"role": "assistant", "content": reply})
        self.last_used = now
        return reply

    def warm(self) -> None:
        """Tiny generation that forces the model into VRAM (and keeps it there).
        Deliberately bypasses chat() so it never touches conversation memory."""
        import requests

        headers = {"Content-Type": "application/json"}
        if self.chat_key:
            headers["Authorization"] = f"Bearer {self.chat_key}"
        requests.post(
            f"{self.chat_url}/chat/completions",
            headers=headers,
            json={
                "model": self.chat_model,
                "messages": [{"role": "user", "content": "ok"}],
                "max_tokens": 1,
            },
            timeout=120,  # generous: covers the initial cold load
        ).raise_for_status()

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
    ap.add_argument("--whisper-model", default="base.en",
                    help="faster-whisper model (tiny.en/base.en/small.en...)")
    ap.add_argument("--device", default="auto",
                    help="ASR device: auto (CUDA, CPU fallback), cuda, cpu")
    ap.add_argument("--llm-url", default=None,
                    help=f"OpenAI-compatible base URL (default {DEFAULT_LLM_URL}; "
                         "overrides OPENAI_BASE_URL)")
    ap.add_argument("--llm-model", default=None,
                    help=f"model name (default {DEFAULT_LLM_MODEL}; overrides OPENAI_MODEL)")
    ap.add_argument("--llm-key", default=None,
                    help="API key if the endpoint needs one (overrides OPENAI_API_KEY)")
    ap.add_argument("--max-tokens", type=int, default=120,
                    help="LLM generation cap (short answers fit the LCD)")
    ap.add_argument("--no-llm", action="store_true",
                    help="skip the LLM; reply with the transcript")
    ap.add_argument("--no-memory", action="store_true",
                    help="disable conversation memory between queries")
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

    async def keep_warm():
        """Load the model at startup, then ping it forever so it stays
        resident (see WARM_INTERVAL_S). Failures are logged, never fatal."""
        first = True
        while True:
            try:
                t0 = time.time()
                await loop.run_in_executor(None, pipeline.warm)
                if first:
                    log.info("LLM warm (%s loaded in %.1fs)",
                             pipeline.chat_model, time.time() - t0)
                    first = False
            except Exception as exc:
                log.warning("LLM keep-warm failed: %s", exc)
            await asyncio.sleep(WARM_INTERVAL_S)

    if not pipeline.echo and pipeline.chat_url:
        asyncio.create_task(keep_warm())

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
