#include "ui_page_assistant.h"
#include "lcd.h"
#include "wake_word.h"
#include "utterance.h"
#include "net.h"
#include "assistant_client.h"
#include "tts_player.h"
#include <stdbool.h>
#include <string.h>

/* VAD energy/floor tuning readout (Phase 7 calibration aid). Flip to 1 to
   restore the live "E:<energy> F:<floor>" line on the Assist tab — handy if the
   VAD ever needs re-tuning against real mic levels. Disabled now that
   VAD_ABS_OFFSET / VAD_MARGIN_NUM are dialed in. Gating it (rather than
   deleting) keeps the code compilable and one flag away from coming back. */
#define ASSIST_VAD_READOUT 0

/* Diagnostic readouts (Caps/Rej tally + Inf/Fire/Conf line), hidden in the
   post-Phase-10 cleanup but kept one flag away, same idiom as above. NB: the
   layout has since been reflowed — the query rows now occupy y=96..130 where
   the Caps/Rej text used to live, so re-enabling draws the tally at DIAG_Y
   alongside the counters rather than at its original row. */
#define ASSIST_DIAG_READOUT 0

#if ASSIST_VAD_READOUT
#include "vad.h"
#endif

/* Layout. The body region is y in [BODY_TOP, LCD_H); we just inline the same
   value (40) used elsewhere rather than expose it from ui.c.

   Post-Phase-10 cleanup: the Caps/Rej and Inf/Fire/Conf diagnostic rows are
   hidden (ASSIST_DIAG_READOUT), the FX switch joined the wake dot on the
   title row, and the freed space shows the ASR transcript of the user's
   request (blue) above the response:

     44   "Assistant" title       [FX switch 388..432][dot 444..460]
     72   state line (Listening / Capturing / Uploading / ...)
     96   query rows (2 x 18 px pitch, blue — what the helper heard)
     136  response rows (5 x 18 px pitch, 55 chars each)
     248  network status                                                   */
#define BODY_TOP_Y         40
#define TITLE_Y            44

#define INDICATOR_X        444
#define INDICATOR_Y        44
#define INDICATOR_SIZE     16

/* 33 ms render tick × 15 = ~495 ms flash duration on each wake-fire. */
#define FLASH_TICKS        15u

#define STATE_Y            72

/* FX-routing switch on the title row, left of the wake dot: "FX" label +
   graphical toggle (lcd_draw_toggle). The 22 px switch is centred on the
   16 px title text (y offset -3). */
#define FX_TOG_X           (INDICATOR_X - LCD_TOGGLE_W - 12)
#define FX_TOG_Y           (TITLE_Y - 3)
#define FX_LABEL_X         (FX_TOG_X - 2 * 8 - 6)

/* Transcribed user request ("Q: " message from the helper), above the
   response. Two wrapped rows is ample for a spoken query. */
#define QUERY_Y0           96
#define QUERY_ROWS         2
#define QUERY_PITCH        18

#define RESP_Y0            136
#define RESP_PITCH         18
#define RESP_COLS          55          /* (480 - 2*20) / 8 px per char */
#if ASSIST_VAD_READOUT
/* Debug build: the VAD line takes the last response row. */
#define RESP_ROWS          4
#define VAD_Y              (RESP_Y0 + 4 * RESP_PITCH)
#else
#define RESP_ROWS          5
#endif

#define DIAG_X             20
#if ASSIST_DIAG_READOUT
#define DIAG_Y             228         /* between response row 5 and the net line */
#endif
#define NET_Y              248
#define NET_STR_CAP        56

#define COL_BG             0x0000          /* LCD_BLACK */
#define COL_FG             0xFFFF          /* LCD_WHITE */
#define COL_TITLE          0x07FF          /* LCD_CYAN */
#define COL_DOT_IDLE       0x4208          /* mid-gray */
#define COL_DOT_FIRE       0x07E0          /* green */

#define COL_STATE_LISTEN   0xFFFF          /* white  — ARMED / listening */
#define COL_STATE_CAPTURE  0x07E0          /* green  — ACTIVE / capturing */
#define COL_STATE_DONE     0x07FF          /* cyan   — ENDED / captured */
#define COL_STATE_REJECT   0xFD20          /* orange — too-short reject flash */

/* Phase 8 network / assistant colours. */
#define COL_NET_OK         0x07E0          /* green  — DHCP bound, IP shown */
#define COL_NET_PENDING    0xFFE0          /* yellow — link up, awaiting DHCP */
#define COL_NET_DOWN       0x8410          /* gray   — no link / no cable */
#define COL_ASSIST_BUSY    0xFFE0          /* yellow — connecting/uploading */
#define COL_ASSIST_WAIT    0x07FF          /* cyan   — waiting for helper */
#define COL_ASSIST_ERR     0xF800          /* red    — round-trip failed */
#define COL_QUERY          0x651F          /* light blue — the ASR transcript
                                              (LCD_RGB565(100,160,255); pure
                                              blue is too dim on black) */

/* Local state — what we've already rendered, so each 33 ms tick is delta-only.
   s_drawn_* hold last-painted values; sentinels force the first draw. */
#if ASSIST_DIAG_READOUT
static uint32_t s_drawn_inferences = 0xFFFFFFFFu;
static uint32_t s_drawn_fires      = 0xFFFFFFFFu;
static int32_t  s_drawn_conf_mille = -100000;
static uint32_t s_drawn_caps       = 0xFFFFFFFFu;
static uint32_t s_drawn_rejs       = 0xFFFFFFFFu;
#endif
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
static uint8_t  s_reject_flash      = 0;     /* "Rejected" overlay countdown */
static uint32_t s_last_seen_rejects = 0;

/* Phase 8 network status render cache (delta-only repaint). */
static char     s_drawn_net_str[NET_STR_CAP] = { '\x01', '\0' };
static uint16_t s_drawn_net_col     = 0xFFFE;

/* Phase 10: FX-routing switch cache (title row). */
static uint8_t  s_drawn_fx          = 0xFF;

/* Phase 8 response render cache: repaint the text area only when a new
   response lands (seq edge). Same pattern for the query/transcript area. */
static uint32_t s_drawn_resp_seq    = 0xFFFFFFFFu;
static uint32_t s_drawn_query_seq   = 0xFFFFFFFFu;

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
   Priority: reject flash > live capture > assistant activity > idle states.
   The assistant statuses slot in when the capture pipeline is quiet — they
   describe what happened to the capture the user just made. */
static uint16_t build_state_text(char *out)
{
    if (s_reject_flash > 0) {
        put_str(out, 0, "Rejected");
        return COL_STATE_REJECT;
    }
    if (utterance_state == UTTERANCE_STATE_ACTIVE) {
        int n = put_str(out, 0, "Capturing ");
        emit_secs(utterance_capture_ms, out + n);
        return COL_STATE_CAPTURE;
    }
    /* Phase 10: speech playback (FILLING counts — audio is about to start). */
    if (tts_player_state != TTS_IDLE) {
        put_str(out, 0, "Speaking...");
        return COL_STATE_CAPTURE;      /* green, like the live-capture state */
    }
    switch (assistant_status) {
    case ASSIST_NO_NET:
        put_str(out, 0, "No network");
        return COL_NET_DOWN;
    case ASSIST_CONNECTING:
        put_str(out, 0, "Connecting...");
        return COL_ASSIST_BUSY;
    case ASSIST_UPLOADING:
        put_str(out, 0, "Uploading...");
        return COL_ASSIST_BUSY;
    case ASSIST_WAITING:
        put_str(out, 0, "Waiting for helper...");
        return COL_ASSIST_WAIT;
    case ASSIST_ERROR: {
        /* Show the failing stage + LwIP err so a bad round-trip is diagnosable
           without a debugger. e.g. "Helper err s4 e-1" = netconn_write ERR_MEM. */
        int n = put_str(out, 0, "Helper err s");
        n += emit_u32((uint32_t)assistant_dbg_step, out + n);
        n  = put_str(out, n, " e");
        int32_t e = assistant_dbg_err;
        if (e < 0) { out[n++] = '-'; out[n] = 0; e = -e; }
        emit_u32((uint32_t)e, out + n);
        return COL_ASSIST_ERR;
    }
    default:
        break;
    }
    if (utterance_state == UTTERANCE_STATE_ENDED) {
        int n = put_str(out, 0, "Captured ");
        emit_secs(utterance_capture_ms, out + n);
        return COL_STATE_DONE;
    }
    put_str(out, 0, "Listening");
    return COL_STATE_LISTEN;
}

/* Compose the network status line into `out`; returns the colour to draw it
   in. "Net: 192.168.1.42  OK: 3  Err: 0" once DHCP-bound, "Net: DHCP..."
   while waiting on a lease, "Net: link down" with no cable. Reads LwIP netif
   status via net.c, which is safe from the UI task (plain field reads). */
static uint16_t build_net_text(char *out)
{
    int n = put_str(out, 0, "Net: ");
    uint16_t col;
    if (net_dhcp_bound()) {
        net_ip_str(out + n, 16);
        n = (int)strlen(out);
        col = COL_NET_OK;
    } else if (net_link_up()) {
        n = put_str(out, n, "DHCP...");
        col = COL_NET_PENDING;
    } else {
        n = put_str(out, n, "link down");
        col = COL_NET_DOWN;
    }
    n  = put_str(out, n, "  OK: ");
    n += emit_u32(assistant_total_ok, out + n);
    n  = put_str(out, n, "  Err: ");
    n += emit_u32(assistant_total_errors, out + n);
    return col;
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

/* "FX" label + graphical switch on the title row, left of the wake dot. */
static void draw_fx_toggle(void)
{
    lcd_draw_text(FX_LABEL_X, TITLE_Y, "FX", COL_FG, COL_BG);
    lcd_draw_toggle(FX_TOG_X, FX_TOG_Y, tts_player_fx_enabled != 0);
    s_drawn_fx = tts_player_fx_enabled;
}

#if ASSIST_DIAG_READOUT
/* Combined diagnostics row (hidden by default): capture tally + wake-word
   counters on one line at DIAG_Y. The pre-cleanup UI showed these as two
   separate rows; merged here so the dormant code needs only one row of the
   reflowed layout. */
static void draw_diag(void)
{
    char m[80];
    int n = put_str(m, 0, "C:");
    n += emit_u32(utterance_total_captures, m + n);
    n  = put_str(m, n, " R:");
    n += emit_u32(utterance_total_rejects, m + n);
    n  = put_str(m, n, " Inf:");
    n += emit_u32(wake_word_total_inferences, m + n);
    n  = put_str(m, n, " F:");
    n += emit_u32(wake_word_total_fires, m + n);
    n  = put_str(m, n, " Cf:");
    int32_t mille = (int32_t)(wake_word_last_confidence * 1000.0f);
    emit_milli(mille, m + n);
    draw_row(DIAG_Y, m, COL_FG);
    s_drawn_caps       = utterance_total_captures;
    s_drawn_rejs       = utterance_total_rejects;
    s_drawn_inferences = wake_word_total_inferences;
    s_drawn_fires      = wake_word_total_fires;
    s_drawn_conf_mille = mille;
}
#endif

/* Word-wrap the ASR transcript (what the helper heard) into the blue query
   rows above the response. Same seq-edge pattern as draw_response. */
static void draw_query(void)
{
    char txt[ASSISTANT_TRANSCRIPT_CAP];
    strncpy(txt, assistant_transcript, sizeof(txt) - 1);
    txt[sizeof(txt) - 1] = 0;

    const char *s = txt;
    for (int r = 0; r < QUERY_ROWS; r++) {
        uint16_t y = (uint16_t)(QUERY_Y0 + r * QUERY_PITCH);
        lcd_fill_rect(DIAG_X, y, 480 - DIAG_X, 16, COL_BG);

        while (*s == ' ') s++;
        if (*s == 0) continue;

        int rem = (int)strlen(s);
        int len = (rem <= RESP_COLS) ? rem : RESP_COLS;
        if (rem > RESP_COLS) {
            int brk = len;
            while (brk > 0 && s[brk] != ' ') brk--;
            if (brk > 0) len = brk;
        }

        char row[RESP_COLS + 1];
        memcpy(row, s, (size_t)len);
        row[len] = 0;
        if (r == QUERY_ROWS - 1 && rem > len && len >= 3) {
            row[len - 1] = '.'; row[len - 2] = '.'; row[len - 3] = '.';
        }
        lcd_draw_text(DIAG_X, y, row, COL_QUERY, COL_BG);
        s += len;
    }
    s_drawn_query_seq = assistant_transcript_seq;
}

/* Word-wrap assistant_response into the response rows. Repaints the whole
   area — called only on a response-seq edge (rare), not per tick. Copies the
   volatile-ish buffer first so a concurrent client write can't shear a row
   (a torn copy self-heals: seq changes again → redraw). */
static void draw_response(void)
{
    char txt[ASSISTANT_RESPONSE_CAP];
    strncpy(txt, assistant_response, sizeof(txt) - 1);
    txt[sizeof(txt) - 1] = 0;

    const char *s = txt;
    for (int r = 0; r < RESP_ROWS; r++) {
        uint16_t y = (uint16_t)(RESP_Y0 + r * RESP_PITCH);
        lcd_fill_rect(DIAG_X, y, 480 - DIAG_X, 16, COL_BG);

        while (*s == ' ') s++;
        if (*s == 0) continue;

        int rem = (int)strlen(s);
        int len = (rem <= RESP_COLS) ? rem : RESP_COLS;
        if (rem > RESP_COLS) {
            /* Greedy word wrap: back up to the last space in the window. */
            int brk = len;
            while (brk > 0 && s[brk] != ' ') brk--;
            if (brk > 0) len = brk;
        }

        char row[RESP_COLS + 1];
        memcpy(row, s, (size_t)len);
        row[len] = 0;
        /* Truncation marker if this is the last row and text remains. */
        if (r == RESP_ROWS - 1 && rem > len && len >= 3) {
            row[len - 1] = '.'; row[len - 2] = '.'; row[len - 3] = '.';
        }
        lcd_draw_text(DIAG_X, y, row, COL_FG, COL_BG);
        s += len;
    }
    s_drawn_resp_seq = assistant_response_seq;
}

/* ----- public API -------------------------------------------------------- */

void ui_page_assistant_redraw(void)
{
    /* Title at the top of the body; FX switch + wake indicator top-right. */
    lcd_draw_text(DIAG_X, TITLE_Y, "Assistant", COL_TITLE, COL_BG);

    /* Force a full repaint of everything below the title. */
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

    /* FX routing switch (tap the title row's right end to toggle). */
    draw_fx_toggle();

#if ASSIST_DIAG_READOUT
    draw_diag();
#endif

    /* Query (transcript) + response areas — persist across tab switches. */
    draw_query();
    draw_response();

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

    /* FX switch — delta-repaint on toggle. */
    if (tts_player_fx_enabled != s_drawn_fx) {
        draw_fx_toggle();
    }

    /* Query + response areas — repaint only on their seq edges. */
    if (assistant_transcript_seq != s_drawn_query_seq) {
        draw_query();
    }
    if (assistant_response_seq != s_drawn_resp_seq) {
        draw_response();
    }

    /* Network status — delta-only. Once DHCP-bound the string is static until
       a counter ticks, so this repaints on transitions, not every tick. */
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

#if ASSIST_DIAG_READOUT
    /* Combined diagnostics — delta-only on any counter change. */
    int32_t mille = (int32_t)(wake_word_last_confidence * 1000.0f);
    if (utterance_total_captures != s_drawn_caps ||
        rejs_now != s_drawn_rejs ||
        wake_word_total_inferences != s_drawn_inferences ||
        fires_now != s_drawn_fires ||
        mille != s_drawn_conf_mille) {
        draw_diag();
    }
#endif
}

void ui_page_assistant_touch(uint16_t tx, uint16_t ty, bool edge)
{
    if (!edge) return;

    /* Tap the FX switch (title row, right end) to toggle the TTS effects
       routing. Generous band around the 22 px widget for finger-sized
       targets; X-gated so future left-side title-row controls stay free.
       Takes effect immediately — even mid-speech (the inject hooks read the
       flag per 2.67 ms block, so effects kick in/out live). */
    if (tx >= FX_LABEL_X - 8u && tx < INDICATOR_X - 4u &&
        ty < TITLE_Y + 28u) {
        tts_player_fx_enabled = tts_player_fx_enabled ? 0u : 1u;
    }
}
