#include "ui_page_record.h"
#include "lcd.h"
#include "recorder.h"
#include "player.h"
#include "sd_card.h"
#include <string.h>

/* ===== Layout ============================================================
   Body region: y = 40..272 (after the 32 px tab strip + 8 px gap).

     y=44       title "Record"                                   TAP:POST  (right)
     y=80..136  [REC]     [STOP]    [PLAY]      (three 140x56 buttons)
     y=152..168 status: "Rec 00:12.345  *Peak*"
     y=176..192 peak meter bar
     y=200..272 file list (3 entries, 16 px tall + 4 px gap)

   The TAB strip drawing (y=0..31) is owned by ui.c; we only touch y>=40. */

#define TAB_H                32
#define BODY_TOP             (TAB_H + 8)

#define TITLE_Y              44
#define TAP_TOGGLE_X         (LCD_W - 8 * LCD_FONT_W - 4)   /* right-aligned "TAP:POST" (8 chars) */
#define TAP_TOGGLE_Y         TITLE_Y

#define BTN_Y                80
#define BTN_H                56
#define BTN_W                140
#define BTN_GAP              5
#define BTN_REC_X            10
#define BTN_STOP_X           (BTN_REC_X + BTN_W + BTN_GAP)
#define BTN_PLAY_X           (BTN_STOP_X + BTN_W + BTN_GAP)
#define BTN_TEXT_Y           (BTN_Y + (BTN_H - LCD_FONT_H) / 2)

#define STATUS_Y             152
#define PEAK_BAR_Y           176
#define PEAK_BAR_H           12

#define FILE_LIST_Y          200
#define FILE_LIST_ROW_H      22
#define FILE_LIST_MAX        3

/* ===== Colors ============================================================ */

#define COL_BG               LCD_BLACK
#define COL_FG               LCD_WHITE
#define COL_DIM              LCD_GRAY
#define COL_BTN_IDLE         LCD_DKGRAY
#define COL_BTN_REC_LIVE     LCD_RED
#define COL_BTN_REC_ARM      LCD_YELLOW
#define COL_BTN_PLAY_LIVE    LCD_GREEN
#define COL_BTN_DISABLED     0x2104          /* very dim */
#define COL_BTN_TEXT_ON      LCD_BLACK
#define COL_BTN_TEXT_OFF     LCD_LTGRAY
#define COL_FILE_SEL         LCD_CYAN
#define COL_FILE_SEL_TEXT    LCD_BLACK
#define COL_TAP              LCD_YELLOW
#define COL_PEAK_BG          LCD_DKGRAY
#define COL_PEAK_FG          LCD_GREEN

/* ===== Drawn state cache ================================================= */

/* Track what's currently on screen so the per-frame tick only repaints
   what's changed. Reset by ui_page_record_redraw() on tab enter. */
static recorder_state_t s_drawn_rec_state;
static player_state_t   s_drawn_play_state;
static uint32_t         s_drawn_elapsed_ms;
static uint32_t         s_drawn_position_ms;
static uint16_t         s_drawn_peak_w;
static recorder_tap_t   s_drawn_tap;
static bool             s_drawn_sd;

/* File list cache. Populated from sd_card_scan_recordings(). UI selection
   index points into this array. */
static char s_files[FILE_LIST_MAX][SD_REC_FILENAME_MAX];
static int  s_file_count;
static int  s_sel_idx;                 /* -1 = none */
static int  s_drawn_sel_idx;

/* ===== Helpers =========================================================== */

static int strlen8(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Format ms as MM:SS.mmm into a 10-char buffer (incl. NUL). Caps mins at 99. */
static void format_time_ms(uint32_t ms, char *out)
{
    uint32_t total_s = ms / 1000u;
    uint32_t mins = total_s / 60u;
    uint32_t secs = total_s % 60u;
    uint32_t milli = ms % 1000u;
    if (mins > 99) mins = 99;
    out[0] = (char)('0' + (mins / 10) % 10);
    out[1] = (char)('0' + (mins) % 10);
    out[2] = ':';
    out[3] = (char)('0' + (secs / 10) % 10);
    out[4] = (char)('0' + (secs) % 10);
    out[5] = '.';
    out[6] = (char)('0' + (milli / 100) % 10);
    out[7] = (char)('0' + (milli / 10)  % 10);
    out[8] = (char)('0' + (milli)       % 10);
    out[9] = '\0';
}

static bool hit_button(uint16_t tx, uint16_t ty, uint16_t x)
{
    return tx >= x && tx < x + BTN_W && ty >= BTN_Y && ty < BTN_Y + BTN_H;
}

/* ===== Button drawing ==================================================== */

static void draw_button(uint16_t x, uint16_t fill, uint16_t text_color, const char *label)
{
    lcd_fill_rect(x, BTN_Y, BTN_W, BTN_H, fill);
    int len = strlen8(label);
    int lx = x + (BTN_W - len * LCD_FONT_W) / 2;
    lcd_draw_text(lx, BTN_TEXT_Y, label, text_color, fill);
}

static void draw_rec_button(recorder_state_t st, bool can_use)
{
    uint16_t fill;
    uint16_t text;
    const char *label = "REC";
    if (!can_use) {
        fill = COL_BTN_DISABLED; text = COL_BTN_TEXT_OFF;
    } else if (st == RECORDER_STATE_ARMED) {
        fill = COL_BTN_REC_ARM;  text = COL_BTN_TEXT_ON;
        label = "ARM";
    } else if (st == RECORDER_STATE_RECORDING || st == RECORDER_STATE_STOPPING) {
        fill = COL_BTN_REC_LIVE; text = COL_BTN_TEXT_ON;
    } else if (st == RECORDER_STATE_ERROR) {
        fill = LCD_MAGENTA;      text = COL_BTN_TEXT_ON;
        label = "ERR";
    } else {
        fill = COL_BTN_IDLE;     text = COL_BTN_TEXT_OFF;
    }
    draw_button(BTN_REC_X, fill, text, label);
}

static void draw_stop_button(bool any_active)
{
    uint16_t fill = any_active ? COL_BTN_IDLE     : COL_BTN_DISABLED;
    uint16_t text = any_active ? COL_BTN_TEXT_OFF : COL_BTN_TEXT_OFF;
    draw_button(BTN_STOP_X, fill, text, "STOP");
}

static void draw_play_button(player_state_t st, bool can_use)
{
    uint16_t fill;
    uint16_t text;
    if (!can_use) {
        fill = COL_BTN_DISABLED; text = COL_BTN_TEXT_OFF;
    } else if (st == PLAYER_STATE_PLAYING || st == PLAYER_STATE_LOADING) {
        fill = COL_BTN_PLAY_LIVE; text = COL_BTN_TEXT_ON;
    } else if (st == PLAYER_STATE_ERROR) {
        fill = LCD_MAGENTA;       text = COL_BTN_TEXT_ON;
    } else {
        fill = COL_BTN_IDLE;      text = COL_BTN_TEXT_OFF;
    }
    draw_button(BTN_PLAY_X, fill, text, "PLAY");
}

/* ===== Status row + peak meter =========================================== */

static void draw_status_row(recorder_state_t rs, player_state_t ps,
                            uint32_t elapsed_ms, uint32_t position_ms)
{
    char line[40];
    int p = 0;
    const char *prefix;
    uint32_t ms;

    if (rs == RECORDER_STATE_RECORDING || rs == RECORDER_STATE_STOPPING) {
        prefix = "Rec ";
        ms = elapsed_ms;
    } else if (ps == PLAYER_STATE_PLAYING) {
        prefix = "Play ";
        ms = position_ms;
    } else {
        prefix = "Idle  ";
        ms = 0;
    }
    while (*prefix) line[p++] = *prefix++;
    char ts[10];
    format_time_ms(ms, ts);
    int k = 0; while (ts[k]) line[p++] = ts[k++];
    line[p] = '\0';

    lcd_fill_rect(0, STATUS_Y, LCD_W, LCD_FONT_H, COL_BG);
    lcd_draw_text(8, STATUS_Y, line, COL_FG, COL_BG);
}

static void draw_peak_bar(uint16_t w_px)
{
    if (w_px > LCD_W) w_px = LCD_W;
    /* Delta-only — fill or clear the changed segment. */
    if (w_px > s_drawn_peak_w) {
        lcd_fill_rect(s_drawn_peak_w, PEAK_BAR_Y, w_px - s_drawn_peak_w, PEAK_BAR_H, COL_PEAK_FG);
    } else if (w_px < s_drawn_peak_w) {
        lcd_fill_rect(w_px, PEAK_BAR_Y, s_drawn_peak_w - w_px, PEAK_BAR_H, COL_PEAK_BG);
    }
    s_drawn_peak_w = w_px;
}

/* ===== File list ========================================================= */

static void draw_file_row(int slot, bool selected)
{
    uint16_t y = FILE_LIST_Y + slot * FILE_LIST_ROW_H;
    uint16_t bg = selected ? COL_FILE_SEL : COL_BG;
    uint16_t fg = selected ? COL_FILE_SEL_TEXT : COL_FG;
    lcd_fill_rect(0, y, LCD_W, FILE_LIST_ROW_H, bg);
    if (slot < s_file_count) {
        lcd_draw_text(16, y + (FILE_LIST_ROW_H - LCD_FONT_H) / 2,
                      s_files[slot], fg, bg);
    } else {
        /* Empty slot — leave blank. */
    }
}

static void draw_file_list(void)
{
    lcd_fill_rect(0, FILE_LIST_Y, LCD_W, FILE_LIST_ROW_H * FILE_LIST_MAX, COL_BG);
    if (s_file_count == 0) {
        const char *msg = sd_card_mounted() ? "(no recordings)" : "(no SD card)";
        int len = strlen8(msg);
        int x = (LCD_W - len * LCD_FONT_W) / 2;
        lcd_draw_text(x, FILE_LIST_Y + (FILE_LIST_ROW_H - LCD_FONT_H) / 2,
                      msg, COL_DIM, COL_BG);
        return;
    }
    for (int i = 0; i < s_file_count; i++) {
        draw_file_row(i, i == s_sel_idx);
    }
}

/* ===== Tap toggle ======================================================== */

static void draw_tap_toggle(recorder_tap_t tap)
{
    const char *txt = (tap == RECORDER_TAP_POST) ? "TAP:POST" : "TAP:PRE ";
    lcd_fill_rect(TAP_TOGGLE_X, TAP_TOGGLE_Y, 8 * LCD_FONT_W, LCD_FONT_H, COL_BG);
    lcd_draw_text(TAP_TOGGLE_X, TAP_TOGGLE_Y, txt, COL_TAP, COL_BG);
}

static bool hit_tap_toggle(uint16_t tx, uint16_t ty)
{
    return tx >= TAP_TOGGLE_X && ty >= TAP_TOGGLE_Y && ty < TAP_TOGGLE_Y + LCD_FONT_H + 4;
}

/* ===== File list management ============================================== */

static void refresh_file_list(void)
{
    int n = 0;
    sd_card_scan_recordings(s_files, FILE_LIST_MAX, &n);
    s_file_count = n;
    if (s_sel_idx >= n) s_sel_idx = (n > 0) ? n - 1 : -1;
    if (s_sel_idx < 0 && n > 0) s_sel_idx = n - 1;       /* default to most-recent */
}

/* ===== Public API ======================================================== */

void ui_page_record_redraw(void)
{
    /* Full body wipe + repaint. ui.c clears the body before calling us;
       we just lay out our content. */

    /* Title centered */
    const char *title = "Record";
    int tlen = strlen8(title);
    int tx   = (LCD_W - tlen * LCD_FONT_W) / 2;
    lcd_draw_text(tx, TITLE_Y, title, COL_FG, COL_BG);

    /* Reset draw cache so every sub-region paints on first tick. */
    s_drawn_rec_state   = (recorder_state_t)0xFF;
    s_drawn_play_state  = (player_state_t)0xFF;
    s_drawn_elapsed_ms  = 0xFFFFFFFFu;
    s_drawn_position_ms = 0xFFFFFFFFu;
    s_drawn_peak_w      = 0;
    s_drawn_tap         = (recorder_tap_t)0xFF;
    s_drawn_sd          = sd_card_mounted();
    s_drawn_sel_idx     = -2;

    /* Peak meter background — full strip painted dark. The tick paints the
       lit segment on top. */
    lcd_fill_rect(0, PEAK_BAR_Y, LCD_W, PEAK_BAR_H, COL_PEAK_BG);

    refresh_file_list();
    draw_file_list();
    s_drawn_sel_idx = s_sel_idx;
}

void ui_page_record_tick(void)
{
    recorder_state_t rs   = recorder_get_state();
    player_state_t   ps   = player_get_state();
    bool             sd   = sd_card_mounted();
    recorder_tap_t   tap  = recorder_get_tap();
    uint32_t         e_ms = recorder_get_elapsed_ms();
    uint32_t         p_ms = player_get_position_ms();
    q15_t            peak = recorder_get_peak();

    /* Card state change → refresh the file list. */
    if (sd != s_drawn_sd) {
        refresh_file_list();
        draw_file_list();
        s_drawn_sel_idx = s_sel_idx;
        s_drawn_sd      = sd;
    }

    /* Recording just finished — the FAT was updated by f_close, so a fresh
       scan will now see the new file. */
    if (rs == RECORDER_STATE_IDLE
        && (s_drawn_rec_state == RECORDER_STATE_RECORDING
            || s_drawn_rec_state == RECORDER_STATE_STOPPING)) {
        refresh_file_list();
        draw_file_list();
        s_drawn_sel_idx = s_sel_idx;
    }

    /* Repaint buttons on transport state change. The disabled-state of
       REC/PLAY depends on whether the OTHER one is active, so any change
       triggers the trio. */
    bool buttons_dirty = (rs != s_drawn_rec_state) || (ps != s_drawn_play_state);
    if (buttons_dirty) {
        bool rec_can_use  = sd && (ps == PLAYER_STATE_IDLE);
        bool play_can_use = sd && (rs == RECORDER_STATE_IDLE) && (s_sel_idx >= 0);
        bool any_active   = (rs != RECORDER_STATE_IDLE && rs != RECORDER_STATE_ERROR) ||
                            (ps != PLAYER_STATE_IDLE   && ps != PLAYER_STATE_ERROR);
        draw_rec_button(rs, rec_can_use);
        draw_stop_button(any_active);
        draw_play_button(ps, play_can_use);
        s_drawn_rec_state  = rs;
        s_drawn_play_state = ps;
    }

    /* Status row repaints when its driving values change. */
    if (e_ms != s_drawn_elapsed_ms || p_ms != s_drawn_position_ms || buttons_dirty) {
        draw_status_row(rs, ps, e_ms, p_ms);
        s_drawn_elapsed_ms  = e_ms;
        s_drawn_position_ms = p_ms;
    }

    /* Peak meter — scale q15 to bar width. Apply a simple decay if the new
       sample is below the drawn level so the bar smooths down naturally.
       (recorder_get_peak() is read-and-reset; subsequent ticks see 0 if
       audio went quiet.) */
    uint16_t target_w = (uint16_t)(((uint32_t)peak * LCD_W) >> 15);
    if (target_w < s_drawn_peak_w) {
        /* Decay: shrink by ~6 px per 33 ms tick → 180 px/s. */
        uint16_t step = 6;
        if (s_drawn_peak_w - target_w > step) {
            target_w = s_drawn_peak_w - step;
        }
    }
    draw_peak_bar(target_w);

    /* Tap toggle. */
    if (tap != s_drawn_tap) {
        draw_tap_toggle(tap);
        s_drawn_tap = tap;
    }

    /* File-list selection change. */
    if (s_sel_idx != s_drawn_sel_idx) {
        if (s_drawn_sel_idx >= 0 && s_drawn_sel_idx < s_file_count) {
            draw_file_row(s_drawn_sel_idx, false);
        }
        if (s_sel_idx >= 0 && s_sel_idx < s_file_count) {
            draw_file_row(s_sel_idx, true);
        }
        s_drawn_sel_idx = s_sel_idx;
    }
}

void ui_page_record_touch(uint16_t tx, uint16_t ty, bool edge)
{
    if (!edge) return;       /* this page is all buttons / list — no drag */

    /* Tap source toggle — top-right corner, only while idle (recorder
       enforces this internally, but checking here gives instant feedback). */
    if (hit_tap_toggle(tx, ty)) {
        recorder_tap_t cur = recorder_get_tap();
        recorder_set_tap(cur == RECORDER_TAP_POST ? RECORDER_TAP_PRE : RECORDER_TAP_POST);
        return;
    }

    /* Transport buttons. */
    if (hit_button(tx, ty, BTN_REC_X)) {
        if (!sd_card_mounted()) return;
        if (player_get_state() != PLAYER_STATE_IDLE) return;
        recorder_start();
        /* New recording will appear when it's saved — refresh on next mount/idle. */
        return;
    }
    if (hit_button(tx, ty, BTN_STOP_X)) {
        if (recorder_get_state() != RECORDER_STATE_IDLE) {
            recorder_stop();
            /* File list refresh deferred to next tick — STOPPING transitions
               to IDLE asynchronously, and the new file isn't visible to
               f_readdir until after the final close. */
        } else if (player_get_state() != PLAYER_STATE_IDLE) {
            player_stop();
        }
        return;
    }
    if (hit_button(tx, ty, BTN_PLAY_X)) {
        if (!sd_card_mounted()) return;
        if (recorder_get_state() != RECORDER_STATE_IDLE) return;
        if (s_sel_idx < 0 || s_sel_idx >= s_file_count) return;
        player_start(s_files[s_sel_idx]);
        return;
    }

    /* File list hit-test. */
    if (ty >= FILE_LIST_Y && ty < FILE_LIST_Y + FILE_LIST_ROW_H * FILE_LIST_MAX) {
        int slot = (ty - FILE_LIST_Y) / FILE_LIST_ROW_H;
        if (slot < s_file_count) {
            s_sel_idx = slot;
        }
    }
}
