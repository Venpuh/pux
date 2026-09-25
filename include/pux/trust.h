#ifndef PUX_TRUST_H
#define PUX_TRUST_H

#include <stddef.h>
#include <stdio.h>

#include "pux/signature.h"

#define PUX_TRUST_DEFAULT_ROOT "/etc/pux/trusted-keys"
#define PUX_TRUST_ENV_ROOT "PUX_TRUSTED_KEYS_ROOT"
#define PUX_TRUST_MAX_KEYS 10000U
#define PUX_TRUST_KEY_NAME_SUFFIX ".pub"
#define PUX_TRUST_KEY_FILE_MAX 4096U

int pux_trust_add_key(const char *trusted_root,
                      const char *public_key_path,
                      char output_keyid[PUX_SIGNATURE_KEYID_HEX_SIZE],
                      char *error, size_t error_size);

int pux_trust_remove_key(const char *trusted_root, const char *keyid,
                         char *error, size_t error_size);

int pux_trust_list_keys(const char *trusted_root, FILE *output,
                        char *error, size_t error_size);

int pux_trust_verify_repository(
    const char *trusted_root, const char *repository_dir,
    char output_keyid[PUX_SIGNATURE_KEYID_HEX_SIZE],
    char *error, size_t error_size);

#endif
