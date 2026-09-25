#ifndef PUX_CONTAINER_H
#define PUX_CONTAINER_H

#include <stddef.h>
#include "pux/package.h"

int pux_package_archive_validate(const char *path,
                                 struct pux_package_manifest *manifest,
                                 char *error, size_t error_size);

int pux_package_archive_info(const char *path,
                             struct pux_package_manifest *manifest,
                             char *error, size_t error_size);

#endif
