# mini-assistant helper server

Reference "big brain" for the mini-assistant board (Phase 8). The STM32 streams
each captured voice command here over the LAN; this server transcribes it,
optionally asks an LLM, and sends a short text reply back for the board's LCD.

```
board mic ──► wake word ──► VAD capture ──► WebSocket (binary PCM) ──► this server
board LCD ◄──────────────────────────────── WebSocket (text reply) ◄── ASR → LLM
```

## ⚠️ Security — read this first

**v1 is plain `ws://` with no TLS and no authentication.** Anyone who can reach
the port can send audio and read replies. Run it **only on a trusted LAN**.
Never port-forward it or bind it to a public interface. If you ever need remote
access, put a TLS-terminating reverse proxy with auth in front of it (WSS
support on the firmware side is future work).

## Protocol (v1)

| Direction | Content |
|---|---|
| board → server | **one binary WebSocket message per utterance**: raw PCM, int16 little-endian, 16 kHz, mono (≤ 8 s ≈ 256 KB) |
| server → board | **one text message**: the reply, plain ASCII, ideally ≤ 240 chars (the board truncates at ~275 rendered chars) |

The board keeps the connection open across utterances and reconnects once if it
drops. Path is `/utterance` but the server accepts any path.

## Quick start (echo mode — no ML dependencies)

Verify the transport end-to-end before installing anything heavy:

```bash
pip install websockets
python server.py --echo
```

Set the server machine's IP in the firmware — `ASSISTANT_HELPER_IP` in
[Core/Inc/assistant_client.h](../Core/Inc/assistant_client.h) — rebuild, flash,
then say "Hey Jarvis" + anything. The board should show
`Echo: received 2.3s of audio (...)` a moment later.

## Full mode (ASR, optionally + LLM)

```bash
pip install -r requirements.txt
python server.py                     # ASR only → replies "You said: ..."
python server.py --whisper-model base.en   # better accuracy, slower
```

First run downloads the whisper model (~75 MB for tiny.en).

To relay transcripts to an LLM, point it at any OpenAI-compatible
`/v1/chat/completions` endpoint:

```bash
# Local Ollama:
export OPENAI_BASE_URL=http://localhost:11434/v1
export OPENAI_MODEL=llama3.2
python server.py

# Hosted API:
export OPENAI_BASE_URL=https://api.example.com/v1
export OPENAI_API_KEY=sk-...
export OPENAI_MODEL=some-model
python server.py
```

The system prompt asks for one/two short ASCII sentences so replies fit the LCD.

## Latency budget (v1 targets)

| Stage | Typical |
|---|---|
| Upload (≤ 8 s of PCM over wired LAN) | 50–500 ms |
| ASR (`tiny.en`, CPU) | 0.5–1.5 s |
| LLM (local Llama / hosted) | 0.5–3 s |
| Reply download | negligible |
| **Mic-stop → text on LCD** | **2–4 s** (≈ 1–2 s without LLM) |

The server logs per-stage timings; use them to decide what to attack first.

## Troubleshooting

- **Board shows "Helper error"**: server not running / wrong
  `ASSISTANT_HELPER_IP` / firewall blocking port 8765 (allow inbound TCP 8765).
- **Board shows "No network"**: board has no DHCP lease — check cable and the
  `Net:` line on the Assist tab.
- **Replies cut off with `...`**: reply exceeded the LCD area; shorten via the
  system prompt or a smaller `max_tokens`.
- **Slow ASR**: try `--device cuda` (needs CUDA + cuDNN), or keep `tiny.en`.
- **Test without the board**: any WebSocket client works — send one binary
  message of PCM and print the text reply:

  ```python
  import asyncio, websockets

  async def test():
      async with websockets.connect("ws://localhost:8765/utterance") as ws:
          await ws.send(b"\x00\x00" * 16000)   # 1 s of silence
          print(await ws.recv())

  asyncio.run(test())
  ```
