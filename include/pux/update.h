#ifndef PUX_UPDATE_H
#define PUX_UPDATE_H

#include <stddef.h>

#define PUX_REMOTE_INDEX_MAX_SIZE (16U * 1024U * 1024U)
#define PUX_REMOTE_SIGNATURE_MAX_SIZE (64U * 1024U)

int pux_repo_update(const char *repository_url,
                    const char *repository_dir,
                    const char *trusted_keys_root,
                    int require_signed,
                    char *error, size_t error_size);

#endif
