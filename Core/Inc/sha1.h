#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>
#include <stddef.h>

/*
 * Minimal SHA-1 + Base64 (Phase 8).
 *
 * Present solely for the WebSocket opening handshake (RFC 6455 computes
 * Sec-WebSocket-Accept as Base64(SHA1(key || GUID))). SHA-1 is fine here —
 * the handshake uses it as a protocol checksum, not for security. Do NOT
 * reach for this module for anything that needs a real cryptographic hash.
 *
 * Runs once per connection attempt; no per-frame cost, no libm, no malloc.
 */

typedef struct {
    uint32_t h[5];
    uint64_t total;        /* total bytes hashed */
    uint8_t  block[64];
    uint32_t fill;         /* bytes buffered in block[] */
} sha1_ctx_t;

void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const void *data, size_t len);
void sha1_final(sha1_ctx_t *ctx, uint8_t out[20]);

/* One-shot convenience. */
void sha1(const void *data, size_t len, uint8_t out[20]);

/* Standard Base64 (RFC 4648, with padding). Writes the NUL-terminated encoding
   of `n` input bytes into `out` (capacity `cap`); returns the string length,
   or -1 if `cap` is too small. */
int base64_encode(const uint8_t *in, size_t n, char *out, size_t cap);

#endif /* SHA1_H */
