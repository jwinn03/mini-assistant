#include "decimator.h"
#include "audio.h"
#include "dsp_util.h"
#include "arm_math.h"
#include <string.h>

/* Filter design constants. 32 taps gives a sharp enough transition for a /3
   decimator at 48 -> 16 kHz, and is a multiple of 4 so CMSIS-DSP's SIMD path
   stays happy. Cutoff at 7 kHz preserves the full speech band; the 7-8 kHz
   transition keeps aliasing out of the 0-8 kHz output Nyquist range. */
#define NUM_TAPS        32u
#define DEC_M           3u
#define BATCH_FRAMES    (3u * AUDIO_HALF_FRAMES)    /* 384 with current audio.h */
#define OUT_PER_BATCH   (BATCH_FRAMES / DEC_M)      /* 128 samples @ 16 kHz */

/* arm_fir_decimate_q15 needs blockSize divisible by M. Catch any future
   change to AUDIO_HALF_FRAMES that would break this at build time. */
_Static_assert(BATCH_FRAMES % DEC_M == 0u,
               "BATCH_FRAMES must be a multiple of DEC_M for arm_fir_decimate_q15");

#define PI_F            3.14159265358979f
#define TWO_PI_F        6.28318530717959f

/* State + scratch in DTCM (.audio_buffers). Section is NOLOAD per the linker
   script so the runtime does not zero these; init memsets them explicitly. */
static q15_t s_state[NUM_TAPS + BATCH_FRAMES - 1] __attribute__((section(".audio_buffers")));
static q15_t s_coeffs[NUM_TAPS]                   __attribute__((section(".audio_buffers")));
static q15_t s_batch[BATCH_FRAMES]                __attribute__((section(".audio_buffers")));
static q15_t s_out[OUT_PER_BATCH]                 __attribute__((section(".audio_buffers")));

/* Plain .bss (zero-initialised by the C runtime) — instance struct holds
   pointers into the DTCM arrays above plus a few small counters. */
static arm_fir_decimate_instance_q15 s_inst;
static uint32_t s_batch_pos;

/* Public ring + monotonic head. DTCM for hot-path access; head is a regular
   global so the C runtime zeroes it at boot. */
int16_t           decimator_ring[DECIMATOR_RING_SIZE] __attribute__((section(".audio_buffers")));
volatile uint32_t decimator_head;

static void compute_coeffs(void)
{
    /* Hamming-windowed ideal sinc lowpass. The taps centre between samples
       (M_off = 15.5 for 32 taps), so m never hits zero — no divide-by-zero
       special case needed. */
    const float omega_c = TWO_PI_F * 7000.0f / 48000.0f;
    const float M_off   = (NUM_TAPS - 1u) * 0.5f;

    float h[NUM_TAPS];
    float sum = 0.0f;
    for (uint32_t n = 0; n < NUM_TAPS; n++) {
        float m   = (float)n - M_off;
        float arg = omega_c * m;
        float h_lp = dsp_util_sinf(arg) / (PI_F * m);
        float w    = 0.54f - 0.46f *
                     dsp_util_cosf(TWO_PI_F * (float)n / (float)(NUM_TAPS - 1u));
        h[n] = h_lp * w;
        sum += h[n];
    }

    /* Normalise to unity DC gain. */
    if (sum != 0.0f) {
        const float k = 1.0f / sum;
        for (uint32_t n = 0; n < NUM_TAPS; n++) h[n] *= k;
    }

    /* Quantise to q15 with rounding + saturation. Peak coefficient is
       ~2*fc/fs = 0.292, comfortably inside [-1, 1). */
    for (uint32_t n = 0; n < NUM_TAPS; n++) {
        int32_t q = (int32_t)(h[n] * 32768.0f + (h[n] >= 0.0f ? 0.5f : -0.5f));
        if (q > 32767)  q = 32767;
        if (q < -32768) q = -32768;
        s_coeffs[n] = (q15_t)q;
    }
}

void decimator_init(void)
{
    memset(s_state, 0, sizeof(s_state));
    memset(s_batch, 0, sizeof(s_batch));
    memset(s_out,   0, sizeof(s_out));
    memset(decimator_ring, 0, sizeof(decimator_ring));
    s_batch_pos    = 0;
    decimator_head = 0;

    compute_coeffs();

    (void)arm_fir_decimate_init_q15(&s_inst, NUM_TAPS, DEC_M,
                                    s_coeffs, s_state, BATCH_FRAMES);
}

void decimator_push_stereo(const int16_t *interleaved, uint32_t frames)
{
    /* Sum L+R then >>1 for one bit of headroom before the FIR convolves. */
    for (uint32_t i = 0; i < frames; i++) {
        int32_t mono = (int32_t)interleaved[2u * i] +
                       (int32_t)interleaved[2u * i + 1u];
        s_batch[s_batch_pos++] = (q15_t)(mono >> 1);

        if (s_batch_pos == BATCH_FRAMES) {
            arm_fir_decimate_q15(&s_inst, s_batch, s_out, BATCH_FRAMES);

            uint32_t h = decimator_head;
            for (uint32_t j = 0; j < OUT_PER_BATCH; j++) {
                decimator_ring[(h + j) & DECIMATOR_RING_MASK] = s_out[j];
            }
            decimator_head = h + OUT_PER_BATCH;

            s_batch_pos = 0;
        }
    }
}
