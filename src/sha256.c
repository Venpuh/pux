#define _POSIX_C_SOURCE 200809L

#include "pux/sha256.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(unsigned) == 4U, "pux SHA-256 requires 32-bit unsigned");

static unsigned rotate_right(unsigned value, unsigned bits)
{
    return (value >> bits) | (value << (32U - bits));
}

static unsigned load_be32(const unsigned char *data)
{
    return ((unsigned)data[0] << 24U) |
           ((unsigned)data[1] << 16U) |
           ((unsigned)data[2] << 8U) |
           (unsigned)data[3];
}

static void store_be32(unsigned char *data, unsigned value)
{
    data[0] = (unsigned char)(value >> 24U);
    data[1] = (unsigned char)(value >> 16U);
    data[2] = (unsigned char)(value >> 8U);
    data[3] = (unsigned char)value;
}

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static void set_errorf(char *error, size_t error_size, const char *format, const char *value)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, format, value);
    }
}

static void transform(struct pux_sha256_ctx *ctx, const unsigned char block[64])
{
    static const unsigned k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    unsigned w[64];
    for (size_t i = 0U; i < 16U; ++i) {
        w[i] = load_be32(block + (i * 4U));
    }
    for (size_t i = 16U; i < 64U; ++i) {
        const unsigned s0 = rotate_right(w[i - 15U], 7U) ^
                            rotate_right(w[i - 15U], 18U) ^
                            (w[i - 15U] >> 3U);
        const unsigned s1 = rotate_right(w[i - 2U], 17U) ^
                            rotate_right(w[i - 2U], 19U) ^
                            (w[i - 2U] >> 10U);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    unsigned a = ctx->state[0];
    unsigned b = ctx->state[1];
    unsigned c = ctx->state[2];
    unsigned d = ctx->state[3];
    unsigned e = ctx->state[4];
    unsigned f = ctx->state[5];
    unsigned g = ctx->state[6];
    unsigned h = ctx->state[7];

    for (size_t i = 0U; i < 64U; ++i) {
        const unsigned s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        const unsigned ch = (e & f) ^ ((~e) & g);
        const unsigned temp1 = h + s1 + ch + k[i] + w[i];
        const unsigned s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        const unsigned maj = (a & b) ^ (a & c) ^ (b & c);
        const unsigned temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void pux_sha256_init(struct pux_sha256_ctx *ctx)
{
    if (ctx == NULL) return;
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->buffer_size = 0U;
    ctx->total_size = 0ULL;
}

void pux_sha256_update(struct pux_sha256_ctx *ctx, const unsigned char *data, size_t size)
{
    if (ctx == NULL || (data == NULL && size != 0U)) return;
    ctx->total_size += (unsigned long long)size;

    while (size > 0U) {
        const size_t remaining = sizeof(ctx->buffer) - ctx->buffer_size;
        const size_t take = size < remaining ? size : remaining;
        memcpy(ctx->buffer + ctx->buffer_size, data, take);
        ctx->buffer_size += take;
        data += take;
        size -= take;
        if (ctx->buffer_size == sizeof(ctx->buffer)) {
            transform(ctx, ctx->buffer);
            ctx->buffer_size = 0U;
        }
    }
}

void pux_sha256_final(struct pux_sha256_ctx *ctx, unsigned char digest[32])
{
    if (ctx == NULL || digest == NULL) return;
    const unsigned long long bit_length = ctx->total_size * 8ULL;

    ctx->buffer[ctx->buffer_size++] = 0x80U;
    if (ctx->buffer_size > 56U) {
        while (ctx->buffer_size < 64U) ctx->buffer[ctx->buffer_size++] = 0U;
        transform(ctx, ctx->buffer);
        ctx->buffer_size = 0U;
    }
    while (ctx->buffer_size < 56U) ctx->buffer[ctx->buffer_size++] = 0U;
    for (unsigned i = 0U; i < 8U; ++i) {
        ctx->buffer[56U + i] = (unsigned char)(bit_length >> (56U - (8U * i)));
    }
    transform(ctx, ctx->buffer);
    for (size_t i = 0U; i < 8U; ++i) {
        store_be32(digest + (i * 4U), ctx->state[i]);
    }
    memset(ctx, 0, sizeof(*ctx));
}

void pux_sha256_hex(const unsigned char digest[32], char output[PUX_SHA256_HEX_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    if (digest == NULL || output == NULL) return;
    for (size_t i = 0U; i < 32U; ++i) {
        output[i * 2U] = hex[digest[i] >> 4U];
        output[(i * 2U) + 1U] = hex[digest[i] & 0x0fU];
    }
    output[64U] = '\0';
}

int pux_sha256_file(const char *path, char output[PUX_SHA256_HEX_SIZE], char *error, size_t error_size)
{
    if (path == NULL || output == NULL) {
        set_error(error, error_size, "invalid SHA-256 argument");
        return -1;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot open file for SHA-256: %s", strerror(errno));
        return -1;
    }

    struct pux_sha256_ctx ctx;
    unsigned char buffer[65536];
    pux_sha256_init(&ctx);
    for (;;) {
        const size_t read_size = fread(buffer, 1U, sizeof(buffer), file);
        if (read_size > 0U) pux_sha256_update(&ctx, buffer, read_size);
        if (read_size < sizeof(buffer)) {
            if (ferror(file) != 0) {
                (void)fclose(file);
                set_errorf(error, error_size, "cannot read file for SHA-256: %s", strerror(errno));
                return -1;
            }
            break;
        }
    }
    if (fclose(file) != 0) {
        set_error(error, error_size, "cannot close file after SHA-256");
        return -1;
    }

    unsigned char digest[32];
    pux_sha256_final(&ctx, digest);
    pux_sha256_hex(digest, output);
    return 0;
}
