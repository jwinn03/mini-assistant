#include "assistant_client.h"
#include "utterance.h"
#include "net.h"
#include "sha1.h"
#include "tts_player.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/api.h"
#include "lwip/ip_addr.h"

#include "stm32f7xx.h"   /* DWT->CYCCNT for mask/nonce entropy */

#include <stdbool.h>
#include <string.h>

/*
 * Hand-rolled RFC 6455 WebSocket client over LwIP's netconn API.
 *
 * Scope is deliberately narrow: ws:// only (no TLS), one connection, one
 * outstanding request. The full utterance is sent as a SINGLE binary frame
 * (16-bit or 64-bit extended length) — the payload is masked straight out of
 * the pinned SDRAM capture buffer in 2 KB chunks, so no second copy of the
 * utterance ever exists. Client→server frames are masked per RFC 6455; the
 * mask key comes from DWT->CYCCNT (cache-poisoning defense is moot on a
 * trusted LAN — it just has to vary).
 *
 * The task never touches the audio path: receives are bounded by recv
 * timeouts (LWIP_SO_RCVTIMEO, enabled in lwipopts.h USER CODE; sends are
 * plain blocking — see the note at netconn_new) and its worst failure mode
 * is a missed response, never a glitch.
 */

/* ---- public state -------------------------------------------------------- */

volatile int32_t  assistant_init_status = 0;
volatile uint8_t  assistant_status      = ASSIST_IDLE;
char              assistant_response[ASSISTANT_RESPONSE_CAP];
volatile uint32_t assistant_response_seq = 0;
char              assistant_transcript[ASSISTANT_TRANSCRIPT_CAP];
volatile uint32_t assistant_transcript_seq = 0;
volatile uint32_t assistant_total_ok     = 0;
volatile uint32_t assistant_total_errors = 0;
volatile uint32_t assistant_last_rtt_ms  = 0;
volatile int32_t  assistant_dbg_step     = 0;
volatile int32_t  assistant_dbg_err      = 0;

/* ---- timeouts / tunables -------------------------------------------------- */

#define POLL_MS            50      /* utterance_state poll cadence */
#define ERROR_HOLD_MS      3000    /* how long ASSIST_ERROR stays on screen */
#define HS_RECV_TIMEOUT_MS 5000    /* handshake response */
#define RESP_TIMEOUT_MS    20000   /* ASR + LLM round-trip budget (to TEXT) */
#define AUDIO_MSG_TIMEOUT_MS 10000 /* Phase 10: gap between TTS messages */
#define TTS_STALL_MS       30000   /* Phase 10: TTS ring full this long = dead */
#define TX_CHUNK           2048    /* masking chunk (bounds our .bss buffer) */

/* ---- connection state ----------------------------------------------------- */

static struct netconn *s_conn   = NULL;
static bool            s_ws_up  = false;

/* RX cursor over netconn_recv's netbuf chain. */
static struct netbuf  *s_nb      = NULL;
static uint16_t        s_nb_off  = 0;    /* offset into current netbuf part */

/* Static work buffers — keeps the 768-word task stack honest. */
static uint8_t s_chunk[TX_CHUNK];        /* masked TX staging */
static char    s_hs[512];                /* handshake request/response */

static TickType_t s_error_until = 0;

/* ---- small utilities ------------------------------------------------------ */

static uint32_t entropy32(void)
{
    /* CYCCNT free-runs at 216 MHz (dsp_init did the LAR unlock); fold in the
       tick count so back-to-back calls in one loop iteration still differ. */
    return DWT->CYCCNT ^ (xTaskGetTickCount() * 2654435761u);
}

static void rx_reset(void)
{
    if (s_nb != NULL) {
        netbuf_delete(s_nb);
        s_nb = NULL;
    }
    s_nb_off = 0;
}

static void conn_close(void)
{
    rx_reset();
    if (s_conn != NULL) {
        netconn_close(s_conn);
        netconn_delete(s_conn);
        s_conn = NULL;
    }
    s_ws_up = false;
}

/* Pull exactly `n` bytes from the connection (across netbuf/part boundaries).
   Returns false on timeout/close/error. */
static bool rx_read(uint8_t *dst, uint32_t n)
{
    while (n > 0) {
        if (s_nb == NULL) {
            if (netconn_recv(s_conn, &s_nb) != ERR_OK) {
                s_nb = NULL;
                return false;
            }
            netbuf_first(s_nb);
            s_nb_off = 0;
        }
        void    *data;
        uint16_t len;
        netbuf_data(s_nb, &data, &len);
        if (s_nb_off >= len) {
            if (netbuf_next(s_nb) < 0) {
                netbuf_delete(s_nb);
                s_nb = NULL;
            } else {
                s_nb_off = 0;
            }
            continue;
        }
        uint32_t take = (uint32_t)len - s_nb_off;
        if (take > n) take = n;
        memcpy(dst, (const uint8_t *)data + s_nb_off, take);
        dst      += take;
        n        -= take;
        s_nb_off += (uint16_t)take;
    }
    return true;
}

/* Discard `n` bytes (oversized payloads we don't want). */
static bool rx_skip(uint32_t n)
{
    uint8_t scratch[32];
    while (n > 0) {
        uint32_t take = (n > sizeof(scratch)) ? sizeof(scratch) : n;
        if (!rx_read(scratch, take)) return false;
        n -= take;
    }
    return true;
}

/* ---- WebSocket framing ----------------------------------------------------- */

/* Send one complete masked frame with a small (<126 B) payload — control
   frames (pong/close) and nothing else. */
static err_t ws_send_small(uint8_t opcode, const uint8_t *payload, uint8_t n)
{
    uint8_t f[2 + 4 + 125];
    uint32_t key = entropy32();
    f[0] = (uint8_t)(0x80u | opcode);
    f[1] = (uint8_t)(0x80u | n);
    memcpy(&f[2], &key, 4);
    for (uint8_t i = 0; i < n; i++) {
        f[6 + i] = payload[i] ^ ((const uint8_t *)&key)[i & 3];
    }
    return netconn_write(s_conn, f, 6u + n, NETCONN_COPY);
}

/* Send one binary frame whose payload is `n` bytes at `src` (the pinned
   utterance buffer). Header first, then the payload masked through s_chunk in
   TX_CHUNK slices — netconn_write(NETCONN_COPY) has copied each slice into
   LwIP's core before returning, so s_chunk is immediately reusable. */
static err_t ws_send_binary(const uint8_t *src, uint32_t n)
{
    uint8_t  h[14];
    uint32_t hn;
    uint32_t key = entropy32();

    h[0] = 0x82;                       /* FIN | binary */
    if (n < 126u) {
        h[1] = (uint8_t)(0x80u | n);
        hn = 2;
    } else if (n < 65536u) {
        h[1] = 0x80u | 126u;
        h[2] = (uint8_t)(n >> 8);
        h[3] = (uint8_t)n;
        hn = 4;
    } else {
        h[1] = 0x80u | 127u;
        memset(&h[2], 0, 4);           /* top 32 bits of the 64-bit length */
        h[6] = (uint8_t)(n >> 24);
        h[7] = (uint8_t)(n >> 16);
        h[8] = (uint8_t)(n >> 8);
        h[9] = (uint8_t)n;
        hn = 10;
    }
    memcpy(&h[hn], &key, 4);
    hn += 4;

    err_t err = netconn_write(s_conn, h, hn, NETCONN_COPY);
    if (err != ERR_OK) return err;

    uint32_t off = 0;
    while (off < n) {
        uint32_t take = n - off;
        if (take > TX_CHUNK) take = TX_CHUNK;
        for (uint32_t i = 0; i < take; i++) {
            s_chunk[i] = src[off + i] ^ ((const uint8_t *)&key)[(off + i) & 3];
        }
        err = netconn_write(s_conn, s_chunk, take, NETCONN_COPY);
        if (err != ERR_OK) return err;
        off += take;
    }
    return ERR_OK;
}

/* Message classification for the Phase 10 pump. */
#define WSM_ERR    (-1)
#define WSM_TEXT     1     /* text message copied into out[] */
#define WSM_AUDIO    2     /* non-empty binary — streamed into tts_player */
#define WSM_EOS      3     /* zero-length binary — end of speech */

/* Receive ONE complete WebSocket message. Text is copied into out[cap]
   (NUL-terminated, non-ASCII sanitized to '?'; out == NULL just skips it).
   Binary payload is streamed straight into tts_player_write() in
   TX_CHUNK-sized pieces — s_chunk is safe to reuse here because we never
   transmit data frames while receiving (pongs use their own stack buffer).
   Backpressure: while the 512 KB TTS ring is full we simply stop reading,
   which stalls TCP flow control upstream; a TTS_STALL_MS guard bounds it.
   Handles ping/pong/close control frames inline. */
static int ws_recv_next(char *out, uint32_t cap)
{
    uint32_t used      = 0;      /* text bytes accumulated */
    uint32_t bin_total = 0;      /* binary bytes streamed */
    uint8_t  msg_kind  = 0;      /* 0 undetermined, 1 text, 2 binary */
    bool     done      = false;

    while (!done) {
        uint8_t hdr[2];
        if (!rx_read(hdr, 2)) return WSM_ERR;

        uint8_t  fin    = hdr[0] & 0x80u;
        uint8_t  opcode = hdr[0] & 0x0Fu;
        uint8_t  masked = hdr[1] & 0x80u;
        uint64_t plen   = hdr[1] & 0x7Fu;

        if (masked) return WSM_ERR;    /* server frames must be unmasked */

        if (plen == 126u) {
            uint8_t e[2];
            if (!rx_read(e, 2)) return WSM_ERR;
            plen = ((uint64_t)e[0] << 8) | e[1];
        } else if (plen == 127u) {
            uint8_t e[8];
            if (!rx_read(e, 8)) return WSM_ERR;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | e[i];
        }

        if (opcode == 0x9u) {          /* ping → pong (echo payload) */
            uint8_t pp[125];
            if (plen > 125u) return WSM_ERR;
            if (!rx_read(pp, (uint32_t)plen)) return WSM_ERR;
            if (ws_send_small(0xA, pp, (uint8_t)plen) != ERR_OK) return WSM_ERR;
            continue;
        }
        if (opcode == 0xAu) {          /* pong — ignore */
            if (!rx_skip((uint32_t)plen)) return WSM_ERR;
            continue;
        }
        if (opcode == 0x8u) {          /* close — acknowledge and bail */
            ws_send_small(0x8, NULL, 0);
            s_ws_up = false;
            return WSM_ERR;
        }

        /* Data frame. Continuations (0x0) inherit the message's first opcode. */
        if (opcode == 0x1u)      msg_kind = 1;
        else if (opcode == 0x2u) msg_kind = 2;
        else if (opcode != 0x0u) return WSM_ERR;
        if (msg_kind == 0)       return WSM_ERR;   /* continuation w/o start */

        if (msg_kind == 1) {
            uint32_t want = (uint32_t)plen;
            uint32_t room = (out != NULL) ? (cap - 1u) - used : 0u;
            uint32_t take = (want > room) ? room : want;
            if (take > 0 && !rx_read((uint8_t *)out + used, take)) return WSM_ERR;
            used += take;
            if (want > take && !rx_skip(want - take)) return WSM_ERR;
        } else {
            uint64_t remaining = plen;
            while (remaining > 0) {
                uint32_t piece = (remaining > TX_CHUNK) ? TX_CHUNK
                                                        : (uint32_t)remaining;
                if (!rx_read(s_chunk, piece)) return WSM_ERR;

                uint32_t   off    = 0;
                TickType_t stall0 = xTaskGetTickCount();
                while (off < piece) {
                    uint32_t acc = tts_player_write(&s_chunk[off], piece - off);
                    if (acc > 0) {
                        off += acc;
                        stall0 = xTaskGetTickCount();
                    } else {
                        if ((xTaskGetTickCount() - stall0) >
                            pdMS_TO_TICKS(TTS_STALL_MS)) {
                            return WSM_ERR;     /* playback wedged — bail */
                        }
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }
                remaining -= piece;
                bin_total += piece;
            }
        }
        if (fin) done = true;
    }

    if (msg_kind == 2) {
        return (bin_total > 0) ? WSM_AUDIO : WSM_EOS;
    }

    if (out != NULL) {
        out[used] = 0;
        /* The LCD font is ASCII 8x16 — replace anything unprintable. Newlines
           become spaces; the UI re-wraps by word anyway. */
        for (uint32_t i = 0; i < used; i++) {
            char c = out[i];
            if (c == '\n' || c == '\r' || c == '\t') out[i] = ' ';
            else if (c < 0x20 || c > 0x7E)           out[i] = '?';
        }
    }
    return WSM_TEXT;
}

/* ---- connect + handshake --------------------------------------------------- */

static bool ws_connect(void)
{
    err_t err;
    ip_addr_t ip;
    assistant_dbg_step = 1;
    if (!ipaddr_aton(ASSISTANT_HELPER_IP, &ip)) {
        assistant_dbg_err = -100;
        return false;
    }

    assistant_dbg_step = 2;
    s_conn = netconn_new(NETCONN_TCP);
    if (s_conn == NULL) { assistant_dbg_err = -100; return false; }

    netconn_set_recvtimeout(s_conn, HS_RECV_TIMEOUT_MS);
    /* Deliberately NO send timeout. LwIP treats a non-zero send_timeout as a
       non-blocking send, and netconn_write() (bytes_written == NULL) then
       returns ERR_VAL because it can't report a partial write. Plain blocking
       writes are correct here; a stalled send is still bounded by TCP-level
       retransmit timeouts, and the recv timeout bounds the reply wait. */

    assistant_dbg_step = 3;
    err = netconn_connect(s_conn, &ip, ASSISTANT_HELPER_PORT);
    if (err != ERR_OK) {
        assistant_dbg_err = err;
        conn_close();
        return false;
    }

    /* Sec-WebSocket-Key: Base64 of 16 nonce bytes. */
    uint32_t nonce[4] = { entropy32(), entropy32() ^ 0xA5A5A5A5u,
                          entropy32() + 0x9E3779B9u, entropy32() ^ 0x5F356495u };
    char key24[32];
    base64_encode((const uint8_t *)nonce, 16, key24, sizeof(key24));

    /* Expected Sec-WebSocket-Accept = Base64(SHA1(key || RFC 6455 GUID)). */
    char accept_src[64];
    int  an = 0;
    an  = (int)strlen(key24);
    memcpy(accept_src, key24, (size_t)an);
    memcpy(accept_src + an, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    uint8_t digest[20];
    sha1(accept_src, (size_t)an + 36, digest);
    char accept28[32];
    base64_encode(digest, 20, accept28, sizeof(accept28));

    int n = 0;
    n  = (int)strlen(strcpy(s_hs, "GET " ASSISTANT_WS_PATH " HTTP/1.1\r\n"
                                  "Host: " ASSISTANT_HELPER_IP "\r\n"
                                  "Upgrade: websocket\r\n"
                                  "Connection: Upgrade\r\n"
                                  "Sec-WebSocket-Version: 13\r\n"
                                  "Sec-WebSocket-Key: "));
    memcpy(s_hs + n, key24, strlen(key24));
    n += (int)strlen(key24);
    memcpy(s_hs + n, "\r\n\r\n", 4);
    n += 4;

    assistant_dbg_step = 4;
    err = netconn_write(s_conn, s_hs, (size_t)n, NETCONN_COPY);
    if (err != ERR_OK) {
        assistant_dbg_err = err;
        conn_close();
        return false;
    }

    /* Read the response headers (byte-wise scan for the blank line — the
       whole thing is a few hundred bytes, once per connection). */
    assistant_dbg_step = 5;
    uint32_t hn = 0;
    while (hn < sizeof(s_hs) - 1) {
        if (!rx_read((uint8_t *)&s_hs[hn], 1)) {
            assistant_dbg_err = -100;
            conn_close();
            return false;
        }
        hn++;
        if (hn >= 4 && memcmp(&s_hs[hn - 4], "\r\n\r\n", 4) == 0) break;
    }
    s_hs[hn] = 0;

    assistant_dbg_step = 6;
    if (strncmp(s_hs, "HTTP/1.1 101", 12) != 0 ||
        strstr(s_hs, accept28) == NULL) {
        assistant_dbg_err = -100;
        conn_close();
        return false;
    }

    s_ws_up = true;
    return true;
}

/* ---- round-trip ------------------------------------------------------------- */

/* Upload the pinned capture and wait for the reply. Returns true on success
   (assistant_response updated + seq bumped). */
static bool do_round_trip(const int16_t *pcm, uint32_t samples)
{
    bool was_up = s_ws_up;

    assistant_status = ASSIST_CONNECTING;
    if (!s_ws_up && !ws_connect()) {
        return false;
    }

    assistant_status = ASSIST_UPLOADING;
    assistant_dbg_step = 7;
    TickType_t t0 = xTaskGetTickCount();

    err_t err = ws_send_binary((const uint8_t *)pcm, samples * 2u);
    if (err != ERR_OK && was_up) {
        /* Persistent connection had gone stale (helper restarted, idle drop).
           One reconnect + resend attempt, then give up. */
        conn_close();
        assistant_status = ASSIST_CONNECTING;
        if (!ws_connect()) return false;
        assistant_status = ASSIST_UPLOADING;
        err = ws_send_binary((const uint8_t *)pcm, samples * 2u);
    }
    if (err != ERR_OK) {
        assistant_dbg_err = err;
        conn_close();
        return false;
    }

    assistant_status = ASSIST_WAITING;
    assistant_dbg_step = 8;
    netconn_set_recvtimeout(s_conn, RESP_TIMEOUT_MS);

    /* New round-trip: clear the previous transcript so a helper that sends
       no "Q:" message (--echo mode) shows a blank query area, not a stale
       pairing of old question with new answer. */
    assistant_transcript[0] = 0;
    assistant_transcript_seq++;

    /* Phase 1: text until the answer arrives. A "Q: <transcript>" message
       (the ASR result, sent while the LLM is still generating) is published
       to the transcript buffer and the wait continues. Early audio (protocol
       is text-first, but tolerate reordering) simply buffers into the TTS
       ring. */
    for (;;) {
        int t = ws_recv_next(assistant_response, ASSISTANT_RESPONSE_CAP);
        if (t == WSM_TEXT) {
            if (assistant_response[0] == 'Q' && assistant_response[1] == ':' &&
                assistant_response[2] == ' ') {
                uint32_t i = 0;
                while (assistant_response[3 + i] != 0 &&
                       i < ASSISTANT_TRANSCRIPT_CAP - 1u) {
                    assistant_transcript[i] = assistant_response[3 + i];
                    i++;
                }
                assistant_transcript[i] = 0;
                assistant_transcript_seq++;
                continue;                /* still waiting for the answer */
            }
            break;                       /* the answer itself */
        }
        if (t == WSM_AUDIO || t == WSM_EOS) continue;
        /* Timeout or protocol error — connection state is unknowable, drop it
           so the next utterance starts clean. */
        tts_player_abort();
        conn_close();
        return false;
    }

    /* Publish immediately — the display updates while the TTS streams. */
    assistant_last_rtt_ms =
        (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
    assistant_response_seq++;

    /* Phase 2 (Phase 10): TTS audio chunks until the zero-length EOS marker.
       The v2 server ALWAYS sends EOS (even with TTS disabled). Failures here
       are non-fatal — the text is already on screen; cut the speech, drop the
       connection, and let the next utterance reconnect. */
    assistant_dbg_step = 9;
    netconn_set_recvtimeout(s_conn, AUDIO_MSG_TIMEOUT_MS);
    for (;;) {
        int t = ws_recv_next(NULL, 0);
        if (t == WSM_EOS) {
            tts_player_eos();
            break;
        }
        if (t == WSM_AUDIO || t == WSM_TEXT) continue;   /* stray text: ignore */
        tts_player_abort();
        conn_close();
        break;
    }
    return true;
}

/* ---- task ------------------------------------------------------------------- */

static void assistant_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        /* Let a displayed error decay back to idle. */
        if (assistant_status == ASSIST_ERROR) {
            if ((int32_t)(xTaskGetTickCount() - s_error_until) >= 0) {
                assistant_status = ASSIST_IDLE;
            }
            continue;
        }
        if (assistant_status == ASSIST_NO_NET &&
            utterance_state != UTTERANCE_STATE_ENDED) {
            assistant_status = ASSIST_IDLE;   /* the capture expired unclaimed */
        }

        if (utterance_state != UTTERANCE_STATE_ENDED) {
            continue;
        }

        if (!net_dhcp_bound()) {
            /* Don't take the capture — utterance auto-re-arms after its hold
               window, and the UI shows why nothing was sent. */
            assistant_status = ASSIST_NO_NET;
            continue;
        }

        int16_t  *pcm = NULL;
        uint32_t  samples = 0;
        if (!utterance_take(&pcm, &samples) || pcm == NULL || samples == 0) {
            continue;
        }

        bool ok = do_round_trip(pcm, samples);

        /* The payload has been fully copied into LwIP (NETCONN_COPY) or the
           attempt failed — either way the capture buffer is done. Release so
           the utterance task can re-arm while we (on success) already idle. */
        utterance_release();

        if (ok) {
            assistant_total_ok++;
            assistant_status = ASSIST_IDLE;
        } else {
            assistant_total_errors++;
            assistant_status = ASSIST_ERROR;
            s_error_until = xTaskGetTickCount() + pdMS_TO_TICKS(ERROR_HOLD_MS);
        }
    }
}

void assistant_client_init(void)
{
    ip_addr_t ip;
    if (!ipaddr_aton(ASSISTANT_HELPER_IP, &ip)) {
        assistant_init_status = -1;
        return;
    }

    assistant_response[0]   = 0;
    assistant_transcript[0] = 0;

    /* 768 words = 3 KB. Deepest path is ws_connect (SHA-1 W[80] + locals,
       ~0.5 KB) on top of netconn API calls; the big buffers are all static. */
    BaseType_t ok = xTaskCreate(assistant_task, "Assist", 768, NULL,
                                tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        assistant_init_status = -2;
    }
}
