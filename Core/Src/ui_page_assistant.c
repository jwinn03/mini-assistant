#include "ui_page_assistant.h"
#include "lcd.h"
#include "wake_word.h"
#include "utterance.h"
#include "net.h"
#include <stdbool.h>
#include <string.h>

/* VAD energy/floor tuning readout (Phase 7 calibration aid). Flip to 1 to
   restore the live "E:<energy> F:<floor>" line on the Assist tab — handy if the
   VAD ever needs re-tuning against real mic levels. Disabled now that
   VAD_ABS_OFFSET / VAD_MARGIN_NUM are dialed in. Gating it (rather than
   deleting) keeps the code compilable and one flag away from coming back. */
#define ASSIST_VAD_READOUT 0

#if ASSIST_VAD_READOUT
#include "vad.h"
#endif

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

/* Phase 7 capture readouts: the utterance state line and a captures/rejects
   tally, above the wake-word indicator dot. */
#define STATE_Y            72
#define CAPMETA_Y          100

#if ASSIST_VAD_READOUT
/* VAD tuning aid: live frame energy vs. adaptive noise floor. Slots just below
   the indicator dot (130..154). */
#define VAD_Y              158
/* VAD aid owns the 158 slot; push the network line to the bottom row. */
#define NET_Y              256
#else
/* Phase 8: network status line in the slot the (disabled) VAD readout freed. */
#define NET_Y              158
#endif
#define NET_STR_CAP        40

#define COL_BG             0x0000          /* LCD_BLACK */
#define COL_FG             0xFFFF          /* LCD_WHITE */
#define COL_TITLE          0x07FF          /* LCD_CYAN */
#define COL_DOT_IDLE       0x4208          /* mid-gray */
#define COL_DOT_FIRE       0x07E0          /* green */

#define COL_STATE_LISTEN   0xFFFF          /* white  — ARMED / listening */
#define COL_STATE_CAPTURE  0x07E0          /* green  — ACTIVE / capturing */
#define COL_STATE_DONE     0x07FF          /* cyan   — ENDED / captured */
#define COL_STATE_REJECT   0xFD20          /* orange — too-short reject flash */

/* Phase 8 network status colours. */
#define COL_NET_OK         0x07E0          /* green  — DHCP bound, IP shown */
#define COL_NET_PENDING    0xFFE0          /* yellow — link up, awaiting DHCP */
#define COL_NET_DOWN       0x8410          /* gray   — no link / no cable */

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

/* Phase 7 capture-state render cache. The state line text changes as the live
   "Capturing N.Ns" counter grows, so it's cached as a string and repainted
   only when it (or its colour) actually changes. */
static char     s_drawn_state_str[48] = { '\x01', '\0' };  /* junk forces first draw */
static uint16_t s_drawn_state_col   = 0xFFFE;
static uint32_t s_drawn_caps        = 0xFFFFFFFFu;
static uint32_t s_drawn_rejs        = 0xFFFFFFFFu;
static uint8_t  s_reject_flash      = 0;     /* "Rejected" overlay countdown */
static uint32_t s_last_seen_rejects = 0;

/* Phase 8 network status render cache (delta-only repaint). */
static char     s_drawn_net_str[NET_STR_CAP] = { '\x01', '\0' };  /* junk forces first draw */
static uint16_t s_drawn_net_col     = 0xFFFE;

#if ASSIST_VAD_READOUT
/* VAD tuning-aid render cache. */
static uint32_t s_drawn_energy      = 0xFFFFFFFFu;
static uint32_t s_drawn_floor       = 0xFFFFFFFFu;
#endif

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

/* Append a NUL-terminated string at out[n]; returns the new length. */
static int put_str(char *out, int n, const char *s)
{
    while (*s) out[n++] = *s++;
    out[n] = 0;
    return n;
}

/* Format milliseconds as "N.Ns" (seconds + tenths). */
static int emit_secs(uint32_t ms, char *out)
{
    int n = emit_u32(ms / 1000u, out);
    out[n++] = '.';
    out[n++] = (char)('0' + (uint8_t)((ms % 1000u) / 100u));
    out[n++] = 's';
    out[n]   = 0;
    return n;
}

/* Compose the capture-state line into `out`; returns the colour to draw it in.
   Honours an active reject-flash overlay, otherwise reflects utterance_state. */
static uint16_t build_state_text(char *out)
{
    if (s_reject_flash > 0) {
        put_str(out, 0, "Rejected");
        return COL_STATE_REJECT;
    }
    switch (utterance_state) {
    case UTTERANCE_STATE_ACTIVE: {
        int n = put_str(out, 0, "Capturing ");
        emit_secs(utterance_capture_ms, out + n);
        return COL_STATE_CAPTURE;
    }
    case UTTERANCE_STATE_ENDED: {
        int n = put_str(out, 0, "Captured ");
        emit_secs(utterance_capture_ms, out + n);
        return COL_STATE_DONE;
    }
    default:
        put_str(out, 0, "Listening");
        return COL_STATE_LISTEN;
    }
}

/* Compose the network status line into `out`; returns the colour to draw it in.
   "Net: 192.168.1.42" once DHCP-bound, "Net: DHCP..." while waiting on a lease,
   "Net: link down" with no cable. Reads LwIP netif status via net.c, which is
   safe to call from the UI task (plain field reads, not socket/netconn API). */
static uint16_t build_net_text(char *out)
{
    int n = put_str(out, 0, "Net: ");
    if (net_dhcp_bound()) {
        net_ip_str(out + n, NET_STR_CAP - n);
        return COL_NET_OK;
    }
    if (net_link_up()) {
        put_str(out, n, "DHCP...");
        return COL_NET_PENDING;
    }
    put_str(out, n, "link down");
    return COL_NET_DOWN;
}

/* ----- drawing primitives ------------------------------------------------ */

/* Wipe one text row and draw `txt` in `col`. */
static void draw_row(uint16_t y, const char *txt, uint16_t col)
{
    lcd_fill_rect(DIAG_X, y, 480 - DIAG_X, 16, COL_BG);
    lcd_draw_text(DIAG_X, y, txt, col, COL_BG);
}

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

    /* Capture state line. Don't let entering the tab spuriously flash a stale
       reject — baseline the reject counter to "now". */
    s_reject_flash      = 0;
    s_last_seen_rejects = utterance_total_rejects;
    {
        char sbuf[48];
        uint16_t scol = build_state_text(sbuf);
        draw_row(STATE_Y, sbuf, scol);
        strncpy(s_drawn_state_str, sbuf, sizeof(s_drawn_state_str) - 1);
        s_drawn_state_str[sizeof(s_drawn_state_str) - 1] = 0;
        s_drawn_state_col = scol;
    }

    /* Captures / rejects tally. */
    {
        char m[48];
        int n = put_str(m, 0, "Caps: ");
        n += emit_u32(utterance_total_captures, m + n);
        n  = put_str(m, n, "  Rej: ");
        n += emit_u32(utterance_total_rejects, m + n);
        draw_row(CAPMETA_Y, m, COL_FG);
        s_drawn_caps = utterance_total_captures;
        s_drawn_rejs = utterance_total_rejects;
    }

    /* Network status line. */
    {
        char nbuf[NET_STR_CAP];
        uint16_t ncol = build_net_text(nbuf);
        draw_row(NET_Y, nbuf, ncol);
        strncpy(s_drawn_net_str, nbuf, sizeof(s_drawn_net_str) - 1);
        s_drawn_net_str[sizeof(s_drawn_net_str) - 1] = 0;
        s_drawn_net_col = ncol;
    }

#if ASSIST_VAD_READOUT
    /* VAD energy / floor tuning line. */
    {
        char m[48];
        int n = put_str(m, 0, "E:");
        n += emit_u32(vad_last_energy, m + n);
        n  = put_str(m, n, " F:");
        n += emit_u32(vad_noise_floor, m + n);
        draw_row(VAD_Y, m, COL_FG);
        s_drawn_energy = vad_last_energy;
        s_drawn_floor  = vad_noise_floor;
    }
#endif

    draw_indicator((s_flash_remaining > 0) ||
                   (utterance_state == UTTERANCE_STATE_ACTIVE));

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

    /* Reject overlay edge-detect (the utterance task bumps the reject counter
       when a capture had too little speech to be a real command). */
    uint32_t rejs_now = utterance_total_rejects;
    if (rejs_now != s_last_seen_rejects) {
        s_reject_flash      = FLASH_TICKS;
        s_last_seen_rejects = rejs_now;
    }

    /* Dot is green while capturing as well as during a wake-fire flash. */
    bool indicator_on = (s_flash_remaining > 0) ||
                        (utterance_state == UTTERANCE_STATE_ACTIVE);
    if (indicator_on != s_drawn_indicator_on) {
        draw_indicator(indicator_on);
    }
    if (s_flash_remaining > 0) s_flash_remaining--;

    /* Capture state line — repaint only when the composed text or colour
       changes (the live "Capturing N.Ns" counter changes it each 100 ms). */
    {
        char sbuf[48];
        uint16_t scol = build_state_text(sbuf);
        if (scol != s_drawn_state_col ||
            strcmp(sbuf, s_drawn_state_str) != 0) {
            draw_row(STATE_Y, sbuf, scol);
            strncpy(s_drawn_state_str, sbuf, sizeof(s_drawn_state_str) - 1);
            s_drawn_state_str[sizeof(s_drawn_state_str) - 1] = 0;
            s_drawn_state_col = scol;
        }
    }
    if (s_reject_flash > 0) s_reject_flash--;

    /* Captures / rejects tally — delta-only. */
    if (utterance_total_captures != s_drawn_caps || rejs_now != s_drawn_rejs) {
        char m[48];
        int n = put_str(m, 0, "Caps: ");
        n += emit_u32(utterance_total_captures, m + n);
        n  = put_str(m, n, "  Rej: ");
        n += emit_u32(rejs_now, m + n);
        draw_row(CAPMETA_Y, m, COL_FG);
        s_drawn_caps = utterance_total_captures;
        s_drawn_rejs = rejs_now;
    }

    /* Network status — delta-only. Once DHCP-bound the string is static, so this
       repaints only on link/lease transitions, not every tick. */
    {
        char nbuf[NET_STR_CAP];
        uint16_t ncol = build_net_text(nbuf);
        if (ncol != s_drawn_net_col || strcmp(nbuf, s_drawn_net_str) != 0) {
            draw_row(NET_Y, nbuf, ncol);
            strncpy(s_drawn_net_str, nbuf, sizeof(s_drawn_net_str) - 1);
            s_drawn_net_str[sizeof(s_drawn_net_str) - 1] = 0;
            s_drawn_net_col = ncol;
        }
    }

#if ASSIST_VAD_READOUT
    /* VAD energy / floor — live tuning aid. Energy changes every frame while
       you speak; delta-compare so a quiet room doesn't churn the panel. */
    {
        uint32_t e  = vad_last_energy;
        uint32_t fl = vad_noise_floor;
        if (e != s_drawn_energy || fl != s_drawn_floor) {
            char m[48];
            int n = put_str(m, 0, "E:");
            n += emit_u32(e, m + n);
            n  = put_str(m, n, " F:");
            n += emit_u32(fl, m + n);
            draw_row(VAD_Y, m, COL_FG);
            s_drawn_energy = e;
            s_drawn_floor  = fl;
        }
    }
#endif

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
