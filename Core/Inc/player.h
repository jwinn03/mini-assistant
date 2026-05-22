#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include "sd_card.h"

/* WAV playback from SD card. Mirror of recorder.{h,c}.

   The player task reads a WAV file in 32 KB chunks from SD into a 256 KB
   SDRAM ring; the audio task consumes from the ring once per half-buffer.
   When the ring underruns (SD stall longer than ~1.3 s, or file ended), the
   audio output is filled with silence.

   Playback is post-DSP — player_inject_post() overwrites the audio TX buffer
   after the DSP chain has run, so the recording is heard exactly as captured.
   The live mic path is effectively muted during playback (its processed
   output is overwritten before it reaches the codec). */

typedef enum {
    PLAYER_STATE_IDLE    = 0,
    PLAYER_STATE_LOADING = 1,        /* opening file + parsing header */
    PLAYER_STATE_PLAYING = 2,
    PLAYER_STATE_ERROR   = 3,
} player_state_t;

void player_init(void);

/* Open `filename` (relative to SD root) and start playback. No-op unless
   state == IDLE. Returns true if accepted — the player task may still
   transition to ERROR on a bad file. */
bool player_start(const char *filename);

/* Stop playback. No-op if already IDLE. */
bool player_stop(void);

player_state_t player_get_state(void);

/* Position and duration in milliseconds. Both round to the nearest ms.
   Duration is fixed at file-open time; position monotonically increases
   while PLAYING (driven by the audio task's consumption). */
uint32_t player_get_position_ms(void);
uint32_t player_get_duration_ms(void);

/* Post-DSP injection point. Call from audio_task AFTER process_audio()
   and AFTER recorder_tap_post(). buf is the interleaved stereo q15 output
   half (audio_tx_buffer[…]); when PLAYING, this function overwrites it
   with samples from the ring. When IDLE/LOADING/ERROR, no-op. */
void player_inject_post(int16_t *buf, uint32_t frames);

#endif
