#include "ui.h"
#include "lcd.h"
#include "ft5336.h"
#include "dsp_chain.h"
#include "effect_gain.h"
#include "effect_clip.h"
#include "effect_fir.h"
#include "effect_eq.h"
#include "effect_delay.h"
#include "effect_chorus.h"
#include "effect_reverb.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>

extern I2C_HandleTypeDef hi2c3;

/* ===== Layout ============================================================ */

#define TAB_H              32
#define TAB_TOUCH_H        36       /* slight overshoot so finger-up at row 33 still counts */
#define BODY_TOP           (TAB_H + 8)
#define PAGE_NAME_Y        44
/* Enable indicator + its touch zone live in the bottom-right corner, below
   the slider 2 readout (which ends at y=236) and to the right of center
   (x>=240). This avoids overlap with the slider zones and keeps the toggle
   reachable on every page. */
#define ENABLE_TEXT_X      (LCD_W - 3 * LCD_FONT_W - 8)
#define ENABLE_TEXT_Y      (LCD_H - LCD_FONT_H - 4)
#define ENABLE_TOUCH_TOP   (LCD_H - 28)
#define ENABLE_TOUCH_LEFT  (LCD_W / 2)
#define SLIDER_H           30
#define SLIDER_LABEL_GAP   6        /* label above the bar */
#define SLIDER_TEXT_GAP    10       /* readout below the bar */
/* Default 1-slider page geometry (Gain, Clip, FIR). 3-slider pages
   (EQ, Delay, Chorus, Reverb) declare their own y positions inline
   and use compact mode (no separate label row). */
#define SLIDER1_BAR_Y      90
#define SLIDER1_LABEL_Y    (SLIDER1_BAR_Y - LCD_FONT_H - SLIDER_LABEL_GAP)
#define SLIDER1_TEXT_Y     (SLIDER1_BAR_Y + SLIDER_H + SLIDER_TEXT_GAP)

#define UI_TAB_ACTIVE      LCD_CYAN
#define UI_TAB_INACTIVE    LCD_DKGRAY
#define UI_TAB_TEXT_ON     LCD_BLACK
#define UI_TAB_TEXT_OFF    LCD_LTGRAY
#define UI_BAR_BG          LCD_DKGRAY
#define UI_BAR_FG          LCD_CYAN
#define UI_TEXT_FG         LCD_WHITE
#define UI_TEXT_BG         LCD_BLACK
#define UI_PLACEHOLDER_FG  LCD_GRAY
#define UI_ENABLE_ON_FG    LCD_GREEN
#define UI_ENABLE_OFF_FG   LCD_GRAY

#define INVALIDATE_BAR_W   0xFFFF   /* sentinel forcing a full slider redraw */

/* ===== Slider widget ===================================================== */

typedef struct slider_s {
    uint16_t y;                 /* top of bar */
    uint16_t label_y;           /* label row (above bar) — ignored if compact */
    uint16_t text_y;            /* readout row (below bar) */
    float    min, max;
    float    value;
    int8_t   decimals;          /* 0 = integer, 1 = 0.0 fixed-point */
    bool     show_sign;         /* prefix '+' for non-negative */
    bool     compact;           /* true = no separate label row; label inlined into readout */
    const char *label;
    const char *unit;
    void   (*set_value)(float); /* push to effect (lock-free volatile write) */
    /* Optional discrete-mode names: when non-NULL, the readout shows
       names[round(value)] instead of the numeric format. Use for selectors
       like the FIR bank picker. */
    const char * const *names;
    uint8_t  name_count;
    /* render state */
    uint16_t last_bar_w;        /* INVALIDATE_BAR_W = full redraw next frame */
    float    last_value;
} slider_t;

#define MAX_SLIDERS_PER_PAGE 3

typedef struct {
    const char *name;       /* matches dsp_chain effect_names[] */
    uint8_t     effect_id;
    uint8_t     slider_count;
    slider_t    sliders[MAX_SLIDERS_PER_PAGE];
} effect_page_t;

/* ===== Pages ============================================================= */

static effect_page_t s_pages[EFFECT_COUNT] = {
    [EFFECT_ID_GAIN] = {
        .name = "Gain",
        .effect_id = EFFECT_ID_GAIN,
        .slider_count = 1,
        .sliders = {
            {
                .y = SLIDER1_BAR_Y,
                .label_y = SLIDER1_LABEL_Y,
                .text_y = SLIDER1_TEXT_Y,
                .min = (float)EFFECT_GAIN_DB_MIN,
                .max = (float)EFFECT_GAIN_DB_MAX,
                .value = 0.0f,
                .decimals = 0,
                .show_sign = true,
                .label = "Gain",
                .unit = "dB",
                .set_value = effect_gain_set_db,
                .last_bar_w = INVALIDATE_BAR_W,
                .last_value = 999999.0f,
            },
        },
    },
    [EFFECT_ID_CLIP] = {
        .name = "Clip",
        .effect_id = EFFECT_ID_CLIP,
        .slider_count = 1,
        .sliders = {
            {
                .y = SLIDER1_BAR_Y,
                .label_y = SLIDER1_LABEL_Y,
                .text_y = SLIDER1_TEXT_Y,
                .min = (float)EFFECT_CLIP_DB_MIN,
                .max = (float)EFFECT_CLIP_DB_MAX,
                .value = 0.0f,
                .decimals = 0,
                .show_sign = false,
                .label = "Threshold",
                .unit = "dB",
                .set_value = effect_clip_set_threshold_db,
                .last_bar_w = INVALIDATE_BAR_W,
                .last_value = 999999.0f,
            },
        },
    },
    [EFFECT_ID_FIR] = {
        .name = "FIR",
        .effect_id = EFFECT_ID_FIR,
        .slider_count = 1,
        .sliders = {
            {
                .y = SLIDER1_BAR_Y,
                .label_y = SLIDER1_LABEL_Y,
                .text_y = SLIDER1_TEXT_Y,
                .min = (float)EFFECT_FIR_BANK_MIN,
                .max = (float)EFFECT_FIR_BANK_MAX,
                .value = 0.0f,
                .label = "Filter",
                .set_value = effect_fir_set_bank_f,
                .names = effect_fir_bank_names,
                .name_count = EFFECT_FIR_BANKS,
                .last_bar_w = INVALIDATE_BAR_W,
                .last_value = 999999.0f,
            },
        },
    },
    [EFFECT_ID_EQ] = {
        /* Three compact stacked sliders. Layout fits within y=60..244
           (between page-name row and enable indicator). */
        .name = "EQ",
        .effect_id = EFFECT_ID_EQ,
        .slider_count = 3,
        .sliders = {
            {
                .y =  66, .text_y = 104,
                .min = (float)EFFECT_EQ_DB_MIN, .max = (float)EFFECT_EQ_DB_MAX,
                .value = 0.0f,
                .decimals = 0, .show_sign = true, .compact = true,
                .label = "Low", .unit = "dB",
                .set_value = effect_eq_set_low_db,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
            {
                .y = 128, .text_y = 166,
                .min = (float)EFFECT_EQ_DB_MIN, .max = (float)EFFECT_EQ_DB_MAX,
                .value = 0.0f,
                .decimals = 0, .show_sign = true, .compact = true,
                .label = "Mid", .unit = "dB",
                .set_value = effect_eq_set_mid_db,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
            {
                .y = 190, .text_y = 228,
                .min = (float)EFFECT_EQ_DB_MIN, .max = (float)EFFECT_EQ_DB_MAX,
                .value = 0.0f,
                .decimals = 0, .show_sign = true, .compact = true,
                .label = "High", .unit = "dB",
                .set_value = effect_eq_set_high_db,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
        },
    },
    [EFFECT_ID_DELAY] = {
        /* Same compact 3-slider layout as EQ. */
        .name = "Delay",
        .effect_id = EFFECT_ID_DELAY,
        .slider_count = 3,
        .sliders = {
            {
                .y =  66, .text_y = 104,
                .min = (float)EFFECT_DELAY_MS_MIN, .max = (float)EFFECT_DELAY_MS_MAX,
                .value = 480.0f,
                .decimals = 0, .show_sign = false, .compact = true,
                .label = "Time", .unit = "ms",
                .set_value = effect_delay_set_time_ms,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
            {
                .y = 128, .text_y = 166,
                .min = (float)EFFECT_DELAY_FB_MIN, .max = (float)EFFECT_DELAY_FB_MAX,
                .value = 40.0f,
                .decimals = 0, .show_sign = false, .compact = true,
                .label = "FB", .unit = "%",
                .set_value = effect_delay_set_feedback_pct,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
            {
                .y = 190, .text_y = 228,
                .min = (float)EFFECT_DELAY_MIX_MIN, .max = (float)EFFECT_DELAY_MIX_MAX,
                .value = 50.0f,
                .decimals = 0, .show_sign = false, .compact = true,
                .label = "Mix", .unit = "%",
                .set_value = effect_delay_set_mix_pct,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
        },
    },
    [EFFECT_ID_CHORUS] = {
        /* Same compact 3-slider layout as EQ / Delay. */
        .name = "Chorus",
        .effect_id = EFFECT_ID_CHORUS,
        .slider_count = 3,
        .sliders = {
            {
                .y =  66, .text_y = 104,
                .min = EFFECT_CHORUS_RATE_HZ_MIN, .max = EFFECT_CHORUS_RATE_HZ_MAX,
                .value = 1.0f,
                .decimals = 1, .show_sign = false, .compact = true,
                .label = "Rate", .unit = "Hz",
                .set_value = effect_chorus_set_rate_hz,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
            {
                .y = 128, .text_y = 166,
                .min = (float)EFFECT_CHORUS_DEPTH_MS_MIN, .max = (float)EFFECT_CHORUS_DEPTH_MS_MAX,
                .value = 3.0f,
                .decimals = 0, .show_sign = false, .compact = true,
                .label = "Depth", .unit = "ms",
                .set_value = effect_chorus_set_depth_ms,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
            {
                .y = 190, .text_y = 228,
                .min = (float)EFFECT_CHORUS_MIX_MIN, .max = (float)EFFECT_CHORUS_MIX_MAX,
                .value = 50.0f,
                .decimals = 0, .show_sign = false, .compact = true,
                .label = "Mix", .unit = "%",
                .set_value = effect_chorus_set_mix_pct,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
        },
    },
    [EFFECT_ID_REVERB] = {
        /* Same compact 3-slider layout as EQ / Delay / Chorus. */
        .name = "Reverb",
        .effect_id = EFFECT_ID_REVERB,
        .slider_count = 3,
        .sliders = {
            {
                .y =  66, .text_y = 104,
                .min = (float)EFFECT_REVERB_SIZE_MIN, .max = (float)EFFECT_REVERB_SIZE_MAX,
                .value = 50.0f,
                .decimals = 0, .show_sign = false, .compact = true,
                .label = "Size", .unit = "%",
                .set_value = effect_reverb_set_size_pct,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
            {
                .y = 128, .text_y = 166,
                .min = (float)EFFECT_REVERB_DAMP_MIN, .max = (float)EFFECT_REVERB_DAMP_MAX,
                .value = 30.0f,
                .decimals = 0, .show_sign = false, .compact = true,
                .label = "Damp", .unit = "%",
                .set_value = effect_reverb_set_damping_pct,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
            {
                .y = 190, .text_y = 228,
                .min = (float)EFFECT_REVERB_MIX_MIN, .max = (float)EFFECT_REVERB_MIX_MAX,
                .value = 30.0f,
                .decimals = 0, .show_sign = false, .compact = true,
                .label = "Mix", .unit = "%",
                .set_value = effect_reverb_set_mix_pct,
                .last_bar_w = INVALIDATE_BAR_W, .last_value = 999999.0f,
            },
        },
    },
};

static uint8_t s_active_page = EFFECT_ID_GAIN;
/* Tracks what's actually on screen for the displayed page's enable indicator,
   so we only repaint that row when it changes. Page switch resets it via
   page_body_redraw. */
static bool    s_drawn_enabled = false;

/* ===== Helpers =========================================================== */

static int strlen8(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Tab horizontal extents (proportional). For EFFECT_COUNT=7 and LCD_W=480
   this yields tabs of 68 or 69 px — close enough that we don't bother with
   a uniform grid. */
static inline uint16_t tab_x(uint8_t i)
{
    return (uint16_t)((uint32_t)i * LCD_W / EFFECT_COUNT);
}

/* Hit-test the X coord to a tab index. */
static inline uint8_t tab_hit(uint16_t x)
{
    uint8_t i = (uint8_t)((uint32_t)x * EFFECT_COUNT / LCD_W);
    if (i >= EFFECT_COUNT) i = EFFECT_COUNT - 1;
    return i;
}

/* Format a float to integer/1-decimal with optional sign + unit suffix.
   No libm: integer round + manual digit emission. Buffer must hold ~16 chars. */
static int format_value(float v, int8_t decimals, bool show_sign,
                        const char *unit, char *out)
{
    int scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    int rounded = (int)(v * scale + (v >= 0.0f ? 0.5f : -0.5f));
    bool neg = (rounded < 0);
    if (neg) rounded = -rounded;

    char *p = out;
    if (neg) {
        *p++ = '-';
    } else if (show_sign) {
        *p++ = '+';
    }

    int int_part = rounded / scale;
    char digits[8];
    int dn = 0;
    if (int_part == 0) {
        digits[dn++] = '0';
    } else {
        while (int_part > 0 && dn < (int)sizeof(digits)) {
            digits[dn++] = (char)('0' + (int_part % 10));
            int_part /= 10;
        }
    }
    while (dn > 0) *p++ = digits[--dn];

    if (decimals > 0) {
        *p++ = '.';
        int frac = rounded % scale;
        /* leading zeros within the fractional part */
        int divider = scale / 10;
        while (divider > 0) {
            *p++ = (char)('0' + (frac / divider) % 10);
            divider /= 10;
        }
    }

    if (unit && *unit) {
        *p++ = ' ';
        while (*unit) *p++ = *unit++;
    }
    *p = '\0';
    return (int)(p - out);
}

/* ===== Slider rendering ================================================== */

static uint16_t slider_bar_width(const slider_t *s)
{
    float v = s->value;
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    float t = (v - s->min) / (s->max - s->min);
    return (uint16_t)(t * LCD_W);
}

static void slider_draw(slider_t *s)
{
    bool full = (s->last_bar_w == INVALIDATE_BAR_W);
    uint16_t bar_w = slider_bar_width(s);

    if (full) {
        /* Label row — only when not compact. */
        if (!s->compact && s->label) {
            int len = strlen8(s->label);
            int lx = (LCD_W - len * LCD_FONT_W) / 2;
            lcd_fill_rect(0, s->label_y, LCD_W, LCD_FONT_H, UI_TEXT_BG);
            lcd_draw_text(lx, s->label_y, s->label, UI_TEXT_FG, UI_TEXT_BG);
        }
        /* Bar full repaint: bg then fg */
        lcd_fill_rect(0, s->y, LCD_W, SLIDER_H, UI_BAR_BG);
        if (bar_w > 0) {
            lcd_fill_rect(0, s->y, bar_w, SLIDER_H, UI_BAR_FG);
        }
    } else if (bar_w > s->last_bar_w) {
        lcd_fill_rect(s->last_bar_w, s->y, bar_w - s->last_bar_w, SLIDER_H, UI_BAR_FG);
    } else if (bar_w < s->last_bar_w) {
        lcd_fill_rect(bar_w, s->y, s->last_bar_w - bar_w, SLIDER_H, UI_BAR_BG);
    }
    s->last_bar_w = bar_w;

    /* Readout — only repaint if the displayed value would change. */
    if (full || s->value != s->last_value) {
        char buf[24];
        const char *txt;
        int len;
        if (s->names && s->name_count > 0) {
            int idx = (int)(s->value + (s->value >= 0.0f ? 0.5f : -0.5f));
            if (idx < 0) idx = 0;
            if (idx >= (int)s->name_count) idx = s->name_count - 1;
            txt = s->names[idx];
            len = strlen8(txt);
        } else if (s->compact && s->label) {
            /* Inline label + value, e.g. "Low +3 dB". */
            int p = 0;
            const char *L = s->label;
            while (*L && p < 12) buf[p++] = *L++;
            buf[p++] = ' ';
            int vlen = format_value(s->value, s->decimals, s->show_sign, s->unit, buf + p);
            len = p + vlen;
            txt = buf;
        } else {
            len = format_value(s->value, s->decimals, s->show_sign, s->unit, buf);
            txt = buf;
        }
        int tx = (LCD_W - len * LCD_FONT_W) / 2;
        lcd_fill_rect(0, s->text_y, LCD_W, LCD_FONT_H, UI_TEXT_BG);
        lcd_draw_text(tx, s->text_y, txt, UI_TEXT_FG, UI_TEXT_BG);
        s->last_value = s->value;
    }
}

/* ===== Tab strip ========================================================= */

static void tab_draw_one(uint8_t i, bool active)
{
    uint16_t x0 = tab_x(i);
    uint16_t x1 = (i + 1 == EFFECT_COUNT) ? LCD_W : tab_x(i + 1);
    uint16_t w  = x1 - x0;
    uint16_t bg = active ? UI_TAB_ACTIVE   : UI_TAB_INACTIVE;
    uint16_t fg = active ? UI_TAB_TEXT_ON  : UI_TAB_TEXT_OFF;
    lcd_fill_rect(x0, 0, w, TAB_H, bg);
    const char *name = effect_names[i];
    int name_len = strlen8(name);
    int max_chars = w / LCD_FONT_W;
    if (name_len > max_chars) name_len = max_chars;
    int nx = x0 + (w - name_len * LCD_FONT_W) / 2;
    int ny = (TAB_H - LCD_FONT_H) / 2;
    /* lcd_draw_text reads up to the null; truncating len means drawing all chars.
       For our names (max 6) and tab width (>=68 px / 8 = 8 chars), truncation
       won't happen — but the clamp keeps us safe. */
    if (name_len == (int)strlen8(name)) {
        lcd_draw_text(nx, ny, name, fg, bg);
    } else {
        for (int k = 0; k < name_len; k++) {
            lcd_draw_char(nx + k * LCD_FONT_W, ny, name[k], fg, bg);
        }
    }
}

static void tabs_draw_all(uint8_t active)
{
    for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
        tab_draw_one(i, i == active);
    }
}

/* ===== Enable indicator ================================================== */

static void enable_indicator_draw(bool enabled)
{
    const char *txt = enabled ? "ON " : "OFF";
    uint16_t fg = enabled ? UI_ENABLE_ON_FG : UI_ENABLE_OFF_FG;
    /* Repaint just the indicator's 3-char strip to avoid touching the page
       name to its left. */
    lcd_fill_rect(ENABLE_TEXT_X, ENABLE_TEXT_Y, 3 * LCD_FONT_W, LCD_FONT_H, UI_TEXT_BG);
    lcd_draw_text(ENABLE_TEXT_X, ENABLE_TEXT_Y, txt, fg, UI_TEXT_BG);
}

/* ===== Page body ========================================================= */

static void page_invalidate(effect_page_t *p)
{
    for (uint8_t i = 0; i < p->slider_count; i++) {
        p->sliders[i].last_bar_w  = INVALIDATE_BAR_W;
        p->sliders[i].last_value  = 999999.0f;
    }
}

static void page_body_redraw(effect_page_t *p)
{
    /* Clear body region. ~480 x 232 = 111360 px x 2 B = ~222 KB. This bursts
       through the SDRAM frame buffer once on a page change — outside the
       VBLANK budget, so brief tearing during a page switch is expected. */
    lcd_fill_rect(0, BODY_TOP, LCD_W, LCD_H - BODY_TOP, UI_TEXT_BG);

    /* Page name centered */
    int len = strlen8(p->name);
    int nx = (LCD_W - len * LCD_FONT_W) / 2;
    lcd_draw_text(nx, PAGE_NAME_Y, p->name, UI_TEXT_FG, UI_TEXT_BG);

    /* Enable indicator on the right of the header row */
    bool en = dsp_chain_get_enabled(p->effect_id);
    enable_indicator_draw(en);
    s_drawn_enabled = en;

    if (p->slider_count == 0) {
        const char *msg = "(not implemented)";
        int mlen = strlen8(msg);
        int mx = (LCD_W - mlen * LCD_FONT_W) / 2;
        int my = (LCD_H + BODY_TOP) / 2 - LCD_FONT_H / 2;
        lcd_draw_text(mx, my, msg, UI_PLACEHOLDER_FG, UI_TEXT_BG);
    }

    page_invalidate(p);
}

/* ===== Touch dispatch ==================================================== */

static int8_t hit_slider(effect_page_t *p, uint16_t y)
{
    /* Generous Y bands around each bar; the bar itself is 30 px tall and the
       user's finger tip is much larger than that. Walks the actual slider
       y positions, so pages with non-default layouts (e.g. EQ's 3 compact
       sliders) work without hard-coded constants. */
    for (uint8_t i = 0; i < p->slider_count; i++) {
        slider_t *s = &p->sliders[i];
        uint16_t top = s->compact ? (s->y - 4) : s->label_y;
        uint16_t bot = s->text_y + LCD_FONT_H + 6;
        if (y >= top && y < bot) return (int8_t)i;
    }
    return -1;
}

static void slider_drag_to(slider_t *s, uint16_t tx)
{
    float t = (float)tx / (float)LCD_W;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float v = t * (s->max - s->min) + s->min;
    if (v != s->value) {
        s->value = v;
        if (s->set_value) s->set_value(v);
    }
}

/* ===== Task ============================================================== */

static void ui_task(void *arg)
{
    (void)arg;

    if (!ft5336_init(&hi2c3)) {
        lcd_draw_text(8, 8, "Touch init failed", LCD_RED, LCD_BLACK);
        vTaskDelete(NULL);
    }

    /* Initial paint — both tab strip and active page body. */
    tabs_draw_all(s_active_page);
    page_body_redraw(&s_pages[s_active_page]);

    bool    prev_touch       = false;
    uint8_t displayed_page   = s_active_page;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(33));

        uint16_t tx, ty;
        bool now_touch = ft5336_read(&tx, &ty);

        /* Tab selection — edge-triggered on touch press inside the tab strip.
           Re-checking each frame would let a held finger flip pages repeatedly. */
        if (now_touch && !prev_touch && ty < TAB_TOUCH_H) {
            uint8_t hit = tab_hit(tx);
            if (hit != s_active_page) {
                s_active_page = hit;
            }
        }

        /* Enable toggle — edge-triggered tap on the bottom-right corner.
           One tap flips the chain enable for the visible effect; next frame's
           redraw refreshes the indicator. The zone sits below all slider
           touch bands, so it never competes with a drag in progress. */
        if (now_touch && !prev_touch
            && tx >= ENABLE_TOUCH_LEFT && ty >= ENABLE_TOUCH_TOP) {
            bool en = dsp_chain_get_enabled(s_active_page);
            dsp_chain_set_enabled(s_active_page, !en);
        }

        /* Slider drag — continuous; updates every frame the touch is held.
           Excludes the enable-toggle corner so a hold in the bottom-right
           doesn't accidentally drag a slider whose touch band reaches that
           far down (relevant for 3-slider pages like EQ). */
        bool in_toggle_corner =
            (tx >= ENABLE_TOUCH_LEFT && ty >= ENABLE_TOUCH_TOP);
        if (now_touch && ty >= BODY_TOP && !in_toggle_corner) {
            effect_page_t *p = &s_pages[s_active_page];
            int8_t si = hit_slider(p, ty);
            if (si >= 0) {
                slider_drag_to(&p->sliders[si], tx);
            }
        }

        prev_touch = now_touch;

        /* Sync rendering to VBLANK. Even page switches happen here — the
           full-body wipe overruns the blanking window, briefly tearing,
           but page switches are rare/user-initiated so we accept it. */
        lcd_wait_vblank();

        if (s_active_page != displayed_page) {
            tab_draw_one(displayed_page, false);
            tab_draw_one(s_active_page,  true);
            page_body_redraw(&s_pages[s_active_page]);
            displayed_page = s_active_page;
        } else {
            /* Same page — repaint only the enable indicator when it changes. */
            bool en = dsp_chain_get_enabled(displayed_page);
            if (en != s_drawn_enabled) {
                enable_indicator_draw(en);
                s_drawn_enabled = en;
            }
        }

        effect_page_t *p = &s_pages[displayed_page];
        for (uint8_t i = 0; i < p->slider_count; i++) {
            slider_draw(&p->sliders[i]);
        }
    }
}

void ui_init(void)
{
    xTaskCreate(ui_task, "UI", 512, NULL, tskIDLE_PRIORITY + 1, NULL);
}
