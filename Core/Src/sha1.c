#include "sha1.h"
#include <string.h>

/* Straightforward FIPS 180-1 SHA-1, in the style of Steve Reid's public-domain
   implementation. Compact and allocation-free; W[80] costs 320 B of stack on
   the (handshake-only) caller. */

static uint32_t rol(uint32_t v, int s)
{
    return (v << s) | (v >> (32 - s));
}

static void sha1_block(sha1_ctx_t *ctx, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2];
    uint32_t d = ctx->h[3], e = ctx->h[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c;
    ctx->h[3] += d; ctx->h[4] += e;
}

void sha1_init(sha1_ctx_t *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->total = 0;
    ctx->fill  = 0;
}

void sha1_update(sha1_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->total += len;

    if (ctx->fill > 0) {
        while (len > 0 && ctx->fill < 64) {
            ctx->block[ctx->fill++] = *p++;
            len--;
        }
        if (ctx->fill == 64) {
            sha1_block(ctx, ctx->block);
            ctx->fill = 0;
        }
    }
    while (len >= 64) {
        sha1_block(ctx, p);
        p += 64;
        len -= 64;
    }
    while (len > 0) {
        ctx->block[ctx->fill++] = *p++;
        len--;
    }
}

void sha1_final(sha1_ctx_t *ctx, uint8_t out[20])
{
    uint64_t bits = ctx->total * 8u;
    uint8_t pad = 0x80;
    sha1_update(ctx, &pad, 1);
    pad = 0x00;
    while (ctx->fill != 56) {
        sha1_update(ctx, &pad, 1);
    }
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) {
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_update(ctx, lenb, 8);

    for (int i = 0; i < 5; i++) {
        out[4 * i]     = (uint8_t)(ctx->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(ctx->h[i]);
    }
}

void sha1(const void *data, size_t len, uint8_t out[20])
{
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}

int base64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 4 * ((n + 2) / 3);
    if (cap < olen + 1) {
        return -1;
    }
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = tbl[(v >> 6) & 0x3F];
        out[o++] = tbl[v & 0x3F];
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? tbl[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return (int)o;
}
