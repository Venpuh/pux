#ifndef PUX_BUILDER_H
#define PUX_BUILDER_H

#include <stddef.h>
#include "pux/package.h"

int pux_package_build(const char *manifest_path, const char *payload_dir,
                      const char *output_path, char *error, size_t error_size);

#endif
