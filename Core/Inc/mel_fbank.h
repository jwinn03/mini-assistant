#ifndef MEL_FBANK_H
#define MEL_FBANK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mel-spectrogram front end for the Phase 6 wake-word path.
   16 kHz mono input → 480-sample (30 ms) Hann-windowed frame → 512-point real
   FFT → power spectrum → 40-bin HTK mel filterbank → natural log + epsilon.

   The window/hop, mel count, and frequency range default to the values most
   widely used by streaming KWS pipelines. If the chosen wake-word model was
   trained with different params (e.g. log10 instead of log, magnitude
   spectrum instead of power), update the constants below and re-verify with
   the offline Python comparison harness. */

#define MEL_FBANK_SAMPLE_RATE  16000u
#define MEL_FBANK_WIN_SIZE     480u                       /* 30 ms */
#define MEL_FBANK_HOP_SIZE     320u                       /* 20 ms */
#define MEL_FBANK_N_FFT        512u
#define MEL_FBANK_N_FREQ       (MEL_FBANK_N_FFT / 2u + 1u)
#define MEL_FBANK_N_MELS       40u
#define MEL_FBANK_F_LO_HZ      0.0f
#define MEL_FBANK_F_HI_HZ      8000.0f                    /* Nyquist */

void mel_fbank_init(void);

/* Compute one log-mel frame.
   `frame_q15`  : MEL_FBANK_WIN_SIZE q15 mono samples at 16 kHz.
   `mels_out`   : caller-provided buffer of MEL_FBANK_N_MELS floats. */
void mel_fbank_process(const int16_t *frame_q15, float *mels_out);

/* Step-5 sanity check. Synthesises a `freq_hz` sine wave at half-scale q15
   amplitude into a 480-sample frame and runs it through mel_fbank_process.
   Useful as a one-shot boot self-test: with freq_hz = 1000, the peak log-mel
   bin should land near mel index ~14 (≈ 1 kHz on the HTK scale, 40 bins
   spanning 0-8 kHz). Stores the resulting frame into the public globals
   so it can be inspected in a debugger. */
void mel_fbank_selftest(float freq_hz);
extern volatile uint32_t mel_fbank_selftest_done;
extern float             mel_fbank_selftest_features[MEL_FBANK_N_MELS];
extern uint32_t          mel_fbank_selftest_peak_bin;

#ifdef __cplusplus
}
#endif

#endif /* MEL_FBANK_H */
