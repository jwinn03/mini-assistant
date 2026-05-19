#include "effect_fir.h"
#include "dsp_util.h"
#include "audio.h"

/* ----- Bank layout ------------------------------------------------------- */

#define FS_HZ        48000.0f
#define M_OFFSET     15.5f       /* (N-1)/2 for N=32, non-integer center */
#define PI_F         3.14159265358979f
#define TWO_PI_F     6.28318530717959f

/* Four banks; each is a 32-tap symmetric q15 windowed-sinc filter. Computed
   at init() — keeps the design transparent in source. Bank labels are
   approximate: transition bandwidth at N=32, fs=48k is ~6 kHz wide, so the
   real -3 dB points are within ~1.5 kHz of these nominal numbers. */
typedef struct {
    const char *name;
    float       fc_hz;
    int         highpass;
} bank_spec_t;

static const bank_spec_t bank_specs[EFFECT_FIR_BANKS] = {
    { "LP 3k", 3000.0f, 0 },
    { "LP 8k", 8000.0f, 0 },
    { "HP 1k", 1000.0f, 1 },
    { "HP 3k", 3000.0f, 1 },
};

const char * const effect_fir_bank_names[EFFECT_FIR_BANKS] = {
    "LP 3k", "LP 8k", "HP 1k", "HP 3k"
};

static q15_t s_bank_coeffs[EFFECT_FIR_BANKS][EFFECT_FIR_TAPS];

/* ----- Per-channel instance + state buffer ------------------------------- */

/* arm_fir_q15 state size: numTaps + blockSize - 1 = 32 + 128 - 1 = 159.
   Round to even for the SIMD path. Pinned to DTCM for single-cycle access. */
#define FIR_STATE_LEN (EFFECT_FIR_TAPS + AUDIO_HALF_FRAMES)

static q15_t s_state_L[FIR_STATE_LEN] __attribute__((section(".audio_buffers")));
static q15_t s_state_R[FIR_STATE_LEN] __attribute__((section(".audio_buffers")));

static arm_fir_instance_q15 s_inst_L;
static arm_fir_instance_q15 s_inst_R;

static volatile uint8_t s_active_bank = 0;

/* ----- Coefficient generation -------------------------------------------- */

/* h_lp_continuous[n] = sin(ωc * (n - M)) / (π * (n - M)), windowed by Hamming.
   HP via cosine modulation: design LP at (fs/2 - fc) and multiply by (-1)^n. */
static void compute_bank(int bank_idx)
{
    float fc = bank_specs[bank_idx].fc_hz;
    int   hp = bank_specs[bank_idx].highpass;

    /* HP via spectral inversion of the complementary-cutoff LP. */
    float design_fc = hp ? (FS_HZ * 0.5f - fc) : fc;
    float omega_c   = TWO_PI_F * design_fc / FS_HZ;

    /* First pass: Hamming-windowed ideal LP impulse response (float). */
    float h[EFFECT_FIR_TAPS];
    float sum = 0.0f;
    for (int n = 0; n < EFFECT_FIR_TAPS; n++) {
        float m  = (float)n - M_OFFSET;            /* always half-integer */
        float arg = omega_c * m;
        /* sinc(πx) form: h = sin(ωc m) / (π m). m never zero for even N. */
        float h_lp = dsp_util_sinf(arg) / (PI_F * m);

        float w = 0.54f - 0.46f * dsp_util_cosf(TWO_PI_F * (float)n
                                                / (float)(EFFECT_FIR_TAPS - 1));
        h[n] = h_lp * w;

        if (hp && (n & 1)) h[n] = -h[n];           /* (-1)^n modulation */
        sum += h[n];
    }

    /* Normalize: LP should sum to ~1 (unity DC gain); HP to ~0 (unity Nyquist
       gain after the (-1)^n shift = sum of |h| would be unity). Renormalize
       to maximize headroom while keeping unity DC (LP) or unity HF (HP). */
    if (!hp) {
        /* sum is DC gain — scale so DC gain = 1 exactly. */
        if (sum != 0.0f) {
            float k = 1.0f / sum;
            for (int n = 0; n < EFFECT_FIR_TAPS; n++) h[n] *= k;
        }
    } else {
        /* For HP, sum of h[n]*(-1)^n already done — re-flip mentally to get
           the LP-equivalent sum, normalize, then re-flip. Easier: compute the
           Nyquist gain = sum of (-1)^n * h[n] and scale by 1/that. */
        float nyq = 0.0f;
        for (int n = 0; n < EFFECT_FIR_TAPS; n++) nyq += (n & 1) ? -h[n] : h[n];
        if (nyq != 0.0f) {
            float k = 1.0f / nyq;
            for (int n = 0; n < EFFECT_FIR_TAPS; n++) h[n] *= k;
        }
    }

    /* Quantize to q15 with saturation. Headroom: the LP impulse peak after
       normalization is ~2*fc/fs (LP 3k -> 0.125, LP 8k -> 0.33). Both fit
       comfortably in q15. HP versions have the same peak magnitude. */
    for (int n = 0; n < EFFECT_FIR_TAPS; n++) {
        int32_t q = (int32_t)(h[n] * 32768.0f + (h[n] >= 0.0f ? 0.5f : -0.5f));
        if (q > 32767)  q = 32767;
        if (q < -32768) q = -32768;
        s_bank_coeffs[bank_idx][n] = (q15_t)q;
    }
}

/* ----- Public API -------------------------------------------------------- */

void effect_fir_init(void)
{
    for (int b = 0; b < EFFECT_FIR_BANKS; b++) {
        compute_bank(b);
    }

    arm_fir_init_q15(&s_inst_L, EFFECT_FIR_TAPS,
                     s_bank_coeffs[0], s_state_L, AUDIO_HALF_FRAMES);
    arm_fir_init_q15(&s_inst_R, EFFECT_FIR_TAPS,
                     s_bank_coeffs[0], s_state_R, AUDIO_HALF_FRAMES);
    s_active_bank = 0;
}

void effect_fir_process(q15_t *L, q15_t *R, uint32_t n)
{
    arm_fir_q15(&s_inst_L, L, L, n);
    arm_fir_q15(&s_inst_R, R, R, n);
}

void effect_fir_set_bank_f(float bank)
{
    int b = (int)(bank + (bank >= 0.0f ? 0.5f : -0.5f));
    if (b < EFFECT_FIR_BANK_MIN) b = EFFECT_FIR_BANK_MIN;
    if (b > EFFECT_FIR_BANK_MAX) b = EFFECT_FIR_BANK_MAX;
    /* pCoeffs is a single 32-bit pointer — atomic store on M7. The audio task
       reads it once at the top of arm_fir_q15; a mid-call swap would only
       affect a single block (~2.7 ms) which is below threshold. State is
       NOT reinitialized — arm_fir_init zeros pState and that would be the
       audible glitch we're avoiding. */
    s_inst_L.pCoeffs = s_bank_coeffs[b];
    s_inst_R.pCoeffs = s_bank_coeffs[b];
    s_active_bank = (uint8_t)b;
}
