#ifndef RECORDER_H
#define RECORDER_H

#include <stdint.h>
#include <stdbool.h>
#include "arm_math.h"

/* SD-card recording subsystem. Phase 5.

   The audio task taps interleaved stereo q15 samples into a 256 KB SDRAM
   ring buffer; a dedicated low-priority recorder task drains the ring to
   FATFS in 32 KB chunks. The ring sizing (256 KB / ~1.36 s) absorbs the
   typical 250-400 ms SD erase stall on consumer cards with margin to spare.

   Tap point is user-selectable: PRE captures the raw mic input (audio_rx_buffer)
   before the DSP chain; POST captures the final processed output (audio_tx_buffer)
   after the DSP chain. Switching only takes effect when state == IDLE — mid-
   recording switches would produce a glitch in the WAV.

   State machine:
     IDLE       — no recording. Tap functions early-return.
     ARMED      — recorder task is opening the file (absorbs the f_open spike).
                  Audio task does NOT tap yet.
     RECORDING  — audio task taps; recorder task drains.
     STOPPING   — audio task has stopped tapping; recorder task is flushing
                  the residual ring and finalizing the WAV header.
     ERROR      — last op failed (card pulled, write error, no space). UI
                  must call recorder_stop() to clear and return to IDLE. */

typedef enum {
    RECORDER_STATE_IDLE      = 0,
    RECORDER_STATE_ARMED     = 1,
    RECORDER_STATE_RECORDING = 2,
    RECORDER_STATE_STOPPING  = 3,
    RECORDER_STATE_ERROR     = 4,
} recorder_state_t;

typedef enum {
    RECORDER_TAP_PRE  = 0,
    RECORDER_TAP_POST = 1,
} recorder_tap_t;

/* Creates the SDRAM ring and the recorder task. Must be called after
   sd_card_init(). */
void recorder_init(void);

/* Request start. No-op unless state == IDLE. Returns true if the command was
   accepted (the recorder task may still fail asynchronously and transition
   to ERROR — UI must poll recorder_get_state()). */
bool recorder_start(void);

/* Request stop. No-op unless state in {ARMED, RECORDING, ERROR}. Returns true
   if accepted. STOPPING → IDLE happens once the recorder task has flushed
   the ring and closed the file. */
bool recorder_stop(void);

/* Switch the tap point. Only takes effect when state == IDLE; otherwise
   ignored. Returns true if applied. */
bool recorder_set_tap(recorder_tap_t tap);
recorder_tap_t recorder_get_tap(void);

recorder_state_t recorder_get_state(void);

/* Wall-clock-ish milliseconds since RECORDING began. Computed from the
   sample-frame count, so it stays exact even under SD stalls (the WAV
   simply has a few hundred ms of "captured" frames buffered in RAM). */
uint32_t recorder_get_elapsed_ms(void);

/* Peak absolute sample seen since the last call. Reads-and-resets so the
   meter naturally decays back to zero when audio goes quiet. Returns 0..32767. */
q15_t recorder_get_peak(void);

/* Pre-DSP tap. Call from audio_task BEFORE process_audio(). buf is the
   interleaved stereo q15 input (audio_rx_buffer half). frames is the per-half
   frame count (AUDIO_HALF_FRAMES). */
void recorder_tap_pre(const int16_t *buf, uint32_t frames);

/* Post-DSP tap. Call from audio_task AFTER process_audio(). buf is the
   interleaved stereo q15 output (audio_tx_buffer half). */
void recorder_tap_post(const int16_t *buf, uint32_t frames);

#endif
