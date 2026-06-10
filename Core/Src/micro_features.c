#include "micro_features.h"

#include <string.h>

#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

volatile int32_t micro_features_init_status = 0;

/* Config + state are plain .bss. The big buffers (window input, FFT scratch,
   filterbank weights — ~10 KB total) are malloc'd by FrontendPopulateState
   from the newlib heap, which grows from _end toward the system stack at the
   top of SRAM (see sysmem.c). One-time allocation at boot; never freed. */
static struct FrontendConfig s_config;
static struct FrontendState  s_state;

int micro_features_init(void)
{
    /* Start from upstream defaults, then set every knob explicitly anyway so
       this file is the single readable record of the training-time settings
       (values match microWakeWord / the hey_jarvis V2 model). */
    FrontendFillConfigWithDefaults(&s_config);

    s_config.window.size_ms                       = MICRO_FEATURES_WINDOW_MS;
    s_config.window.step_size_ms                  = MICRO_FEATURES_STEP_MS;

    s_config.filterbank.num_channels              = MICRO_FEATURES_N_CHANNELS;
    s_config.filterbank.lower_band_limit          = 125.0f;
    s_config.filterbank.upper_band_limit          = 7500.0f;

    s_config.noise_reduction.smoothing_bits       = 10;
    s_config.noise_reduction.even_smoothing       = 0.025f;
    s_config.noise_reduction.odd_smoothing        = 0.06f;
    s_config.noise_reduction.min_signal_remaining = 0.05f;

    s_config.pcan_gain_control.enable_pcan        = 1;
    s_config.pcan_gain_control.strength           = 0.95f;
    s_config.pcan_gain_control.offset             = 80.0f;
    s_config.pcan_gain_control.gain_bits          = 21;

    s_config.log_scale.enable_log                 = 1;
    s_config.log_scale.scale_shift                = 6;

    if (!FrontendPopulateState(&s_config, &s_state,
                               (int)MICRO_FEATURES_SAMPLE_RATE)) {
        micro_features_init_status = -1;
        return -1;
    }

    micro_features_init_status = 1;
    return 0;
}

void micro_features_reset(void)
{
    if (micro_features_init_status == 1) {
        FrontendReset(&s_state);
    }
}

int micro_features_process_hop(const int16_t *samples,
                               uint16_t out[MICRO_FEATURES_N_CHANNELS])
{
    int produced = 0;
    const int16_t *p = samples;
    size_t remaining = MICRO_FEATURES_HOP_SAMPLES;

    /* One hop = one window step, so steady-state is a single iteration that
       consumes all 160 samples and emits one frame. While the very first
       window is still filling (480 samples needed), calls consume everything
       and emit nothing. The loop covers the general contract anyway:
       FrontendProcessSamples stops consuming at each emitted frame. */
    while (remaining > 0) {
        size_t num_read = 0;
        struct FrontendOutput o =
            FrontendProcessSamples(&s_state, p, remaining, &num_read);
        p += num_read;
        remaining -= num_read;

        if (o.values != NULL && o.size == MICRO_FEATURES_N_CHANNELS) {
            /* o.values points into frontend state and dies on the next call —
               copy out immediately. */
            memcpy(out, o.values,
                   MICRO_FEATURES_N_CHANNELS * sizeof(uint16_t));
            produced = 1;
        }

        if (num_read == 0) {
            break;  /* defensive: cannot make progress */
        }
    }

    return produced;
}
