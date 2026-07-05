#ifndef ASSISTANT_CLIENT_H
#define ASSISTANT_CLIENT_H

#include <stdint.h>

/*
 * Phase 8 assistant client — the consumer of the Phase 7 utterance buffer.
 *
 * A dedicated task polls for a completed capture (utterance_take), streams the
 * 16 kHz mono q15 PCM to the helper server as one WebSocket binary message,
 * waits for the text reply, and surfaces it (plus a status enum) for the
 * Assist tab. One persistent connection is reused across utterances, with a
 * single reconnect attempt when it has gone stale.
 *
 * Protocol (v1, matches helper/server.py):
 *   ws://ASSISTANT_HELPER_IP:8765/utterance
 *   client → server : one binary message = one utterance (int16 LE, 16 kHz mono)
 *   server → client : one text message   = the assistant's response (ASCII)
 * Trusted-LAN only — no TLS in v1 (see helper/README.md).
 */

/* ---- configuration ------------------------------------------------------ */

/* EDIT ME: IPv4 address of the machine running helper/server.py. mDNS
   resolution of mini-helper.local is future work; v1 uses a static literal. */
#define ASSISTANT_HELPER_IP    "192.168.1.100"
#define ASSISTANT_HELPER_PORT  8765
#define ASSISTANT_WS_PATH      "/utterance"

/* Response text capacity (bytes incl. NUL). The Assist tab renders ~275 chars;
   anything longer is truncated here first. */
#define ASSISTANT_RESPONSE_CAP 512

/* ---- status surfaced to the UI ------------------------------------------ */

typedef enum {
    ASSIST_IDLE = 0,       /* nothing in flight */
    ASSIST_NO_NET,         /* capture ready but DHCP not bound — will expire */
    ASSIST_CONNECTING,     /* TCP connect + WebSocket handshake */
    ASSIST_UPLOADING,      /* streaming PCM frames */
    ASSIST_WAITING,        /* upload done, waiting for the helper's reply */
    ASSIST_ERROR,          /* last attempt failed (auto-clears after ~3 s) */
} assistant_status_t;

/* Spawns the assistant task. Call from StartDefaultTask after utterance_init()
   and BEFORE wake_word_init() (heap-order rule: wake-word stays last). */
void assistant_client_init(void);

/* 0 = ok; -1 = ASSISTANT_HELPER_IP failed to parse; -2 = xTaskCreate failed. */
extern volatile int32_t  assistant_init_status;

extern volatile uint8_t  assistant_status;         /* assistant_status_t */

/* Response text (ASCII, NUL-terminated). Written by the client task, then
   assistant_response_seq is incremented — readers should copy the buffer and
   treat seq as the "new response" edge. A torn read is theoretically possible
   but self-heals on the next 30 Hz UI tick. */
extern char              assistant_response[ASSISTANT_RESPONSE_CAP];
extern volatile uint32_t assistant_response_seq;

/* Diagnostics. */
extern volatile uint32_t assistant_total_ok;       /* responses received */
extern volatile uint32_t assistant_total_errors;   /* failed round-trips */
extern volatile uint32_t assistant_last_rtt_ms;    /* upload→response time */

#endif /* ASSISTANT_CLIENT_H */
