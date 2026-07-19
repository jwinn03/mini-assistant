#ifndef TTS_PLAYER_H
#define TTS_PLAYER_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Phase 10 TTS playback path.
 *
 * The assistant task streams 16 kHz mono int16 PCM (the helper's TTS output,
 * protocol v2 binary chunks) into a 512 KB SDRAM ring. A low-priority task
 * upsamples it 16 k -> 48 k (arm_fir_interpolate_q15, x3 — the exact mirror
 * of the Phase 6 decimator) into a small 48 kHz staging ring. The audio task
 * injects staging frames into the codec path at one of two points:
 *
 *   FX ON  (default, tts_player_fx_enabled != 0): tts_inject_pre() overwrites
 *          the RX buffer BEFORE process_audio — the response plays through the
 *          live effect chain (gain -> ... -> reverb) and the mic is muted by
 *          the overwrite. After speech ends, zeros are fed through the chain
 *          for TTS_FX_TAIL_MS so delay/reverb tails decay before the mic
 *          returns.
 *   FX OFF: tts_inject_post() overwrites the TX buffer after every other
 *          stage — clean voice, chain bypassed.
 *
 * Wake-word interaction: the decimator taps the raw mic before either inject
 * point, so the wake path never sees TTS digitally — but the speakers are
 * audible to the MEMS mic, so utterance.c gates capture on tts_player_active()
 * (true through playback plus a short trailing window).
 */

/* States (tts_player_state diagnostic). */
typedef enum {
    TTS_IDLE = 0,      /* nothing playing */
    TTS_FILLING,       /* receiving; waiting for prefill before audio starts */
    TTS_PLAYING,       /* staging ring feeding the codec */
    TTS_TAIL,          /* speech done; FX-ON only: zeros through the chain */
} tts_player_state_t;

void tts_player_init(void);

/* Feed PCM from the assistant task (16 kHz mono int16 LE, any byte count —
   a trailing odd byte is dropped). Returns the number of BYTES accepted;
   less than len when the ring is full (caller retries later — natural
   backpressure against the WebSocket stream). First write starts a stream. */
uint32_t tts_player_write(const uint8_t *data, uint32_t len);

/* End-of-speech marker received (zero-length binary message). */
void tts_player_eos(void);

/* Network died mid-stream: drop unplayed PCM (the ~85 ms already staged
   plays out) and finish gracefully. */
void tts_player_abort(void);

/* True from stream start until ~300 ms after the last injected frame —
   utterance.c uses this to ignore wake fires caused by the board's own
   voice. */
bool tts_player_active(void);

/* Audio-task hooks (audio.c). Each is a no-op single volatile check unless
   playing in the matching FX mode. `buf` is interleaved stereo q15,
   AUDIO_HALF_FRAMES frames. */
void tts_inject_pre(int16_t *buf, uint32_t frames);
void tts_inject_post(int16_t *buf, uint32_t frames);

/* FX routing toggle (UI writes, audio task reads). 0 = post-chain (clean),
   nonzero = pre-chain (through the effect chain). Default 1 (FX on). */
extern volatile uint8_t  tts_player_fx_enabled;

/* Diagnostics. */
extern volatile uint8_t  tts_player_state;        /* tts_player_state_t */
extern volatile uint32_t tts_player_underruns;    /* staging starved mid-speech */
extern volatile uint32_t tts_player_ms_played;    /* cumulative injected audio */

#endif /* TTS_PLAYER_H */
