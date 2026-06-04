#ifndef DECIMATOR_H
#define DECIMATOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 48 kHz stereo q15 -> 16 kHz mono q15 for the Phase 6 wake-word path.
   The audio task feeds 128-frame interleaved half-buffers in; we sum L+R into
   mono, accumulate 3 half-buffers (384 q15 samples) into a DTCM batch, then
   run arm_fir_decimate_q15 (32-tap Hamming-windowed sinc, M=3, cutoff 7 kHz,
   stopband 8 kHz). The 128 q15 outputs land in a small DTCM ring sized for
   ~64 ms of buffering.

   Call decimator_init() once before SAI DMA starts. Audio task calls
   decimator_push_stereo() in the same fork-point as recorder_tap_pre. */

void decimator_init(void);
void decimator_push_stereo(const int16_t *interleaved, uint32_t frames);

/* Output ring buffer. Single-writer (audio task) increments `decimator_head`
   monotonically; readers (Phase 6 wake-word task in step 6) keep their own
   tail and read decimator_ring[tail & DECIMATOR_RING_MASK]. 32-bit head
   writes are atomic on M7 so no locking is needed for the head/tail. */
#define DECIMATOR_RING_LOG2  10u                            /* 1024 samples */
#define DECIMATOR_RING_SIZE  (1u << DECIMATOR_RING_LOG2)
#define DECIMATOR_RING_MASK  (DECIMATOR_RING_SIZE - 1u)

extern int16_t           decimator_ring[DECIMATOR_RING_SIZE];
extern volatile uint32_t decimator_head;

#ifdef __cplusplus
}
#endif

#endif /* DECIMATOR_H */
