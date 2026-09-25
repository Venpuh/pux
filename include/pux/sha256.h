#ifndef PUX_SHA256_H
#define PUX_SHA256_H

#include <stddef.h>

#define PUX_SHA256_HEX_SIZE 65U

struct pux_sha256_ctx {
    unsigned state[8];
    unsigned char buffer[64];
    size_t buffer_size;
    unsigned long long total_size;
};

void pux_sha256_init(struct pux_sha256_ctx *ctx);
void pux_sha256_update(struct pux_sha256_ctx *ctx,
                       const unsigned char *data, size_t size);
void pux_sha256_final(struct pux_sha256_ctx *ctx,
                      unsigned char digest[32]);
void pux_sha256_hex(const unsigned char digest[32],
                    char output[PUX_SHA256_HEX_SIZE]);
int pux_sha256_file(const char *path,
                    char output[PUX_SHA256_HEX_SIZE],
                    char *error, size_t error_size);

#endif
