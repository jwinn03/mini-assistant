#include "mel_fbank.h"
#include "dsp_util.h"
#include "arm_math.h"
#include <string.h>

#define PI_F      3.14159265358979f
#define TWO_PI_F  6.28318530717959f

/* Natural-log form of the HTK mel scale: mel = 1127 * ln(1 + f/700). The
   familiar 2595*log10(...) form is equivalent. We use natural-log because
   CMSIS-DSP's arm_vlog_f32 is the ln implementation. */
#define MEL_K     1127.01048f

/* log(power + LOG_EPS) handles silent frames without -inf. The constant
   matches the value microWakeWord's audio_preprocessor uses; if the chosen
   model was trained with a different floor, change it here and re-verify. */
#define LOG_EPS   1e-6f

/* DTCM scratch. The mel filterbank LUT and Hann window are written once at
   boot in mel_fbank_init() and read every frame thereafter. */
static float s_hann[MEL_FBANK_WIN_SIZE]   __attribute__((section(".audio_buffers")));
static float s_fft_in[MEL_FBANK_N_FFT]    __attribute__((section(".audio_buffers")));
static float s_fft_out[MEL_FBANK_N_FFT]   __attribute__((section(".audio_buffers")));
static float s_power[MEL_FBANK_N_FREQ]    __attribute__((section(".audio_buffers")));

/* Sparse mel filterbank. Each filter `m` contributes weights from
   s_mel_weights[s_mel_offset[m] .. s_mel_offset[m] + s_mel_count[m]) onto
   power-spectrum bins [s_mel_start[m] .. s_mel_start[m] + s_mel_count[m]).
   Upper bound on total nonzero weights for 40 triangle filters over 257
   bins is ~480 (avg ~12 bins per filter); the 768-element pool gives slack. */
#define MEL_WEIGHTS_POOL 768u
static uint16_t s_mel_start[MEL_FBANK_N_MELS]   __attribute__((section(".audio_buffers")));
static uint16_t s_mel_count[MEL_FBANK_N_MELS]   __attribute__((section(".audio_buffers")));
static uint16_t s_mel_offset[MEL_FBANK_N_MELS]  __attribute__((section(".audio_buffers")));
static float    s_mel_weights[MEL_WEIGHTS_POOL] __attribute__((section(".audio_buffers")));

/* Plain .bss — zeroed by the C runtime. The RFFT instance struct holds
   pointers into FFT factor tables that arm_rfft_fast_init_f32 fills in. */
static arm_rfft_fast_instance_f32 s_rfft;

/* Scalar wrappers around the vectorised CMSIS-DSP fast-math routines.
   The block-of-1 trick avoids pulling in libm's logf/expf, which the
   linker script discards. Boot-path use only — process() uses the
   vectorised form directly on a 40-element array. */
static float arm_lnf(float x)
{
    float y;
    arm_vlog_f32(&x, &y, 1);
    return y;
}

static float arm_expf_f(float x)
{
    float y;
    arm_vexp_f32(&x, &y, 1);
    return y;
}

static float hz_to_mel(float hz)
{
    return MEL_K * arm_lnf(1.0f + hz / 700.0f);
}

static float mel_to_hz(float mel)
{
    return 700.0f * (arm_expf_f(mel / MEL_K) - 1.0f);
}

static void build_filterbank(void)
{
    /* 42 mel-domain edge points: filter m peaks at index m+1 and covers
       [m .. m+2]. Triangle weights ramp 0 → 1 → 0 across that span. */
    const float mel_lo = hz_to_mel(MEL_FBANK_F_LO_HZ);
    const float mel_hi = hz_to_mel(MEL_FBANK_F_HI_HZ);
    const float bin_hz = (float)MEL_FBANK_SAMPLE_RATE / (float)MEL_FBANK_N_FFT;

    float mel_pts[MEL_FBANK_N_MELS + 2u];
    float hz_pts [MEL_FBANK_N_MELS + 2u];
    float bin_pts[MEL_FBANK_N_MELS + 2u];
    for (uint32_t i = 0; i < MEL_FBANK_N_MELS + 2u; i++) {
        mel_pts[i] = mel_lo + (mel_hi - mel_lo) *
                              (float)i / (float)(MEL_FBANK_N_MELS + 1u);
        hz_pts [i] = mel_to_hz(mel_pts[i]);
        bin_pts[i] = hz_pts[i] / bin_hz;
    }

    /* Pack filters into the sparse weight pool. */
    uint32_t pool_used = 0;
    for (uint32_t m = 0; m < MEL_FBANK_N_MELS; m++) {
        const float left   = bin_pts[m];
        const float centre = bin_pts[m + 1u];
        const float right  = bin_pts[m + 2u];

        int32_t k_start = (int32_t)left;        if (k_start < 0) k_start = 0;
        int32_t k_end   = (int32_t)right + 1;   if (k_end > (int32_t)MEL_FBANK_N_FREQ) k_end = MEL_FBANK_N_FREQ;

        s_mel_start[m]  = (uint16_t)k_start;
        s_mel_offset[m] = (uint16_t)pool_used;

        uint32_t count = 0;
        for (int32_t k = k_start; k < k_end; k++) {
            float w = 0.0f;
            const float kf = (float)k;
            if (kf >= left && kf <= centre && centre > left) {
                w = (kf - left) / (centre - left);
            } else if (kf > centre && kf <= right && right > centre) {
                w = (right - kf) / (right - centre);
            }
            if (pool_used >= MEL_WEIGHTS_POOL) break;  /* defensive */
            s_mel_weights[pool_used++] = w;
            count++;
        }
        s_mel_count[m] = (uint16_t)count;
    }
}

void mel_fbank_init(void)
{
    memset(s_hann,         0, sizeof(s_hann));
    memset(s_fft_in,       0, sizeof(s_fft_in));
    memset(s_fft_out,      0, sizeof(s_fft_out));
    memset(s_power,        0, sizeof(s_power));
    memset(s_mel_weights,  0, sizeof(s_mel_weights));

    /* Hann window: w[n] = 0.5 - 0.5*cos(2π n / (N-1)). */
    for (uint32_t n = 0; n < MEL_FBANK_WIN_SIZE; n++) {
        const float arg = TWO_PI_F * (float)n / (float)(MEL_FBANK_WIN_SIZE - 1u);
        s_hann[n] = 0.5f - 0.5f * dsp_util_cosf(arg);
    }

    build_filterbank();

    (void)arm_rfft_fast_init_f32(&s_rfft, MEL_FBANK_N_FFT);
}

void mel_fbank_process(const int16_t *frame_q15, float *mels_out)
{
    /* q15 -> float, window in place, zero-pad to N_FFT. The 32 trailing
       zero-pad samples are already zeroed (s_fft_in is all-zero after init;
       this loop overwrites only the first WIN_SIZE entries, leaving the
       trailing pad untouched once a process call has already run. The
       memset above guarantees the first call sees zeros there too). */
    for (uint32_t n = 0; n < MEL_FBANK_WIN_SIZE; n++) {
        s_fft_in[n] = ((float)frame_q15[n] * (1.0f / 32768.0f)) * s_hann[n];
    }

    arm_rfft_fast_f32(&s_rfft, s_fft_in, s_fft_out, 0);

    /* Power spectrum from packed RFFT output.
       Layout: out[0] = X[0] (real DC), out[1] = X[N/2] (real Nyquist),
                 out[2k], out[2k+1] = Re/Im of X[k] for k = 1..N/2-1. */
    s_power[0]                       = s_fft_out[0] * s_fft_out[0];
    s_power[MEL_FBANK_N_FFT / 2u]    = s_fft_out[1] * s_fft_out[1];
    for (uint32_t k = 1; k < MEL_FBANK_N_FFT / 2u; k++) {
        const float re = s_fft_out[2u * k];
        const float im = s_fft_out[2u * k + 1u];
        s_power[k] = re * re + im * im;
    }

    /* Sparse filterbank multiply: mel_energy[m] = sum_k power[k] * w[k]. */
    float mel_energy[MEL_FBANK_N_MELS];
    for (uint32_t m = 0; m < MEL_FBANK_N_MELS; m++) {
        float acc = 0.0f;
        const uint32_t start = s_mel_start[m];
        const uint32_t count = s_mel_count[m];
        const float   *w     = &s_mel_weights[s_mel_offset[m]];
        for (uint32_t i = 0; i < count; i++) {
            acc += s_power[start + i] * w[i];
        }
        mel_energy[m] = acc + LOG_EPS;
    }

    /* log() over the 40-element mel-energy array. arm_vlog_f32 is the
       polynomial-approximation natural log; on M7 with FPU it runs in
       single-digit cycles per element. */
    arm_vlog_f32(mel_energy, mels_out, MEL_FBANK_N_MELS);
}

/* ----- step-5 self test ------------------------------------------------- */

volatile uint32_t mel_fbank_selftest_done       = 0u;
float             mel_fbank_selftest_features[MEL_FBANK_N_MELS];
uint32_t          mel_fbank_selftest_peak_bin   = 0u;

void mel_fbank_selftest(float freq_hz)
{
    /* Synthesise WIN_SIZE samples of a pure tone at half-scale q15. */
    int16_t frame[MEL_FBANK_WIN_SIZE];
    const float omega = TWO_PI_F * freq_hz / (float)MEL_FBANK_SAMPLE_RATE;
    for (uint32_t n = 0; n < MEL_FBANK_WIN_SIZE; n++) {
        const float s = dsp_util_sinf(omega * (float)n);
        frame[n] = (int16_t)(s * 16384.0f);
    }

    mel_fbank_process(frame, mel_fbank_selftest_features);

    /* Find peak bin so the result can be cross-checked at a glance. */
    uint32_t peak = 0;
    float    pv   = mel_fbank_selftest_features[0];
    for (uint32_t m = 1; m < MEL_FBANK_N_MELS; m++) {
        if (mel_fbank_selftest_features[m] > pv) {
            pv = mel_fbank_selftest_features[m];
            peak = m;
        }
    }
    mel_fbank_selftest_peak_bin = peak;
    mel_fbank_selftest_done     = 1u;
}
