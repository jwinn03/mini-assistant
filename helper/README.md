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

## Protocol (v2)

| Direction | Content |
|---|---|
| board → server | **one binary WebSocket message per utterance**: raw PCM, int16 little-endian, 16 kHz, mono (≤ 8 s ≈ 256 KB) |
| server → board | optionally **one text message `Q: <transcript>`** (the ASR result, sent as soon as transcription finishes — the board shows it in blue while the LLM generates), then **one text message** (the reply, plain ASCII, ideally ≤ 240 chars), then the spoken reply as **binary messages** (~8 KB chunks, 16 kHz mono int16 LE), then a **zero-length binary message = end-of-speech**. The EOS is always sent, even with TTS disabled. |

Text is sent first so the board's display updates while the speech synthesizes
and streams. The board keeps the connection open across utterances and
reconnects once if it drops. Path is `/utterance` but the server accepts any
path.

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

## Full mode (Phase 9: GPU ASR + local LLM)

One-time setup:

```bash
pip install -r requirements.txt      # includes CUDA runtime DLLs for the GPU
winget install Ollama.Ollama         # or the installer from ollama.com
ollama pull llama3.2:3b              # ~2 GB; any small instruct model works
```

Then just:

```bash
python server.py
```

Defaults do the right thing on a CUDA machine: faster-whisper `base.en` on the
GPU (`--device auto` falls back to CPU if CUDA fails), and transcripts are
relayed to the local Ollama endpoint (`http://localhost:11434/v1`, model
`llama3.2:3b`). The system prompt asks for one/two short ASCII sentences so
replies fit the LCD.

Useful variants:

```bash
python server.py --no-llm                      # ASR only → "You said: ..."
python server.py --device cpu                  # force CPU ASR
python server.py --whisper-model small.en      # better transcripts, still fast on GPU
python server.py --llm-model qwen2.5:3b-instruct
python server.py --llm-url https://api.example.com/v1 --llm-key sk-... \
                 --llm-model some-model        # any OpenAI-compatible endpoint
python server.py --no-memory                   # disable conversation context
```

`OPENAI_BASE_URL` / `OPENAI_MODEL` / `OPENAI_API_KEY` env vars still work; CLI
flags take precedence.

Conversation memory keeps the last 6 exchanges so follow-ups like "what about
at sunset?" work; it resets after 5 minutes of silence.

**GPU notes (RTX 5070 / Blackwell)**: ctranslate2 ≥ 4.6 ships sm_120 CUDA
kernels, so the 5070 works with current wheels. The `nvidia-cublas-cu12` /
`nvidia-cudnn-cu12` pip packages provide the CUDA-12-series DLLs ctranslate2
loads — needed even if a CUDA 13 toolkit is installed system-wide (different
DLL names).

**Cold-start**: loading llama3.2:3b into VRAM measured **44 s** on this machine
— far beyond the board's 20 s response timeout — so the server warms the model
at startup and pings it every 4 minutes to keep it resident (Ollama would
otherwise unload after ~5 idle minutes). A query fired in the first seconds
after server start may still queue behind the initial load; wait for the
`LLM warm (... loaded in ...)` log line.

## Spoken replies (Phase 10: Piper TTS)

The server speaks its replies through the board if the **Piper** binary and a
voice are present under `helper/piper/` (gitignored — ~85 MB of binaries):

```powershell
# from helper/piper/ (create it if absent):
#  1. piper_windows_amd64.zip from https://github.com/rhasspy/piper/releases
#     (tag 2023.11.14-2), extracted here (gives piper/piper.exe)
#  2. a voice pair from https://huggingface.co/rhasspy/piper-voices, e.g.
#     en/en_US/lessac/medium/en_US-lessac-medium.onnx and .onnx.json
```

At startup the server logs `TTS ready: piper, voice ... (22050 Hz -> 16000 Hz)`
or a warning + text-only fallback if the files are missing. `--no-tts` forces
text-only. Voice swap = drop a different `.onnx`+`.onnx.json` pair into
`helper/piper/` (first alphabetically wins). Piper runs on the CPU
(faster-than-realtime), so ASR + LLM keep the GPU. Output is resampled to the
16 kHz wire format with `soxr` and peak-normalized to -3 dBFS so playback
level on the board is consistent across voices.

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
  The board's error line encodes the failing stage — `s3` connect, `s4-6`
  handshake, `s7` upload, `s8` reply wait — plus the LwIP `err_t`.
- **Board stuck a long time at "Connecting..." with no server logs** (Windows):
  the SYN is being silently dropped. Two stacked gotchas: (1) allow rules are
  often scoped to *Private* networks while a direct board link is classified
  *Public/Unidentified*; (2) a previously dismissed firewall prompt creates
  `Python` inbound **Block** rules on Public — and Block rules beat Allow rules.
  Check with `netsh advfirewall firewall show rule name="Python"`, delete any
  inbound Block entries, and add the port rule with `profile=any`:
  `netsh advfirewall firewall add rule name="mini-helper 8765" dir=in
  action=allow protocol=TCP localport=8765 profile=any` (admin shell).
  Note: a loopback test on the server machine bypasses the firewall entirely,
  so it proves the server works but not that the board can reach it; check the
  board's MAC (`00-80-e1-...`) in `arp -a` to confirm L2 reachability instead.
  **The Block rules come back**: Windows re-prompts when a python.exe with no
  existing rule starts listening, and a dismissed prompt re-creates them. Add
  a program-level allow for your exact interpreter so it never prompts again:
  `netsh advfirewall firewall add rule name="Python allow (all profiles)"
  dir=in action=allow program="C:\Python314\python.exe" profile=any`
  — and re-do this whenever the Python install path changes (upgrades!).
- **Board shows "No network"**: board has no DHCP lease — check cable and the
  `Net:` line on the Assist tab.
- **Replies cut off with `...`**: reply exceeded the LCD area; shorten via the
  system prompt or a smaller `max_tokens`.
- **ASR falls back to CPU** (startup log shows `ASR init on cuda failed`):
  check that `nvidia-cublas-cu12` / `nvidia-cudnn-cu12` are installed in the
  same Python environment; the warning line names the missing DLL. CPU
  fallback still works, just slower — use `--whisper-model tiny.en` there.
- **Replies ignore earlier context**: memory resets after 5 idle minutes, or
  you started with `--no-memory`.
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
