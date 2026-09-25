#ifndef PUX_SIGNATURE_H
#define PUX_SIGNATURE_H

#include <stddef.h>

#define PUX_SIGNATURE_ALGORITHM "ed25519"
#define PUX_SIGNATURE_BYTES 64U
#define PUX_SIGNATURE_PRIVATE_KEY_SIZE 32U
#define PUX_SIGNATURE_PUBLIC_KEY_SIZE 32U
#define PUX_SIGNATURE_HEX_SIZE (PUX_SIGNATURE_BYTES * 2U + 1U)
#define PUX_SIGNATURE_KEYID_HEX_SIZE 65U
#define PUX_SIGNATURE_MAX_INPUT (16U * 1024U * 1024U)
#define PUX_SIGNATURE_MAX_LINE 512U

int pux_signature_keygen(const char *private_key_path,
                         const char *public_key_path,
                         char *error, size_t error_size);

int pux_signature_keyid(const char *public_key_path,
                        char output[PUX_SIGNATURE_KEYID_HEX_SIZE],
                        char *error, size_t error_size);

int pux_signature_sign_file(const char *input_path,
                            const char *private_key_path,
                            const char *signature_path,
                            char *error, size_t error_size);

int pux_signature_verify_file(const char *input_path,
                              const char *signature_path,
                              const char *public_key_path,
                              char *error, size_t error_size);

int pux_signature_file_keyid(const char *signature_path,
                             char output[PUX_SIGNATURE_KEYID_HEX_SIZE],
                             char *error, size_t error_size);

#endif
