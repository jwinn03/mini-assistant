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
#include "ui_page_record.h"
#include "ui_page_assistant.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>

/* Four top-level tabs: Volume (the gain stage, previously the "Gain" tab),
   Effects (a left-edge submenu selects one of the six other effects, whose
   sliders render in the remaining area), Record and Assistant (delegated to
   their ui_page_* modules, unchanged). 480/4 = 120 px per tab — double the
   tap target of the old 9-tab strip. The effect page definitions themselves
   (s_pages) are untouched; only the framing around them changed. */
#define UI_TAB_COUNT       4
#define UI_TAB_VOLUME      0
#define UI_TAB_EFFECTS     1
#define UI_TAB_RECORD      2
#define UI_TAB_ASSISTANT   3

extern I2C_HandleTypeDef hi2c3;

/* ===== Layout ============================================================ */

#define TAB_H              32
#define TAB_TOUCH_H        36       /* slight overshoot so finger-up at row 33 still counts */
#define BODY_TOP           (TAB_H + 8)
#define PAGE_NAME_Y        44
/* Enable toggle switch + its touch zone live in the bottom-right corner,
   below the slider 3 readout (which ends at y=244) and to the right of
   center (x>=240). This avoids overlap with the slider zones and keeps the
   toggle reachable on every effect page. */
#define ENABLE_TOG_X       (LCD_W - LCD_TOGGLE_W - 8)
#define ENABLE_TOG_Y       (LCD_H - LCD_TOGGLE_H - 6)
#define ENABLE_TOUCH_TOP   (LCD_H - 28)
#define ENABLE_TOUCH_LEFT  (LCD_W / 2)
/* Effects-tab submenu: one entry per non-Gain effect down the left edge.
   6 entries × 38 px fill the 232 px body almost exactly. Entries are drawn
   like vertical tabs (same active/inactive colours as the top strip) with a
   small green dot showing each effect's enable state at a glance. */
#define SUBMENU_W          96
#define SUBMENU_ITEM_H     38
#define FX_MENU_COUNT      (EFFECT_COUNT - 1)   /* Clip..Reverb */
#define AREA_GAP           6        /* gap between submenu and slider area */
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
#define UI_ENABLE_ON_FG    LCD_GREEN

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

static uint8_t s_active_tab = UI_TAB_VOLUME;
/* Tracks what's actually on screen for the displayed page's enable toggle,
   so we only repaint it when it changes. Tab switch resets it via
   tab_body_redraw. */
static bool    s_drawn_enabled = false;

/* Effects-tab submenu selection (0..FX_MENU_COUNT-1 → effect_id = sel+1).
   Static so the tab remembers the last-viewed effect across tab switches. */
static uint8_t s_fx_sel       = 0;              /* boot default: Clip */
static uint8_t s_drawn_fx_sel = 0xFF;           /* forces first submenu draw */
static bool    s_drawn_menu_en[FX_MENU_COUNT];  /* per-entry enable-dot cache */

/* Active slider area (x origin + width). Volume tab: the full screen, exactly
   the old layout. Effects tab: the region right of the submenu. Set by
   set_area() before any slider_* call so the slider widget code stays
   area-relative instead of assuming x=0..LCD_W. */
static uint16_t s_area_x0 = 0;
static uint16_t s_area_w  = LCD_W;

static void set_area(void)
{
    if (s_active_tab == UI_TAB_EFFECTS) {
        s_area_x0 = SUBMENU_W + AREA_GAP;
        s_area_w  = (uint16_t)(LCD_W - (SUBMENU_W + AREA_GAP));
    } else {
        s_area_x0 = 0;
        s_area_w  = LCD_W;
    }
}

/* The effect page the Effects tab currently shows. */
static effect_page_t *fx_page(void)
{
    return &s_pages[s_fx_sel + 1];
}

/* ===== Helpers =========================================================== */

static int strlen8(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Tab horizontal extents (proportional): 480/4 = 120 px per tab. */
static inline uint16_t tab_x(uint8_t i)
{
    return (uint16_t)((uint32_t)i * LCD_W / UI_TAB_COUNT);
}

/* Hit-test the X coord to a tab index. */
static inline uint8_t tab_hit(uint16_t x)
{
    uint8_t i = (uint8_t)((uint32_t)x * UI_TAB_COUNT / LCD_W);
    if (i >= UI_TAB_COUNT) i = UI_TAB_COUNT - 1;
    return i;
}

static const char * const s_tab_names[UI_TAB_COUNT] = {
    "Volume", "Effects", "Record", "Assistant"
};

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

/* All slider geometry below is relative to the active area (s_area_x0 /
   s_area_w): the full screen on the Volume tab, the region right of the
   submenu on the Effects tab. */
static uint16_t slider_bar_width(const slider_t *s)
{
    float v = s->value;
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    float t = (v - s->min) / (s->max - s->min);
    return (uint16_t)(t * s_area_w);
}

static void slider_draw(slider_t *s)
{
    bool full = (s->last_bar_w == INVALIDATE_BAR_W);
    uint16_t bar_w = slider_bar_width(s);

    if (full) {
        /* Label row — only when not compact. */
        if (!s->compact && s->label) {
            int len = strlen8(s->label);
            int lx = s_area_x0 + (s_area_w - len * LCD_FONT_W) / 2;
            lcd_fill_rect(s_area_x0, s->label_y, s_area_w, LCD_FONT_H, UI_TEXT_BG);
            lcd_draw_text(lx, s->label_y, s->label, UI_TEXT_FG, UI_TEXT_BG);
        }
        /* Bar full repaint: bg then fg */
        lcd_fill_rect(s_area_x0, s->y, s_area_w, SLIDER_H, UI_BAR_BG);
        if (bar_w > 0) {
            lcd_fill_rect(s_area_x0, s->y, bar_w, SLIDER_H, UI_BAR_FG);
        }
    } else if (bar_w > s->last_bar_w) {
        lcd_fill_rect(s_area_x0 + s->last_bar_w, s->y,
                      bar_w - s->last_bar_w, SLIDER_H, UI_BAR_FG);
    } else if (bar_w < s->last_bar_w) {
        lcd_fill_rect(s_area_x0 + bar_w, s->y,
                      s->last_bar_w - bar_w, SLIDER_H, UI_BAR_BG);
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
        int tx = s_area_x0 + (s_area_w - len * LCD_FONT_W) / 2;
        lcd_fill_rect(s_area_x0, s->text_y, s_area_w, LCD_FONT_H, UI_TEXT_BG);
        lcd_draw_text(tx, s->text_y, txt, UI_TEXT_FG, UI_TEXT_BG);
        s->last_value = s->value;
    }
}

/* ===== Tab strip ========================================================= */

static void tab_draw_one(uint8_t i, bool active)
{
    uint16_t x0 = tab_x(i);
    uint16_t x1 = (i + 1 == UI_TAB_COUNT) ? LCD_W : tab_x(i + 1);
    uint16_t w  = x1 - x0;
    uint16_t bg = active ? UI_TAB_ACTIVE   : UI_TAB_INACTIVE;
    uint16_t fg = active ? UI_TAB_TEXT_ON  : UI_TAB_TEXT_OFF;
    lcd_fill_rect(x0, 0, w, TAB_H, bg);
    const char *name = s_tab_names[i];
    int name_len = strlen8(name);
    int max_chars = w / LCD_FONT_W;
    if (name_len > max_chars) name_len = max_chars;
    int nx = x0 + (w - name_len * LCD_FONT_W) / 2;
    int ny = (TAB_H - LCD_FONT_H) / 2;
    /* 120 px tabs fit 15 chars — every label fits whole, but keep the clamp
       so a future longer label can't spill into the next tab. */
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
    for (uint8_t i = 0; i < UI_TAB_COUNT; i++) {
        tab_draw_one(i, i == active);
    }
}

/* ===== Enable toggle ===================================================== */

static void enable_indicator_draw(bool enabled)
{
    lcd_draw_toggle(ENABLE_TOG_X, ENABLE_TOG_Y, enabled);
}

/* ===== Effects submenu =================================================== */

static void submenu_item_draw(uint8_t i, bool selected)
{
    uint16_t y0 = (uint16_t)(BODY_TOP + i * SUBMENU_ITEM_H);
    uint16_t bg = selected ? UI_TAB_ACTIVE  : UI_TAB_INACTIVE;
    uint16_t fg = selected ? UI_TAB_TEXT_ON : UI_TAB_TEXT_OFF;

    lcd_fill_rect(0, y0, SUBMENU_W, SUBMENU_ITEM_H - 2, bg);
    lcd_draw_text(6, y0 + (SUBMENU_ITEM_H - 2 - LCD_FONT_H) / 2,
                  effect_names[i + 1], fg, bg);

    /* Enable-state dot at the right edge: green when the effect is in the
       chain, background-coloured (invisible) when bypassed. */
    bool en = dsp_chain_get_enabled((uint8_t)(i + 1));
    lcd_fill_rect(SUBMENU_W - 16, y0 + (SUBMENU_ITEM_H - 2) / 2 - 4, 8, 8,
                  en ? UI_ENABLE_ON_FG : bg);
    s_drawn_menu_en[i] = en;
}

static void submenu_draw_all(void)
{
    for (uint8_t i = 0; i < FX_MENU_COUNT; i++) {
        submenu_item_draw(i, i == s_fx_sel);
    }
    s_drawn_fx_sel = s_fx_sel;
}

/* ===== Page body ========================================================= */

static void page_invalidate(effect_page_t *p)
{
    for (uint8_t i = 0; i < p->slider_count; i++) {
        p->sliders[i].last_bar_w  = INVALIDATE_BAR_W;
        p->sliders[i].last_value  = 999999.0f;
    }
}

/* Draw an effect page's static furniture into the current slider area (no
   wipe — callers clear the region first): centered title, enable toggle,
   and slider invalidation so the next tick repaints them. `title` is the
   tab/submenu-facing name ("Volume" for the gain page). */
static void effect_content_draw(effect_page_t *p, const char *title)
{
    int len = strlen8(title);
    int nx = s_area_x0 + (s_area_w - len * LCD_FONT_W) / 2;
    lcd_draw_text(nx, PAGE_NAME_Y, title, UI_TEXT_FG, UI_TEXT_BG);

    bool en = dsp_chain_get_enabled(p->effect_id);
    enable_indicator_draw(en);
    s_drawn_enabled = en;

    page_invalidate(p);
}

/* Full body repaint for the active tab. ~480 x 232 px wipe bursts through
   the SDRAM frame buffer — outside the VBLANK budget, so brief tearing on a
   tab switch is expected (rare, user-initiated). */
static void tab_body_redraw(void)
{
    lcd_fill_rect(0, BODY_TOP, LCD_W, LCD_H - BODY_TOP, UI_TEXT_BG);
    set_area();
    switch (s_active_tab) {
    case UI_TAB_VOLUME:
        effect_content_draw(&s_pages[EFFECT_ID_GAIN], "Volume");
        break;
    case UI_TAB_EFFECTS:
        submenu_draw_all();
        effect_content_draw(fx_page(), fx_page()->name);
        break;
    case UI_TAB_RECORD:
        ui_page_record_redraw();
        break;
    default:
        ui_page_assistant_redraw();
        break;
    }
}

/* Submenu selection changed within the Effects tab: restyle the two menu
   entries and repaint only the content area right of the submenu. */
static void effects_content_switch(void)
{
    if (s_drawn_fx_sel != 0xFF && s_drawn_fx_sel != s_fx_sel) {
        submenu_item_draw(s_drawn_fx_sel, false);
    }
    submenu_item_draw(s_fx_sel, true);
    s_drawn_fx_sel = s_fx_sel;

    lcd_fill_rect(SUBMENU_W, BODY_TOP, LCD_W - SUBMENU_W, LCD_H - BODY_TOP,
                  UI_TEXT_BG);
    effect_content_draw(fx_page(), fx_page()->name);
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
    float t = (float)((int)tx - (int)s_area_x0) / (float)s_area_w;
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

    /* Initial paint — both tab strip and active tab body. */
    tabs_draw_all(s_active_tab);
    tab_body_redraw();

    bool    prev_touch    = false;
    uint8_t displayed_tab = s_active_tab;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(33));

        uint16_t tx, ty;
        bool now_touch = ft5336_read(&tx, &ty);
        bool edge      = (now_touch && !prev_touch);

        /* Tab selection — edge-triggered on touch press inside the tab strip.
           Re-checking each frame would let a held finger flip tabs repeatedly. */
        if (edge && ty < TAB_TOUCH_H) {
            uint8_t hit = tab_hit(tx);
            if (hit != s_active_tab) {
                s_active_tab = hit;
            }
        }

        if (s_active_tab == UI_TAB_VOLUME || s_active_tab == UI_TAB_EFFECTS) {
            set_area();
            effect_page_t *p = (s_active_tab == UI_TAB_VOLUME)
                                   ? &s_pages[EFFECT_ID_GAIN] : fx_page();

            /* Submenu selection (Effects tab): edge-tap in the left column. */
            if (s_active_tab == UI_TAB_EFFECTS && edge &&
                tx < SUBMENU_W && ty >= BODY_TOP) {
                uint8_t item = (uint8_t)((ty - BODY_TOP) / SUBMENU_ITEM_H);
                if (item < FX_MENU_COUNT) {
                    s_fx_sel = item;
                }
            }

            /* Enable toggle: bottom-right corner tap. */
            if (edge && tx >= ENABLE_TOUCH_LEFT && ty >= ENABLE_TOUCH_TOP) {
                bool en = dsp_chain_get_enabled(p->effect_id);
                dsp_chain_set_enabled(p->effect_id, !en);
            }

            /* Slider drag — only inside the slider area (right of the
               submenu on the Effects tab) and outside the toggle corner. */
            bool in_toggle_corner =
                (tx >= ENABLE_TOUCH_LEFT && ty >= ENABLE_TOUCH_TOP);
            if (now_touch && ty >= BODY_TOP && tx >= s_area_x0 &&
                !in_toggle_corner) {
                int8_t si = hit_slider(p, ty);
                if (si >= 0) {
                    slider_drag_to(&p->sliders[si], tx);
                }
            }
        } else if (s_active_tab == UI_TAB_RECORD) {
            /* Record page: delegate to its hit-test. */
            if (now_touch && ty >= BODY_TOP) {
                ui_page_record_touch(tx, ty, edge);
            }
        } else {
            if (now_touch && ty >= BODY_TOP) {
                ui_page_assistant_touch(tx, ty, edge);
            }
        }

        prev_touch = now_touch;

        /* Sync rendering to VBLANK. Even tab switches happen here — the
           full-body wipe overruns the blanking window, briefly tearing,
           but tab switches are rare/user-initiated so we accept it. */
        lcd_wait_vblank();

        if (s_active_tab != displayed_tab) {
            tab_draw_one(displayed_tab, false);
            tab_draw_one(s_active_tab,  true);
            tab_body_redraw();
            displayed_tab = s_active_tab;
        } else if (displayed_tab == UI_TAB_EFFECTS &&
                   s_fx_sel != s_drawn_fx_sel) {
            /* Submenu selection changed — partial repaint, same tab. */
            set_area();
            effects_content_switch();
        }

        if (displayed_tab == UI_TAB_VOLUME || displayed_tab == UI_TAB_EFFECTS) {
            set_area();
            effect_page_t *p = (displayed_tab == UI_TAB_VOLUME)
                                   ? &s_pages[EFFECT_ID_GAIN] : fx_page();

            /* Enable toggle — repaint on change. */
            bool en = dsp_chain_get_enabled(p->effect_id);
            if (en != s_drawn_enabled) {
                enable_indicator_draw(en);
                s_drawn_enabled = en;
            }

            for (uint8_t i = 0; i < p->slider_count; i++) {
                slider_draw(&p->sliders[i]);
            }

            /* Submenu enable dots — delta-repaint entries whose effect was
               toggled (the current page's toggle is the only writer today,
               but the check is cheap and future-proof). */
            if (displayed_tab == UI_TAB_EFFECTS) {
                for (uint8_t i = 0; i < FX_MENU_COUNT; i++) {
                    bool e2 = dsp_chain_get_enabled((uint8_t)(i + 1));
                    if (e2 != s_drawn_menu_en[i]) {
                        submenu_item_draw(i, i == s_drawn_fx_sel);
                    }
                }
            }
        } else if (displayed_tab == UI_TAB_RECORD) {
            ui_page_record_tick();
        } else {
            ui_page_assistant_tick();
        }
    }
}

void ui_init(void)
{
    xTaskCreate(ui_task, "UI", 512, NULL, tskIDLE_PRIORITY + 1, NULL);
}
