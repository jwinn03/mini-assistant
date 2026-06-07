#include "ui_page_assistant.h"
#include "lcd.h"
#include "wake_word.h"
#include <stdbool.h>
#include <string.h>

/* Layout. The body region is y in [BODY_TOP, LCD_H); we just inline the same
   value (40) used elsewhere rather than expose it from ui.c. */
#define BODY_TOP_Y         40
#define TITLE_Y            44

#define INDICATOR_X        ((480 / 2) - 12)
#define INDICATOR_Y        130
#define INDICATOR_SIZE     24

/* 33 ms render tick × 15 = ~495 ms flash duration on each wake-fire. */
#define FLASH_TICKS        15u

#define DIAG_X             20
#define DIAG_CONF_Y        180
#define DIAG_FIRES_Y       210
#define DIAG_INFER_Y       240

#define COL_BG             0x0000          /* LCD_BLACK */
#define COL_FG             0xFFFF          /* LCD_WHITE */
#define COL_TITLE          0x07FF          /* LCD_CYAN */
#define COL_DOT_IDLE       0x4208          /* mid-gray */
#define COL_DOT_FIRE       0x07E0          /* green */

/* Local state — what we've already rendered, so each 33 ms tick is delta-only.
   s_drawn_* hold last-painted values; INT32_MAX sentinel forces first draw. */
static uint32_t s_drawn_inferences = 0xFFFFFFFFu;
static uint32_t s_drawn_fires      = 0xFFFFFFFFu;
static int32_t  s_drawn_conf_mille = -100000;
static bool     s_drawn_indicator_on = false;

/* Flash countdown: positive == still showing the green dot. Decremented on
   each tick until it hits zero, then the dot reverts to gray. */
static uint8_t  s_flash_remaining = 0;
static uint32_t s_last_seen_fires = 0;

/* ----- tiny formatting helpers (no libm) -------------------------------- */

static int emit_u32(uint32_t v, char *out)
{
    char buf[12];
    int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    while (v > 0) { buf[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (int i = 0; i < n; i++) out[i] = buf[n - 1 - i];
    out[n] = 0;
    return n;
}

/* Format confidence as "0.XXX" / "1.000". Input is in milli-confidence
   (integer 0..1000); avoids any float printf path. */
static int emit_milli(int32_t mille, char *out)
{
    if (mille < 0) mille = 0;
    if (mille > 1000) mille = 1000;
    int n = 0;
    out[n++] = (char)('0' + (mille / 1000));
    out[n++] = '.';
    int frac = mille % 1000;
    out[n++] = (char)('0' + (frac / 100));
    out[n++] = (char)('0' + ((frac / 10) % 10));
    out[n++] = (char)('0' + (frac % 10));
    out[n]   = 0;
    return n;
}

/* ----- drawing primitives ------------------------------------------------ */

static void draw_indicator(bool on)
{
    uint16_t col = on ? COL_DOT_FIRE : COL_DOT_IDLE;
    lcd_fill_rect(INDICATOR_X, INDICATOR_Y, INDICATOR_SIZE, INDICATOR_SIZE, col);
    s_drawn_indicator_on = on;
}

static void draw_diag_line(uint16_t y, const char *label, const char *value)
{
    /* Wipe the entire row first to clear stale digits when the value shrinks. */
    lcd_fill_rect(DIAG_X, y, 480 - DIAG_X, 16, COL_BG);
    char buf[64];
    int li = 0;
    while (label[li] && li < 30) { buf[li] = label[li]; li++; }
    int vi = 0;
    while (value[vi] && (li + vi) < 60) { buf[li + vi] = value[vi]; vi++; }
    buf[li + vi] = 0;
    lcd_draw_text(DIAG_X, y, buf, COL_FG, COL_BG);
}

/* ----- public API -------------------------------------------------------- */

void ui_page_assistant_redraw(void)
{
    /* Title centred-ish at the top of the body. */
    lcd_draw_text(DIAG_X, TITLE_Y, "Assistant", COL_TITLE, COL_BG);

    /* Force a full repaint of everything below the title. */
    s_drawn_inferences   = 0xFFFFFFFFu;
    s_drawn_fires        = 0xFFFFFFFFu;
    s_drawn_conf_mille   = -100000;
    s_drawn_indicator_on = !s_drawn_indicator_on;   /* force draw_indicator */

    draw_indicator(s_flash_remaining > 0);

    /* Initial diag lines so the user sees something even before the first tick. */
    char num[32];
    emit_u32(wake_word_total_inferences, num);
    draw_diag_line(DIAG_INFER_Y, "Inferences: ", num);
    s_drawn_inferences = wake_word_total_inferences;

    emit_u32(wake_word_total_fires, num);
    draw_diag_line(DIAG_FIRES_Y, "Wake fires: ", num);
    s_drawn_fires = wake_word_total_fires;

    int32_t mille = (int32_t)(wake_word_last_confidence * 1000.0f);
    emit_milli(mille, num);
    draw_diag_line(DIAG_CONF_Y, "Confidence: ", num);
    s_drawn_conf_mille = mille;
}

void ui_page_assistant_tick(void)
{
    /* Wake-fire edge detection. wake_word_total_fires is volatile uint32_t
       written by the wake-word task at idle+2; this 30 Hz reader running at
       idle+1 just polls. A genuine increment kicks the flash counter; the
       counter decays on every tick regardless. */
    uint32_t fires_now = wake_word_total_fires;
    if (fires_now != s_last_seen_fires) {
        s_flash_remaining = FLASH_TICKS;
        s_last_seen_fires = fires_now;
    }

    bool indicator_on = (s_flash_remaining > 0);
    if (indicator_on != s_drawn_indicator_on) {
        draw_indicator(indicator_on);
    }
    if (s_flash_remaining > 0) s_flash_remaining--;

    /* Diagnostic updates — delta-only so the LCD doesn't churn when nothing
       interesting is happening. */
    uint32_t inferences_now = wake_word_total_inferences;
    if (inferences_now != s_drawn_inferences) {
        char num[16];
        emit_u32(inferences_now, num);
        draw_diag_line(DIAG_INFER_Y, "Inferences: ", num);
        s_drawn_inferences = inferences_now;
    }

    if (fires_now != s_drawn_fires) {
        char num[16];
        emit_u32(fires_now, num);
        draw_diag_line(DIAG_FIRES_Y, "Wake fires: ", num);
        s_drawn_fires = fires_now;
    }

    int32_t mille = (int32_t)(wake_word_last_confidence * 1000.0f);
    if (mille != s_drawn_conf_mille) {
        char num[16];
        emit_milli(mille, num);
        draw_diag_line(DIAG_CONF_Y, "Confidence: ", num);
        s_drawn_conf_mille = mille;
    }
}

void ui_page_assistant_touch(uint16_t tx, uint16_t ty, bool edge)
{
    /* Stub has no interactive controls. Phase 7 will use this hook for the
       utterance-capture transport. */
    (void)tx; (void)ty; (void)edge;
}
