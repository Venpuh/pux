#ifndef PUX_EXTRACT_H
#define PUX_EXTRACT_H

#include <stddef.h>

int pux_package_archive_extract(const char *package_path,
                                const char *destination,
                                char *error, size_t error_size);

#endif
