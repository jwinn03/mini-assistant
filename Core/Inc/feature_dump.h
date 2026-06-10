#ifndef FEATURE_DUMP_H
#define FEATURE_DUMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 6.5 verification harness — captures the wake-word front end's exact
   input and output so tools/compare_features.py can diff the on-device
   features against the pymicro-features reference, frame by frame.

   Capture starts armed at boot, synchronized with the microfrontend's fresh
   state: the PCM buffer receives every 16 kHz mono sample fed to
   micro_features_process_hop (in order, from the first hop), and the frame
   buffer receives every uint16[40] feature frame produced. Capture stops when
   the PCM buffer fills (~10 s) and feature_dump_done goes to 1.

   Dumping (debugger, target halted or running — SDRAM reads are safe):
     arm-none-eabi-gdb build/Debug/mini-assistant.elf
       (gdb) dump binary memory pcm.bin   &feature_dump_pcm_buf \
             (char*)&feature_dump_pcm_buf + feature_dump_pcm_count*2
       (gdb) dump binary memory feat.bin  &feature_dump_frame_buf \
             (char*)&feature_dump_frame_buf + feature_dump_frame_count*80
   then: python tools/compare_features.py pcm.bin feat.bin

   Recapture: set feature_dump_rearm = 1 from the debugger. The wake-word task
   notices at the next hop boundary, resets the microfrontend state (so device
   and reference both start from frame 0 of a fresh pipeline) and restarts the
   capture. Speak during the ~10 s window — features from real speech exercise
   PCAN and noise reduction far harder than room tone does.

   feature_dump_invalid = 1 means the wake task fell behind and skipped ring
   samples mid-capture; the dump no longer matches the frontend's true input
   stream. Discard and rearm. */

#define FEATURE_DUMP_PCM_CAPACITY    160000u  /* 10 s @ 16 kHz, 320 KB        */
#define FEATURE_DUMP_FRAME_CAPACITY  1000u    /* >= 998 frames from 10 s      */

void feature_dump_init(void);

/* Append n mono q15 samples / one feature frame. No-ops once done. Call only
   from the wake-word task, in lockstep with micro_features_process_hop. */
void feature_dump_pcm(const int16_t *samples, uint32_t n);
void feature_dump_frame(const uint16_t *feat);

/* Wake task helpers. */
int  feature_dump_capturing(void);    /* 1 while capture in progress          */
int  feature_dump_take_rearm(void);   /* consume rearm request; 1 if restarted */
void feature_dump_mark_invalid(void); /* ring overrun during capture          */

/* Debugger-facing state. */
extern volatile uint32_t feature_dump_pcm_count;
extern volatile uint32_t feature_dump_frame_count;
extern volatile uint32_t feature_dump_done;
extern volatile uint32_t feature_dump_invalid;
extern volatile uint32_t feature_dump_rearm;     /* set to 1 to recapture */

extern int16_t  feature_dump_pcm_buf[FEATURE_DUMP_PCM_CAPACITY];
extern uint16_t feature_dump_frame_buf[FEATURE_DUMP_FRAME_CAPACITY][40];

#ifdef __cplusplus
}
#endif

#endif /* FEATURE_DUMP_H */
